#include "redisxslot.h"

slots_meta_info g_slots_meta_info;
db_slot_info* db_slot_infos;

// declare defined static var to inner use (private prototypes)
static RedisModuleDict* slotsmgrt_cached_ctx_connects;
// declare static function to inner use (private prototypes)
static const char* slots_tag(const char* s, int* plen);

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

void slots_init(RedisModuleCtx* ctx, uint32_t hash_slots_size, int databases,
                int num_threads) {
    crc32_init();

    g_slots_meta_info.hash_slots_size = hash_slots_size;
    g_slots_meta_info.databases = databases;
    g_slots_meta_info.cronloops = 0;
    g_slots_meta_info.slots_mgrt_threads = num_threads;
    g_slots_meta_info.slots_restore_threads = num_threads;

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
            for (uint32_t i = 0; i < g_slots_meta_info.hash_slots_size; i++) {
                m_dictRelease(db_slot_infos[j].slotkey_tables[i]);
            }
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

static void SlotsMGRT_CloseConn(RedisModuleCtx* ctx, const sds host,
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

// SlotsMGRT_CloseTimedoutConns
// like migrateCloseTimedoutSockets
// for server cron job to check timeout connect
void SlotsMGRT_CloseTimedoutConns(RedisModuleCtx* ctx) {
    // maybe use cached server cron time, a little faster.
    // time_t unixtime = time(NULL);
    time_t unixtime = (time_t)(RedisModule_CachedMicroseconds() / 1e6);

    // m_dictIterator* di =
    // m_dictGetSafeIterator(slotsmgrt_cached_ctx_connects);
    RedisModuleDictIter* di = RedisModule_DictIteratorStartC(
        slotsmgrt_cached_ctx_connects, "^", NULL, 0);

    void* k;
    db_slot_mgrt_connect* conn;

    // m_dictEntry* de;
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
    UNUSED(ctx);
    // const char* argv[3 * n + 1];
    const char** argv = RedisModule_Alloc(sizeof(char*) * 3 * n + 1);
    // size_t argvlen[3 * n + 1];
    size_t* argvlen = RedisModule_Alloc(sizeof(size_t) * 3 * n + 1);
    argv[0] = "SLOTSRESTORE";
    argvlen[0] = 12;
    for (int i = 0; i < n; i++) {
        size_t ksz, vsz;
        const char* k = RedisModule_StringPtrLen(objs[i]->key, &ksz);
        argv[i * 3 + 1] = k;
        argvlen[i * 3 + 1] = ksz;

        time_t ttlms = objs[i]->ttlms;
        char buf[REDIS_LONGSTR_SIZE];
        int len = m_ll2string(buf, sizeof(buf), (long)ttlms);
        argv[i * 3 + 2] = buf;
        argvlen[i * 3 + 2] = (size_t)len;

        const char* v = RedisModule_StringPtrLen(objs[i]->val, &vsz);
        argv[i * 3 + 3] = v;
        argvlen[i * 3 + 3] = vsz;
    }

    redisReply* rr = redisCommandArgv(conn->conn_ctx, 3 * n + 1, argv, argvlen);
    if (rr == NULL) {
        RedisModule_Free(argv);
        RedisModule_Free(argvlen);
        return SLOTS_MGRT_ERR;
    }
    if (rr->type == REDIS_REPLY_ERROR) {
        freeReplyObject(rr);
        RedisModule_Free(argv);
        RedisModule_Free(argvlen);
        return SLOTS_MGRT_ERR;
    }

    freeReplyObject(rr);
    RedisModule_Free(argv);
    RedisModule_Free(argvlen);

    return n;
}

static int Pipeline_Restore(RedisModuleCtx* ctx, db_slot_mgrt_connect* conn,
                            rdb_dump_obj* objs[], int n) {
    UNUSED(ctx);
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
        int r = redisGetReply(conn->conn_ctx, (void**)&rr);
        if (r == REDIS_ERR || rr == NULL) {
            return SLOTS_MGRT_ERR;
        }
        if (rr->type == REDIS_REPLY_ERROR) {
            freeReplyObject(rr);
            return SLOTS_MGRT_ERR;
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
        return SLOTS_MGRT_ERR;
    }
    // todo auth

    redisReply* rr;
    int db = RedisModule_GetSelectedDb(ctx);
    rr = redisCommand(conn->conn_ctx, "SELECT %d", db);
    if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
        if (rr != NULL) {
            freeReplyObject(rr);
        }
        return SLOTS_MGRT_ERR;
    }
    freeReplyObject(rr);

    if (mgrtType != NULL && strcasecmp(mgrtType, "withrestore") == 0) {
        int ret = Pipeline_Restore(ctx, conn, objs, n);
        SlotsMGRT_CloseConn(ctx, host, port);
        return ret;
    }

    int ret = BatchSend_SlotsRestore(ctx, conn, objs, n);
    SlotsMGRT_CloseConn(ctx, host, port);
    return ret;
}

// SlotsMGRT_GetRdbDumpObjs
// return value:
//  -1 - error happens
//  >=0 - # of success get rdb_dump_objs num (0 or n)
// rdb_dump_obj* objs[] outsied alloc to fill rdb_dump_obj, use over to free.
static int getRdbDumpObjs(RedisModuleCtx* ctx, RedisModuleString* keys[], int n,
                          rdb_dump_obj* objs[]) {
    if (n <= 0) {
        return 0;
    }

    int j = 0;
    RedisModuleCallReply* reply;
    for (int i = 0; i < n; i++) {
        reply = RedisModule_Call(ctx, "DUMP", "s", keys[i]);
        if (reply == NULL)
            continue;
        int type = RedisModule_CallReplyType(reply);
        if (type == REDISMODULE_REPLY_NULL) {
            RedisModule_FreeCallReply(reply);
            continue;
        }
        if (type != REDISMODULE_REPLY_STRING) {
            // RedisModule_ReplyWithCallReply(ctx, reply);
            RedisModule_FreeCallReply(reply);
            return SLOTS_MGRT_ERR;
        }
        RedisModuleString* val = RedisModule_CreateStringFromCallReply(reply);
        RedisModule_FreeCallReply(reply);

        reply = RedisModule_Call(ctx, "PTTL", "s", keys[i]);
        if (reply == NULL)
            continue;
        type = RedisModule_CallReplyType(reply);
        if (type == REDISMODULE_REPLY_NULL) {
            RedisModule_FreeCallReply(reply);
            continue;
        }
        if (type != REDISMODULE_REPLY_INTEGER) {
            // RedisModule_ReplyWithCallReply(ctx, reply);
            RedisModule_FreeCallReply(reply);
            return SLOTS_MGRT_ERR;
        }
        long long ttlms = RedisModule_CallReplyInteger(reply);
        RedisModule_FreeCallReply(reply);

        objs[j]->key = keys[j];
        objs[j]->ttlms = ttlms;
        objs[j]->val = val;
        j++;
    }
    return j;
}

static int delKeys(RedisModuleCtx* ctx, RedisModuleString* keys[], int n) {
    RedisModuleCallReply* reply;
    int ret = 0;
    for (int i = 0; i < n; i++) {
        reply = RedisModule_Call(ctx, "DEL", "s", keys[i]);
        int type = RedisModule_CallReplyType(reply);
        if (reply == NULL)
            continue;
        if (type == REDISMODULE_REPLY_NULL) {
            RedisModule_FreeCallReply(reply);
            continue;
        }
        if (type != REDISMODULE_REPLY_INTEGER) {
            // RedisModule_ReplyWithCallReply(ctx, reply);
            RedisModule_FreeCallReply(reply);
            return SLOTS_MGRT_ERR;
        }
        RedisModule_FreeCallReply(reply);
        ret++;
    }
    return ret;
}

static int migrateKeys(RedisModuleCtx* ctx, const sds host, const sds port,
                       time_t timeoutMS, RedisModuleString* keys[], int n,
                       const sds mgrtType) {
    if (n <= 0) {
        return 0;
    }

    // get rdb dump objs
    rdb_dump_obj* objs = RedisModule_Alloc(sizeof(rdb_dump_obj) * n);
    int ret = getRdbDumpObjs(ctx, keys, n, &objs);
    if (ret == 0) {
        RedisModule_Free(objs);
        return 0;
    }
    if (ret == SLOTS_MGRT_ERR) {
        RedisModule_Free(objs);
        return SLOTS_MGRT_ERR;
    }

    // migrate
    ret = MGRT(ctx, host, port, timeoutMS, &objs, ret, mgrtType);
    if (ret == SLOTS_MGRT_ERR) {
        RedisModule_Free(objs);
        return SLOTS_MGRT_ERR;
    }
    if (ret == 0) {
        RedisModule_Free(objs);
        return ret;
    }
    RedisModule_Free(objs);

    // del
    if (delKeys(ctx, keys, n) == SLOTS_MGRT_ERR) {
        return SLOTS_MGRT_ERR;
    }

    return n;
}

// SlotsMGRT_OneKey
// do migrate a key-value for slotsmgrt/slotsmgrtone commands
// 1.dump key rdb obj val
// 2.batch migrate send to host:port with r/w timeout
// 3.if migrate ok, remove key
// return value:
//  -1 - error happens
//  >=0 - # of success migration (0 or 1)
int SlotsMGRT_OneKey(RedisModuleCtx* ctx, const char* host, const char* port,
                     time_t timeout, RedisModuleString* key,
                     const char* mgrtType) {
    return migrateKeys(ctx, (const sds)host, (const sds)port, timeout,
                       (RedisModuleString*[]){key}, 1, (const sds)mgrtType);
}

static void notifyOne(RedisModuleCtx* ctx, RedisModuleString* key) {
    RedisModuleKey* okey
        = RedisModule_OpenKey(ctx, key, REDISMODULE_READ | REDISMODULE_WRITE);

    // inner type notify
    if (RedisModule_KeyType(okey) == REDISMODULE_KEYTYPE_STRING) {
        RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_STRING,
                                        "slotsmgrt-restore", key);
    }
    if (RedisModule_KeyType(okey) == REDISMODULE_KEYTYPE_HASH) {
        RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_HASH,
                                        "slotsmgrt-restore", key);
    }
    if (RedisModule_KeyType(okey) == REDISMODULE_KEYTYPE_LIST) {
        RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_LIST,
                                        "slotsmgrt-restore", key);
    }
    if (RedisModule_KeyType(okey) == REDISMODULE_KEYTYPE_SET) {
        RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_SET,
                                        "slotsmgrt-restore", key);
    }
    if (RedisModule_KeyType(okey) == REDISMODULE_KEYTYPE_ZSET) {
        RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_ZSET,
                                        "slotsmgrt-restore", key);
    }

    // todo if use outside 3rd extra type
    // need to register keyspace notify and sub event

    RedisModule_CloseKey(okey);
}

static int restoreOne(RedisModuleCtx* ctx, rdb_dump_obj* obj) {
    if (obj->ttlms < 0) {
        obj->ttlms = 0;
    }
    const char* k = RedisModule_StringPtrLen(obj->key, NULL);
    size_t vsz;
    const char* v = RedisModule_StringPtrLen(obj->val, &vsz);
    RedisModuleCallReply* reply
        = RedisModule_Call(ctx, "RESTORE", "clb", k, obj->ttlms, v, vsz);
    if (reply == NULL) {
        return 0;
    }
    int type = RedisModule_CallReplyType(reply);
    if (type == REDISMODULE_REPLY_NULL) {
        RedisModule_FreeCallReply(reply);
        return 0;
    }
    if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_ERROR) {
        RedisModule_FreeCallReply(reply);
        return SLOTS_MGRT_ERR;
    }
    if (RedisModule_NotifyKeyspaceEvent) {  // 6.0
        notifyOne(ctx, obj->key);
    } else {
        Slots_Add(ctx, RedisModule_GetSelectedDb(ctx), obj->key);
    }
    RedisModule_FreeCallReply(reply);

    return 1;
}

static int restoreMutli(RedisModuleCtx* ctx, rdb_dump_obj* objs[], int n) {
    for (int i = 0; i < n; i++) {
        if (restoreOne(ctx, objs[i]) == SLOTS_MGRT_ERR) {
            return SLOTS_MGRT_ERR;
        }
    }

    return n;
}

static void restoreOneTask(void* arg) {
    slots_restore_one_task_params* params = (slots_restore_one_task_params*)arg;
    params->result_code = restoreOne(params->ctx, params->obj);
}

static int restoreMutliWithThreadPool(RedisModuleCtx* ctx, rdb_dump_obj* objs[],
                                      int n) {
    UNUSED(ctx);
    // use redis dep's jemalloc allcator instead of libc allocator (often
    // prevents fragmentation problems)
    slots_restore_one_task_params* params
        = RedisModule_Alloc(sizeof(slots_restore_one_task_params) * n);
    // slots_restore_one_task_params* params
    //    = (slots_restore_one_task_params*)malloc(
    //        sizeof(slots_restore_one_task_params) * n);

    threadpool thpool = thpool_init(g_slots_meta_info.slots_restore_threads);
    for (int i = 0; i < n; i++) {
        // params[i].ctx = ctx;
        params[i].ctx = RedisModule_GetThreadSafeContext(NULL);
        params[i].obj = objs[i];
        params[i].result_code = 0;
        thpool_add_work(thpool, restoreOneTask, (void*)&params[i]);
    }
    thpool_wait(thpool);
    thpool_destroy(thpool);

    for (int i = 0; i < n; i++) {
        RedisModule_FreeThreadSafeContext(params[i].ctx);
    }

    for (int i = 0; i < n; i++) {
        if (params[i].result_code == SLOTS_MGRT_ERR) {
            RedisModule_Free(params);
            // free(params);
            return SLOTS_MGRT_ERR;
        }
    }

    RedisModule_Free(params);
    // free(params);
    return n;
}

int SlotsMGRT_Restore(RedisModuleCtx* ctx, rdb_dump_obj* objs[], int n) {
    if (g_slots_meta_info.slots_restore_threads > 0) {
        return restoreMutliWithThreadPool(ctx, objs, n);
    }
    return restoreMutli(ctx, objs, n);
}

int SlotsMGRT_SlotOneKey(RedisModuleCtx* ctx, const char* host,
                         const char* port, time_t timeout, int slot,
                         const char* mgrtType) {
    int db = RedisModule_GetSelectedDb(ctx);
    const m_dictEntry* de
        = m_dictGetRandomKey(db_slot_infos[db].slotkey_tables[slot]);
    if (de == NULL) {
        return 0;
    }
    const sds k = dictGetKey(de);
    RedisModuleString* key = RedisModule_CreateString(ctx, k, sdslen(k));
    int ret = SlotsMGRT_OneKey(ctx, host, port, timeout, key, mgrtType);
    if (ret > 0) {
        // should sub cron_loop(server loop) to del
        // m_dictDelete(db_slot_infos[db].slotkey_tables[slot], k);
    }
    RedisModule_FreeString(ctx, key);
    return ret;
}

int SlotsMGRT_TagKeys(RedisModuleCtx* ctx, const char* host, const char* port,
                      time_t timeout, RedisModuleString* key,
                      const char* mgrtType) {
    const char* k = RedisModule_StringPtrLen(key, NULL);
    uint32_t crc;
    int hastag;
    int slot = slots_num(k, &crc, &hastag);
    if (!hastag) {
        return SlotsMGRT_OneKey(ctx, host, port, timeout, key, mgrtType);
    }

    int db = RedisModule_GetSelectedDb(ctx);
    dict* d = db_slot_infos[db].slotkey_tables[slot];
    if (dictSize(d) == 0) {
        return 0;
    }

    m_zrangespec range;
    range.min = (long long)crc;
    range.minex = 0;
    range.max = (long long)crc;
    range.maxex = 0;

    // ... need slice to append like golang slice
    list* l = m_listCreate();
    m_zskiplistNode* node
        = m_zslFirstInRange(db_slot_infos[db].tagged_key_list, &range);
    while (node != NULL && node->score == (long long)crc) {
        m_listAddNodeTail(l, node->member);
        node = node->level[0].forward;
    }
    int max = listLength(l);
    if (max == 0) {
        m_listRelease(l);
        return 0;
    }
    RedisModuleString** keys
        = RedisModule_Alloc(sizeof(RedisModuleString*) * max);
    int n = 0;
    for (int i = 0; i < max; i++) {
        m_listNode* head = listFirst(l);
        RedisModuleString* k = listNodeValue(head);
        if (k != NULL) {
            keys[n] = key;
            n++;
        }
        m_listDelNode(l, head);
    }
    m_listRelease(l);

    int ret = migrateKeys(ctx, (const sds)host, (const sds)port, timeout, keys,
                          n, (const sds)mgrtType);
    RedisModule_Free(keys);
    return ret;
}

int SlotsMGRT_TagSlotKeys(RedisModuleCtx* ctx, const char* host,
                          const char* port, time_t timeout, int slot,
                          const char* mgrtType) {
    int db = RedisModule_GetSelectedDb(ctx);
    const m_dictEntry* de
        = m_dictGetRandomKey(db_slot_infos[db].slotkey_tables[slot]);
    if (de == NULL) {
        return 0;
    }
    const sds k = dictGetKey(de);
    RedisModuleString* key = RedisModule_CreateString(ctx, k, sdslen(k));
    int ret = SlotsMGRT_TagKeys(ctx, host, port, timeout, key, mgrtType);
    if (ret > 0) {
        // should sub cron_loop(server loop) to del
        // m_dictDelete(db_slot_infos[db].slotkey_tables[slot], k);
    }
    RedisModule_FreeString(ctx, key);
    return ret;
}

static void slotsScanRedisModuleKeyCallback(void* l, const m_dictEntry* de) {
    RedisModuleString* key = dictGetKey(de);
    m_listAddNodeTail((list*)l, key);
}

void SlotsMGRT_Scan(RedisModuleCtx* ctx, int slot, unsigned long count,
                    unsigned long cursor, list* l) {
    int db = RedisModule_GetSelectedDb(ctx);
    dict* d = db_slot_infos[db].slotkey_tables[slot];
    long loops = count * 10;  // see dictScan
    do {
        cursor
            = m_dictScan(d, cursor, slotsScanRedisModuleKeyCallback, NULL, l);
        loops--;
    } while (cursor != 0 && loops > 0 && listLength(l) < count);
}

int SlotsMGRT_DelSlotKeys(RedisModuleCtx* ctx, int db, int slots[], int n) {
    for (int i = 0; i < n; i++) {
        dict* d = db_slot_infos[db].slotkey_tables[slots[i]];
        int s = dictSize(d);
        if (s == 0) {
            continue;
        }
        list* l = m_listCreate();
        unsigned long cursor = 0;
        do {
            cursor = m_dictScan(d, cursor, slotsScanRedisModuleKeyCallback,
                                NULL, l);
            while (1) {
                m_listNode* head = listFirst(l);
                if (head == NULL) {
                    break;
                }
                RedisModuleString* key = listNodeValue(head);
                if (delKeys(ctx, (RedisModuleString*[]){key}, 1)
                    == SLOTS_MGRT_ERR) {
                    return SLOTS_MGRT_ERR;
                }
                m_listDelNode(l, head);
            }
        } while (cursor != 0);
        m_listRelease(l);
    }

    return n;
}

/**
* This function can be used instead of `RedisModule_RetainString()`.
* The main difference between the two is that this function will always
* succeed, whereas `RedisModule_RetainString()` may fail because of an
* assertion.
*
* The function returns a pointer to RedisModuleString, which is owned
* by the caller. It requires a call to `RedisModule_FreeString()` to free
* the string when automatic memory management is disabled for the context.
* When automatic memory management is enabled, you can either call
* `RedisModule_FreeString()` or let the automation free it.
*
* This function is more efficient than `RedisModule_CreateStringFromString()`
* because whenever possible, it avoids copying the underlying
* RedisModuleString. The disadvantage of using this function is that it
* might not be possible to use `RedisModule_StringAppendBuffer()` on the
* returned RedisModuleString.
* It is possible to call this function with a NULL context.
*
When strings are going to be held for an extended duration, it is good
practice to also call `RedisModule_TrimStringAllocation()` in order to
optimize memory usage.

Threaded modules that reference held strings from other threads *must*
explicitly trim the allocation as soon as the string is held. Not doing
so may result with automatic trimming which is not thread safe.

so reuse by refcount, if threaded modules please to copy(create a new)
*/
RedisModuleString* takeAndRef(RedisModuleString* str) {
    // check high -> low version
    if (RedisModule_HoldString) {
        RedisModule_HoldString(NULL, str);  // 6.0
    } else if (RedisModule_RetainString) {
        RedisModule_RetainString(NULL, str);  // 4.0
    }
    return str;
}

void Slots_Add(RedisModuleCtx* ctx, int db, RedisModuleString* key) {
    const char* kstr = RedisModule_StringPtrLen(key, NULL);
    uint32_t crc;
    int hastag;
    int slot = slots_num(kstr, &crc, &hastag);
    RedisModuleString* sval
        = RedisModule_CreateStringFromLongLong(ctx, (long long)crc);
    if (m_dictAdd(db_slot_infos[db].slotkey_tables[slot], takeAndRef(key),
                  (void*)sval)
        == DICT_OK) {
        if (hastag) {
            m_zslInsert(db_slot_infos[db].tagged_key_list, (long long)crc,
                        takeAndRef(key));
        }
    }
}

void Slots_Del(RedisModuleCtx* ctx, int db, RedisModuleString* key) {
    UNUSED(ctx);
    const char* kstr = RedisModule_StringPtrLen(key, NULL);
    uint32_t crc;
    int hastag;
    int slot = slots_num(kstr, &crc, &hastag);
    // entry key,val free
    if (m_dictDelete(db_slot_infos[db].slotkey_tables[slot], key) == DICT_OK) {
        if (hastag) {
            // node key free with score
            m_zslDelete(db_slot_infos[db].tagged_key_list, (long long)crc, key,
                        NULL);
        }
    }
}