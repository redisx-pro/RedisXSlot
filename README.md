# RedisxSlot

**Supported redis version**: redis >= 7.0
1. redis >= 5.0 (use module `dict/zset` api; if use 4.\*.\*, cp `dict/zset` op from redis do the same op, use `RedisModule_SubscribeToKeyspaceEvents` api sub keyspace events)
2. redis >= 6.0 (use module `RedisModule_NotifyKeyspaceEvent` and Server events definitions `RedisModuleEvent_**` api, sub CronLoop(serverCron) event to do resize/rehash dict)
3. redis >= 7.0 (use module `RedisModuleEvent_EVENTLOOP` api and use AE api with hiredis(1.2) adapter to connect)

**Tips**: use `ruby gendoc.rb | less` see api help doc and hello** example.

# Feature
1. load module init hash slot size, defualt size 2^10, max size 2^16. (once make sure the slot size, don't change it)
