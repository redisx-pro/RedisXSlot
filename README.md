# RedisxSlot

**Supported redis version**: redis >= 5.0 (use moduel `dict` api; if use 4.\*.\*, cp `dict` op from redis do the same op)

**Tips**: use `ruby gendoc.rb | less` see api help doc and hello** example.

# Feature
1. load module init hash slot size, defualt size 2^10, max size 2^16. (once make sure the slot size, don't change it)