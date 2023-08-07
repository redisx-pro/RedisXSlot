#ifndef REDISXSLOT_H
#define REDISXSLOT_H

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "dep/dict.h"
#include "dep/skiplist.h"
#include "redismodule.h"

// define error
#define REDISXSLOT_ERRORMSG_SYNTAX "ERR syntax error"

// define const
#define DEFAULT_HASH_SLOTS_MASK 0x000003ff
#define DEFAULT_HASH_SLOTS_SIZE (DEFAULT_HASH_SLOTS_MASK + 1)
#define MAX_HASH_SLOTS_MASK 0x0000ffff
#define MAX_HASH_SLOTS_SIZE (MAX_HASH_SLOTS_MASK + 1)

// define struct
typedef struct _slots_meta_info {
    uint32_t hash_slots_size;
    // from config databases
    int databases;
} slots_meta_info;

typedef struct _db_slot_info {
    int db;
    int slotkey_table_rehashing;
    dict** slotkey_tables;
    m_zskiplist* tagged_key_list;
} db_slot_info;

// declare define var
slots_meta_info g_slots_meta_info;
db_slot_info* arr_db_slot_info;
static RedisModuleDict* slotsmgrt_cached_sockfds;

// declare function
void crc32_init();
uint32_t crc32_checksum(const char* buf, int len);
static const char* slots_tag(const char* s, int* plen);
int slots_num(const char* s, uint32_t* pcrc, int* phastag);
void slots_init(RedisModuleCtx* ctx, uint32_t hash_slots_size, int databases);
void slots_free(RedisModuleCtx* ctx);

#endif /* REDISXSLOT_H */