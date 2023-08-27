# Test Data
* case1. big tag: slot have many the same hash tag key
* case2. big key: the key have many field(hash)/member(set/zset)
```shell
# random test type data (random hash key 10m requests, 1m keys)
redis/src/redis-benchmark -h 127.0.0.1 -p 6379 -t set,lpush,hset,sadd,zadd -n 10000000 -r 1000000  -q
# test string key(the same hash tag key 10m requests, 1m keys, random maybe same key)
redis/src/redis-benchmark -h 127.0.0.1 -p 6379 -n 10000000 -r 1000000 set mystring{stringtag}__rand_int__ __rand_int__
# test big list key(the hash tag big key 1m requests, 1m keys per type key)
redis/src/redis-benchmark -h 127.0.0.1 -p 6379 -n 1000000 -r 1000000 lpush mylist{listtag} __rand_int__
# test big hash key(the hash tag big key 10m requests, 1m keys per type key, random maybe same key)
redis/src/redis-benchmark -h 127.0.0.1 -p 6379 -n 10000000 -r 1000000 hset myhash{hashtag} __rand_int__ __rand_int__
# test big set key(the hash tag big key 10m requests, 1m keys per type key, random maybe same key)
redis/src/redis-benchmark -h 127.0.0.1 -p 6379 -n 10000000 -r 1000000 sadd myset{settag} __rand_int__
# test big zset key(the hash tag big key 10m requests, 1m keys per type key, random maybe same key)
redis/src/redis-benchmark -h 127.0.0.1 -p 6379 -n 10000000 -r 1000000 zadd myzset{zsettag} __rand_int__ __rand_int__
```
# MGRT
* env: vm ubuntu 6.2.0-27-generic 4 cores, Intel(R) Core(TM) i5-1038NG7 CPU @ 2.00GHz
* init slot 1024, activingrehash dict, `slotsrestore` cmd split params size 1M, local mgrt(no net driver queue i/o).
```shell
# mgrt the same hash tag 1m keys (tag: {stringtag} slot 835)
SLOTSMGRTTAGSLOT 127.0.0.1 6372 30000 835
# mgrt the big list 1m members (tag: {listtag} slot 56)
SLOTSMGRTTAGSLOT 127.0.0.1 6372 30000 56
# mgrt the big hash 1m fields (tag: {hashtag} slot 609)
SLOTSMGRTTAGSLOT 127.0.0.1 6372 30000 609
# mgrt the big set 1m fields (tag: myset{settag} slot 939)
SLOTSMGRTTAGSLOT 127.0.0.1 6372 30000 939
# mgrt the big zset 1m fields (tag: myzset{zsettag} slot 531)
SLOTSMGRTTAGSLOT 127.0.0.1 6372 30000 531
```
1. no thread pool, one thread dump&send and one thread restore
```shell
# hash tag {tag} 1m string keys
127.0.0.1:6379> SLOTSMGRTTAGSLOT 127.0.0.1 6372 30000 899
1) (integer) 1000000
2) (integer) 0
(21.38s)

35534:M 22 Aug 2023 17:14:46.303 * <redisxslot> 1000000 objs dump cost 6437.805000 ms
35534:M 22 Aug 2023 17:14:55.033 * <redisxslot> 1000000 objs mgrt cost 8729.308000 ms
35534:M 22 Aug 2023 17:14:59.854 * <redisxslot> 1000000 keys del cost 5821.002000 ms
```
2. use thread pool size 4, 4 worker threads(4 connects) dump&send and one thread restore
```shell
# hash tag {tag} 1m string keys
127.0.0.1:6379> SLOTSMGRTTAGSLOT 127.0.0.1 6372 30000 899
1) (integer) 1000000
2) (integer) 0
(20.48s)

34674:M 22 Aug 2023 16:59:08.364 * <redisxslot> 1000000 objs dump cost 6318.328000 ms
34674:M 22 Aug 2023 16:59:16.381 * <redisxslot> 1000000 objs mgrt cost 8016.692000 ms
34674:M 22 Aug 2023 16:59:22.427 * <redisxslot> 1000000 keys del cost 6046.166000 ms
```
3. no thread pool, one thread dump&send and one async block thread restore
```shell
127.0.0.1:6372> SLOTSMGRTTAGSLOT 127.0.0.1 6379 30000 835
1) (integer) 1000000
2) (integer) 0
(15.67s)
4511:M 25 Aug 2023 03:29:47.003 * <redisxslot> 1000000 objs dump cost 4884.850000 ms
4511:M 25 Aug 2023 03:31:38.797 * <redisxslot> 1000000 objs mgrt cost 6801.582000 ms
4511:M 25 Aug 2023 03:31:42.368 * <redisxslot> 1000000 keys del cost 3570.632000 ms
```
4. use thread pool size 4, 4 worker threads(4 connects) dump&send and one async block thread restore
```shell
127.0.0.1:6372> SLOTSMGRTTAGSLOT 127.0.0.1 6379 30000 835
1) (integer) 1000000
2) (integer) 0
(15.15s)

6860:M 25 Aug 2023 04:06:46.319 * <redisxslot> 1000000 objs dump cost 5029.533000 ms
6860:M 25 Aug 2023 04:06:52.640 * <redisxslot> 1000000 objs mgrt cost 6320.432000 ms
6860:M 25 Aug 2023 04:06:56.308 * <redisxslot> 1000000 keys del cost 3668.252000 ms
```

tips: 
1. slot keys mgrt/retore use async to do, sched cpu don't block or less block other cmd.