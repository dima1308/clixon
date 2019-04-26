/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

 * Clixon Datastore (XMLDB)
 * Saves Clixon data as clear-text XML (or JSON)
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_string.h"
#include "clixon_file.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_plugin.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_yang_module.h"
#include "clixon_datastore.h"

#include "clixon_datastore_write.h"
#include "clixon_datastore_read.h"

/*! Translate from symbolic database name to actual filename in file-system
 * @param[in]   th       text handle handle
 * @param[in]   db       Symbolic database name, eg "candidate", "running"
 * @param[out]  filename Filename. Unallocate after use with free()
 * @retval      0        OK
 * @retval     -1        Error
 * @note Could need a way to extend which databases exists, eg to register new.
 * The currently allowed databases are: 
 *   candidate, tmp, running, result
 * The filename reside in CLICON_XMLDB_DIR option
 */
int
xmldb_db2file(clicon_handle  h, 
	      const char    *db,
	      char         **filename)
{
    int   retval = -1;
    cbuf *cb = NULL;
    char *dir;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((dir = clicon_xmldb_dir(h)) == NULL){
	clicon_err(OE_XML, errno, "dbdir not set");
	goto done;
    }
    cprintf(cb, "%s/%s_db", dir, db);
    if ((*filename = strdup4(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Validate database name
 * @param[in]   db    Name of database 
 * @retval  0   OK
 * @retval  -1  Failed validate, xret set to error
 * XXX why is this function here? should be handled by netconf yang validation
 */
int
xmldb_validate_db(const char *db)
{
    if (strcmp(db, "running") != 0 && 
	strcmp(db, "candidate") != 0 && 
	strcmp(db, "startup") != 0 && 
	strcmp(db, "tmp") != 0)
	return -1;
    return 0;
}

/*! Connect to a datastore plugin, allocate resources to be used in API calls
 * @param[in]  h    Clicon handle
 * @retval     0    OK
 * @retval    -1    Error
 */
int
xmldb_connect(clicon_handle h)
{
    return 0;
}

/*! Disconnect from a datastore plugin and deallocate resources
 * @param[in]  handle  Disconect and deallocate from this handle
 * @retval     0       OK
 * @retval    -1    Error
 */
int
xmldb_disconnect(clicon_handle h)
{
    int       retval = -1;
    char    **keys = NULL;
    size_t    klen;
    int       i;
    db_elmnt *de;
    
    if (hash_keys(clicon_db_elmnt(h), &keys, &klen) < 0)
	goto done;
    for(i = 0; i < klen; i++) 
	if ((de = hash_value(clicon_db_elmnt(h), keys[i], NULL)) != NULL){
	    if (de->de_xml){
		xml_free(de->de_xml);
		de->de_xml = NULL;
	    }
	}
    retval = 0;
 done:
    if (keys)
	free(keys);
    return retval;
}


/*! Copy database from db1 to db2
 * @param[in]  h     Clicon handle
 * @param[in]  from  Source database
 * @param[in]  to    Destination database
 * @retval -1  Error
 * @retval  0  OK
  */
int 
xmldb_copy(clicon_handle h, 
	   const char   *from, 
	   const char   *to)
{
    int                 retval = -1;
    char               *fromfile = NULL;
    char               *tofile = NULL;
    db_elmnt           *de1 = NULL; /* from */
    db_elmnt           *de2 = NULL; /* to */
    db_elmnt            de0 = {0,};
    cxobj              *x1 = NULL;  /* from */
    cxobj              *x2 = NULL;  /* to */
		
    /* XXX lock */
    if (clicon_option_bool(h, "CLICON_XMLDB_CACHE")){
	/* Copy in-memory cache */
	/* 1. "to" xml tree in x1 */
	if ((de1 = clicon_db_elmnt_get(h, from)) != NULL)
	    x1 = de1->de_xml;
	if ((de2 = clicon_db_elmnt_get(h, to)) != NULL)
	    x2 = de2->de_xml;
	if (x1 == NULL && x2 == NULL){
	    /* do nothing */
	}
	else if (x1 == NULL){  /* free x2 and set to NULL */
	    xml_free(x2);
	    x2 = NULL;
	}
	else  if (x2 == NULL){ /* create x2 and copy from x1 */
	    if ((x2 = xml_new(xml_name(x1), NULL, xml_spec(x1))) == NULL)
		goto done;
	    if (xml_copy(x1, x2) < 0) 
		goto done;
	}
	else{ /* copy x1 to x2 */
	    xml_free(x2);
	    if ((x2 = xml_new(xml_name(x1), NULL, xml_spec(x1))) == NULL)
		goto done;
	    if (xml_copy(x1, x2) < 0) 
		goto done;
	}
	/* always set cache although not strictly necessary in case 1
	 * above, but logic gets complicated due to differences with
	 * de and de->de_xml */
	if (de2)
	    de0 = *de2;
	de0.de_xml = x2; /* The new tree */
	clicon_db_elmnt_set(h, to, &de0);
    }
    /* Copy the files themselves (above only in-memory cache) */
    if (xmldb_db2file(h, from, &fromfile) < 0)
	goto done;
    if (xmldb_db2file(h, to, &tofile) < 0)
	goto done;
    if (clicon_file_copy(fromfile, tofile) < 0)
	goto done;
    retval = 0;
 done:
    if (fromfile)
	free(fromfile);
    if (tofile)
	free(tofile);
    return retval;

}

/*! Lock database
 * @param[in]  h    Clicon handle
 * @param[in]  db   Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
  */
int 
xmldb_lock(clicon_handle h, 
	   const char   *db, 
	   int           pid)
{
    db_elmnt  *de = NULL;
    db_elmnt   de0 = {0,};

    if ((de = clicon_db_elmnt_get(h, db)) != NULL)
	de0 = *de;
    de0.de_pid = pid;
    clicon_db_elmnt_set(h, db, &de0);
    clicon_debug(1, "%s: locked by %u",  db, pid);
    return 0;
}

/*! Unlock database
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @param[in]  pid  Process id
 * @retval -1  Error
 * @retval  0  OK
 * Assume all sanity checks have been made
 */
int 
xmldb_unlock(clicon_handle h, 
	     const char   *db)
{
    db_elmnt  *de = NULL;

    if ((de = clicon_db_elmnt_get(h, db)) != NULL){
	de->de_pid = 0;
	clicon_db_elmnt_set(h, db, de);
    }
    return 0;
}

/*! Unlock all databases locked by pid (eg process dies) 
 * @param[in]    h   Clicon handle
 * @param[in]    pid Process / Session id
 * @retval -1    Error
 * @retval  0   OK
 */
int
xmldb_unlock_all(clicon_handle h, 
		 int           pid)
{
    int                 retval = -1;
    char              **keys = NULL;
    size_t              klen;
    int                 i;
    db_elmnt           *de;

    if (hash_keys(clicon_db_elmnt(h), &keys, &klen) < 0)
	goto done;
    for (i = 0; i < klen; i++) 
	if ((de = clicon_db_elmnt_get(h, keys[i])) != NULL &&
	    de->de_pid == pid){
	    de->de_pid = 0;
	    clicon_db_elmnt_set(h, keys[i], de);
	}
    retval = 0;
 done:
    if (keys)
	free(keys);
    return retval;
}

/*! Check if database is locked
 * @param[in]    h   Clicon handle
 * @param[in]    db  Database
 * @retval -1    Error
 * @retval   0   Not locked
 * @retval  >0   Id of locker
  */
int 
xmldb_islocked(clicon_handle h, 
	       const char   *db)
{
    db_elmnt  *de;

    if ((de = clicon_db_elmnt_get(h, db)) == NULL)
	return 0;
    return de->de_pid;
}

/*! Check if db exists 
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  No it does not exist
 * @retval  1  Yes it exists
 */
int 
xmldb_exists(clicon_handle h, 
	     const char   *db)
{
    int                 retval = -1;
    char               *filename = NULL;
    struct stat         sb;

    if (xmldb_db2file(h, db, &filename) < 0)
	goto done;
    if (lstat(filename, &sb) < 0)
	retval = 0;
    else
	retval = 1;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! Delete database. Remove file 
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @retval -1  Error
 * @retval  0  OK
 */
int 
xmldb_delete(clicon_handle h, 
	     const char   *db)
{
    int                 retval = -1;
    char               *filename = NULL;
    db_elmnt           *de = NULL;
    cxobj              *xt = NULL;
    struct stat         sb;
    
    if (clicon_option_bool(h, "CLICON_XMLDB_CACHE")){
	if ((de = clicon_db_elmnt_get(h, db)) != NULL){
	    if ((xt = de->de_xml) != NULL){
		xml_free(xt);
		de->de_xml = NULL;
	    }
	}
    }
    if (xmldb_db2file(h, db, &filename) < 0)
	goto done;
    if (lstat(filename, &sb) == 0)
	if (unlink(filename) < 0){
	    clicon_err(OE_DB, errno, "unlink %s", filename);
	    goto done;
	}
    retval = 0;
 done:
    if (filename)
	free(filename);
    return retval;
}

/*! Create a database. Open database for writing.
 * @param[in]  h   Clicon handle
 * @param[in]  db  Database
 * @retval  0  OK
 * @retval -1  Error
 */
int 
xmldb_create(clicon_handle h, 
	     const char   *db)
{
    int                 retval = -1;
    char               *filename = NULL;
    int                 fd = -1;
    db_elmnt           *de = NULL;
    cxobj              *xt = NULL;

    if (clicon_option_bool(h, "CLICON_XMLDB_CACHE")){ /* XXX This should not really happen? */
	if ((de = clicon_db_elmnt_get(h, db)) != NULL){
	    if ((xt = de->de_xml) != NULL){
		xml_free(xt);
		de->de_xml = NULL;
	    }
	}
    }
    if (xmldb_db2file(h, db, &filename) < 0)
	goto done;
    if ((fd = open(filename, O_CREAT|O_WRONLY, S_IRWXU)) == -1) {
	clicon_err(OE_UNIX, errno, "open(%s)", filename);
	goto done;
    }
   retval = 0;
 done:
    if (filename)
	free(filename);
    if (fd != -1)
	close(fd);
    return retval;
}