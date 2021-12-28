/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  
  Restconf event stream implementation. 
  See RFC 8040  RESTCONF Protocol
  Sections 3.8, 6, 9.3

  RFC8040:
   A RESTCONF server MAY send the "retry" field, and if it does, RESTCONF
   clients SHOULD use it.  A RESTCONF server SHOULD NOT send the "event" 
   or "id" fields, as there are no meaningful values. RESTCONF
   servers that do not send the "id" field also do not need to support
   the HTTP header field "Last-Event-ID"

   The RESTCONF client can then use this URL value to start monitoring
   the event stream:

      GET /streams/NETCONF HTTP/1.1
      Host: example.com
      Accept: text/event-stream
      Cache-Control: no-cache
      Connection: keep-alive

   The server MAY support the "start-time", "stop-time", and "filter"
   query parameters, defined in Section 4.8.  Refer to Appendix B.3.6
   for filter parameter examples.

   * Note that this implementation includes some hardcoded things for FCGI.
   * These are:
   * - req->listen_sock is used to register incoming fd events from (nginx) fcgi server
   * - The stream_child struct copies the FCGX_Request by value, so FCGX_Free() can be called 
   *   asynchronously
   * - In the forked variant, FCGX_Finish_r() and  FCGX_Free() are called (minor)
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>

#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <libgen.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include <fcgiapp.h> /* Need to be after clixon_xml.h due to attribute format */

#include "restconf_lib.h"
#include "restconf_handle.h"
#include "restconf_api.h"
#include "restconf_err.h"
#include "restconf_stream.h"

/*
 * Constants
 */
/* Enable for forking stream subscription loop. 
 * Disable to get single threading but blocking on streams
 */
#define STREAM_FORK 1

/* Keep track of children - when they exit - their FCGX handle needs to be 
 * freed with  FCGX_Free(&rbk, 0);
 */
struct stream_child{
    qelem_t              sc_q;   /* queue header */
    int                  sc_pid; /* Child process id */
    FCGX_Request         sc_r;   /* FCGI stream data. XXX this is by value */
};
/* Linked list of children
 * @note could hang STREAM_CHILD list on clicon handle instead.
 */
static struct stream_child *STREAM_CHILD = NULL; 

/*! Find restconf child using PID and cleanup FCGI Request data
 * @param[in]  h   Clicon handle
 * @param[in]  pid Process id of child
 * @note could hang STREAM_CHILD list on clicon handle instead.
 */
int
stream_child_free(clicon_handle h,
		  int           pid)
{
    struct stream_child *sc;
    
    if ((sc = STREAM_CHILD) != NULL){
	do {
	    if (pid == sc->sc_pid){
		DELQ(sc, STREAM_CHILD, struct stream_child *);
		FCGX_Free(&sc->sc_r, 0); /* XXX pointer to actual copied struct */
		free(sc);
		goto done;
	    }
	    sc = NEXTQ(struct stream_child *, sc);
	} while (sc && sc !=  STREAM_CHILD);
    }
 done:
    return 0;
}

int
stream_child_freeall(clicon_handle h)
{
    struct stream_child *sc;

    while ((sc = STREAM_CHILD) != NULL){
	DELQ(sc, STREAM_CHILD, struct stream_child *);
	FCGX_Free(&sc->sc_r, 1); /* XXX pointer to actual copied struct */
	free(sc);
    }
    return 0;
}

/*! Callback when stream notifications arrive from backend
 * @param[in]  s    Socket
 * @param[in]  req  Generic Www handle (can be part of clixon handle)
 */
static int
restconf_stream_cb(int   s, 
		   void *arg)
{
    int                retval = -1;
    FCGX_Request      *r = (FCGX_Request *)arg;
    int                eof;
    struct clicon_msg *reply = NULL;
    cxobj             *xtop = NULL; /* top xml */
    cxobj             *xn;        /* notification xml */
    cbuf              *cb = NULL;
    int                pretty = 0; /* XXX should be via arg */
    int                ret;
    
    clicon_debug(1, "%s", __FUNCTION__);
    /* get msg (this is the reason this function is called) */
    if (clicon_msg_rcv(s, &reply, &eof) < 0){
	clicon_debug(1, "%s msg_rcv error", __FUNCTION__);
	goto done;
    }
    clicon_debug(1, "%s msg: %s", __FUNCTION__, reply?reply->op_body:"null");
    /* handle close from remote end: this will exit the client */
    if (eof){
	clicon_debug(1, "%s eof", __FUNCTION__);
	clicon_err(OE_PROTO, ESHUTDOWN, "Socket unexpected close");
	errno = ESHUTDOWN;
	FCGX_FPrintF(r->out, "SHUTDOWN\r\n");
	FCGX_FPrintF(r->out, "\r\n");
	FCGX_FFlush(r->out);
	clixon_exit_set(1); 
	goto done;
    }
    if ((ret = clicon_msg_decode(reply, NULL, NULL, &xtop, NULL)) < 0)  /* XXX pass yang_spec */
	goto done;
    if (ret == 0){
	clicon_err(OE_XML, EFAULT, "Invalid notification");
	goto done;
    }
    /* create event */
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");
	goto done;
    }
    if ((xn = xpath_first(xtop, NULL, "notification")) == NULL)
	goto ok;
#ifdef notused
    xt = xpath_first(xn, NULL, "eventTime");
    if ((xe = xpath_first(xn, NULL, "event")) == NULL) /* event can depend on yang? */
	goto ok;

    if (xt)
	FCGX_FPrintF(r->out, "M#id: %s\r\n", xml_body(xt));
    else{ /* XXX */
	gettimeofday(&tv, NULL);
	FCGX_FPrintF(r->out, "M#id: %02d:0\r\n", tv.tv_sec);
    }
#endif
    if (clicon_xml2cbuf(cb, xn, 0, pretty, -1) < 0)
	goto done;
    FCGX_FPrintF(r->out, "data: %s\r\n", cbuf_get(cb));
    FCGX_FPrintF(r->out, "\r\n");
    FCGX_FFlush(r->out);
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval: %d", __FUNCTION__, retval);
    if (xtop != NULL)
	xml_free(xtop);
    if (reply)
	free(reply);
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Send subsctription to backend
 * @param[in]  h     Clicon handle
 * @param[in]  req   Generic Www handle (can be part of clixon handle)
 * @param[in]  name  Stream name
 * @param[in]  qvec
 * @param[in]  pretty    Pretty-print json/xml reply
 * @param[in]  media_out Restconf output media
 * @param[out] sp    Socket -1 if not set
 */
static int
restconf_stream(clicon_handle h,
		void         *req,
		char         *name,
		cvec         *qvec, 
		int           pretty,
		restconf_media media_out,
		int          *sp)
{
    int     retval = -1;
    cxobj  *xret = NULL;
    cxobj  *xe;
    cbuf   *cb = NULL;
    int     s; /* socket */
    int     i;
    cg_var *cv;
    char   *vname;

    clicon_debug(1, "%s", __FUNCTION__);
    *sp = -1;
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" %s><create-subscription xmlns=\"%s\"><stream>%s</stream>",
	    NETCONF_BASE_NAMESPACE, NETCONF_MESSAGE_ID_ATTR, EVENT_RFC5277_NAMESPACE, name);
    /* Print all fields */
    for (i=0; i<cvec_len(qvec); i++){
        cv = cvec_i(qvec, i);
	vname = cv_name_get(cv);
	if (strcmp(vname, "start-time") == 0){
	    cprintf(cb, "<startTime>");
	    cv2cbuf(cv, cb);
	    cprintf(cb, "</startTime>");
	}
	else if (strcmp(vname, "stop-time") == 0){
	    cprintf(cb, "<stopTime>");
	    cv2cbuf(cv, cb);
	    cprintf(cb, "</stopTime>");
	}
    }
    cprintf(cb, "</create-subscription></rpc>]]>]]>");
    if (clicon_rpc_netconf(h, cbuf_get(cb), &xret, &s) < 0)
	goto done;
    if ((xe = xpath_first(xret, NULL, "rpc-reply/rpc-error")) != NULL){
	if (api_return_err(h, req, xe, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }

    /* Setting up stream */
    if (restconf_reply_header(req, "Content-Type", "text/event-stream") < 0)
	goto done;
    if (restconf_reply_header(req, "Cache-Control", "no-cache") < 0)
	goto done;
    if (restconf_reply_header(req, "Connection", "keep-alive") < 0)
	goto done;
    if (restconf_reply_header(req, "X-Accel-Buffering", "no") < 0)
	goto done;
    if (restconf_reply_send(req, 201, NULL, 0) < 0)
	goto done;
    *sp = s;
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval: %d", __FUNCTION__, retval);
    if (xret)
	xml_free(xret);
    if (cb)
	cbuf_free(cb);
    return retval;
}

/* restconf */
#include "restconf_lib.h"
#include "restconf_stream.h"

/*! Listen sock callback (from proxy?)
 * @param[in]  s    Socket
 * @param[in]  req  Generic Www handle (can be part of clixon handle)
 */
static int
stream_checkuplink(int   s, 
		   void *arg)
{
    FCGX_Request      *r = (FCGX_Request *)arg;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (FCGX_GetError(r->out) != 0){ /* break loop */
	clicon_debug(1, "%s FCGX_GetError upstream", __FUNCTION__);
	clixon_exit_set(1);
    }
    return 0;
}

int
stream_timeout(int   s,
	       void *arg)
{
    struct timeval t;
    struct timeval t1;
    FCGX_Request *r = (FCGX_Request *)arg;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (FCGX_GetError(r->out) != 0){ /* break loop */
	clicon_debug(1, "%s FCGX_GetError upstream", __FUNCTION__);
	clixon_exit_set(1);
    }
    else{
	gettimeofday(&t, NULL);
	t1.tv_sec = 1; t1.tv_usec = 0;
	timeradd(&t, &t1, &t);
	clixon_event_reg_timeout(t, stream_timeout, arg, "Stream timeout");
    }
    return 0;
} 

/*! Process a stream request
 * @param[in]  h          Clicon handle
 * @param[in]  req        Generic Www handle (can be part of clixon handle)
 * @param[in]  qvec       Query parameters, ie the ?<id>=<val>&<id>=<val> stuff
 * @param[in]  streampath URI path for streams, eg /streams, see CLICON_STREAM_PATH
 * @param[out] finish 	  Set to zero, if request should not be finnished by upper layer
 */
int
api_stream(clicon_handle h,
	   void         *req,
	   cvec         *qvec,
	   char         *streampath,
	   int          *finish)
{
    int            retval = -1;
    FCGX_Request  *rfcgi = (FCGX_Request *)req; /* XXX */
    char          *path = NULL;
    char          *method;
    char         **pvec = NULL;
    int            pn;
    cvec          *pcvec = NULL; /* for rest api */
    cbuf          *cb = NULL;
    char          *indata;
    int            pretty;
    restconf_media media_out = YANG_DATA_XML; /* XXX default */
    cbuf          *cbret = NULL;
    int            s = -1;
    int            ret;
    cxobj         *xerr = NULL;
#ifdef STREAM_FORK
    int            pid;
    struct stream_child *sc;
#endif

    clicon_debug(1, "%s", __FUNCTION__);
    if ((path = restconf_uripath(h)) == NULL)
	goto done;
    pretty = restconf_pretty_get(h);
    if ((pvec = clicon_strsep(path, "/", &pn)) == NULL)
	goto done;
    /* Sanity check of path. Should be /stream/<name> */
    if (pn != 3){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid path, /stream/<name> expected") < 0)
	    goto done; 
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if (strlen(pvec[0]) != 0){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid path, /stream/<name> expected") < 0)
	    goto done; 
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if (strcmp(pvec[1], streampath)){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid path, /stream/<name> expected") < 0)
	    goto done; 
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    if ((method = pvec[2]) == NULL){
	if (netconf_invalid_value_xml(&xerr, "protocol", "Invalid path, /stream/<name> expected") < 0)
	    goto done; 
	if (api_return_err0(h, req, xerr, pretty, media_out, 0) < 0)
	    goto done;
	goto ok;
    }
    clicon_debug(1, "%s: method=%s", __FUNCTION__, method);

    if (uri_str2cvec(path, '/', '=', 1, &pcvec) < 0) /* rest url eg /album=ricky/foo */
	goto done;
    /* data */
    if ((cb = restconf_get_indata(req)) == NULL)
	goto done;
    indata = cbuf_get(cb);
    clicon_debug(1, "%s DATA=%s", __FUNCTION__, indata);

    /* If present, check credentials. See "plugin_credentials" in plugin  
     * See RFC 8040 section 2.5
     */
    if ((ret = restconf_authentication_cb(h, req, pretty, media_out)) < 0)
	goto done;
    if (ret == 0)
	goto ok;
    if (restconf_stream(h, req, method, qvec, pretty, media_out, &s) < 0)
	goto done;
    if (s != -1){
#ifdef STREAM_FORK
	if ((pid = fork()) == 0){ /* child */
	    if (pvec)
		free(pvec);
	    if (qvec)
		cvec_free(qvec);
	    if (pcvec)
		cvec_free(pcvec);
	    if (cb)
		cbuf_free(cb);
	    if (cbret)
		cbuf_free(cbret);
#endif /* STREAM_FORK */
	    /* Listen to backend socket */
	    if (clixon_event_reg_fd(s, 
			     restconf_stream_cb, 
			     req,
			     "stream socket") < 0)
		goto done;
	    if (clixon_event_reg_fd(rfcgi->listen_sock,
				    stream_checkuplink, 
				    req,
				    "stream socket") < 0)
		goto done;
	    /* Poll upstream errors */
	    stream_timeout(0, req);
	    /* Start loop */
	    clixon_event_loop(h);
	    close(s);
	    clixon_event_unreg_fd(s, restconf_stream_cb);
	    clixon_event_unreg_fd(rfcgi->listen_sock,
				  restconf_stream_cb);
	    clixon_event_unreg_timeout(stream_timeout, (void*)req);
	    clixon_exit_set(0); /* reset */
#ifdef STREAM_FORK
	    FCGX_Finish_r(rfcgi);
	    FCGX_Free(rfcgi, 0);	    
	    restconf_terminate(h);
	    exit(0);
	}
	/* parent */
	/* Create stream_child struct and store pid and FCGI data, when child
	 * killed, call FCGX_Free
	 */
	if ((sc = malloc(sizeof(struct stream_child))) == NULL){
	    clicon_err(OE_XML, errno, "malloc");
	    goto done;
	}
	memset(sc, 0, sizeof(struct stream_child));
	sc->sc_pid = pid;
	sc->sc_r = *rfcgi; /* XXX by value */

	ADDQ(sc, STREAM_CHILD);
	*finish = 0; /* If spawn child, we should not finish this stream */
#endif /* STREAM_FORK */
    }
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (xerr)
	xml_free(xerr);
    if (pvec)
	free(pvec);
    if (pcvec)
	cvec_free(pcvec);
    if (cb)
	cbuf_free(cb);
    if (cbret)
	cbuf_free(cbret);
    if (path)
	free(path);
    return retval;
}
