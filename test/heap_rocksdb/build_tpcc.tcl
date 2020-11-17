#!/bin/tclsh
puts "SETTING CONFIGURATION"

global complete
proc wait_to_complete {} {
    global complete
    set complete [vucomplete]
    if {!$complete} {
        after 1000 wait_to_complete
    } else {
        exit
    }
}

dbset db pg
dbset bm TPC-C
diset connection pg_host 127.0.0.1
diset connection pg_port 15432

diset tpcc pg_count_ware 10
diset tpcc pg_num_vu 5
diset tpcc pg_superuser wookongdb
diset tpcc pg_superuserpass wookongdb
diset tpcc pg_defaultdbase template1
diset tpcc pg_user tpccrocksdb
diset tpcc pg_pass tpccrocksdb
diset tpcc pg_dbase tpccrocksdb
vuset logtotemp 1
vuset unique 1
diset tpcc pg_rampup 1
diset tpcc pg_duration 1
buildschema

wait_to_complete
vudestroy
