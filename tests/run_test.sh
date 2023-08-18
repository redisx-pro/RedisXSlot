 #!/bin/bash
 # for linux ubuntu os
 set -e
 sudo apt-get install tcl8.6 tclx
 work_path=$(pwd)
 module_path=$work_path
 redis_path=$work_path
 sed -e "s#YOUR_PATH#$module_path#g" tests/redisxslot.tcl > $redis_path/tests/unit/type/redisxslot.tcl
 sed -i 's#set ::all_tests {#set ::all_tests {\nunit/type/redisxslot#g' $redis_path/tests/test_helper.tcl
 cd $redis_path
 ./runtest --stack-logging --single unit/type/redisxslot