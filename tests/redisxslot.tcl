set testmodule [file normalize YOUR_PATH/redisxslot.so]

start_server {tags {"redisslot_default"}} {
    r module load $testmodule

    test {test rm_call auto mode} {
    }

    test {test slot } {
    }

    test {test mgrt } {
    }

    test "Unload the module - test" {
        assert_equal {OK} [r module unload test]
    }
}

start_server {tags {"redisslot_threads"}} {
    r module load $testmodule 1024 4

    test {test rm_call auto mode} {
    }

    test {test slot } {
    }

    test {test mgrt thread pool jobs} {
    }

    test "Unload the module - test" {
        assert_equal {OK} [r module unload test]
    }
}

start_server {tags {"modules external:skip"} overrides {enable-module-command no}} {
    test {module command disabled} {
        assert_error "ERR *MODULE command not allowed*" {r module load $testmodule}
    }
}
