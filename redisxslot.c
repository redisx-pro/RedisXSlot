/*
 * Copyright (c) 2023, weedge <weege007 at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "redisxslot.h"

slots_meta_info g_slots_meta_info;
db_slot_info* db_slot_infos;

// declare defined static var to inner use (private prototypes)
static RedisModuleDict* slotsmgrt_cached_ctx_connects;
static pthread_mutex_t slotsmgrt_cached_ctx_connects_lock
    = PTHREAD_MUTEX_INITIALIZER;
// rm_call big locker, need change redis struct to support multi threads :|
// so (*mgrt*)/restore job should async block run,
// splite batch todo, don't or less block other cmd run :)
// if change redis struct, use RedisModule_ThreadSafeContextLock GIL instead it.
static pthread_mutex_t rm_call_lock = PTHREAD_MUTEX_INITIALIZER;

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

void Slots_Init(RedisModuleCtx* ctx, uint32_t hash_slots_size, int databases,
                int num_threads, int activerehashing, int async) {
    crc32_init();

    g_slots_meta_info.hash_slots_size = hash_slots_size;
    g_slots_meta_info.databases = databases;
    g_slots_meta_info.async = async;
    g_slots_meta_info.activerehashing = activerehashing;
    g_slots_meta_info.cronloops = 0;

    // worker thread pool for each indepence task, less mutex case.
    // if more mutex, maybe don't use it. want to learn, open it :)
    // g_slots_meta_info.slots_dump_threads = num_threads;
    g_slots_meta_info.slots_mgrt_threads = num_threads;
    // g_slots_meta_info.slots_restore_threads = num_threads;

    /* like bio define diff type job thread, just one type job thread todo. no
     * mutex, but no wait, so use async job, such as async net/disk io */

    db_slot_infos = RedisModule_Alloc(sizeof(db_slot_info) * databases);
    for (int j = 0; j < databases; j++) {
        db_slot_infos[j].slotkey_tables
            = RedisModule_Alloc(sizeof(dict*) * hash_slots_size);
        db_slot_infos[j].slotkey_table_rwlocks
            = RedisModule_Alloc(sizeof(pthread_rwlock_t) * hash_slots_size);
        for (uint32_t i = 0; i < hash_slots_size; i++) {
            db_slot_infos[j].slotkey_tables[i]
                = m_dictCreate(&hashSlotDictType, NULL);
            pthread_rwlock_init(&(db_slot_infos[j].slotkey_table_rwlocks[i]),
                                NULL);
        }
        db_slot_infos[j].slotkey_table_rehashing = 0;
        db_slot_infos[j].tagged_key_list = m_zslCreate();
        pthread_rwlock_init(&(db_slot_infos[j].tagged_key_list_rwlock), NULL);
    }

    slotsmgrt_cached_ctx_connects = RedisModule_CreateDict(ctx);
}

void Slots_Free(RedisModuleCtx* ctx) {
    RedisModule_Log(ctx, "notice", "slots free");
    for (int j = 0; j < g_slots_meta_info.databases; j++) {
        if (db_slot_infos != NULL && db_slot_infos[j].slotkey_tables != NULL) {
            for (uint32_t i = 0; i < g_slots_meta_info.hash_slots_size; i++) {
                pthread_rwlock_wrlock(
                    &(db_slot_infos[j].slotkey_table_rwlocks[i]));
                m_dictRelease(db_slot_infos[j].slotkey_tables[i]);
                pthread_rwlock_unlock(
                    &(db_slot_infos[j].slotkey_table_rwlocks[i]));
                pthread_rwlock_destroy(
                    &(db_slot_infos[j].slotkey_table_rwlocks[i]));
            }
            RedisModule_Free(db_slot_infos[j].slotkey_tables);
            db_slot_infos[j].slotkey_tables = NULL;
            RedisModule_Free(db_slot_infos[j].slotkey_table_rwlocks);
            db_slot_infos[j].slotkey_table_rwlocks = NULL;
        }
        if (db_slot_infos != NULL && db_slot_infos[j].tagged_key_list != NULL) {
            pthread_rwlock_wrlock(&(db_slot_infos[j].tagged_key_list_rwlock));
            m_zslFree(db_slot_infos[j].tagged_key_list);
            pthread_rwlock_unlock(&(db_slot_infos[j].tagged_key_list_rwlock));
            db_slot_infos[j].tagged_key_list = NULL;
            pthread_rwlock_destroy(&(db_slot_infos[j].tagged_key_list_rwlock));
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

static time_t get_unixtime(void) {
#if (REDIS_VERSION == 70200)
    return (time_t)(RedisModule_CachedMicroseconds() / 1e6);
#endif
    return (time_t)(RedisModule_Milliseconds() / 1e3);
}

// getConnName
// return {host}:{port}@{thread_id}
static sds getConnName(const sds host, const sds port) {
    sds name = sdsempty();
    name = sdscatlen(name, host, sdslen(host));
    name = sdscatlen(name, ":", 1);
    name = sdscatlen(name, port, sdslen(port));
    char buf[REDIS_LONGSTR_SIZE];
    long long tid = (long long)gettid();
    // long long tid = (long long)pthread_self();
    m_ll2string(buf, sizeof(buf), tid);
    name = sdscatlen(name, "@", 1);
    name = sdscatlen(name, buf, strlen(buf));
    return name;
}

static db_slot_mgrt_connect* SlotsMGRT_GetConnCtx(RedisModuleCtx* ctx,
                                                  slot_mgrt_connet_meta* meta) {
    time_t unixtime = get_unixtime();
    sds name = getConnName(meta->host, meta->port);

    db_slot_mgrt_connect* conn = NULL;
    pthread_mutex_lock(&slotsmgrt_cached_ctx_connects_lock);
    // conn = m_dictFetchValue(slotsmgrt_cached_ctx_connects, name);
    conn = RedisModule_DictGetC(slotsmgrt_cached_ctx_connects, (void*)name,
                                sdslen(name), NULL);
    pthread_mutex_unlock(&slotsmgrt_cached_ctx_connects_lock);
    if (conn != NULL) {
        sdsfree(name);
        conn->last_time = unixtime;
        return conn;
    }

    redisContext* c = redisConnect(meta->host, atoi(meta->port));
    if (c->err) {
        char errLog[200];
        sprintf(errLog, "Err: slotsmgrt connect to target %s, error = '%s'",
                name, c->errstr);
        RedisModule_Log(ctx, "warning", "%s", errLog);
        sdsfree(name);
        return NULL;
    }
    redisSetTimeout(c, meta->timeout);
    RedisModule_Log(ctx, "notice",
                    "slotsmgrt: connect to target %s set timeout: %ld.%ld",
                    name, meta->timeout.tv_sec, meta->timeout.tv_usec);

    conn = RedisModule_Alloc(sizeof(db_slot_mgrt_connect));
    conn->conn_ctx = c;
    conn->last_time = unixtime;
    conn->meta = meta;

    pthread_mutex_lock(&slotsmgrt_cached_ctx_connects_lock);
    // m_dictAdd(slotsmgrt_cached_ctx_connects, name, conn);
    RedisModule_DictSetC(slotsmgrt_cached_ctx_connects, (void*)name,
                         sdslen(name), conn);
    pthread_mutex_unlock(&slotsmgrt_cached_ctx_connects_lock);

    sdsfree(name);
    return conn;
}

static void SlotsMGRT_CloseConn(RedisModuleCtx* ctx,
                                slot_mgrt_connet_meta* meta) {
    sds name = getConnName(meta->host, meta->port);

    pthread_mutex_lock(&slotsmgrt_cached_ctx_connects_lock);
    // db_slot_mgrt_connect* conn =
    // m_dictFetchValue(slotsmgrt_cached_ctx_connects, name);
    db_slot_mgrt_connect* conn = RedisModule_DictGetC(
        slotsmgrt_cached_ctx_connects, (void*)name, sdslen(name), NULL);
    pthread_mutex_unlock(&slotsmgrt_cached_ctx_connects_lock);
    if (conn == NULL) {
        RedisModule_Log(ctx, "warning", "slotsmgrt: close target %s again",
                        name);
        sdsfree(name);
        return;
    }
    RedisModule_Log(ctx, "notice", "slotsmgrt: close target %s ok", name);
    pthread_mutex_lock(&slotsmgrt_cached_ctx_connects_lock);
    // m_dictDelete(slotsmgrt_cached_ctx_connects, name);
    RedisModule_DictDelC(slotsmgrt_cached_ctx_connects, (void*)name,
                         sdslen(name), NULL);
    pthread_mutex_unlock(&slotsmgrt_cached_ctx_connects_lock);
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
    time_t unixtime = get_unixtime();

    pthread_mutex_lock(&slotsmgrt_cached_ctx_connects_lock);
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
        if ((unixtime - conn->last_time) > MGRT_BATCH_KEY_TIMEOUT) {
            RedisModule_Log(
                ctx, "notice",
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
    pthread_mutex_unlock(&slotsmgrt_cached_ctx_connects_lock);
}

static void freeHiRedisSlotsRestoreArgs(char** argv, size_t* argvlen, int n) {
    for (int i = 0; i < n; i++) {
        RedisModule_Free(argv[i * 3 + 1]);
    }
    RedisModule_Free(argv);
    RedisModule_Free(argvlen);
}

/*
 *  err return SLOTS_MGRT_ERR, ok return obj cn,
 */
static int doSplitRestoreCommand(RedisModuleCtx* ctx,
                                 db_slot_mgrt_connect* conn, char** argv,
                                 size_t* argvlen, int start_pos, int end_pos) {
    int obj_cn = end_pos - start_pos;
    char** sub_argv = RedisModule_Alloc(sizeof(char*) * 3 * obj_cn + 1);
    size_t* sub_argvlen = RedisModule_Alloc(sizeof(size_t) * 3 * obj_cn + 1);
    sub_argv[0] = "SLOTSRESTORE";
    // cp pointer
    memcpy(&sub_argv[1], &argv[start_pos * 3], sizeof(char*) * obj_cn * 3);
    sub_argvlen[0] = strlen(sub_argv[0]);
    memcpy(&sub_argvlen[1], &argvlen[start_pos * 3],
           sizeof(size_t) * obj_cn * 3);

    redisReply* rr
        = redisCommandArgv(conn->conn_ctx, 3 * obj_cn + 1,
                           (const char**)sub_argv, (const size_t*)sub_argvlen);
    if (conn->conn_ctx->err) {
        RedisModule_Log(ctx, "warning", "errno %d errstr %s",
                        conn->conn_ctx->err, conn->conn_ctx->errstr);
        freeHiRedisSlotsRestoreArgs(sub_argv, sub_argvlen, 0);
        return SLOTS_MGRT_ERR;
    }
    if (rr == NULL) {
        RedisModule_Log(ctx, "warning", "reply is NULL");
        freeHiRedisSlotsRestoreArgs(sub_argv, sub_argvlen, 0);
        return SLOTS_MGRT_ERR;
    }
    if (rr->type == REDIS_REPLY_ERROR) {
        RedisModule_Log(ctx, "warning", "reply err %s", rr->str);
        freeReplyObject(rr);
        freeHiRedisSlotsRestoreArgs(sub_argv, sub_argvlen, 0);
        return SLOTS_MGRT_ERR;
    }

    RedisModule_Log(ctx, "notice", "start_pos %d end_pos %d send ok", start_pos,
                    end_pos);

    freeReplyObject(rr);
    freeHiRedisSlotsRestoreArgs(sub_argv, sub_argvlen, 0);
    return obj_cn;
}

static void doSplitRestoreCmdTask(void* arg) {
    RedisModuleCtx* ctx = RedisModule_GetThreadSafeContext(NULL);
    slots_split_restore_params* params = (slots_split_restore_params*)arg;
    db_slot_mgrt_connect* conn = SlotsMGRT_GetConnCtx(ctx, params->meta);
    if (conn == NULL) {
        params->result_code = SLOTS_MGRT_ERR;
        return;
    }
    // todo auth

    redisReply* rr;
    rr = redisCommand(conn->conn_ctx, "SELECT %d", params->meta->db);
    if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
        if (rr != NULL) {
            freeReplyObject(rr);
        }
        params->result_code = SLOTS_MGRT_ERR;
        return;
    }
    freeReplyObject(rr);

    params->result_code
        = doSplitRestoreCommand(ctx, conn, params->argv, params->argvlen,
                                params->start_pos, params->end_pos);

    conn->last_time = get_unixtime();
    SlotsMGRT_CloseConn(ctx, params->meta);
    RedisModule_FreeThreadSafeContext(ctx);
}

static int BatchSendWithThreadPool_SlotsRestore(RedisModuleCtx* ctx,
                                                slot_mgrt_connet_meta* meta,
                                                rdb_dump_obj* objs[], int n) {
    UNUSED(ctx);
    threadpool thpool = thpool_init(g_slots_meta_info.slots_mgrt_threads);
    slots_split_restore_params* params
        = RedisModule_Alloc(sizeof(slots_split_restore_params) * n);
    int params_cn = 0;

    // char* argv[3 * n];
    char** argv = RedisModule_Alloc(sizeof(char*) * 3 * n);
    // size_t argvlen[3 * n];
    size_t* argvlen = RedisModule_Alloc(sizeof(size_t) * 3 * n);
    char buf[REDIS_LONGSTR_SIZE];
    size_t cmd_size = 0;
    int start_pos = 0;
    for (int i = 0; i < n; i++) {
        // split cmd (bigkey? if async block mgrt, maybe don't think this)
        if (cmd_size > REDIS_MGRT_CMD_PARAMS_SIZE) {
            params[params_cn].meta = meta;
            params[params_cn].argv = argv;
            params[params_cn].argvlen = argvlen;
            params[params_cn].start_pos = start_pos;
            params[params_cn].end_pos = i;
            params[params_cn].result_code = 0;
            thpool_add_work(thpool, doSplitRestoreCmdTask,
                            (void*)&params[params_cn]);
            params_cn++;

            cmd_size = 0;
            start_pos = i;
        }

        size_t ksz, vsz;
        argv[i * 3 + 0] = (char*)RedisModule_StringPtrLen(objs[i]->key, &ksz);
        argvlen[i * 3 + 0] = ksz;
        cmd_size += argvlen[i * 3 + 0];

        time_t ttlms = objs[i]->ttlms > 0 ? objs[i]->ttlms : 0;
        argvlen[i * 3 + 1] = m_ll2string(buf, sizeof(buf), (long long)ttlms);
        argv[i * 3 + 1] = RedisModule_Strdup(buf);
        cmd_size += argvlen[i * 3 + 1];

        argv[i * 3 + 2] = (char*)RedisModule_StringPtrLen(objs[i]->val, &vsz);
        argvlen[i * 3 + 2] = vsz;
        cmd_size += argvlen[i * 3 + 2];
    }

    params[params_cn].meta = meta;
    params[params_cn].argv = argv;
    params[params_cn].argvlen = argvlen;
    params[params_cn].start_pos = start_pos;
    params[params_cn].end_pos = n;
    params[params_cn].result_code = 0;
    thpool_add_work(thpool, doSplitRestoreCmdTask, (void*)&params[params_cn]);
    params_cn++;

    thpool_wait(thpool);
    thpool_destroy(thpool);

    for (int i = 0; i < params_cn; i++) {
        if (params[i].result_code == SLOTS_MGRT_ERR) {
            freeHiRedisSlotsRestoreArgs(argv, argvlen, n);
            RedisModule_Free(params);
            return SLOTS_MGRT_ERR;
        }
    }

    freeHiRedisSlotsRestoreArgs(argv, argvlen, n);
    RedisModule_Free(params);
    return n;
}

static int BatchSend_SlotsRestore(RedisModuleCtx* ctx,
                                  db_slot_mgrt_connect* conn,
                                  rdb_dump_obj* objs[], int n) {
    // char* argv[3 * n];
    char** argv = RedisModule_Alloc(sizeof(char*) * 3 * n);
    // size_t argvlen[3 * n];
    size_t* argvlen = RedisModule_Alloc(sizeof(size_t) * 3 * n);
    char buf[REDIS_LONGSTR_SIZE];
    size_t cmd_size = 0;
    int start_pos = 0;
    for (int i = 0; i < n; i++) {
        // split cmd to send,(todo: bigkey)
        if (cmd_size > REDIS_MGRT_CMD_PARAMS_SIZE) {
            if (doSplitRestoreCommand(ctx, conn, argv, argvlen, start_pos, i)
                == SLOTS_MGRT_ERR) {
                freeHiRedisSlotsRestoreArgs(argv, argvlen, i - start_pos);
                return SLOTS_MGRT_ERR;
            }
            cmd_size = 0;
            start_pos = i;
        }

        size_t ksz, vsz;
        argv[i * 3 + 0] = (char*)RedisModule_StringPtrLen(objs[i]->key, &ksz);
        argvlen[i * 3 + 0] = ksz;
        cmd_size += argvlen[i * 3 + 0];

        time_t ttlms = objs[i]->ttlms > 0 ? objs[i]->ttlms : 0;
        argvlen[i * 3 + 1] = m_ll2string(buf, sizeof(buf), (long long)ttlms);
        argv[i * 3 + 1] = RedisModule_Strdup(buf);
        cmd_size += argvlen[i * 3 + 1];

        argv[i * 3 + 2] = (char*)RedisModule_StringPtrLen(objs[i]->val, &vsz);
        argvlen[i * 3 + 2] = vsz;
        cmd_size += argvlen[i * 3 + 2];
    }

    if (doSplitRestoreCommand(ctx, conn, argv, argvlen, start_pos, n)
        == SLOTS_MGRT_ERR) {
        freeHiRedisSlotsRestoreArgs(argv, argvlen, n);
        return SLOTS_MGRT_ERR;
    }

    conn->last_time = get_unixtime();
    freeHiRedisSlotsRestoreArgs(argv, argvlen, n);
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

        redisAppendCommand(conn->conn_ctx, "RESTORE %b %ld %b replace", k, ksz,
                           ttlms, v, vsz);
        // todo pre split to chunk to send,(maybe think bigkey)
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
    int db = RedisModule_GetSelectedDb(ctx);
    struct timeval timeout
        = {.tv_sec = timeoutMS / 1000, .tv_usec = (timeoutMS % 1000) * 1000};
    slot_mgrt_connet_meta meta
        = {.db = db, .host = host, .port = port, .timeout = timeout};

    // if use thread pool, each worker thread new one connect to mgrt
    if (g_slots_meta_info.slots_mgrt_threads > 0) {
        int ret = BatchSendWithThreadPool_SlotsRestore(ctx, &meta, objs, n);
        return ret;
    }

    db_slot_mgrt_connect* conn = SlotsMGRT_GetConnCtx(ctx, &meta);
    if (conn == NULL) {
        return SLOTS_MGRT_ERR;
    }
    // todo auth

    redisReply* rr;
    rr = redisCommand(conn->conn_ctx, "SELECT %d", db);
    if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
        if (rr != NULL) {
            freeReplyObject(rr);
        }
        SlotsMGRT_CloseConn(ctx, &meta);
        return SLOTS_MGRT_ERR;
    }
    freeReplyObject(rr);

    if (mgrtType != NULL && strcasecmp(mgrtType, "withrestore") == 0) {
        int ret = Pipeline_Restore(ctx, conn, objs, n);
        SlotsMGRT_CloseConn(ctx, &meta);
        return ret;
    }

    int ret = BatchSend_SlotsRestore(ctx, conn, objs, n);
    SlotsMGRT_CloseConn(ctx, &meta);
    return ret;
}

// dumpObj
// return 0 nothing todo -1 error happens; if dump ok, return a new dump obj
static int dumpObj(RedisModuleCtx* ctx, RedisModuleString* key,
                   rdb_dump_obj** obj) {
    // RedisModule_ThreadSafeContextLock(ctx);
    pthread_mutex_lock(&rm_call_lock);
    RedisModuleCallReply* reply = RedisModule_Call(ctx, "DUMP", "s", key);
    pthread_mutex_unlock(&rm_call_lock);
    // RedisModule_ThreadSafeContextUnlock(ctx);
    if (reply == NULL)
        return SLOTS_MGRT_NOTHING;
    int type = RedisModule_CallReplyType(reply);
    if (type == REDISMODULE_REPLY_NULL) {
        RedisModule_FreeCallReply(reply);
        return SLOTS_MGRT_NOTHING;
    }
    if (type != REDISMODULE_REPLY_STRING) {
        RedisModule_FreeCallReply(reply);
        return SLOTS_MGRT_ERR;
    }
    RedisModuleString* val = RedisModule_CreateStringFromCallReply(reply);
    RedisModule_FreeCallReply(reply);

    // RedisModule_ThreadSafeContextLock(ctx);
    pthread_mutex_lock(&rm_call_lock);
    reply = RedisModule_Call(ctx, "PTTL", "s", key);
    pthread_mutex_unlock(&rm_call_lock);
    // RedisModule_ThreadSafeContextUnlock(ctx);
    if (reply == NULL)
        return SLOTS_MGRT_NOTHING;
    type = RedisModule_CallReplyType(reply);
    if (type == REDISMODULE_REPLY_NULL) {
        RedisModule_FreeCallReply(reply);
        return SLOTS_MGRT_NOTHING;
    }
    if (type != REDISMODULE_REPLY_INTEGER) {
        RedisModule_FreeCallReply(reply);
        return SLOTS_MGRT_ERR;
    }
    long long ttlms = RedisModule_CallReplyInteger(reply);
    RedisModule_FreeCallReply(reply);

    rdb_dump_obj* a_obj = RedisModule_Alloc(sizeof(rdb_dump_obj));
    a_obj->key = key;
    a_obj->ttlms = ttlms;
    a_obj->val = val;
    *obj = a_obj;
    return 1;
}

static void dumpObjTask(void* arg) {
    dump_obj_params* params = (dump_obj_params*)arg;
    RedisModuleCtx* ctx = RedisModule_GetThreadSafeContext(NULL);
    params->result_code = dumpObj(ctx, params->key, params->obj);
    RedisModule_FreeThreadSafeContext(ctx);
}

static int getRdbDumpObjsWithThreadPool(RedisModuleCtx* ctx,
                                        RedisModuleString* keys[], int n,
                                        rdb_dump_obj** objs) {
    UNUSED(ctx);
    if (n <= 0) {
        return 0;
    }

    int num_threads = n > g_slots_meta_info.slots_dump_threads
                          ? g_slots_meta_info.slots_dump_threads
                          : n;
    dump_obj_params* params = RedisModule_Alloc(sizeof(dump_obj_params) * n);
    threadpool thpool = thpool_init(num_threads);
    for (int i = 0; i < n; i++) {
        params[i].key = keys[i];
        params[i].obj = &objs[i];
        params[i].result_code = SLOTS_MGRT_NOTHING;
        thpool_add_work(thpool, dumpObjTask, (void*)&params[i]);
    }
    thpool_wait(thpool);
    thpool_destroy(thpool);

    int j = 0;
    for (int i = 0; i < n; i++) {
        if (params[i].result_code == SLOTS_MGRT_NOTHING)
            continue;
        if (params[i].result_code == SLOTS_MGRT_ERR) {
            RedisModule_Free(params);
            return SLOTS_MGRT_ERR;
        }
        j++;
    }

    RedisModule_Free(params);
    return j;
}

// SlotsMGRT_GetRdbDumpObjs
// return value:
//  -1 - error happens
//  >=0 - # of success get rdb_dump_objs num (0 or n)
// rdb_dump_obj* objs[] outsied alloc to fill rdb_dump_obj, use over to free.
static int getRdbDumpObjs(RedisModuleCtx* ctx, RedisModuleString* keys[], int n,
                          rdb_dump_obj** objs) {
    if (n <= 0) {
        return SLOTS_MGRT_NOTHING;
    }

    if (g_slots_meta_info.slots_dump_threads > 0) {
        return getRdbDumpObjsWithThreadPool(ctx, keys, n, objs);
    }

    int j = 0;
    for (int i = 0; i < n; i++) {
        int r = dumpObj(ctx, keys[i], &objs[i]);
        if (r == SLOTS_MGRT_NOTHING)
            continue;
        if (r == SLOTS_MGRT_ERR)
            return SLOTS_MGRT_ERR;
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
            RedisModule_FreeCallReply(reply);
            return SLOTS_MGRT_ERR;
        }
        RedisModule_FreeCallReply(reply);
        ret++;
    }
    return ret;
}

void FreeDumpObjs(rdb_dump_obj** objs, int n) {
    for (int i = 0; i < n; i++) {
        if (objs[i] != NULL) {
            RedisModule_Free(objs[i]);
            objs[i] = NULL;
        }
    }
    if (objs != NULL) {
        RedisModule_Free(objs);
        objs = NULL;
    }
}

static double get_us(struct timeval t) {
    return (t.tv_sec * 1000000 + t.tv_usec);
}

static int migrateKeys(RedisModuleCtx* ctx, const sds host, const sds port,
                       time_t timeoutMS, RedisModuleString* keys[], int n,
                       const sds mgrtType) {
    if (n <= 0) {
        return 0;
    }
    struct timeval start_time, stop_time;

    // get rdb dump objs
    gettimeofday(&start_time, NULL);
    rdb_dump_obj** objs = RedisModule_Alloc(sizeof(rdb_dump_obj*) * n);
    int ret = getRdbDumpObjs(ctx, keys, n, objs);
    if (ret == SLOTS_MGRT_NOTHING) {
        FreeDumpObjs(objs, ret);
        return 0;
    }
    if (ret == SLOTS_MGRT_ERR) {
        FreeDumpObjs(objs, ret);
        return SLOTS_MGRT_ERR;
    }
    gettimeofday(&stop_time, NULL);
    RedisModule_Log(ctx, "notice", "%d objs dump cost %f ms", ret,
                    (get_us(stop_time) - get_us(start_time)) / 1000);

    // migrate
    gettimeofday(&start_time, NULL);
    int m_ret = MGRT(ctx, host, port, timeoutMS, objs, ret, mgrtType);
    if (m_ret == SLOTS_MGRT_ERR) {
        FreeDumpObjs(objs, ret);
        return SLOTS_MGRT_ERR;
    }
    if (m_ret == 0) {
        FreeDumpObjs(objs, ret);
        return m_ret;
    }
    FreeDumpObjs(objs, ret);
    gettimeofday(&stop_time, NULL);
    RedisModule_Log(ctx, "notice", "%d objs mgrt cost %f ms", m_ret,
                    (get_us(stop_time) - get_us(start_time)) / 1000);

    // del (unlink?)
    gettimeofday(&start_time, NULL);
    ret = delKeys(ctx, keys, n);
    if (ret == SLOTS_MGRT_ERR) {
        return SLOTS_MGRT_ERR;
    }
    gettimeofday(&stop_time, NULL);
    RedisModule_Log(ctx, "notice", "%d keys del cost %f ms", ret,
                    (get_us(stop_time) - get_us(start_time)) / 1000);

    return ret;
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

static int restoreOneWithReplace(RedisModuleCtx* ctx, rdb_dump_obj* obj) {
    if (obj->ttlms < 0) {
        obj->ttlms = 0;
    }
    const char* k = RedisModule_StringPtrLen(obj->key, NULL);
    size_t vsz;
    const char* v = RedisModule_StringPtrLen(obj->val, &vsz);
    RedisModuleCallReply* reply = RedisModule_Call(
        ctx, "RESTORE", "clbc", k, obj->ttlms, v, vsz, "replace");
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

    notifyOne(ctx, obj->key);
    RedisModule_FreeCallReply(reply);

    return 1;
}

static int restoreMutli(RedisModuleCtx* ctx, rdb_dump_obj* objs[], int n) {
    for (int i = 0; i < n; i++) {
        if (restoreOneWithReplace(ctx, objs[i]) == SLOTS_MGRT_ERR) {
            return SLOTS_MGRT_ERR;
        }
    }

    return n;
}

static void restoreOneTask(void* arg) {
    RedisModuleCtx* ctx = RedisModule_GetThreadSafeContext(NULL);
    slots_restore_one_task_params* params = (slots_restore_one_task_params*)arg;
    params->result_code = restoreOneWithReplace(ctx, params->obj);
    RedisModule_FreeThreadSafeContext(ctx);
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
        params[i].obj = objs[i];
        params[i].result_code = 0;
        thpool_add_work(thpool, restoreOneTask, (void*)&params[i]);
    }
    thpool_wait(thpool);
    thpool_destroy(thpool);

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
                         const char* mgrtType, int* left) {
    int db = RedisModule_GetSelectedDb(ctx);
    pthread_rwlock_rdlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    const m_dictEntry* de
        = m_dictGetRandomKey(db_slot_infos[db].slotkey_tables[slot]);
    pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    if (de == NULL) {
        return 0;
    }

    RedisModuleString* key = dictGetKey(de);
    int ret = SlotsMGRT_OneKey(ctx, host, port, timeout, key, mgrtType);
    if (ret == SLOTS_MGRT_ERR) {
        RedisModule_FreeString(ctx, key);
        return SLOTS_MGRT_ERR;
    }
    if (ret > 0) {
        // should sub cron_loop(server loop) to del
        // m_dictDelete(db_slot_infos[db].slotkey_tables[slot], k);
    }
    RedisModule_FreeString(ctx, key);
    if (left != NULL) {
        pthread_rwlock_rdlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
        *left = dictSize(db_slot_infos[db].slotkey_tables[slot]);
        pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    }
    return ret;
}

int SlotsMGRT_TagKeys(RedisModuleCtx* ctx, const char* host, const char* port,
                      time_t timeout, RedisModuleString* key,
                      const char* mgrtType, int* left) {
    const char* k = RedisModule_StringPtrLen(key, NULL);
    uint32_t crc;
    int hastag;
    int slot = slots_num(k, &crc, &hastag);
    if (!hastag) {
        return SlotsMGRT_OneKey(ctx, host, port, timeout, key, mgrtType);
    }

    int db = RedisModule_GetSelectedDb(ctx);
    pthread_rwlock_rdlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    dict* d = db_slot_infos[db].slotkey_tables[slot];
    unsigned long s = dictSize(d);
    pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    if (s == 0) {
        return 0;
    }

    m_zrangespec range;
    range.min = (long long)crc;
    range.minex = 0;
    range.max = (long long)crc;
    range.maxex = 0;

    // ... need slice to append like golang slice
    list* l = m_listCreate();

    // like read current snapshot, but not iter
    pthread_rwlock_rdlock(&(db_slot_infos[db].tagged_key_list_rwlock));
    m_zskiplistNode* node
        = m_zslFirstInRange(db_slot_infos[db].tagged_key_list, &range);
    while (node != NULL && node->score == (long long)crc) {
        m_listAddNodeTail(l, node->member);
        node = node->level[0].forward;
    }
    pthread_rwlock_unlock(&(db_slot_infos[db].tagged_key_list_rwlock));

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
        if (head != NULL) {
            RedisModuleString* k = listNodeValue(head);
            if (k != NULL) {
                keys[n] = k;
                n++;
            }
        }
        m_listDelNode(l, head);
    }
    m_listRelease(l);

    int ret = migrateKeys(ctx, (const sds)host, (const sds)port, timeout, keys,
                          n, (const sds)mgrtType);
    RedisModule_Free(keys);
    if (left != NULL) {
        pthread_rwlock_rdlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
        *left = dictSize(db_slot_infos[db].slotkey_tables[slot]);
        pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    }
    return ret;
}

int SlotsMGRT_TagSlotKeys(RedisModuleCtx* ctx, const char* host,
                          const char* port, time_t timeout, int slot,
                          const char* mgrtType, int* left) {
    int db = RedisModule_GetSelectedDb(ctx);
    pthread_rwlock_rdlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    const m_dictEntry* de
        = m_dictGetRandomKey(db_slot_infos[db].slotkey_tables[slot]);
    pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    if (de == NULL) {
        return 0;
    }
    RedisModuleString* key = dictGetKey(de);
    int ret = SlotsMGRT_TagKeys(ctx, host, port, timeout, key, mgrtType, left);
    if (ret > 0) {
        // should sub cron_loop(server loop) to del
        // m_dictDelete(db_slot_infos[db].slotkey_tables[slot], k);
    }
    return ret;
}

static void slotsScanRedisModuleKeyCallback(void* l, const m_dictEntry* de) {
    RedisModuleString* key = dictGetKey(de);
    m_listAddNodeTail((list*)l, key);
}

void SlotsMGRT_Scan(RedisModuleCtx* ctx, int slot, unsigned long count,
                    unsigned long cursor, list* l) {
    int db = RedisModule_GetSelectedDb(ctx);
    pthread_rwlock_rdlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    dict* d = db_slot_infos[db].slotkey_tables[slot];
    pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    long loops = count * 10;  // see dictScan
    do {
        pthread_rwlock_rdlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
        cursor
            = m_dictScan(d, cursor, slotsScanRedisModuleKeyCallback, NULL, l);
        pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
        loops--;
    } while (cursor != 0 && loops > 0 && listLength(l) < count);
}

int SlotsMGRT_DelSlotKeys(RedisModuleCtx* ctx, int db, int slots[], int n) {
    for (int i = 0; i < n; i++) {
        pthread_rwlock_rdlock(
            &(db_slot_infos[db].slotkey_table_rwlocks[slots[i]]));
        dict* d = db_slot_infos[db].slotkey_tables[slots[i]];
        int s = dictSize(d);
        pthread_rwlock_unlock(
            &(db_slot_infos[db].slotkey_table_rwlocks[slots[i]]));
        if (s == 0) {
            continue;
        }
        list* l = m_listCreate();
        unsigned long cursor = 0;
        do {
            pthread_rwlock_rdlock(
                &(db_slot_infos[db].slotkey_table_rwlocks[slots[i]]));
            cursor = m_dictScan(d, cursor, slotsScanRedisModuleKeyCallback,
                                NULL, l);
            pthread_rwlock_unlock(
                &(db_slot_infos[db].slotkey_table_rwlocks[slots[i]]));
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

issue: https://github.com/redis/redis/issues/7544

so reuse by refcount, if threaded modules please to copy(create a new)
*/
RedisModuleString* takeAndRef(RedisModuleCtx* ctx, RedisModuleString* str) {
    // check high->low version
    if (RedisModule_HoldString) {
        str = RedisModule_HoldString(ctx, str);  // 6.0.7
    } else if (RedisModule_RetainString) {
        RedisModule_RetainString(ctx, str);  // 4.0.0
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

    // entry key,val add
    pthread_rwlock_wrlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    int r = m_dictAdd(db_slot_infos[db].slotkey_tables[slot],
                      takeAndRef(NULL, key), (void*)sval);
    pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    if (r != DICT_OK) {
        return;
    }

    if (hastag) {
        // node key add with score(crc32)
        pthread_rwlock_wrlock(&(db_slot_infos[db].tagged_key_list_rwlock));
        m_zslInsert(db_slot_infos[db].tagged_key_list, (long long)crc,
                    takeAndRef(NULL, key));
        pthread_rwlock_unlock(&(db_slot_infos[db].tagged_key_list_rwlock));
    }
}

void Slots_Del(RedisModuleCtx* ctx, int db, RedisModuleString* key) {
    UNUSED(ctx);
    const char* kstr = RedisModule_StringPtrLen(key, NULL);
    uint32_t crc;
    int hastag;
    int slot = slots_num(kstr, &crc, &hastag);

    // entry key,val free
    pthread_rwlock_wrlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    int r = m_dictDelete(db_slot_infos[db].slotkey_tables[slot], key);
    pthread_rwlock_unlock(&(db_slot_infos[db].slotkey_table_rwlocks[slot]));
    if (r != DICT_OK) {
        return;
    }

    if (hastag) {
        // node key free with score(crc32)
        pthread_rwlock_wrlock(&(db_slot_infos[db].tagged_key_list_rwlock));
        m_zslDelete(db_slot_infos[db].tagged_key_list, (long long)crc, key,
                    NULL);
        pthread_rwlock_unlock(&(db_slot_infos[db].tagged_key_list_rwlock));
    }
}