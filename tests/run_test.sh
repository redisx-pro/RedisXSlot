#!/bin/bash
set -e

# prepare install tclsh
OS=`uname -s 2>/dev/null || echo not`
NODENAME=`uname -n 2>/dev/null || echo not`
which tclsh
if [[ $? -eq 1 ]];then
    # for linux ubuntu os
    if [[ $NODENAME == "ubuntu" ]] && [[ $OS == "Linux" ]];then
        sudo apt-get install tcl8.6 tclx
    fi
    # for mac os
    if [ $OS == "Darwin" ];then
        brew install tcl-tk
    fi
fi

# run test case
work_path=$(pwd)
module_path=$work_path
redis_path=$work_path/redis
[[ -n $1 ]] && redis_path=$1
sed -e "s#YOUR_PATH#$module_path#g" tests/redisxslot.tcl > $redis_path/tests/unit/moduleapi/redisxslot.tcl
sed -i -e 's#set ::all_tests {#set ::all_tests {\nunit/moduleapi/redisxslot#g' $redis_path/tests/test_helper.tcl
cd $redis_path
./runtest --stack-logging --single unit/moduleapi/redisxslot

# after run test, clear
sed -i -e 'N;s#unit/moduleapi/redisxslot\n##g' $redis_path/tests/test_helper.tcl 
rm $redis_path/tests/unit/moduleapi/redisxslot.tcl