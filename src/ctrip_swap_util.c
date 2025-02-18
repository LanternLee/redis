#include "ctrip_swap.h"

/* Copyright (c) 2021, ctrip.com
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

#include "endianconv.h"

typedef unsigned int keylen_t;

int rocksGetObjectType(unsigned char enc_type) {
    switch (enc_type) {
    case ENC_TYPE_STRING: return OBJ_STRING;
    case ENC_TYPE_LIST: return OBJ_LIST;
    case ENC_TYPE_SET: return OBJ_SET;
    case ENC_TYPE_SET_SUB: return OBJ_SET;
    case ENC_TYPE_ZSET: return OBJ_ZSET;
    case ENC_TYPE_ZSET_SUB: return OBJ_ZSET;
    case ENC_TYPE_HASH: return OBJ_HASH;
    case ENC_TYPE_HASH_SUB: return OBJ_HASH;
    case ENC_TYPE_MODULE: return OBJ_MODULE;
    case ENC_TYPE_STREAM: return OBJ_STREAM;
    default: return -1;
    }
}

static inline char rocksGetKeyEncType(int obj_type) {
    switch (obj_type) {
    case OBJ_STRING : return ENC_TYPE_STRING  ;
    case OBJ_LIST   : return ENC_TYPE_LIST    ;
    case OBJ_SET    : return ENC_TYPE_SET     ;
    case OBJ_ZSET   : return ENC_TYPE_ZSET    ;
    case OBJ_HASH   : return ENC_TYPE_HASH    ;
    case OBJ_MODULE : return ENC_TYPE_MODULE  ;
    case OBJ_STREAM : return ENC_TYPE_STREAM  ;
    default         : return ENC_TYPE_UNKNOWN ;
    }
}

static inline char rocksGetSubkeyEncType(int obj_type) {
    switch (obj_type) {
    case OBJ_LIST   : return ENC_TYPE_LIST_SUB  ; 
    case OBJ_SET    : return ENC_TYPE_SET_SUB   ; 
    case OBJ_ZSET   : return ENC_TYPE_ZSET_SUB  ; 
    case OBJ_HASH   : return ENC_TYPE_HASH_SUB  ; 
    case OBJ_MODULE : return ENC_TYPE_MODULE_SUB; 
    case OBJ_STREAM : return ENC_TYPE_STREAM_SUB; 
    default         : return ENC_TYPE_UNKNOWN_SUB;
    }
}

unsigned char rocksGetEncType(int obj_type, int big) {
    return big ? rocksGetSubkeyEncType(obj_type) : rocksGetKeyEncType(obj_type);
}

unsigned char rocksGetObjectEncType(robj *o) {
    return rocksGetEncType(o->type,o->big);
}

sds rocksEncodeKey(unsigned char enc_type, sds key) {
    sds rawkey = sdsnewlen(SDS_NOINIT,1+sdslen(key));
    rawkey[0] = enc_type;
    memcpy(rawkey+1, key, sdslen(key));
    return rawkey;
}

sds rocksEncodeSubkey(unsigned char enc_type, sds key, sds subkey) {
    size_t subkeylen = subkey ? sdslen(subkey) : 0;
    size_t rawkeylen = 1+sizeof(keylen_t)+sdslen(key)+subkeylen;
    sds rawkey = sdsnewlen(SDS_NOINIT,rawkeylen);
            
    char *ptr = rawkey;
    keylen_t keylen = (keylen_t)sdslen(key);
    ptr[0] = enc_type, ptr++;
    memcpy(ptr, &keylen, sizeof(keylen_t)), ptr += sizeof(keylen_t);
    memcpy(ptr, key, sdslen(key)), ptr += sdslen(key);
    if (subkey) {
        memcpy(ptr, subkey, sdslen(subkey)), ptr += sdslen(subkey);
    }
    return rawkey;
}

sds rocksCalculateNextKey(sds current) {
    sds next = NULL;
    size_t nextlen = sdslen(current);

    do {
        if (current[nextlen - 1] != (char)0xff) break;
        nextlen--;
    } while(nextlen > 0);

    if (0 == nextlen) return NULL;

    next = sdsnewlen(current, nextlen);
    next[nextlen - 1]++;

    return next;
}

sds rocksEncodeValRdb(robj *value) {
    rio sdsrdb;
    rioInitWithBuffer(&sdsrdb,sdsempty());
    rdbSaveObjectType(&sdsrdb,value) ;
    rdbSaveObject(&sdsrdb,value,NULL);
    return sdsrdb.io.buffer.ptr;
}

robj *rocksDecodeValRdb(sds raw) {
    robj *value;
    rio sdsrdb;
    int rdbtype;
    rioInitWithBuffer(&sdsrdb,raw);
    rdbtype = rdbLoadObjectType(&sdsrdb);
    value = rdbLoadObject(rdbtype,&sdsrdb,NULL,NULL);
    return value;
}

int rocksDecodeKey(const char *raw, size_t rawlen, const char **key,
        size_t *klen) {
    int obj_type;
    if (rawlen < 2) return -1;
    if ((obj_type = rocksGetObjectType(raw[0])) < 0) return -1;
    raw++, rawlen--;
    if (key) *key = raw;
    if (klen) *klen = rawlen;
    return obj_type;
}

int rocksDecodeSubkey(const char *raw, size_t rawlen,
                      const char **key, size_t *klen, const char **sub, size_t *slen) {
    int obj_type;
    keylen_t _klen;
    if (rawlen <= 1+sizeof(keylen_t)) return -1;

    if ((obj_type = rocksGetObjectType(raw[0])) < 0) return -1;
    raw++, rawlen--;

    _klen = *(keylen_t*)raw;
    if (klen) *klen = (size_t)_klen;
    raw += sizeof(keylen_t);
    rawlen -= sizeof(keylen_t);

    if (key) *key = raw;
    raw += _klen;
    rawlen-= _klen;

    if (rawlen < 0) return -1;
    if (obj_type == OBJ_SET || obj_type == OBJ_ZSET) {
        if (rawlen == 0) {
            raw = NULL;
        }
    } else if (rawlen == 0) {
        return -1;
    }
    if (sub) *sub = raw;
    if (slen) *slen = rawlen;
    return obj_type;
}

sds objectDump(robj *o) {
    sds repr = sdsempty();

    repr = sdscatprintf(repr,"type:%s, ", getObjectTypeName(o));
    switch (o->encoding) {
    case OBJ_ENCODING_INT:
        repr = sdscatprintf(repr, "encoding:int, value:%ld", (long)o->ptr);
        break;
    case OBJ_ENCODING_EMBSTR:
        repr = sdscatprintf(repr, "encoding:emedstr, value:%.*s", (int)sdslen(o->ptr), (sds)o->ptr);
        break;
    case OBJ_ENCODING_RAW:
        repr = sdscatprintf(repr, "encoding:raw, value:%.*s", (int)sdslen(o->ptr), (sds)o->ptr);
        break;
    default:
        repr = sdscatprintf(repr, "encoding:%d, value:nan", o->encoding);
        break;
    }
    return repr;
}

/* For big Hash/Set/Zset object, object might changed by swap thread in
 * createOrMergeObject, so iterating those big objects in main thread without
 * requestGetIOAndLock is not safe. intead we just estimate those object size. */
size_t objectComputeSize(robj *o, size_t sample_size);
size_t objectEstimateSize(robj *o) {
    size_t asize = 0;
    if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        dict *d = o->ptr;
        asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
        asize += DEFAULT_HASH_FIELD_SIZE*dictSize(d);
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        dict *d = o->ptr;
        asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
        asize += DEFAULT_SET_FIELD_SIZE*dictSize(d);
    } else {
        asize = objectComputeSize(o,5);
    }
    return asize;
}
size_t keyEstimateSize(redisDb *db, robj *key) {
    robj *val = lookupKey(db, key, LOOKUP_NOTOUCH);
    return val ? objectEstimateSize(val): 0;
}

/* Create an unshared object from src, note that o.refcount decreased. */
robj *unshareStringValue(robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        decrRefCount(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        return o;
    } else {
        return o;
    }
}

const char *strObjectType(int type) {
    switch (type) {
    case OBJ_STRING: return "string";
    case OBJ_HASH: return "hash";
    case OBJ_LIST: return "list";
    case OBJ_SET: return "set";
    case OBJ_ZSET: return "zset";
    case OBJ_STREAM: return "stream";
    default: return "unknown";
    }
}

#ifdef REDIS_TEST

int testRocksCalculateNextKey(int argc, char **argv, int accurate) {
    sds current = NULL, next = NULL;
    int error = 0;

    TEST("claculate-next-key: empty string") {
        current = next = NULL;
        current = sdsempty();
        next = rocksCalculateNextKey(current);
        test_assert(NULL == next);
        sdsfree(current);
        sdsfree(next);
    }

    TEST("claculate-next-key: string full with 0xff") {
        current = next = NULL;
        char str[10] = {0};
        memset(str, (char)0xff, 9);
        current = sdsnew(str);
        next = rocksCalculateNextKey(current);
        test_assert(NULL == next);
        sdsfree(current);
        sdsfree(next);
    }

    TEST("claculate-next-key: end with 0xff") {
        current = next = NULL;
        char str[] = {'t', 'e', 's', 't', (char)0xff, (char)0xff, 0};
        current = sdsnew(str);
        next = rocksCalculateNextKey(current);
        test_assert(NULL != next);
        test_assert(4 == sdslen(next));
        test_assert(0 == memcmp("tesu", next, 4));
        sdsfree(current);
        sdsfree(next);
    }

    TEST("claculate-next-key: normal string") {
        current = next = NULL;
        current = sdsnew("normal string");
        next = rocksCalculateNextKey(current);
        test_assert(NULL != next);
        test_assert(13 == sdslen(next));
        test_assert(0 == memcmp("normal strinh", next, 13));
        sdsfree(current);
        sdsfree(next);
    }

    return error;
}

#endif
