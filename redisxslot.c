#include "redisxslot.h"

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

void slots_init(uint32_t hash_slots_size, int databases) {
    crc32_init();

    g_slots_meta_info.hash_slots_size = hash_slots_size;
    g_slots_meta_info.databases = databases;

    arr_db_slot_info = RedisModule_Alloc(sizeof(db_slot_info) * databases);

    for (int j = 0; j < databases; j++) {
        arr_db_slot_info[j].slotkey_tables
            = RedisModule_Alloc(sizeof(dict) * hash_slots_size);
        for (int i = 0; i < hash_slots_size; i++) {
            arr_db_slot_info[j].slotkey_tables[i]
                = m_dictCreate(&hashSlotDictType, NULL);
        }
        arr_db_slot_info[j].slotkey_table_rehashing = 0;
        arr_db_slot_info[j].tagged_key_list = m_zslCreate();
    }
}

void slots_free() {
    for (int j = 0; j < g_slots_meta_info.databases; j++) {
        if (arr_db_slot_info != NULL
            && arr_db_slot_info[j].slotkey_tables != NULL) {
            RedisModule_Free(arr_db_slot_info[j].slotkey_tables);
        }
        if (arr_db_slot_info != NULL
            && arr_db_slot_info[j].tagged_key_list != NULL) {
            m_zslFree(arr_db_slot_info[j].tagged_key_list);
        }
    }
    if (arr_db_slot_info != NULL) {
        RedisModule_Free(arr_db_slot_info);
    }
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
