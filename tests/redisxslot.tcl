set testmodule [file normalize YOUR_PATH/redisxslot.so]

tags "modules" {
    test {modules are default 1024 slots no async block no thread pool} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            assert_equal 899 [r slotshashkey "{tag}"]
        }
    }
}