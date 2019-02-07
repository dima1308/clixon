#!/bin/bash
# Authentication and authorization and IETF NACM
# External NACM file
# See RFC 8341 A.2
# But replaced ietf-netconf-monitoring with *

APPNAME=example
# include err() and new() functions and creates $dir
. ./lib.sh
. ./nacm.sh

cfg=$dir/conf_yang.xml
fyang=$dir/nacm-example.yang
nacmfile=$dir/nacmfile

# Note filter out example_backend_nacm.so in CLICON_BACKEND_REGEXP below
cat <<EOF > $cfg
<config>
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$IETFRFC</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_FILE>$fyang</CLICON_YANG_MAIN_FILE>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_REGEXP>example_backend.so$</CLICON_BACKEND_REGEXP>
  <CLICON_RESTCONF_DIR>/usr/local/lib/$APPNAME/restconf</CLICON_RESTCONF_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_CLI_GENMODEL_COMPLETION>1</CLICON_CLI_GENMODEL_COMPLETION>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
  <CLICON_XMLDB_PLUGIN>/usr/local/lib/xmldb/text.so</CLICON_XMLDB_PLUGIN>
  <CLICON_RESTCONF_PRETTY>false</CLICON_RESTCONF_PRETTY>
  <CLICON_NACM_MODE>external</CLICON_NACM_MODE>
  <CLICON_NACM_FILE>$nacmfile</CLICON_NACM_FILE>
</config>
EOF

cat <<EOF > $fyang
module nacm-example{
  yang-version 1.1;
  namespace "urn:example:nacm";
  import clixon-example {
	prefix ex;
  }
  prefix nacm;
  container authentication {
	description "Example code for enabling www basic auth and some example 
                     users";
    leaf basic_auth{
	description "Basic user / password authentication as in HTTP basic auth";
	type boolean;
	default true;
    }
    list auth {
	description "user / password entries. Valid if basic_auth=true";
	key user;
	leaf user{
	    description "User name";
	    type string;
	}
	leaf password{
	    description "Password";
	    type string;
	}
      }
    }
  leaf x{
    type int32;
    description "something to edit";
  }
}
EOF

cat <<EOF > $nacmfile
   <nacm>
     <enable-nacm>true</enable-nacm>
     <read-default>permit</read-default>
     <write-default>deny</write-default>
     <exec-default>deny</exec-default>

     $NGROUPS

     <rule-list>
       <name>guest-acl</name>
       <group>guest</group>
       <rule>
         <name>deny-ncm</name>
         <module-name>*</module-name>
         <access-operations>*</access-operations>
         <action>deny</action>
         <comment>
             Do not allow guests any access to any information.
         </comment>
       </rule>
     </rule-list>
     <rule-list>
       <name>limited-acl</name>
       <group>limited</group>
       <rule>
         <name>permit-get</name>
         <rpc-name>get</rpc-name>
         <module-name>*</module-name>
         <access-operations>exec</access-operations>
         <action>permit</action>
         <comment>
             Allow get 
         </comment>
       </rule>
       <rule>
         <name>permit-get-config</name>
         <rpc-name>get-config</rpc-name>
         <module-name>*</module-name>
         <access-operations>exec</access-operations>
         <action>permit</action>
         <comment>
             Allow get-config
         </comment>
       </rule>
     </rule-list>

     $NADMIN

   </nacm>
   <x xmlns="urn:example:nacm">0</x>
EOF

new "test params: -f $cfg"

if [ $BE -ne 0 ]; then
    new "kill old backend -zf $cfg "
    sudo clixon_backend -zf $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    sleep 1
    new "start backend -s init -f $cfg"
    # start new backend
    sudo $clixon_backend -s init -f $cfg -D $DBG
    if [ $? -ne 0 ]; then
	err
    fi
fi

new "kill old restconf daemon"
sudo pkill -u www-data -f "/www-data/clixon_restconf"

new "start restconf daemon (-a is enable http basic auth)"
sudo su -c "$clixon_restconf -f $cfg -D $DBG -- -a" -s /bin/sh www-data &

sleep $RCWAIT

new "auth get"
expecteq "$(curl -u andy:bar -sS -X GET http://localhost/restconf/data/clixon-example:state)" 0 '{"clixon-example:state": {"op": ["42","41","43"]}}
'

new "Set x to 0"
expecteq "$(curl -u andy:bar -sS -X PUT -d '{"nacm-example:x": 0}' http://localhost/restconf/data/nacm-example:x)" 0 ""

new "auth get (no user: access denied)"
expecteq "$(curl -sS -X GET -H \"Accept:\ application/yang-data+json\" http://localhost/restconf/data)" 0 '{"ietf-restconf:errors" : {"error": {"error-type": "protocol","error-tag": "access-denied","error-severity": "error","error-message": "The requested URL was unauthorized"}}}'

new "auth get (wrong passwd: access denied)"
expecteq "$(curl -u andy:foo -sS -X GET http://localhost/restconf/data)" 0 '{"ietf-restconf:errors" : {"error": {"error-type": "protocol","error-tag": "access-denied","error-severity": "error","error-message": "The requested URL was unauthorized"}}}'

new "auth get (access)"
expecteq "$(curl -u andy:bar -sS -X GET http://localhost/restconf/data/nacm-example:x)" 0 '{"nacm-example:x": 0}
'

new "admin get nacm"
expecteq "$(curl -u andy:bar -sS -X GET http://localhost/restconf/data/nacm-example:x)" 0 '{"nacm-example:x": 0}
'

new "limited get nacm"
expecteq "$(curl -u wilma:bar -sS -X GET http://localhost/restconf/data/nacm-example:x)" 0 '{"nacm-example:x": 0}
'

new "guest get nacm"
expecteq "$(curl -u guest:bar -sS -X GET http://localhost/restconf/data/nacm-example:x)" 0 '{"ietf-restconf:errors" : {"error": {"error-type": "application","error-tag": "access-denied","error-severity": "error","error-message": "access denied"}}}'

new "admin edit nacm"
expecteq "$(curl -u andy:bar -sS -X PUT -d '{"nacm-example:x": 1}' http://localhost/restconf/data/nacm-example:x)" 0 ""

new "limited edit nacm"
expecteq "$(curl -u wilma:bar -sS -X PUT -d '{"x": 2}' http://localhost/restconf/data/nacm-example:x)" 0 '{"ietf-restconf:errors" : {"error": {"error-type": "application","error-tag": "access-denied","error-severity": "error","error-message": "default deny"}}}'

new "guest edit nacm"
expecteq "$(curl -u guest:bar -sS -X PUT -d '{"x": 3}' http://localhost/restconf/data/nacm-example:x)" 0 '{"ietf-restconf:errors" : {"error": {"error-type": "application","error-tag": "access-denied","error-severity": "error","error-message": "access denied"}}}'

new "cli show conf as admin"
expectfn "$clixon_cli -1 -U andy -l o -f $cfg show conf" 0 "^x 1;$"

new "cli show conf as limited"
expectfn "$clixon_cli -1 -U wilma -l o -f $cfg show conf" 0 "^x 1;$"

new "cli show conf as guest"
expectfn "$clixon_cli -1 -U guest -l o -f $cfg show conf" 255 "protocol access-denied"

new "cli rpc as admin"
expectfn "$clixon_cli -1 -U andy -l o -f $cfg rpc ipv4" 0 '<x xmlns="urn:example:clixon">ipv4</x><y xmlns="urn:example:clixon">42</y>'

new "cli rpc as limited"
expectfn "$clixon_cli -1 -U wilma -l o -f $cfg rpc ipv4" 255 "protocol access-denied default deny"

new "cli rpc as guest"
expectfn "$clixon_cli -1 -U guest -l o -f $cfg rpc ipv4" 255 "protocol access-denied access denied"

new "Kill restconf daemon"
sudo pkill -u www-data -f "/www-data/clixon_restconf"

if [ $BE -eq 0 ]; then
    exit # BE
fi

new "Kill backend"
# Check if premature kill
pid=`pgrep -u root -f clixon_backend`
if [ -z "$pid" ]; then
    err "backend already dead"
fi
# kill backend
sudo clixon_backend -z -f $cfg
if [ $? -ne 0 ]; then
    err "kill backend"
fi

rm -rf $dir
