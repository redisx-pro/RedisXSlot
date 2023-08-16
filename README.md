# RedisxSlot

**Supported redis version**: redis >= 7.0
1. redis >= 5.0 (use module `dict/zset` api; if use 4.\*.\*, cp `dict/zset` op from redis do the same op, use `RedisModule_SubscribeToKeyspaceEvents` api sub keyspace events,see `RedisModuleEventCallback` api help doc detail)
2. redis >= 6.0 (use module `RedisModule_NotifyKeyspaceEvent` and Server events definitions `RedisModuleEvent_**` api, sub CronLoop(serverCron) event to do resize/rehash dict)
3. redis >= 7.0 (use module `RedisModuleEvent_EVENTLOOP` api and use AE api with hiredis(1.2) adapter to connect)

**Tips**: use `ruby gendoc.rb | less` or `cat Module_Call | less` see api help doc,hello** example and test/modules case.

# Feature
1. load module init hash slot size, defualt size 2^10, max size 2^16. (once make sure the slot size, don't change it)
2. load module init activerehashing from config, used to sub server event to rehash slot keys dict 
3. sub ServerEvent `CronLoop(ServerLoop),FlushDB,Shutdown`
    1. sub CronLoop server event hook to resize/rehash dict (db slot keys tables)
    2. sub FlushDB server event hook to delete one/all dict (db slot keys tables)
    2. sub Shutdown server event hook to release dicts (db slot keys tables) and free memory.
4. sub KeyspaceEvents `STRING,LIST,HAHS,SET,ZSET, LOADED; GENERIC, EXPIRED`
    1. sub keyspaces `STRING,LIST,HAHS,SET,ZSET, LOADED` notify event hook to add dict/skiplist keys
    2. sub keyspaces `GENERIC, EXPIRED` notify event hook to delete dict/skiplist keys
5. support slot tag key migrate, for (smart client)/proxy)'s configSrv admin contoller lay use it.
    use `SLOTSMGRTTAGSLOT` cmd to migrate slot's key with same tag
```shell
127.0.0.1:6660> setex 122{tag} 86400 v3
OK
127.0.0.1:6660> setex 1{tag} 86400 v3
OK
127.0.0.1:6660> sadd 12{tag} m1 m2 m3
(integer) 0
127.0.0.1:6660> hmset 123{tag} f1 v1 f2 v2
OK
127.0.0.1:6660> lpush 123{tag} l1 l2
(integer) 2
127.0.0.1:6660> zadd 123{tag} 100 z1 10 z2
(integer) 2
127.0.0.1:6660> slotshashkey 123{tag}
1) (integer) 899
127.0.0.1:6660> slotsinfo 899 0 withsize
1) 1) (integer) 899
   2) (integer) 6
127.0.0.1:6660> SLOTSMGRTTAGSLOT 127.0.0.1 6666 300000 899
1) (integer) 6
2) (integer) 0
127.0.0.1:6660> SLOTSMGRTTAGSLOT 127.0.0.1 6666 300000 899
1) (integer) 0
2) (integer) 0
```
```shell
127.0.0.1:6666> slotsinfo 0 1024 withsize
1) 1) (integer) 899
   2) (integer) 6
127.0.0.1:6666> get 122{tag}
"v3"
127.0.0.1:6666> ttl 122{tag}
(integer) 86133
127.0.0.1:6666> get 1{tag}
"v3"
127.0.0.1:6666> ttl 1{tag}
(integer) 86120
127.0.0.1:6666> hgetall 123{tag}
1) "f1"
2) "v1"
3) "f2"
4) "v2"
127.0.0.1:6666> lrange 123{tag} 0 -1
1) "l2"
2) "l1"
127.0.0.1:6666> zrange 123{tag} 0 10 withscores
1) "z2"
2) "10"
3) "z1"
4) "100"
```
tips: if use codis-proxy, codis-dashboard config set `migration_method = "sync"`