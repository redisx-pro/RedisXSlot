# Reference
1. [redis-rehash-practice-optimization](https://tech.meituan.com/2018/07/27/redis-rehash-practice-optimization.html)
2. [redis-setcpuaffinity](https://cloud.tencent.com/developer/article/1633269) [patch](https://github.com/redis/redis/commit/1a0deab2a548fa306171f03439e858c00836fe69)
3. [redis-crash](http://antirez.com/news/43?spm=a2c6h.12873639.article-detail.7.2e612412YdEr2y) [debug](https://developer.aliyun.com/article/73676)
crash `STRACK TRACE` sample
```
------ STACK TRACE ------
EIP:
src/redis-server 127.0.0.1:21611(listRelease+0x11)[0x56349d5b5b41]

Backtrace:
src/redis-server 127.0.0.1:21611(sigsegvHandler+0x89)[0x56349d61bba9]
/lib/x86_64-linux-gnu/libc.so.6(+0x3c4b0)[0x7fed1f63c4b0]
src/redis-server 127.0.0.1:21611(listRelease+0x11)[0x56349d5b5b41]
src/redis-server 127.0.0.1:21611(moduleFreeCallReplyRec+0x2a)[0x56349d655b8a]
src/redis-server 127.0.0.1:21611(RM_FreeCallReply+0x17)[0x56349d655c37]
/home/weedge/project/c/redisxslot/redisxslot.so(+0x13f8f)[0x7fed206c5f8f]
```
use `nm` `addr2line` like this
```shell
nm -l ./redisxslot.so  | grep -w listRelease
addr2line -e ./redisxslot.so 0x50FD (0x50ec+0x11)
```