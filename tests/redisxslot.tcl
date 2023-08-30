set testmodule [file normalize YOUR_PATH/redisxslot.so]

# reference:
# 0. https://github.com/tcltk/tcl
# 0. https://wiki.tcl-lang.org/page/Tcllib+Installation
# 1. tests/unit/cluster/cli.tcl
# 2. tests/unit/dump.tcl
# 3. tests/unit/moduleapi/propagate.tcl
# 4. tests/unit/support/cluster.tcl

# need install tcllib
package require crc32

#--------------------- prepare -----------------#
proc init_config_1 {r} {
    $r config set hz 100
    # try to config set, it will fail if redis was compiled without it
    catch {$r config set activerehashing yes} e
    # (error) ERR CONFIG SET failed (possibly related to argument 'enable-module-command') - can't set immutable config
    catch {$r config set enable-module-command yes} e
}

#--------------------- test -------------------#
proc add_test_data {r n tag} {
    set key_list {}
    for {set i 0} {$i < $n} {incr i} {
        set key "$i"
        append key [randstring 0 512 alpha]
        append key "{$tag}"
        lappend key_list $key
        if {$i >=10 && $i< 20} {
            $r sadd $key m1 m2 m3
            continue
        }
        if {$i >=20 && $i< 30} {
            $r hmset $key f1 v1 f2 v2
            continue
        }
        if {$i >=30 && $i< 40} {
            $r lpush $key l1 l2
            continue
        }
        if {$i >=40 && $i< 50} {
            $r zadd $key 100 z1 10 z2
            continue
        }
        $r setex $key 86400 [randstring 0 512 alpha]
    }
    return $key_list
}

proc put_slot_list {r slotsize n tag_list} {
    set slot_list {}
    foreach tag $tag_list {
        add_test_data $r $n $tag
        set c32 [crc::crc32 $tag]
        lappend slot_list [expr {$c32%$slotsize}]
    }
    return [lsort -integer $slot_list]
}

proc print_module_args {r} {
    set mlist [$r module list]
    puts "$mlist"
}

proc test_local_cmd {r slotsize} {
    test "test slotshashkey - slotsize: $slotsize" {
        set c32 [crc::crc32 "tag"]
        set key_slot [expr {$c32%$slotsize}]
        set res_list [$r slotshashkey "{tag}" "123{tag}" "1{tag}" "1{tag}{tag123}"]
        foreach item $res_list {
            assert_equal $item $key_slot
        }

        foreach type {alpha compr} {
            for {set i 0} {$i < 10} {incr i} {
                set key [randstring 0 512 $type]
                set c32 [crc::crc32 $key]
                set key_slot [expr {$c32%$slotsize}]
                assert_equal $key_slot [$r slotshashkey $key]
            }
        }
    }

    test "test slotsinfo - slotsize: $slotsize" {
        $r flushdb
        set res [$r slotsinfo 0 $slotsize]
        assert_equal 0 [llength $res]

        set n 100
        set tag_list {"tag0" "tag1" "tag2" "tag3" "tag4" "tag5"}
        set slot_list [put_slot_list $r $slotsize $n $tag_list]
        assert_equal [llength $slot_list] [llength $tag_list]
        #puts "slot_list $slot_list"
        set res [$r slotsinfo 0 $slotsize]
        #puts "slotsinfo_res $res"
        assert_equal [llength $slot_list] [llength $res]
        for {set i 0} {$i < [llength $slot_list]} {incr i} {
            assert_equal [lindex $slot_list $i] [lindex [lindex $res $i] 0]
            assert_equal $n [lindex [lindex $res $i] 1]
        }
    }

    test "test slotsscan - slotsize: $slotsize" {
        $r flushdb
        set res [$r slotsinfo 0 $slotsize]
        assert_equal 0 [llength $res]

        set n 100
        set tag "tag5"
        set slot [expr {[crc::crc32 $tag]%$slotsize}]
        set key_list [add_test_data $r $n $tag]
        assert_equal $n [llength $key_list]
        set res [$r slotsscan $slot 0 count $n]
        assert_equal 2 [llength $res]
        assert_equal [llength $key_list] [llength [lindex $res 1]]
        set res [$r slotsscan $slot 0 count [expr {$n+1}]]
        assert_equal 2 [llength $res]
        assert_equal [llength $key_list] [llength [lindex $res 1]]
    }

    test "test slotsdel - slotsize: $slotsize" {
        $r flushdb
        set res [$r slotsinfo 0 $slotsize]
        assert_equal 0 [llength $res]

        set n 100
        set tag_list {"tag0" "tag1" "tag2" "tag3" "tag4" "tag5"}
        set slot_list [put_slot_list $r $slotsize $n $tag_list]
        assert_equal [llength $slot_list] [llength $tag_list]
        foreach slot $slot_list {
            set res [$r slotsdel $slot]
            assert_equal 1 [llength $res]
            assert_equal 2 [llength [lindex $res 0]]
            assert_equal $slot [lindex [lindex $res 0] 0]
            assert_equal 0 [lindex [lindex $res 0] 1]
        }
    }
}

proc test_mgrt_cmd {r} {
}

#--------------------- unload -----------------#
proc test_unload {r} {
    test "Unload the module - redisxslot" {
        set info [r config get enable-module-command]
        if { $info eq "enable-module-command yes"} {
            assert_equal {OK} [r module unload redisxslot]
            puts "unload ok"
        }
    }
}

#--------------------- start server loadmodule test -------------------#
tags "modules" {
    test {start redis server loadmodule: default 1024 slots no async block no thread pool} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            print_module_args r
            init_config_1 r
            test_local_cmd r 1024
            test_unload r
        }
    }

    test {start redis server loadmodule: 65536 slots no async block no thread pool} {
        start_server [list overrides [list loadmodule "$testmodule 65536"]] {
            print_module_args r
            init_config_1 r
            test_local_cmd r 65536
            test_unload r
        }
    }
}

#------------ start server and request module load test --------------#
start_server {tags {"modules"}} {
    test {cli module load: default 1024 slots no async block no thread pool} {
        init_config_1 r
        assert_equal {OK} [r module load $testmodule]
        print_module_args r
        test_local_cmd r 1024
        test_unload r
    }
}
