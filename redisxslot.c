#include "redisxslot.h"

slots_meta_info g_slots_meta_info;
db_slot_info* db_slot_infos;

uint64_t dictModuleStrHash(const void* key) {
    size_t len;
    const char* buf = RedisModule_StringPtrLen(key, &len);
    return m_dictGenHashFunction(buf, (int)len);
}

int dictModuleStrKeyCompare(void* privdata, const void* key1,
                            const void* key2) {
    size_t l1, l2;
    DICT_NOTUSED(privdata);

    const char* buf1 = RedisModule_StringPtrLen(key1, &l1);
    const char* buf2 = RedisModule_StringPtrLen(key2, &l2);
    if (l1 != l2)
        return 0;
    return memcmp(buf1, buf2, l1) == 0;
}

void dictModuleKeyDestructor(void* privdata, void* val) {
    DICT_NOTUSED(privdata);
    if (val) {
        RedisModule_FreeString(NULL, val);
    }
}

void dictModuleValueDestructor(void* privdata, void* val) {
    DICT_NOTUSED(privdata);
    if (val) {
        RedisModule_FreeString(NULL, val);
    }
}

m_dictType hashSlotDictType = {
    dictModuleStrHash,        /* hash function */
    NULL,                     /* key dup */
    NULL,                     /* val dup */
    dictModuleStrKeyCompare,  /* key compare */
    dictModuleKeyDestructor,  /* key destructor */
    dictModuleValueDestructor /* val destructor */
};

void slots_init(RedisModuleCtx* ctx, uint32_t hash_slots_size, int databases) {
    crc32_init();

    g_slots_meta_info.hash_slots_size = hash_slots_size;
    g_slots_meta_info.databases = databases;

    db_slot_infos = RedisModule_Alloc(sizeof(db_slot_info) * databases);
    for (int j = 0; j < databases; j++) {
        db_slot_infos[j].slotkey_tables
            = RedisModule_Alloc(sizeof(dict) * hash_slots_size);
        for (uint32_t i = 0; i < hash_slots_size; i++) {
            db_slot_infos[j].slotkey_tables[i]
                = m_dictCreate(&hashSlotDictType, NULL);
        }
        db_slot_infos[j].slotkey_table_rehashing = 0;
        db_slot_infos[j].tagged_key_list = m_zslCreate();
    }

    slotsmgrt_cached_ctx_connects = RedisModule_CreateDict(ctx);
}

void slots_free(RedisModuleCtx* ctx) {
    for (int j = 0; j < g_slots_meta_info.databases; j++) {
        if (db_slot_infos != NULL && db_slot_infos[j].slotkey_tables != NULL) {
            RedisModule_Free(db_slot_infos[j].slotkey_tables);
            db_slot_infos[j].slotkey_tables = NULL;
        }
        if (db_slot_infos != NULL && db_slot_infos[j].tagged_key_list != NULL) {
            m_zslFree(db_slot_infos[j].tagged_key_list);
            db_slot_infos[j].tagged_key_list = NULL;
        }
    }
    if (db_slot_infos != NULL) {
        RedisModule_Free(db_slot_infos);
        db_slot_infos = NULL;
    }
    if (slotsmgrt_cached_ctx_connects != NULL) {
        RedisModule_FreeDict(ctx, slotsmgrt_cached_ctx_connects);
        slotsmgrt_cached_ctx_connects = NULL;
    }
}

/*
 * params s key, pcrc crc32 sum, phastag has tag
 * return slot num
 */
int slots_num(const char* s, uint32_t* pcrc, int* phastag) {
    int taglen;
    int hastag = 0;
    const char* tag = slots_tag(s, &taglen);
    if (tag == NULL) {
        tag = s, taglen = strlen(s);
    } else {
        hastag = 1;
    }
    uint32_t crc = crc32_checksum(tag, taglen);
    printf("tag %s taglen %d crc %u g_hash_slots_size %u \n", tag, taglen, crc,
           g_slots_meta_info.hash_slots_size);
    if (pcrc != NULL) {
        *pcrc = crc;
    }
    if (phastag != NULL) {
        *phastag = hastag;
    }
    return crc & (g_slots_meta_info.hash_slots_size - 1);
}

/*
 * params s key, plen tag len
 * return tag start pos char *
 */
static const char* slots_tag(const char* s, int* plen) {
    int i, j, n = strlen(s);
    for (i = 0; i < n && s[i] != '{'; i++) {
    }
    if (i == n) {
        return NULL;
    }
    i++;
    for (j = i; j < n && s[j] != '}'; j++) {
    }
    if (j == n) {
        return NULL;
    }
    if (plen != NULL) {
        *plen = j - i;
    }
    return s + i;
}

static db_slot_mgrt_connect* SlotsMGRT_GetConnCtx(RedisModuleCtx* ctx,
                                                  const sds host,
                                                  const sds port,
                                                  struct timeval timeout) {
    // time_t unixtime = time(NULL);
    time_t unixtime = (time_t)(RedisModule_CachedMicroseconds() / 1e6);

    sds name = sdsempty();
    name = sdscatlen(name, host, sdslen(host));
    name = sdscatlen(name, ":", 1);
    name = sdscatlen(name, port, sdslen(port));

    // db_slot_mgrt_connect* conn =
    // m_dictFetchValue(slotsmgrt_cached_ctx_connects, name);
    db_slot_mgrt_connect* conn = RedisModule_DictGetC(
        slotsmgrt_cached_ctx_connects, (void*)name, sdslen(name), NULL);
    if (conn != NULL) {
        sdsfree(name);
        conn->last_time = unixtime;
        return conn;
    }

    redisContext* c = redisConnect(host, atoi(port));
    if (c->err) {
        char errLog[200];
        sprintf(errLog, "Err: slotsmgrt connect to target %s:%s, error = '%s'",
                host, port, c->errstr);
        RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_WARNING, "%s", errLog);
        RedisModule_ReplyWithError(ctx, (const char*)errLog);
        sdsfree(name);
        return NULL;
    }
    redisSetTimeout(c, timeout);
    RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_NOTICE,
                    "slotsmgrt: connect to target %s:%s", host, port);

    conn = RedisModule_Alloc(sizeof(db_slot_mgrt_connect));
    conn->conn_ctx = c;
    conn->last_time = unixtime;
    conn->db = -1;

    // m_dictAdd(slotsmgrt_cached_ctx_connects, name, conn);
    RedisModule_DictSetC(slotsmgrt_cached_ctx_connects, (void*)name,
                         sdslen(name), conn);
    sdsfree(name);
    return conn;
}

static void SlotsMGRT_CloseSocket(RedisModuleCtx* ctx, const sds host,
                                  const sds port) {
    sds name = sdsempty();
    name = sdscatlen(name, host, sdslen(host));
    name = sdscatlen(name, ":", 1);
    name = sdscatlen(name, port, sdslen(port));

    // db_slot_mgrt_connect* conn =
    // m_dictFetchValue(slotsmgrt_cached_ctx_connects, name);
    db_slot_mgrt_connect* conn = RedisModule_DictGetC(
        slotsmgrt_cached_ctx_connects, (void*)name, sdslen(name), NULL);
    if (conn != NULL) {
        RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_WARNING,
                        "slotsmgrt: close target %s:%s again", host, port);
        sdsfree(name);
        return;
    }
    RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_NOTICE,
                    "slotsmgrt: close target %s:%s ok", host, port);
    // m_dictDelete(slotsmgrt_cached_ctx_connects, name);
    RedisModule_DictDelC(slotsmgrt_cached_ctx_connects, (void*)name,
                         sdslen(name), NULL);
    redisFree(conn->conn_ctx);
    RedisModule_Free(conn);
    conn = NULL;
    sdsfree(name);
}

// SlotsMGRT_CloseTimedoutSockets
// like migrateCloseTimedoutSockets
// for server cron job to check timeout connect
static void SlotsMGRT_CloseTimedoutSockets(RedisModuleCtx* ctx) {
    // maybe use cached server cron time, a little faster.
    // time_t unixtime = time(NULL);
    time_t unixtime = (time_t)(RedisModule_CachedMicroseconds() / 1e6);

    // m_dictIterator* di =
    // m_dictGetSafeIterator(slotsmgrt_cached_ctx_connects);
    RedisModuleDictIter* di = RedisModule_DictIteratorStartC(
        slotsmgrt_cached_ctx_connects, "^", NULL, 0);

    m_dictEntry* de;
    void* k;
    db_slot_mgrt_connect* conn;

    // while ((de = dictNext(di)) != NULL) {
    //     k = dictGetKey(de);
    //     conn = dictGetVal(de);
    size_t keyLen;
    while ((k = RedisModule_DictNextC(di, &keyLen, (void**)&conn))) {
        if ((unixtime - conn->last_time) > MGRT_ONE_KEY_TIMEOUT) {
            RedisModule_Log(
                ctx, REDISMODULE_LOGLEVEL_NOTICE,
                "slotsmgrt: timeout target %s, lasttime = %ld, now = %ld",
                (sds)k, conn->last_time, unixtime);

            // m_dictDelete(slotsmgrt_cached_ctx_connects, k);
            RedisModule_DictDelC(slotsmgrt_cached_ctx_connects, k,
                                 strlen((sds)k), NULL);

            redisFree(conn->conn_ctx);
            RedisModule_Free(conn);
            conn = NULL;
        }
    }

    // m_dictReleaseIterator(di);
    RedisModule_DictIteratorStop(di);
}

static int BatchSend_SlotsRestore(RedisModuleCtx* ctx,
                                  db_slot_mgrt_connect* conn,
                                  rdb_dump_obj* objs[], int n) {
    const char* argv[3 * n + 1];
    size_t argvlen[3 * n + 1];
    for (int i = 0; i < n; i++) {
        size_t ksz, vsz;
        const char* k = RedisModule_StringPtrLen(objs[i]->key, &ksz);
        const char* v = RedisModule_StringPtrLen(objs[i]->val, &vsz);
        argv[i * 3 + 0] = k;
        argvlen[i * 3 + 0] = ksz;

        time_t ttlms = objs[i]->ttlms;
        char buf[REDIS_LONGSTR_SIZE];
        int len = m_ll2string(buf, sizeof(buf), (long)ttlms);
        argv[i * 3 + 1] = buf;
        argvlen[i * 3 + 1] = (size_t)len;

        argv[i * 3 + 2] = v;
        argvlen[i * 3 + 2] = vsz;
    }

    redisReply* rr = redisCommandArgv(conn->conn_ctx, 3 * n + 1, argv, argvlen);
    if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
        if (rr != NULL) {
            RedisModule_ReplyWithError(ctx, rr->str);
            freeReplyObject(rr);
        }
        return -1;
    }
    freeReplyObject(rr);

    return n;
}

static int Pipeline_Restore(RedisModuleCtx* ctx, db_slot_mgrt_connect* conn,
                            rdb_dump_obj* objs[], int n) {
    for (int i = 0; i < n; i++) {
        size_t ksz, vsz;
        const char* k = RedisModule_StringPtrLen(objs[i]->key, &ksz);
        const char* v = RedisModule_StringPtrLen(objs[i]->val, &vsz);
        time_t ttlms = objs[i]->ttlms;

        redisAppendCommand(conn->conn_ctx, "RESTORE %b %ld %b", k, ksz, ttlms,
                           v, vsz);
    }

    redisReply* rr;
    for (int i = 0; i < n; i++) {
        int r = redisGetReply(conn, (void**)&rr);
        if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
            if (rr != NULL) {
                RedisModule_ReplyWithError(ctx, rr->str);
                freeReplyObject(rr);
            }
            return -1;
        }
        freeReplyObject(rr);
    }

    return n;
}

// MGRT
// batch migrate send to host:port with r/w timeout,
// use withrestore use redis self restore to migrate,
// default with SlotsRestore.
// return value:
//    -1 - error happens
//   >=0 - # of success migration (0 or n)
static int MGRT(RedisModuleCtx* ctx, const sds host, const sds port,
                time_t timeoutMS, rdb_dump_obj* objs[], int n, sds mgrtType) {
    struct timeval timeout
        = {.tv_sec = timeoutMS / 1000, .tv_usec = (timeoutMS % 1000) * 1000};
    db_slot_mgrt_connect* conn = SlotsMGRT_GetConnCtx(ctx, host, port, timeout);
    if (conn == NULL) {
        return -1;
    }
    // todo auth

    redisReply* rr;
    int db = RedisModule_GetSelectedDb(ctx);
    rr = redisCommand(conn->conn_ctx, "SELECT %d", db);
    if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
        if (rr != NULL) {
            freeReplyObject(rr);
            RedisModule_ReplyWithError(ctx, rr->str);
        }
        return REDISMODULE_ERR;
    }
    freeReplyObject(rr);

    sdstolower(mgrtType);
    if (sdscmp("withrestore", mgrtType) == 0) {
        return Pipeline_Restore(ctx, conn, objs, n);
    }

    return BatchSend_SlotsRestore(ctx, conn, objs, n);
}

// SlotsMGRT_OneKey
// do migrate a key-value for slotsmgrt/slotsmgrtone commands
// 1.dump key rdb obj val
// 2.batch migrate send to host:port with r/w timeout
// 3.if migrate ok, remove key
// return value:
//    -1 - error happens
//   >=0 - # of success migration (0 or 1)
static int SlotsMGRT_OneKey(RedisModuleCtx* ctx, const sds host, const sds port,
                            time_t timeout, RedisModuleString* key,
                            sds mgrtType) {
    RedisModuleCallReply* reply;
    reply = RedisModule_Call(ctx, "DUMP", "s", key);
    if (reply == NULL
        || RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_STRING) {
        RedisModule_ReplyWithCallReply(ctx, reply);
        return REDISMODULE_ERR;
    }
    RedisModuleString* val = RedisModule_CreateStringFromCallReply(reply);

    reply = RedisModule_Call(ctx, "PTTL", "s", key);
    if (reply == NULL
        || RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_INTEGER) {
        RedisModule_ReplyWithCallReply(ctx, reply);
        return REDISMODULE_ERR;
    }
    long long ttlms = RedisModule_CallReplyInteger(reply);

    rdb_dump_obj* obj = RedisModule_Alloc(sizeof(rdb_dump_obj));
    obj->key = key;
    obj->val = val;
    obj->ttlms = ttlms;
    rdb_dump_obj* objs[] = {obj};
    int ret = MGRT(ctx, host, port, timeout, objs, 1, mgrtType);
    if (ret < 0) {
        return REDISMODULE_ERR;
    }

    if (ret > 0) {
        reply = RedisModule_Call(ctx, "DEL", "s", key);
        if (reply == NULL
            || RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_INTEGER) {
            RedisModule_ReplyWithCallReply(ctx, reply);
            return REDISMODULE_ERR;
        }
    }

    RedisModule_Free(obj);
    return ret;
}

// todo try to add thread pool to migrate, slotsrestore add thread pool to
// restore , need lock shared obj.