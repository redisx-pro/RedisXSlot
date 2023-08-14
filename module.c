/* slots migrate module -- batch dump rdb obj entries to migrate slots
 *
 * -----------------------------------------------------------------------------
 *
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

// todo:
// 1. sub notify event hook to add/remove dict/skiplist (db slot keys)
// 2. sub CronLoop event hook to resize/rehash dict (db slot keys)
// 3. db slot key meta info save to rdb, load from rdb

/* Check if Redis version is compatible with the adapter. */
static inline int redisModuleCompatibilityCheckV5(void) {
    if (!RedisModule_CreateDict) {
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static inline int redisModuleCompatibilityCheckV6(void) {
    if (!RedisModule_HoldString) {
        return REDIS_ERR;
    }
    return REDIS_OK;
}

static inline int redisModuleCompatibilityCheckV7(void) {
    if (!RedisModule_EventLoopAdd || !RedisModule_EventLoopDel
        || !RedisModule_CreateTimer || !RedisModule_StopTimer) {
        return REDIS_ERR;
    }
    return REDIS_OK;
}

int SlotsTest_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply* reply;

    RedisModule_Call(ctx, "SET", "cc", "k2", "v22");
    reply = RedisModule_Call(ctx, "DUMP", "c", "k2");
    size_t sz;
    const char* str = RedisModule_CallReplyStringPtr(reply, &sz);

    RedisModuleString* strr = RedisModule_CreateStringFromCallReply(reply);
    reply = RedisModule_Call(ctx, "RESTORE", "ccs", "k4", "0", strr);
    /*
    int type = RedisModule_CallReplyType(reply);
    do {
        size_t sz;
        const char* str = RedisModule_CallReplyStringPtr(reply, &sz);
        printf("restore reply %p str %s len %ld type %d \n", reply, str, sz,
               type);
    } while (0);
    */

    redisContext* c = redisConnect("127.0.0.1", 6679);
    if (c->err) {
        printf("redis connect Error-->: %s\n", c->errstr);
        return REDISMODULE_ERR;
    }
    redisSetTimeout(c, (struct timeval){.tv_sec = 3, .tv_usec = 0});

    redisReply* rr = redisCommand(c, "RESTORE k2 0 %b", str, sz);
    if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
        if (rr != NULL)
            RedisModule_ReplyWithError(ctx, rr->str);
        return REDISMODULE_ERR;
    }
    freeReplyObject(rr);
    rr = redisCommand(c, "GET k2");
    if (rr == NULL || rr->type == REDIS_REPLY_ERROR) {
        if (rr != NULL)
            RedisModule_ReplyWithError(ctx, rr->str);
        return REDISMODULE_ERR;
    }
    printf("%s\n", rr->str);
    freeReplyObject(rr);
    redisFree(c);

    // reply = RedisModule_Call(ctx, "slotshashkey", "c", "k4");
    RedisModule_ReplyWithCallReply(ctx, reply);

    RedisModule_FreeCallReply(reply);
    return REDISMODULE_OK;
}

/*---------------------- command implementation ------------------------------*/
int SlotsHashKey_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc < 2)
        return RedisModule_WrongArity(ctx);

    RedisModule_ReplyWithArray(ctx, argc - 1);
    for (int i = 1; i < argc; i++) {
        const char* key_ptr = RedisModule_StringPtrLen(argv[i], NULL);
        int slot = slots_num(key_ptr, NULL, NULL);
        RedisModule_Log(ctx, "debug", "s = %s slot = %d \n", key_ptr, slot);
        RedisModule_ReplyWithLongLong(ctx, slot);
    }

    return REDISMODULE_OK;
}

/* *
 * slotsinfo [start] [count]
 * */
int SlotsInfo_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc >= 4)
        return RedisModule_WrongArity(ctx);

    long long start = 0;
    if (argc >= 2
        && RedisModule_StringToLongLong(argv[1], &start) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }

    long long count = 0;
    if (argc >= 3
        && RedisModule_StringToLongLong(argv[2], &count) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }

    int slots_slot[g_slots_meta_info.hash_slots_size];
    int slots_size[g_slots_meta_info.hash_slots_size];
    int n = 0;
    long long end = start + count;
    int db = RedisModule_GetSelectedDb(ctx);
    for (int i = start; i < end; i++) {
        int s = dictSize(db_slot_infos[db].slotkey_tables[i]);
        if (s == 0) {
            continue;
        }
        slots_slot[n] = i;
        slots_size[n] = s;
        n++;
    }

    RedisModule_ReplyWithArray(ctx, n);
    for (int i = 0; i < n; i++) {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithLongLong(ctx, slots_slot[i]);
        RedisModule_ReplyWithLongLong(ctx, slots_size[i]);
    }

    return REDISMODULE_OK;
}

/* *
 * slotsmgrtone host port timeout key
 * */
int SlotsMGRTOne_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
    if (argc != 5 && argc != 6)
        return RedisModule_WrongArity(ctx);

    const char* host = RedisModule_StringPtrLen(argv[1], NULL);
    const char* port = RedisModule_StringPtrLen(argv[2], NULL);
    long long timeout = 0;
    if (RedisModule_StringToLongLong(argv[3], &timeout) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }

    const char* mgrtType;
    if (argc == 6) {
        mgrtType = RedisModule_StringPtrLen(argv[5], NULL);
    }

    int r = SlotsMGRT_OneKey(ctx, host, port, timeout, argv[4], mgrtType);
    if (r == SLOTS_MGRT_ERR) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_MGRT);
        return REDISMODULE_ERR;
    }
    RedisModule_ReplyWithLongLong(ctx, r);
    return REDISMODULE_OK;
}

/* *
 * slotsmgrtslot host port timeout slot
 * */
int SlotsMGRTSlot_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                               int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc != 5 && argc != 6)
        return RedisModule_WrongArity(ctx);

    const char* host = RedisModule_StringPtrLen(argv[1], NULL);
    const char* port = RedisModule_StringPtrLen(argv[2], NULL);
    long long timeout = 0;
    if (RedisModule_StringToLongLong(argv[3], &timeout) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }
    long long slot = 0;
    if (RedisModule_StringToLongLong(argv[4], &slot) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }

    const char* mgrtType;
    if (argc == 6) {
        mgrtType = RedisModule_StringPtrLen(argv[5], NULL);
    }

    int db = RedisModule_GetSelectedDb(ctx);
    int r = SlotsMGRT_SlotOneKey(ctx, host, port, timeout, (int)slot, mgrtType);
    if (r == SLOTS_MGRT_ERR) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_MGRT);
        return REDISMODULE_ERR;
    }
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, r);
    RedisModule_ReplyWithLongLong(
        ctx, dictSize(db_slot_infos[db].slotkey_tables[slot]));
    return REDISMODULE_OK;
}

/* *
 * slotsmgrttagone host port timeout key
 * */
int SlotsMGRTTagOne_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                                 int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc != 5 && argc != 6)
        return RedisModule_WrongArity(ctx);

    const char* host = RedisModule_StringPtrLen(argv[1], NULL);
    const char* port = RedisModule_StringPtrLen(argv[2], NULL);
    long long timeout = 0;
    if (RedisModule_StringToLongLong(argv[3], &timeout) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }

    const char* mgrtType;
    if (argc == 6) {
        mgrtType = RedisModule_StringPtrLen(argv[5], NULL);
    }

    int r = SlotsMGRT_TagKeys(ctx, host, port, timeout, argv[3], mgrtType);
    if (r == SLOTS_MGRT_ERR) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_MGRT);
        return REDISMODULE_ERR;
    }
    RedisModule_ReplyWithLongLong(ctx, r);
    return REDISMODULE_OK;
}

/* *
 * slotsmgrttagslot host port timeout slot
 * */
int SlotsMGRTTagSlot_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                                  int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc != 5)
        return RedisModule_WrongArity(ctx);

    const char* host = RedisModule_StringPtrLen(argv[1], NULL);
    const char* port = RedisModule_StringPtrLen(argv[2], NULL);
    long long timeout = 0;
    if (RedisModule_StringToLongLong(argv[3], &timeout) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }
    long long slot = 0;
    if (RedisModule_StringToLongLong(argv[4], &slot) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }
    const char* mgrtType;
    if (argc == 6) {
        mgrtType = RedisModule_StringPtrLen(argv[5], NULL);
    }

    int r
        = SlotsMGRT_TagSlotKeys(ctx, host, port, timeout, (int)slot, mgrtType);
    if (r == SLOTS_MGRT_ERR) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_MGRT);
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithLongLong(ctx, r);
    return REDISMODULE_OK;
}

/* *
 * slotsdel slot1 [slot2 ...]
 * */
int SlotsDel_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                          int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc < 2)
        return RedisModule_WrongArity(ctx);

    int slots[argc - 1];
    for (int i = 1; i < argc; i++) {
        long long slot = 0;
        if (RedisModule_StringToLongLong(argv[i], &slot) != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
            return REDISMODULE_ERR;
        }
        slots[i - 1] = (int)slot;
    }

    int db = RedisModule_GetSelectedDb(ctx);
    if (SlotsMGRT_DelSlotKeys(ctx, db, slots, argc - 1) == SLOTS_MGRT_ERR) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_DEL);
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithArray(ctx, argc - 1);
    for (int i = 0; i < argc - 1; i++) {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithLongLong(ctx, slots[i]);
        RedisModule_ReplyWithLongLong(
            ctx, dictSize(db_slot_infos[db].slotkey_tables[slots[i]]));
    }

    return REDISMODULE_OK;
}

/* *
 * slotsrestore key ttl val [key ttl val ...]
 * */
int SlotsRestore_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                              int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc < 4 || (argc - 1) % 3 != 0)
        return RedisModule_WrongArity(ctx);

    int n = (argc - 1) / 3;
    rdb_dump_obj* objs = RedisModule_Alloc(sizeof(rdb_dump_obj) * n);
    for (int i = 0; i < n; i++) {
        // del -> add -> ttlms (>0)
        objs[i].key = argv[i * 3 + 0];
        long long ttlms = 0;
        if (RedisModule_StringToLongLong(argv[i * 3 + 1], &ttlms)
            != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
            RedisModule_Free(objs);
            return REDISMODULE_ERR;
        }
        objs[i].ttlms = (time_t)ttlms;
        objs[i].val = argv[i * 3 + 2];
    }

    int ret = SlotsMGRT_Restore(ctx, &objs, n);
    if (ret == SLOTS_MGRT_ERR) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_MGRT);
        RedisModule_Free(objs);
        return REDISMODULE_ERR;
    }

    RedisModule_ReplyWithLongLong(ctx, ret);
    RedisModule_Free(objs);
    return REDISMODULE_OK;
}

/* *
 * slotsscan slotnum cursor [COUNT count]
 * */
int SlotsScan_RedisCommand(RedisModuleCtx* ctx, RedisModuleString** argv,
                           int argc) {
    /* Use automatic memory management. */
    RedisModule_AutoMemory(ctx);

    if (argc != 3 && argc != 5)
        return RedisModule_WrongArity(ctx);

    long long slot = 0;
    if (RedisModule_StringToLongLong(argv[1], &slot) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }

    long long cursor = 0;
    if (RedisModule_StringToLongLong(argv[1], &cursor) != REDISMODULE_OK) {
        RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
        return REDISMODULE_ERR;
    }

    long long count = 10;
    if (argc == 5) {
        const char* str = RedisModule_StringPtrLen(argv[3], NULL);
        if (strcasecmp(str, "count") != 0) {
            RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
            return REDISMODULE_ERR;
        }
        long long v = 0;
        if (RedisModule_StringToLongLong(argv[4], &v) != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
            return REDISMODULE_ERR;
        }
        if (v < 1) {
            RedisModule_ReplyWithError(ctx, REDISXSLOT_ERRORMSG_SYNTAX);
            return REDISMODULE_ERR;
        }
        count = v;
    }

    list* l = m_listCreate();
    SlotsMGRT_Scan(ctx, (int)slot, (unsigned long)count, (unsigned long)cursor,
                   l);
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, cursor);
    RedisModule_ReplyWithArray(ctx, listLength(l));
    do {
        m_listNode* head = listFirst(l);
        if (head == NULL) {
            break;
        }
        RedisModuleString* key = listNodeValue(head);
        RedisModule_ReplyWithString(ctx, key);
        m_listDelNode(l, head);
    } while (1);
    m_listRelease(l);

    return REDISMODULE_OK;
}

static int redisModule_SlotsInit(RedisModuleCtx* ctx, RedisModuleString** argv,
                                 int argc) {
    // config get databases
    RedisModule_AutoMemory(ctx);
    RedisModuleCallReply* reply
        = RedisModule_Call(ctx, "CONFIG", "cc", "GET", "databases");
    long long items = RedisModule_CallReplyLength(reply);
    if (items != 2)
        return REDISMODULE_ERR;
    long long databases;
    RedisModule_StringToLongLong(
        RedisModule_CreateStringFromCallReply(
            RedisModule_CallReplyArrayElement(reply, 1)),
        &databases);

    // databases
    long long hash_slots_size = DEFAULT_HASH_SLOTS_SIZE;
    if (argc >= 1
        && RedisModule_StringToLongLong(argv[0], &hash_slots_size)
               == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (hash_slots_size <= 0) {
        printf("[ERROR] ModuleLoaded hash slots size %lld <=0\n",
               hash_slots_size);
        return REDISMODULE_ERR;
    }
    if (hash_slots_size > MAX_HASH_SLOTS_SIZE) {
        printf("[ERROR] ModuleLoaded hash slots size %lld > max size %d\n",
               hash_slots_size, MAX_HASH_SLOTS_SIZE);
        return REDISMODULE_ERR;
    }

    // num_threads
    long long num_threads = 0;
    if (argc >= 2
        && RedisModule_StringToLongLong(argv[0], &num_threads)
               == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (num_threads < 0) {
        printf("[ERROR] ModuleLoaded threads num %lld <0\n", num_threads);
        return REDISMODULE_ERR;
    }
    if (num_threads > MAX_NUM_THREADS) {
        printf("[ERROR] ModuleLoaded threads num %lld > max num %d\n",
               num_threads, MAX_NUM_THREADS);
        return REDISMODULE_ERR;
    }

    slots_init(NULL, hash_slots_size, databases, num_threads);
    return REDISMODULE_OK;
}

/*-------------------------------- event handler  --------------------------*/
int htNeedsResize(dict* dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size > DICT_HT_INITIAL_SIZE
            && (used * 100 / size < HASHTABLE_MIN_FILL));
}
/* If the percentage of used slots in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory */
void tryResizeDbSlotHashTables(int dbid, int slot) {
    if (htNeedsResize(db_slot_infos[dbid].slotkey_tables[slot]))
        m_dictResize(db_slot_infos[dbid].slotkey_tables[slot]);
}
/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns 1 if some rehashing was performed, otherwise 0
 * is returned. */
int incrementallyDbSlotRehash(int dbid, int slot) {
    /* Keys dictionary */
    if (dictIsRehashing(db_slot_infos[dbid].slotkey_tables[slot])) {
        m_dictRehashMilliseconds(db_slot_infos[dbid].slotkey_tables[slot], 1);
        return 1; /* already used our millisecond for this loop... */
    }
    return 0;
}

void dbSlotCron(void) {
    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    // todo  need check have child pid (disk io)
    // pr: https://github.com/redis/redis/pull/12482

    /* We use global counters so if we stop the computation at a given
     * DB we'll be able to start from the successive in the next
     * cron loop iteration. */
    static unsigned int resize_db = 0;
    static unsigned int rehash_db = 0;

    // ReSize
    for (int db = 0; db < g_slots_meta_info.databases; db++) {
        for (int slot = 0; slot < (int)g_slots_meta_info.hash_slots_size;
             slot++) {
            tryResizeDbSlotHashTables(resize_db % g_slots_meta_info.databases,
                                      slot);
            resize_db++;
        }
    }  // end for

    // ReHash
    if (!g_slots_meta_info.activerehashing) {
        return;
    }
    for (int db = 0; db < g_slots_meta_info.databases; db++) {
        for (int slot = 0; slot < (int)g_slots_meta_info.hash_slots_size;
             slot++) {
            int work_done = incrementallyDbSlotRehash(db, slot);
            if (work_done) {
                /* If the function did some work, stop here, we'll do
                 * more at the next cron loop. */
                return;
            } else {
                /* If this db didn't need rehash, we'll try the next one. */
                rehash_db++;
                rehash_db %= g_slots_meta_info.databases;
            }
        }
    }  // end for
}

// serverCron --> databasesCron --> resize,rehash
// moduleFireServerEvent REDISMODULE_EVENT_CRON_LOOP
// sub REDISMODULE_EVENT_CRON_LOOP do resize,rehash db slot->keys dict
// like tryResizeHashTables
// like incrementallyRehash
void CronLoopCallback(RedisModuleCtx* ctx, RedisModuleEvent e, uint64_t sub,
                      void* data) {
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(sub);
    RedisModule_AutoMemory(ctx);

    RedisModuleCronLoop* ei = data;
    RedisModule_Log(ctx, "debug", "CronLoopCallback hz %d sub %lu", ei->hz,
                    sub);
    dbSlotCron();
    run_with_period(1000, ei->hz) {
        SlotsMGRT_CloseTimedoutConns(ctx);
    }

    g_slots_meta_info.cronloops++;
}

// when FLUSHALL, FLUSHDB or an internal flush happen;
// emptyData --> Fire the flushdb modules event with sub event (start,end)
// moduleFireServerEvent REDISMODULE_EVENT_FLUSHDB
// like emptyDbStructure
// emptyDbAsync to async emtpySlot with threadpool
void FlushdbCallback(RedisModuleCtx* ctx, RedisModuleEvent e, uint64_t sub,
                     void* data) {
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(sub);
    RedisModule_AutoMemory(ctx);

    RedisModuleFlushInfo* fi = data;
    if (sub == REDISMODULE_SUBEVENT_FLUSHDB_START) {
        if (fi->dbnum != -1) {
            int db = (int)fi->dbnum;
            for (int slot = 0; slot < (int)g_slots_meta_info.hash_slots_size;
                 slot++) {
                if (dictSize(db_slot_infos[db].slotkey_tables[slot]) == 0) {
                    continue;
                }
                m_dictEmpty(db_slot_infos[db].slotkey_tables[slot], NULL);
            }
            if (db_slot_infos[db].tagged_key_list->length != 0) {
                m_zslFree(db_slot_infos[db].tagged_key_list);
                db_slot_infos[db].tagged_key_list = m_zslCreate();
            }
        } else {
            for (int db = 0; db < g_slots_meta_info.databases; db++) {
                for (int slot = 0;
                     slot < (int)g_slots_meta_info.hash_slots_size; slot++) {
                    if (dictSize(db_slot_infos[db].slotkey_tables[slot]) == 0) {
                        continue;
                    }
                    m_dictEmpty(db_slot_infos[db].slotkey_tables[slot], NULL);
                }
                if (db_slot_infos[db].tagged_key_list->length != 0) {
                    m_zslFree(db_slot_infos[db].tagged_key_list);
                    db_slot_infos[db].tagged_key_list = m_zslCreate();
                }
            }  // end for
        }      // end if
    }          // end if
    // if (sub == REDISMODULE_SUBEVENT_FLUSHDB_END) {
    // }
}

// showtdown cmd -> prepareForShutdown -> finishShutdown
// -> Fire the shutdown modules event REDISMODULE_EVENT_SHUTDOWN
void ShutdownCallback(RedisModuleCtx* ctx, RedisModuleEvent e, uint64_t sub,
                      void* data) {
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(data);
    REDISMODULE_NOT_USED(sub);

    RedisModule_Log(ctx, "warning", "ShutdownCallback module-event-%s",
                    "shutdown");

    for (int db = 0; db < g_slots_meta_info.databases; db++) {
        for (int slot = 0; slot < (int)g_slots_meta_info.hash_slots_size;
             slot++) {
            m_dictRelease(db_slot_infos[db].slotkey_tables[slot]);
        }
        RedisModule_Free(db_slot_infos[db].slotkey_tables);
        m_zslFree(db_slot_infos[db].tagged_key_list);
    }
}

/*------------------------------ notify handler --------------------------*/
int NotifyTypeChangeCallback(RedisModuleCtx* ctx, int type, const char* event,
                             RedisModuleString* key) {
    // don't auto freee key
    // RedisModule_AutoMemory(ctx);
    int db = RedisModule_GetSelectedDb(ctx);
    RedisModule_Log(ctx, "debug",
                    "NotifyTypeChangeCallback db %d event type %d, "
                    "event %s, key %s",
                    db, type, event, RedisModule_StringPtrLen(key, NULL));
    Slots_Add(ctx, db, key);
    return REDISMODULE_OK;
}

int NotifyGenericCallback(RedisModuleCtx* ctx, int type, const char* event,
                          RedisModuleString* key) {
    RedisModule_AutoMemory(ctx);
    int dbid = RedisModule_GetSelectedDb(ctx);
    RedisModule_Log(
        ctx, "debug",
        "NotifyGenericCallback db %d event type %d, event %s, key %s", dbid,
        type, event, RedisModule_StringPtrLen(key, NULL));

    if (strcmp(event, "del") == 0) {
        Slots_Del(ctx, dbid, key);
        return REDISMODULE_OK;
    }

    // save event stat for pair events
    // notice: just for single thread process cmd,
    // (rename_from->rename_to), (move_from->move_to)
    static RedisModuleString *from_key = NULL, *to_key = NULL;
    static int from_dbid, to_dbid;
    static int cmd_flag = CMD_NONE;
    if (strcmp(event, "rename_from") == 0) {
        from_key = RedisModule_CreateStringFromString(NULL, key);
    } else if (strcmp(event, "rename_to") == 0) {
        to_key = RedisModule_CreateStringFromString(NULL, key);
        cmd_flag = CMD_RENAME;
    } else if (strcmp(event, "move_from") == 0) {
        from_dbid = dbid;
    } else if (strcmp(event, "move_to") == 0) {
        to_dbid = dbid;
        cmd_flag = CMD_MOVE;
    }

    if (cmd_flag == CMD_NONE) {
        return REDISMODULE_OK;
    }

    RedisModuleString *local_from_key = NULL, *local_to_key = NULL;
    int local_from_dbid, local_to_dbid;
    /* We assign values in advance so that `move` and `rename` can be
     * processed uniformly. */
    if (cmd_flag == CMD_RENAME) {
        local_from_key = from_key;
        local_to_key = to_key;
        /* `rename` does not change the dbid of the key. */
        local_from_dbid = dbid;
        local_to_dbid = dbid;
    } else {
        /* `move` does not change the name of the key. */
        local_from_key = key;
        local_to_key = key;
        local_from_dbid = from_dbid;
        local_to_dbid = to_dbid;
    }

    Slots_Del(ctx, local_from_dbid, local_from_key);
    Slots_Add(ctx, local_to_dbid, local_to_key);

    /* Release sources. */
    if (cmd_flag == CMD_RENAME) {
        if (to_key) {
            RedisModule_FreeString(NULL, to_key);
            to_key = NULL;
        }

        if (from_key) {
            RedisModule_FreeString(NULL, from_key);
            from_key = NULL;
        }
    }

    /* Reset flag. */
    cmd_flag = CMD_NONE;

    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in
 * order to register the commands into the Redis server.
 *  __attribute__((visibility("default"))) for the same func name with redis
 * or other Dynamic Shared Lib *.so,  more detail man gcc or see
 * https://gcc.gnu.org/wiki/Visibility
 */
int __attribute__((visibility("default")))
RedisModule_OnLoad(RedisModuleCtx* ctx, RedisModuleString** argv, int argc) {
    if (RedisModule_Init(ctx, "redisxslot", REDISXSLOT_APIVER_1,
                         REDISMODULE_APIVER_1)
        == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    // check
    if (redisModuleCompatibilityCheckV5() != REDISMODULE_OK) {
        printf("Redis 5.0 or above is required! \n");
        return REDISMODULE_ERR;
    }

    // Log the list of parameters passing loading the module.
    for (int j = 0; j < argc; j++) {
        const char* s = RedisModule_StringPtrLen(argv[j], NULL);
        printf("ModuleLoaded with argv[%d] = %s\n", j, s);
    }

    // init
    if (redisModule_SlotsInit(ctx, argv, argc) != REDISMODULE_OK) {
        printf("redisModule_SlotsInit fail! \n");
        return REDISMODULE_ERR;
    }

    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_CronLoop,
                                       CronLoopCallback);
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_FlushDB,
                                       FlushdbCallback);
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Shutdown,
                                       ShutdownCallback);

    RedisModule_SubscribeToKeyspaceEvents(
        ctx,
        REDISMODULE_NOTIFY_HASH | REDISMODULE_NOTIFY_SET
            | REDISMODULE_NOTIFY_STRING | REDISMODULE_NOTIFY_LIST
            | REDISMODULE_NOTIFY_ZSET,
        NotifyTypeChangeCallback);

    RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_GENERIC,
                                          NotifyGenericCallback);

    CREATE_ROMCMD("slotshashkey", SlotsHashKey_RedisCommand, 0, 0, 0);
    CREATE_ROMCMD("slotsinfo", SlotsInfo_RedisCommand, 0, 0, 0);
    CREATE_ROMCMD("slotsscan", SlotsScan_RedisCommand, 0, 0, 0);

    CREATE_WRMCMD("slotsmgrtone", SlotsMGRTOne_RedisCommand, 0, 0, 0);
    CREATE_WRMCMD("slotsmgrtslot", SlotsMGRTSlot_RedisCommand, 0, 0, 0);
    CREATE_WRMCMD("slotsmgrttagone", SlotsMGRTTagOne_RedisCommand, 0, 0, 0);
    CREATE_WRMCMD("slotsmgrttagslot", SlotsMGRTTagSlot_RedisCommand, 0, 0, 0);
    CREATE_WRMCMD("slotsrestore", SlotsRestore_RedisCommand, 0, 0, 0);
    CREATE_WRMCMD("slotsdel", SlotsDel_RedisCommand, 0, 0, 0);
    CREATE_WRMCMD("slotstest", SlotsTest_RedisCommand, 0, 0, 0);

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx* ctx) {
    UNUSED(ctx);
    slots_free(NULL);
    return REDISMODULE_OK;
}
