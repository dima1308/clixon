/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * Yang module and feature handling
 * @see https://tools.ietf.org/html/rfc7895
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <regex.h>
#include <dirent.h>
#include <sys/types.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_log.h"
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_file.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_io.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"
#include "clixon_netconf_lib.h"
#include "clixon_xml_map.h"
#include "clixon_yang_parse_lib.h"

/*! Force add ietf-yang-library@2019-01-04 on all mount-points
 * This is a limitation of of the current implementation
 */
#define YANG_SCHEMA_MOUNT_YANG_LIB_FORCE

/*! Create modstate structure
 *
 * @retval     md    modstate struct
 * @retval     NULL  Error
 * @see modstate_diff_free
 */
modstate_diff_t *
modstate_diff_new(void)
{
    modstate_diff_t *md;

    if ((md = malloc(sizeof(modstate_diff_t))) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        return NULL;
    }
    memset(md, 0, sizeof(modstate_diff_t));
    return md;
}

/*! Free modstate structure
 *
 * @param[in] md Modstate struct
 * @retval    0     OK
 * @see modstate_diff_new
 */
int
modstate_diff_free(modstate_diff_t *md)
{
    if (md == NULL)
        return 0;
    if (md->md_content_id)
       free(md->md_content_id);
    if (md->md_diff)
        xml_free(md->md_diff);
    free(md);
    return 0;
}

/*! Init the Yang module library
 *
 * Load RFC7895 yang spec, module-set-id, etc.
 * @param[in]  h       Clicon handle
 * @retval     0     OK
 * @retval    -1     Error
 * @see netconf_module_load
 */
int
yang_modules_init(clicon_handle h)
{
    int        retval = -1;
    yang_stmt *yspec;

    yspec = clicon_dbspec_yang(h);      
    if (!clicon_option_bool(h, "CLICON_YANG_LIBRARY"))
        goto ok;
    /* Ensure module-set-id is set */
    if (!clicon_option_exists(h, "CLICON_MODULE_SET_ID")){
        clicon_err(OE_CFG, ENOENT, "CLICON_MODULE_SET_ID must be defined when CLICON_YANG_LIBRARY is enabled");
        goto done;
    }
    /* Ensure revision exists is set */
    if (yang_spec_parse_module(h, "ietf-yang-library", NULL, yspec)< 0)
        goto done;
    /* Find revision */
    if (yang_modules_revision(h) == NULL){
        clicon_err(OE_CFG, ENOENT, "Yang client library yang spec has no revision");
        goto done;
    }
 ok:
     retval = 0;
 done:
     return retval;
}

/*! Return RFC7895 revision (if parsed)
 * @param[in]    h        Clicon handle
 * @retval       revision String (dont free)
 * @retval       NULL     Error: RFC7895 not loaded or revision not found
 */
char *
yang_modules_revision(clicon_handle h)
{
    yang_stmt *yspec;
    yang_stmt *ymod;
    yang_stmt *yrev;
    char      *revision = NULL;

    yspec = clicon_dbspec_yang(h);
    if ((ymod = yang_find(yspec, Y_MODULE, "ietf-yang-library")) != NULL ||
        (ymod = yang_find(yspec, Y_SUBMODULE, "ietf-yang-library")) != NULL){
        if ((yrev = yang_find(ymod, Y_REVISION, NULL)) != NULL){
            revision = yang_argument_get(yrev);
        }
    }
    return revision;
}

/*! Actually build the yang modules state XML tree according to RFC8525
 *
 * @param[in]  h     Clixon handle
 * @param[in]  yspec
 * @param[in]  msid
 * @param[in]  brief
 * @param[out] cb
 * @retval     0     OK
 * @retval    -1     Error
 * This assumes CLICON_YANG_LIBRARY is enabled
 * @see RFC8525 
 */
int
yang_modules_state_build(clicon_handle    h,
                         yang_stmt       *yspec,
                         char            *msid,
                         int              brief,
                         cbuf            *cb)
{
    int         retval = -1;
    yang_stmt  *ylib = NULL; /* ietf-yang-library */
    char       *module = "ietf-yang-library";
    yang_stmt  *ys;
    yang_stmt  *yc;
    yang_stmt  *ymod;        /* generic module */
    yang_stmt  *yns = NULL;  /* namespace */
    yang_stmt  *yinc;
    yang_stmt  *ysub;
    char       *name;

    /* In case of several mountpoints, this is always the top-level */
    if ((ylib = yang_find(yspec, Y_MODULE, module)) == NULL
        /* &&        (ylib = yang_find(yspec0, Y_SUBMODULE, module)) == NULL */
        ){
            clicon_err(OE_YANG, 0, "%s not found", module);
            goto done;
        }
    if ((yns = yang_find(ylib, Y_NAMESPACE, NULL)) == NULL){
        clicon_err(OE_YANG, 0, "%s yang namespace not found", module);
        goto done;
    }
    /* RFC 8525 */
    cprintf(cb,"<yang-library xmlns=\"%s\">", yang_argument_get(yns));
    cprintf(cb,"<content-id>%s</content-id>", msid);
    cprintf(cb,"<module-set><name>default</name>");
    ymod = NULL;
    while ((ymod = yn_each(yspec, ymod)) != NULL) {
        if (yang_keyword_get(ymod) != Y_MODULE)
            continue;
        cprintf(cb,"<module>");
        cprintf(cb,"<name>%s</name>", yang_argument_get(ymod));
        if ((ys = yang_find(ymod, Y_REVISION, NULL)) != NULL)
            cprintf(cb,"<revision>%s</revision>", yang_argument_get(ys));
        else{
            /* RFC7895 1 If no (such) revision statement exists, the module's or 
               submodule's revision is the zero-length string. 
               But in RFC8525 this has changed to: 
               If no revision statement is present in the YANG module or submodule, this
               leaf is not instantiated.
               cprintf(cb,"<revision></revision>");
            */
        }
        if ((ys = yang_find(ymod, Y_NAMESPACE, NULL)) != NULL)
            cprintf(cb,"<namespace>%s</namespace>", yang_argument_get(ys));
        else
            cprintf(cb,"<namespace></namespace>");
        /* This follows order in rfc 7895: feature, conformance-type, 
           submodules */
        if (!brief){
            yc = NULL;
            while ((yc = yn_each(ymod, yc)) != NULL) {
                switch(yang_keyword_get(yc)){
                case Y_FEATURE:
                    if (yang_cv_get(yc) && cv_bool_get(yang_cv_get(yc)))
                        cprintf(cb,"<feature>%s</feature>", yang_argument_get(yc));
                    break;
                default:
                    break;
                }
            }
        }
        yinc = NULL;
        while ((yinc = yn_each(ymod, yinc)) != NULL) {
            if (yang_keyword_get(yinc) != Y_INCLUDE)
                continue;
            cprintf(cb,"<submodule>");
            name = yang_argument_get(yinc);
            cprintf(cb,"<name>%s</name>", name);
            if ((ysub = yang_find(yspec, Y_SUBMODULE, name)) != NULL){
                if ((ys = yang_find(ysub, Y_REVISION, NULL)) != NULL)
                    cprintf(cb,"<revision>%s</revision>", yang_argument_get(ys));
            }
            cprintf(cb,"</submodule>");
        }
        cprintf(cb,"</module>");
    }
    cprintf(cb,"</module-set></yang-library>");
    retval = 0;
 done:
    return retval;
}

/*! Get modules state according to RFC 7895
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   XML Xpath
 * @param[in]     nsc     XML Namespace context for xpath
 * @param[in]     brief   Just name, revision and uri (no cache)
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @retval        1       OK
 * @retval        0       Statedata callback failed
 * @retval       -1       Error (fatal)
 * @notes NYI: schema, deviation
x      +--ro modules-state
x         +--ro module-set-id    string
x         +--ro module* [name revision]
x            +--ro name                yang:yang-identifier
x            +--ro revision            union
            +--ro schema?             inet:uri
x            +--ro namespace           inet:uri
            +--ro feature*            yang:yang-identifier
            +--ro deviation* [name revision]
            |  +--ro name        yang:yang-identifier
            |  +--ro revision    union
            +--ro conformance-type    enumeration
            +--ro submodule* [name revision]
               +--ro name        yang:yang-identifier
               +--ro revision    union
               +--ro schema?     inet:uri
 * @see netconf_hello_server
 */
int
yang_modules_state_get(clicon_handle    h,
                       yang_stmt       *yspec,
                       char            *xpath,
                       cvec            *nsc,
                       int              brief,
                       cxobj          **xret)
{
    int         retval = -1;
    cxobj      *x = NULL; /* Top tree, some juggling w top symbol */
    char       *msid; /* modules-set-id */
    cxobj      *xc;   /* original cache */
    cbuf       *cb = NULL;
    int         ret;
    cxobj     **xvec = NULL;
    size_t      xlen;
    int         i;

    msid = clicon_option_str(h, "CLICON_MODULE_SET_ID"); /* In RFC 8525 changed to "content-id" */
    if ((xc = clicon_modst_cache_get(h, brief)) != NULL){
        cxobj *xw; /* tmp top wrap object */
        /* xc is here: <modules-state>... 
         * need to wrap it for xpath: <top><modules-state> */
        /* xc is also original tree, need to copy it */
        if ((xw = xml_wrap(xc, "top")) == NULL)
            goto done;
        if (xpath_first(xw, nsc, "%s", xpath)){
            if ((x = xml_dup(xc)) == NULL) /* Make copy and use below */
                goto done;
        }
        if (xml_rootchild_node(xw, xc) < 0)  /* Unwrap x / free xw */
            goto done;
    }
    else { /* No cache -> build the tree */
        if ((cb = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, 0, "clicon buffer");
            goto done;
        }
        /* Build a cb string: <modules-state>... */
        if (yang_modules_state_build(h, yspec, msid, brief, cb) < 0)
            goto done;
        /* Parse cb, x is on the form: <top><modules-state>... 
         * Note, list is not sorted since it is state (should not be)
         */
        if (clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &x, NULL) < 0){
            if (xret && netconf_operation_failed_xml(xret, "protocol", clicon_err_reason)< 0)
                goto done;
            goto fail;
        }
        if (xml_rootchild(x, 0, &x) < 0)
            goto done;
        /* x is now: <modules-state>... */
        if (clicon_modst_cache_set(h, brief, x) < 0) /* move to fn above? */
            goto done;
    }
    if (x){ /* x is here a copy (This code is ugly and I think wrong) */
        /* Wrap x (again) with new top-level node "top" which xpath wants */
        if ((x = xml_wrap(x, "top")) < 0)
            goto done;
        /* extract xpath part of module-state tree */
        if (xpath_vec(x, nsc, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
            goto done;
        if (xvec != NULL){
            for (i=0; i<xlen; i++)
                xml_flag_set(xvec[i], XML_FLAG_MARK);
        }
        /* Remove everything that is not marked */
        if (xml_tree_prune_flagged_sub(x, XML_FLAG_MARK, 1, NULL) < 0)
            goto done;
        if ((ret = netconf_trymerge(x, yspec, xret)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (xvec)
        free(xvec);
    if (cb)
        cbuf_free(cb);
    if (x)
        xml_free(x);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! For single module state with namespace, get revisions and send upgrade callbacks
 * @param[in]  h        Clicon handle
 * @param[in]  xt       Top-level XML tree to be updated (includes other ns as well)
 * @param[in]  xmod     XML module state diff (for one yang module)
 * @param[in]  ns       Namespace of module state we are looking for
 * @param[out] cbret    Netconf error message if invalid
 * @retval     1        OK
 * @retval     0        Validation failed (cbret set)
 * @retval    -1        Error
 */
static int
mod_ns_upgrade(clicon_handle h,
               cxobj        *xt,
               cxobj        *xmod,
               char         *ns,
               cbuf         *cbret)
{
    int        retval = -1;
    char      *b; /* string body */
    yang_stmt *ymod;
    yang_stmt *yrev;
    uint32_t   from = 0;
    uint32_t   to = 0;
    int        ret;
    yang_stmt *yspec;

    /* If modified or removed get from revision from file */
    if (xml_flag(xmod, (XML_FLAG_CHANGE|XML_FLAG_DEL)) != 0x0){
        if ((b = xml_find_body(xmod, "revision")) != NULL) /* Module revision */
            if (ys_parse_date_arg(b, &from) < 0)
                goto done;
    }
    /* If modified or added get to revision from system */
    if (xml_flag(xmod, (XML_FLAG_CHANGE|XML_FLAG_ADD)) != 0x0){
        yspec = clicon_dbspec_yang(h);
        if ((ymod = yang_find_module_by_namespace(yspec, ns)) == NULL){
            cprintf(cbret, "Module-set upgrade header contains namespace %s, but is not found in running system", ns);
            goto fail;
        }
        if ((yrev = yang_find(ymod, Y_REVISION, NULL)) == NULL){
            retval = 1;
            goto done;
        }
        if (ys_parse_date_arg(yang_argument_get(yrev), &to) < 0)
            goto done;
    }
    if ((ret = upgrade_callback_call(h, xt, ns,
                                     xml_flag(xmod, (XML_FLAG_ADD|XML_FLAG_DEL|XML_FLAG_CHANGE)),
                                     from, to, cbret)) < 0)
        goto done;
    if (ret == 0) /* XXX ignore and continue? */
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Upgrade XML
 * @param[in]  h    Clicon handle
 * @param[in]  xt   XML tree (to upgrade)
 * @param[in]  msd  Modules-state differences of xt
 * @param[out] cbret Netconf error message if invalid
 * @retval     1    OK
 * @retval     0    Validation failed (cbret set)
 * @retval    -1    Error
 */
int
clixon_module_upgrade(clicon_handle    h,
                      cxobj           *xt,
                      modstate_diff_t *msd,
                      cbuf            *cbret)
{
    int      retval = -1;
    char   *ns;           /* Namespace */
    cxobj  *xmod;           /* XML module state diff */
    int     ret;

    if (msd == NULL){
        clicon_err(OE_CFG, EINVAL, "No modstate");
        goto done;
    }
    if (msd->md_status == 0) /* No modstate in startup */
        goto ok;
    /* Iterate through xml modified module state 
     * Note top-level here is typically module-set
     */
    xmod = NULL;
    while ((xmod = xml_child_each(msd->md_diff, xmod, CX_ELMNT)) != NULL) {
        /* Extract namespace */
        if ((ns = xml_find_body(xmod, "namespace")) == NULL)
            goto done;
        /* Extract revisions and make callbacks */
        if ((ret = mod_ns_upgrade(h, xt, xmod, ns, cbret)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Given a yang statement and a prefix, return yang module to that relative prefix
 * Note, not the other module but the proxy import statement only
 * @param[in]  ys      A yang statement
 * @param[in]  prefix  prefix
 * @retval     ymod    Yang module statement if found
 * @retval     NULL    not found
 * @note Prefixes are relative to the module they are defined
 * @see yang_find_module_by_name
 * @see yang_find_module_by_namespace
 * @see yang_find_namespace_by_prefix
 */
yang_stmt *
yang_find_module_by_prefix(yang_stmt *ys, 
                           char      *prefix)
{
    yang_stmt *yimport;
    yang_stmt *yprefix;
    yang_stmt *my_ymod;
    yang_stmt *ymod = NULL;
    yang_stmt *yspec;
    char      *myprefix;

    if ((yspec = ys_spec(ys)) == NULL){
        clicon_err(OE_YANG, 0, "My yang spec not found");
        goto done;
    }
    /* First try own module */
    if ((my_ymod = ys_module(ys)) == NULL){
        clicon_err(OE_YANG, 0, "My yang module not found");
        goto done;
    }
    myprefix = yang_find_myprefix(ys);
    if (myprefix && strcmp(myprefix, prefix) == 0){
        ymod = my_ymod;
        goto done;
    }
    /* If no match, try imported modules */
    yimport = NULL;
    while ((yimport = yn_each(my_ymod, yimport)) != NULL) {
        if (yang_keyword_get(yimport) != Y_IMPORT)
            continue;
        if ((yprefix = yang_find(yimport, Y_PREFIX, NULL)) != NULL &&
            strcmp(yang_argument_get(yprefix), prefix) == 0){
            break;
        }
    }
    if (yimport){
        if ((ymod = yang_find(yspec, Y_MODULE, yang_argument_get(yimport))) == NULL){
            clicon_err(OE_YANG, 0, "No module or sub-module found with prefix %s", 
                       prefix); 
            yimport = NULL;
            goto done; /* unresolved */
        }
    }
 done:
    return ymod;
}

/* Get module from its own prefix 
 * This is really not a valid usecase, a kludge for the identityref derived
 * list workaround (IDENTITYREF_KLUDGE)
 * Actually, for canonical prefixes it is!
 */ 
yang_stmt *
yang_find_module_by_prefix_yspec(yang_stmt *yspec, 
                                 char      *prefix)
{
    yang_stmt *ymod = NULL;
    yang_stmt *yprefix;
    
    while ((ymod = yn_each(yspec, ymod)) != NULL) 
        if (yang_keyword_get(ymod) == Y_MODULE &&
            (yprefix = yang_find(ymod, Y_PREFIX, NULL)) != NULL &&
            strcmp(yang_argument_get(yprefix), prefix) == 0)
            return ymod;
    return NULL;
}

/*! Given a yang spec and a namespace, return yang module 
 *
 * @param[in]  yspec      A yang specification
 * @param[in]  ns         namespace
 * @retval     ymod       Yang module statement if found
 * @retval     NULL       not found
 * @see yang_find_module_by_name
 * @see yang_find_module_by_prefix    module-specific prefix
 * @see yang_find_prefix_by_namespace
 */
yang_stmt *
yang_find_module_by_namespace(yang_stmt *yspec, 
                              char      *ns)
{
    yang_stmt *ymod = NULL;

    if (ns == NULL)
        goto done;
    while ((ymod = yn_each(yspec, ymod)) != NULL) {
        if (yang_find(ymod, Y_NAMESPACE, ns) != NULL)
            break;
    }
 done:
    return ymod;
}

/*! Given a yang spec, a namespace and revision, return yang module 
 *
 * @param[in]  yspec      A yang specification
 * @param[in]  ns         Namespace
 * @param[in]  rev        Revision
 * @retval     ymod       Yang module statement if found
 * @retval     NULL       not found
 * @see yang_find_module_by_namespace
 * @note a module may have many revisions, but only the first is significant
 */
yang_stmt *
yang_find_module_by_namespace_revision(yang_stmt  *yspec, 
                                       const char *ns,
                                       const char *rev)
{
    yang_stmt *ymod = NULL;
    yang_stmt *yrev;
    char      *rev1;

    if (ns == NULL || rev == NULL){
        clicon_err(OE_CFG, EINVAL, "No ns or rev");
        goto done;
    }
    while ((ymod = yn_each(yspec, ymod)) != NULL) {
        if (yang_find(ymod, Y_NAMESPACE, ns) != NULL)
            /* Get FIRST revision */
            if ((yrev = yang_find(ymod, Y_REVISION, NULL)) != NULL){
                rev1 = yang_argument_get(yrev);
                if (strcmp(rev, rev1) == 0)
                    break; /* return this ymod */
            }
    }
 done:
    return ymod;
}

/*! Given a yang spec, name and revision, return yang module 
 *
 * @param[in]  yspec      A yang specification
 * @param[in]  name       Name
 * @param[in]  rev        Revision
 * @retval     ymod       Yang module statement if found
 * @retval     NULL       not found
 * @see yang_find_module_by_namespace
 * @note a module may have many revisions, but only the first is significant
 */
yang_stmt *
yang_find_module_by_name_revision(yang_stmt  *yspec,
                                  const char *name,
                                  const char *rev)
{
    yang_stmt *ymod = NULL;
    yang_stmt *yrev;
    char      *rev1;

    if (name == NULL){
        clicon_err(OE_CFG, EINVAL, "No ns or rev");
        goto done;
    }
    while ((ymod = yn_each(yspec, ymod)) != NULL) {
        if (yang_keyword_get(ymod) != Y_MODULE)
            continue;
        if (strcmp(yang_argument_get(ymod), name) != 0)
            continue;
        if (rev == NULL)
            break; /* Matching revision is NULL, match that */
        /* Get FIRST revision */
        if ((yrev = yang_find(ymod, Y_REVISION, NULL)) != NULL){
            rev1 = yang_argument_get(yrev);
            if (strcmp(rev, rev1) == 0)
                break; /* return this ymod */
        }
    }
 done:
    return ymod;
}

/*! Given a yang spec and a module name, return yang module or submodule
 *
 * @param[in]  yspec      A yang specification
 * @param[in]  name       Name of module
 * @retval     ymod       Yang module statement if found
 * @retval     NULL       not found
 * @see yang_find_module_by_namespace
 * @see yang_find_module_by_prefix    module-specific prefix
 */
yang_stmt *
yang_find_module_by_name(yang_stmt *yspec, 
                         char      *name)
{
    yang_stmt *ymod = NULL;
    
    while ((ymod = yn_each(yspec, ymod)) != NULL) 
        if ((yang_keyword_get(ymod) == Y_MODULE || yang_keyword_get(ymod) == Y_SUBMODULE) &&
            strcmp(yang_argument_get(ymod), name)==0)
            return ymod;
    return NULL;
}

/*! Callback for handling RFC 7952 annotations
 *
 * A server indicates that it is prepared to handle that annotation according to the
 * annotation's definition.  That is, an annotation advertised by the
 * server may be attached to an instance of a data node defined in any
 * YANG module that is implemented by the server.
 * Possibly add them to yang parsing, cardinality, etc?
 * as described in Section 3.
 * Note this is called by the module using the extension md:annotate, not by 
 * ietf-yang-metadata.yang
 * @see yang_metadata_annotation_check
 */
static int
ietf_yang_metadata_extension_cb(clicon_handle h,
                                yang_stmt    *yext,
                                yang_stmt    *ys)
{
    int        retval = -1;
    char      *extname;
    char      *modname;
    yang_stmt *ymod;
    char      *name;
    
    ymod = ys_module(yext);
    modname = yang_argument_get(ymod);
    extname = yang_argument_get(yext);
    if (strcmp(modname, "ietf-yang-metadata") != 0 || strcmp(extname, "annotation") != 0)
        goto ok;
    name = cv_string_get(yang_cv_get(ys));
    clicon_debug(1, "%s Enabled extension:%s:%s:%s", __FUNCTION__, modname, extname, name);
    /* XXX Nothing yet - this should signal that xml attribute annotations are allowed 
     * Possibly, add an "annotation" YANG node.
     */
 ok:
    retval = 0;
    // done:
    return retval;
}

/*! Check annotation extension
 *
 * @param[in]   xa     XML attribute      
 * @param[in]   ys     YANG something
 * @param[out]  ismeta Set to 1 if this is an annotation
 * @retval      0      OK
 * @retval      -1     Error
 * @see ietf_yang_metadata_extension_cb
 * XXX maybe a cache would be appropriate?
 * XXX: return type?
 */
int
yang_metadata_annotation_check(cxobj     *xa,
                               yang_stmt *ymod,
                               int       *ismeta)
{
    int        retval = -1;
    yang_stmt *yma = NULL;
    char      *name;
    cg_var    *cv;
            
    /* Loop through annotations */
    while ((yma = yn_each(ymod, yma)) != NULL){ 
        /* Assume here md:annotation is written using canonical prefix */
        if (yang_keyword_get(yma) != Y_UNKNOWN)
            continue;
        if (strcmp(yang_argument_get(yma), "md:annotation") != 0)
            continue;
        if ((cv = yang_cv_get(yma)) != NULL &&
            (name = cv_string_get(cv)) != NULL){
            if (strcmp(name, xml_name(xa)) == 0){
                /* XXX: yang_find(yma,Y_TYPE,0) */
                *ismeta = 1;
                break;
            }
        }
    }
    retval = 0;
    // done:
    return retval;
}

/*! In case ietf-yang-metadata is loaded by application, handle annotation extension 
 * Consider moving fn
 * Must be called after clixon_plugin_module_init
 */
int
yang_metadata_init(clicon_handle h)
{
    int              retval = -1;
    clixon_plugin_t *cp = NULL;

    /* Create a pseudo-plugin to create extension callback to set the ietf-yang-meta
     * yang-data extension for api-root top-level restconf function.
     */
    if (clixon_pseudo_plugin(h, "pseudo yang metadata", &cp) < 0)
        goto done;
    clixon_plugin_api_get(cp)->ca_extension = ietf_yang_metadata_extension_cb;
    retval = 0;
 done:
    return retval;
}

/*! Given yang-lib module-set XML tree, parse modules into an yspec
 * 
 * Skip module if already loaded
 * This function is used where a yang-lib module-set is available to populate
 * an XML mount-point.
 * @param[in] h       Clicon handle
 * @param[in] yanglib XML tree on the form <yang-lib>...
 * @param[in] yspec   Will be populated with YANGs, is consumed
 * @retval    1       OK
 * @retval    0       Parse error
 * @retval   -1       Error
 * @see xml_schema_add_mount_points
 * XXX: Ensure yang-lib is always there otherwise get state dont work for mountpoint
 */
int
yang_lib2yspec(clicon_handle h,
               cxobj        *yanglib,
               yang_stmt    *yspec)
{
    int        retval = -1;
    cxobj     *xi;
    char      *name;
    char      *revision;
    cvec      *nsc = NULL;
    cxobj    **vec = NULL;
    size_t     veclen;
    int        i;
    yang_stmt *ymod;
    yang_stmt *yrev;
    int        modmin = 0;

    if (xpath_vec(yanglib, nsc, "module-set/module", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xi = vec[i];
        if ((name = xml_find_body(xi, "name")) == NULL)
            continue;
        revision = xml_find_body(xi, "revision");
        if ((ymod = yang_find(yspec, Y_MODULE, name)) != NULL ||
            (ymod = yang_find(yspec, Y_SUBMODULE, name)) != NULL){
            /* Skip if matching or no revision 
             * Note this algorithm does not work for multiple revisions
             */
            if ((yrev = yang_find(ymod, Y_REVISION, NULL)) == NULL){
                modmin++;
                continue;
            }
            if (revision && strcmp(yang_argument_get(yrev), revision) == 0){
                modmin++;
                continue;
            }
        }
        if (yang_parse_module(h, name, revision, yspec, NULL) == NULL)
            goto fail;
    }
#ifdef YANG_SCHEMA_MOUNT_YANG_LIB_FORCE
    /* Force add ietf-yang-library@2019-01-04 on all mount-points 
       otherwise get state dont work for mountpoint */
    if ((ymod = yang_find(yspec, Y_MODULE, "ietf-yang-library")) != NULL &&
        (yrev = yang_find(ymod, Y_REVISION, NULL)) != NULL &&
        strcmp(yang_argument_get(yrev), "2019-01-04") == 0){
        modmin++;
    }
    else if (yang_parse_module(h, "ietf-yang-library", "2019-01-04", yspec, NULL) < 0)
        goto fail;
    if ((modmin = yang_len_get(yspec) - (1+veclen - modmin)) < 0)
        goto fail;
    if (yang_parse_post(h, yspec, modmin) < 0)
        goto done;
#else
    if ((modmin = yang_len_get(yspec) - (1+veclen - modmin)) < 0)
        goto fail;
    if (yang_parse_post(h, yspec, modmin) < 0)
        goto done;
#endif
    retval = 1;
 done:
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}
