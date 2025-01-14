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

#include "ctrip_swap.h"
#include <dirent.h>
#include <sys/stat.h>

#define RIO_SCAN_NUMKEYS_ALLOC_INIT 16
#define RIO_SCAN_NUMKEYS_ALLOC_LINER 4096

/* --- RIO --- */
void RIOInitGet(RIO *rio, sds rawkey) {
    rio->action = ROCKS_GET;
    rio->get.rawkey = rawkey;
    rio->err = NULL;
}

void RIOInitPut(RIO *rio, sds rawkey, sds rawval) {
    rio->action = ROCKS_PUT;
    rio->put.rawkey = rawkey;
    rio->put.rawval = rawval;
    rio->err = NULL;
}

void RIOInitDel(RIO *rio, sds rawkey) {
    rio->action = ROCKS_DEL;
    rio->del.rawkey = rawkey;
    rio->err = NULL;
}

void RIOInitWrite(RIO *rio, rocksdb_writebatch_t *wb) {
    rio->action = ROCKS_WRITE;
    rio->write.wb = wb;
    rio->err = NULL;
}

void RIOInitMultiGet(RIO *rio, int numkeys, sds *rawkeys) {
    rio->action = ROCKS_MULTIGET;
    rio->multiget.numkeys = numkeys;
    rio->multiget.rawkeys = rawkeys;
    rio->multiget.rawvals = NULL;
    rio->err = NULL;
}

void RIOInitScan(RIO *rio, sds prefix) {
    rio->action = ROCKS_SCAN;
    rio->scan.prefix = prefix;
    rio->err = NULL;
}

void RIOInitDeleteRange(RIO *rio, sds start_key, sds end_key) {
    rio->action = ROCKS_DELETERANGE;
    rio->delete_range.start_key = start_key;
    rio->delete_range.end_key = end_key;
    rio->err = NULL;
}

void RIODeinit(RIO *rio) {
    int i;

    switch (rio->action) {
    case  ROCKS_GET:
        sdsfree(rio->get.rawkey);
        rio->get.rawkey = NULL;
        sdsfree(rio->get.rawval);
        rio->get.rawval = NULL;
        break;
    case  ROCKS_PUT:
        sdsfree(rio->put.rawkey);
        rio->put.rawkey = NULL;
        sdsfree(rio->put.rawval);
        rio->put.rawval = NULL;
        break;
    case  ROCKS_DEL:
        sdsfree(rio->del.rawkey);
        rio->del.rawkey = NULL;
        break;
    case  ROCKS_MULTIGET:
        for (i = 0; i < rio->multiget.numkeys; i++) {
            if (rio->multiget.rawkeys) sdsfree(rio->multiget.rawkeys[i]);
            if (rio->multiget.rawvals) sdsfree(rio->multiget.rawvals[i]);
        }
        zfree(rio->multiget.rawkeys);
        rio->multiget.rawkeys = NULL;
        zfree(rio->multiget.rawvals);
        rio->multiget.rawvals = NULL;
        break;
    case  ROCKS_SCAN:
        sdsfree(rio->scan.prefix);
        for (i = 0; i < rio->scan.numkeys; i++) {
            if (rio->scan.rawkeys) sdsfree(rio->scan.rawkeys[i]);
            if (rio->scan.rawvals) sdsfree(rio->scan.rawvals[i]);
        }
        zfree(rio->scan.rawkeys);
        rio->scan.rawkeys = NULL;
        zfree(rio->scan.rawvals);
        rio->scan.rawvals = NULL;
        break;
    case  ROCKS_WRITE:
        rocksdb_writebatch_destroy(rio->write.wb);
        rio->write.wb = NULL;
        break;
    case  ROCKS_DELETERANGE:
        sdsfree(rio->delete_range.start_key);
        rio->delete_range.start_key = NULL;
        sdsfree(rio->delete_range.end_key);
        rio->delete_range.end_key = NULL;
        break;
    default:
        break;
    }
}

static int doRIOGet(RIO *rio) {
    size_t vallen;
    char *err = NULL, *val;

    val = rocksdb_get(server.rocks->db, server.rocks->ropts,
            rio->get.rawkey, sdslen(rio->get.rawkey), &vallen, &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb get failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    if (val == NULL) {
        rio->get.rawval = NULL;
    } else  {
        rio->get.rawval = sdsnewlen(val, vallen);
        zlibc_free(val);
    }

    return 0;
}

static int doRIOPut(RIO *rio) {
    char *err = NULL;
    rocksdb_put(server.rocks->db, server.rocks->wopts,
            rio->put.rawkey, sdslen(rio->put.rawkey),
            rio->put.rawval, sdslen(rio->put.rawval), &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb write failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    return 0;
}

static int doRIODel(RIO *rio) {
    char *err = NULL;
    rocksdb_delete(server.rocks->db, server.rocks->wopts,
            rio->del.rawkey, sdslen(rio->del.rawkey), &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    return 0;
}

static int doRIOWrite(RIO *rio) {
    char *err = NULL;
    rocksdb_write(server.rocks->db, server.rocks->wopts,
            rio->write.wb, &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb write failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    return 0;
}

static int doRIOMultiGet(RIO *rio) {
    int ret = 0, i;
    char **keys_list = zmalloc(rio->multiget.numkeys*sizeof(char*));
    char **values_list = zmalloc(rio->multiget.numkeys*sizeof(char*));
    size_t *keys_list_sizes = zmalloc(rio->multiget.numkeys*sizeof(size_t));
    size_t *values_list_sizes = zmalloc(rio->multiget.numkeys*sizeof(size_t));
    char **errs = zmalloc(rio->multiget.numkeys*sizeof(char*));

    for (i = 0; i < rio->multiget.numkeys; i++) {
        keys_list[i] = rio->multiget.rawkeys[i];
        keys_list_sizes[i] = sdslen(rio->multiget.rawkeys[i]);
    }

    rocksdb_multi_get(server.rocks->db, server.rocks->ropts,
            rio->multiget.numkeys,
            (const char**)keys_list, (const size_t*)keys_list_sizes,
            values_list, values_list_sizes, errs);

    rio->multiget.rawvals = zmalloc(rio->multiget.numkeys*sizeof(sds));
    for (i = 0; i < rio->multiget.numkeys; i++) {
        if (values_list[i] == NULL) {
            rio->multiget.rawvals[i] = NULL;
        } else {
            rio->multiget.rawvals[i] = sdsnewlen(values_list[i],
                    values_list_sizes[i]);
            zlibc_free(values_list[i]);
        }
        if (errs[i]) {
            if (rio->err == NULL) {
                rio->err = sdsnew(errs[i]);
                serverLog(LL_WARNING,"[rocks] do rocksdb multiget failed: %s",
                        rio->err);
            }
            zlibc_free(errs[i]);
        }
    }

    if (rio->err != NULL) {
        ret = -1;
        goto end;
    }

end:
    zfree(keys_list);
    zfree(values_list);
    zfree(keys_list_sizes);
    zfree(values_list_sizes);
    zfree(errs);
    return ret;
}

static int doRIOScan(RIO *rio) {
    int ret = 0;
    char *err = NULL;
    rocksdb_iterator_t *iter = NULL;
    sds prefix = rio->scan.prefix;
    size_t numkeys = 0, numalloc = 8;
    sds *rawkeys = zmalloc(numalloc*sizeof(sds));
    sds *rawvals = zmalloc(numalloc*sizeof(sds));

    iter = rocksdb_create_iterator(server.rocks->db,server.rocks->ropts);
    rocksdb_iter_seek(iter,prefix,sdslen(prefix));

    while (rocksdb_iter_valid(iter)) {
        size_t klen, vlen;
        const char *rawkey, *rawval;
        rawkey = rocksdb_iter_key(iter, &klen);

        if (klen < sdslen(prefix) || memcmp(rawkey, prefix, sdslen(prefix)))
            break;

        numkeys++;
        /* make room for key/val */
        if (numkeys >= numalloc) {
            if (numalloc >= RIO_SCAN_NUMKEYS_ALLOC_LINER) {
                numalloc += RIO_SCAN_NUMKEYS_ALLOC_LINER;
            } else {
                numalloc *= 2;
            }
            rawkeys = zrealloc(rawkeys, numalloc*sizeof(sds));
            rawvals = zrealloc(rawvals, numalloc*sizeof(sds));
        }

        rawval = rocksdb_iter_value(iter, &vlen);
        rawkeys[numkeys-1] = sdsnewlen(rawkey, klen);
        rawvals[numkeys-1] = sdsnewlen(rawval, vlen);

        rocksdb_iter_next(iter);
    }

    rocksdb_iter_get_error(iter, &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb scan failed: %s", err);
        ret = -1;
    }
    
    rio->scan.numkeys = numkeys;
    rio->scan.rawkeys = rawkeys;
    rio->scan.rawvals = rawvals;
    rocksdb_iter_destroy(iter);

    return ret;
}

static int doRIODeleteRange(RIO *rio) {
    char *err = NULL;
    rocksdb_delete_range_cf(server.rocks->db, server.rocks->wopts,
            server.rocks->default_cf,
            rio->delete_range.start_key, sdslen(rio->delete_range.start_key),
            rio->delete_range.end_key, sdslen(rio->delete_range.end_key), &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb delete range failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    return 0;
}

void dumpRIO(RIO *rio) {
    sds repr = sdsnew("[ROCKS] ");
    switch (rio->action) {
    case ROCKS_GET:
        repr = sdscat(repr, "GET rawkey=");
        repr = sdscatrepr(repr, rio->get.rawkey, sdslen(rio->get.rawkey));
        repr = sdscat(repr, ", rawval=");
        if (rio->get.rawval) {
            repr = sdscatrepr(repr, rio->get.rawval, sdslen(rio->get.rawval));
        } else {
            repr = sdscatfmt(repr, "<nil>");
        }
        break;
    case ROCKS_PUT:
        repr = sdscat(repr, "PUT rawkey=");
        repr = sdscatrepr(repr, rio->put.rawkey, sdslen(rio->put.rawkey));
        repr = sdscat(repr, ", rawval=");
        repr = sdscatrepr(repr, rio->put.rawval, sdslen(rio->put.rawval));
        break;
    case ROCKS_DEL:
        repr = sdscat(repr, "DEL ");
        repr = sdscatrepr(repr, rio->del.rawkey, sdslen(rio->del.rawkey));
        break;
    case ROCKS_WRITE:
        repr = sdscat(repr, "WRITE ");
        break;
    case ROCKS_MULTIGET:
        repr = sdscat(repr, "MULTIGET:\n");
        for (int i = 0; i < rio->multiget.numkeys; i++) {
            repr = sdscat(repr, "  (");
            repr = sdscatrepr(repr, rio->multiget.rawkeys[i],
                    sdslen(rio->multiget.rawkeys[i]));
            repr = sdscat(repr, ")=>(");
            if (rio->multiget.rawvals[i]) {
                repr = sdscatrepr(repr, rio->multiget.rawvals[i],
                        sdslen(rio->multiget.rawvals[i]));
            } else {
                repr = sdscatfmt(repr, "<nil>");
            }
            repr = sdscat(repr,")\n");
        }
        break;
    case ROCKS_SCAN:
        repr = sdscat(repr, "SCAN:(");
        repr = sdscatrepr(repr, rio->scan.prefix, sdslen(rio->scan.prefix));
        repr = sdscat(repr, ")\n");
        for (int i = 0; i < rio->scan.numkeys; i++) {
            repr = sdscat(repr, "  (");
            repr = sdscatrepr(repr, rio->scan.rawkeys[i],
                    sdslen(rio->scan.rawkeys[i]));
            repr = sdscat(repr, ")=>(");
            repr = sdscatrepr(repr, rio->scan.rawvals[i],
                    sdslen(rio->scan.rawvals[i]));
            repr = sdscat(repr,")\n");
        }
        break;
    case ROCKS_DELETERANGE:
        repr = sdscat(repr, "DELETERANGE start_key=%s");
        repr = sdscatrepr(repr, rio->delete_range.start_key, sdslen(rio->delete_range.start_key));
        repr = sdscat(repr, ", end_key=");
        repr = sdscatrepr(repr, rio->delete_range.end_key, sdslen(rio->delete_range.end_key));
        break;
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        break;
    }
    serverLog(LL_NOTICE, "%s", repr);
    sdsfree(repr);
}

int doRIO(RIO *rio) {
    int ret;
    if (server.debug_rio_latency) usleep(server.debug_rio_latency*1000);
    if (server.debug_rio_error > 0) {
        server.debug_rio_error--;
        return SWAP_ERR_EXEC_RIO_FAIL;
    }

    switch (rio->action) {
    case ROCKS_GET:
        ret = doRIOGet(rio);
        break;
    case ROCKS_PUT:
        ret = doRIOPut(rio);
        break;
    case ROCKS_DEL:
        ret = doRIODel(rio);
        break;
    case ROCKS_WRITE:
        ret = doRIOWrite(rio);
        break;
    case ROCKS_MULTIGET:
        ret = doRIOMultiGet(rio);
        break;
    case ROCKS_SCAN:
        ret = doRIOScan(rio);
        break;
    case ROCKS_DELETERANGE:
        ret = doRIODeleteRange(rio);
        break;
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        return -1;
    }

#ifdef ROCKS_DEBUG
    dumpRIO(rio);
#endif

    return ret ? SWAP_ERR_EXEC_RIO_FAIL : 0;
}

static void doNotify(swapRequest *req, int errcode) {
    req->errcode = errcode;
    req->notify_cb(req, req->notify_pd);
}

static void executeSwapDelRequest(swapRequest *req) {
    int i, numkeys, errcode = 0, action;
    sds *rawkeys = NULL;
    RIO _rio = {0}, *rio = &_rio;
    rocksdb_writebatch_t *wb;
    swapData *data = req->data;

    if ((errcode = swapDataEncodeKeys(data,req->intention,req->datactx,
                &action,&numkeys,&rawkeys))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"execswap-del-encodekeys",
            "action=%s, numkeys=%d", rocksActionName(action), numkeys);

    if (numkeys == 0) goto end;

    if (action == ROCKS_WRITE) {
        wb = rocksdb_writebatch_create();
        for (i = 0; i < numkeys; i++) {
            rocksdb_writebatch_delete(wb, rawkeys[i], sdslen(rawkeys[i]));
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-write","numkeys=%d.",numkeys);
        RIOInitWrite(rio,wb);
    } else if (action == ROCKS_DEL) {
        serverAssert(numkeys == 1 && rawkeys);
        DEBUG_MSGS_APPEND(req->msgs,"execswap-del-del","rawkey=%s",rawkeys[0]);
        RIOInitDel(rio,rawkeys[0]);
        zfree(rawkeys), rawkeys = NULL;
    } else if (action == ROCKS_DELETERANGE) {
        serverAssert(numkeys == 2 && rawkeys);
        DEBUG_MSGS_APPEND(req->msgs,"execswap-del-deleterange",
                "start_key=%s end_key=%s",rawkeys[0],rawkeys[1]);
        RIOInitDeleteRange(rio,rawkeys[0],rawkeys[1]);
        zfree(rawkeys), rawkeys = NULL;
    } else {
        errcode = SWAP_ERR_EXEC_UNEXPECTED_ACTION;
        goto end;
    }

    updateStatsSwapRIO(req, rio);
    if ((errcode = doRIO(rio))) {
        goto end;
    }

end:
    doNotify(req, errcode);
    RIODeinit(rio);
}


/*
    calculate the size of all files in a folder
*/
static long get_dir_size(char *dirname)
{
    DIR *dir;
    struct dirent *ptr;
    long total_size = 0;
    char path[PATH_MAX] = {0};

    dir = opendir(dirname);
    if(dir == NULL)
    {
        serverLog(LL_WARNING,"open dir(%s) failed.", dirname);
        return -1;
    }

    while((ptr=readdir(dir)) != NULL)
    {
        snprintf(path, (size_t)PATH_MAX, "%s/%s", dirname,ptr->d_name);
        struct stat buf;
        if(lstat(path, &buf) < 0) {
            serverLog(LL_WARNING, "path(%s) lstat error", path);
        }
        if(strcmp(ptr->d_name,".") == 0) {
            total_size += buf.st_size;
            continue;
        }
        if(strcmp(ptr->d_name,"..") == 0) {
            continue;
        }
        if (S_ISDIR(buf.st_mode))
        {
            total_size += get_dir_size(path);
            memset(path, 0, sizeof(path));
        } else {
            total_size += buf.st_size;
        }
    }
    closedir(dir);
    return total_size;
}

static void executeCompactRange(swapRequest *req) {
    char dir[ROCKS_DIR_MAX_LEN];
    snprintf(dir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, server.rocksdb_epoch);
    serverLog(LL_WARNING, "[rocksdb compact range before] dir(%s) size(%ld)", dir, get_dir_size(dir));
    rocksdb_compact_range(server.rocks->db, NULL, 0, NULL, 0);
    serverLog(LL_WARNING, "[rocksdb compact range after] dir(%s) size(%ld)", dir, get_dir_size(dir));
    doNotify(req, 0);
}

static void executeGetRocksdbStats(swapRequest* req) {
    req->finish_pd = rocksdb_property_value(server.rocks->db, "rocksdb.stats");
    doNotify(req, 0);
}


static void executeRocksdbUtils(swapRequest *req) {
    switch(req->intention_flags) {
        case COMPACT_RANGE_TASK:
            executeCompactRange(req);
            break;
        case GET_ROCKSDB_STATS_TASK:
            executeGetRocksdbStats(req);
            break;
        default:
            req->errcode = SWAP_ERR_EXEC_UNEXPECTED_UTIL;
            break;
    }
}

static void executeSwapOutRequest(swapRequest *req) {
    int i, numkeys, errcode = 0, action;
    sds *rawkeys = NULL, *rawvals = NULL;
    RIO _rio = {0}, *rio = &_rio;
    rocksdb_writebatch_t *wb = NULL;
    swapData *data = req->data;

    if ((errcode = swapDataEncodeData(data,req->intention,req->datactx,
                &action,&numkeys, &rawkeys,&rawvals))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"execswap-out-encodedata","action=%s, numkeys=%d",
            rocksActionName(action), numkeys);

    if (numkeys <= 0) goto end;

    if (action == ROCKS_PUT) {
        serverAssert(numkeys == 1);

#ifdef SWAP_DEBUG
        sds rawval_repr = sdscatrepr(sdsempty(), rawvals[0], sdslen(rawvals[0]));
        DEBUG_MSGS_APPEND(req->msgs,"execswap-out-put","rawkey=%s,rawval=%s",
                rawkeys[0], rawval_repr);
        sdsfree(rawval_repr);
#endif

        RIOInitPut(rio,rawkeys[0],rawvals[0]);
        zfree(rawkeys), rawkeys = NULL;
        zfree(rawvals), rawvals = NULL;
    } else if (action == ROCKS_WRITE) {
        wb = rocksdb_writebatch_create();
        for (i = 0; i < numkeys; i++) {
            rocksdb_writebatch_put(wb,rawkeys[i],sdslen(rawkeys[i]),
                    rawvals[i], sdslen(rawvals[i]));
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-out-write","numkeys=%d",numkeys);
        RIOInitWrite(rio, wb);
    } else {
        errcode = SWAP_ERR_EXEC_UNEXPECTED_ACTION;
        goto end;
    }

    updateStatsSwapRIO(req,rio);
    if ((errcode = doRIO(rio))) {
        goto end;
    }

    if ((errcode = swapDataCleanObject(data,req->datactx))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"execswap-out-cleanobject","ok");


end:

    DEBUG_MSGS_APPEND(req->msgs,"execswap-out-end","retval=%d",retval);
    doNotify(req, errcode);
    if (rawkeys) {
        for (i = 0; i < numkeys; i++) {
            sdsfree(rawkeys[i]);
        }
        zfree(rawkeys);
    }
    if (rawvals) {
        for (i = 0; i < numkeys; i++) {
            sdsfree(rawvals[i]);
        }
        zfree(rawvals);
    }
    RIODeinit(rio);
}

static int doSwapIntentionDelRange(swapRequest *req, sds start, sds end) {
    RIO _rio = {0}, *rio = &_rio;
    int retval;
    RIOInitDeleteRange(rio, start, end);
    updateStatsSwapRIO(req,rio);
    retval = doRIO(rio);
    RIODeinit(rio);
    return retval;
}

static int doSwapIntentionDel(swapRequest *req, int numkeys, sds *rawkeys) {
    RIO _rio = {0}, *rio = &_rio;
    int i, retval;
    UNUSED(req);

    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();
    for (i = 0; i < numkeys; i++) {
        rocksdb_writebatch_delete(wb,rawkeys[i],sdslen(rawkeys[i]));
    }

    RIOInitWrite(rio, wb);
    updateStatsSwapRIO(req,rio);
    retval = doRIO(rio);
    RIODeinit(rio);

    DEBUG_MSGS_APPEND(req->msgs,"execswap-in.del","numkeys=%d,retval=%d",
            numkeys, retval);
    RIODeinit(rio);
    return retval;
}


static void executeSwapInRequest(swapRequest *req) {
    robj *decoded;
    int numkeys, errcode, action;
    sds *rawkeys = NULL;
    RIO _rio = {0}, *rio = &_rio;
    swapData *data = req->data;
    int del_flag = SWAPIN_NO_DEL;

    if ((errcode = swapDataEncodeKeys(data,req->intention,req->datactx,
                &action,&numkeys,&rawkeys))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"execswap-in-encodekeys","action=%s, numkeys=%d",
            rocksActionName(action),numkeys);

    if (numkeys <= 0) goto end;

    if (action == ROCKS_MULTIGET) {
        RIOInitMultiGet(rio,numkeys,rawkeys);
        if ((errcode = doRIO(rio))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-multiget",
                "numkeys=%d,rio=ok", numkeys);

        if ((errcode = swapDataDecodeData(data,rio->multiget.numkeys,
                    rio->multiget.rawkeys,rio->multiget.rawvals,&decoded))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-decodedata","decoded=%p",(void*)decoded);

        if (req->intention_flags & INTENTION_IN_DEL) {
            if ((errcode = doSwapIntentionDel(req,numkeys,rawkeys))) {
                goto end;
            }
            del_flag = SWAPIN_DEL;
        }
    } else if (action == ROCKS_GET) {
        serverAssert(numkeys == 1);
        RIOInitGet(rio, rawkeys[0]);
        if ((errcode = doRIO(rio))) {
            goto end;
        }

#ifdef SWAP_DEBUG
        sds rawval_repr = sdscatrepr(sdsempty(),rio->get.rawval,
                sdslen(rio->get.rawval));
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-get","rawkey=%s,rawval=%s",rawkeys[0],rawval_repr);
        sdsfree(rawval_repr);
#endif

        if ((errcode = swapDataDecodeData(data,1,&rio->get.rawkey,
                    &rio->get.rawval,&decoded))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-decodedata","decoded=%p",(void*)decoded);

        if (req->intention_flags & INTENTION_IN_DEL) {
            if ((errcode = doSwapIntentionDel(req, numkeys, rawkeys))) {
                goto end;
            }
            //type must is wholekey 
            del_flag = SWAPIN_DEL | SWAPIN_DEL_FULL;
        }
        /* rawkeys not moved, only rakeys[0] moved, free when done. */
        zfree(rawkeys);
    } else if (action == ROCKS_SCAN) {
        serverAssert(numkeys == 1);
        RIOInitScan(rio, rawkeys[0]);
        if ((errcode = doRIO(rio))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-scan","prefix=%s,rio=ok",rawkeys[0]);
        if ((errcode = swapDataDecodeData(data,rio->scan.numkeys,rio->scan.rawkeys,
                    rio->scan.rawvals,&decoded))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-decodedata", "decoded=%p",(void*)decoded);

        if (req->intention_flags & INTENTION_IN_DEL) {            
            if ((errcode = doSwapIntentionDelRange(req, sdsdup(rawkeys[0]), rocksCalculateNextKey(rawkeys[0])))) {
                goto end;
            }
            /* assert rocksdb is null*/
            del_flag = SWAPIN_DEL | SWAPIN_DEL_FULL;
        }
        /* rawkeys not moved, only rakeys[0] moved, free when done. */
        zfree(rawkeys);
    } else {
        errcode = SWAP_ERR_EXEC_UNEXPECTED_ACTION;
        goto end;
    }

    req->result = swapDataCreateOrMergeObject(data,decoded,req->datactx, del_flag);
    DEBUG_MSGS_APPEND(req->msgs,"execswap-in-createormerge","result=%p",(void*)req->result);

end:
    updateStatsSwapRIO(req,rio);
    DEBUG_MSGS_APPEND(req->msgs,"execswap-in-end","retval=%d",retval);
    doNotify(req, errcode);
    RIODeinit(rio);
}

void executeSwapRequest(swapRequest *req) {
    serverAssert(req->errcode == 0);
    switch (req->intention) {
    case SWAP_IN:
        executeSwapInRequest(req);
        break;
    case SWAP_OUT:
        executeSwapOutRequest(req);
        break;
    case SWAP_DEL:
        executeSwapDelRequest(req);
        break;
    case ROCKSDB_UTILS:
        executeRocksdbUtils(req);
        break;
    default: 
        req->errcode = SWAP_ERR_EXEC_FAIL;
        break;
    }
}

/* Called by async-complete-queue or parallel-sync in server thread
 * to swap in/out/del data */
void finishSwapRequest(swapRequest *req) {
    int retval = 0;

    if (req->errcode) return;

    DEBUG_MSGS_APPEND(req->msgs,"execswap-finish","intention=%s",
            swapIntentionName(req->intention));

    switch(req->intention) {
    case SWAP_IN:
        retval = swapDataSwapIn(req->data,req->result,req->datactx);
        break;
    case SWAP_OUT:
        retval = swapDataSwapOut(req->data,req->datactx);
        break;
    case SWAP_DEL: 
        retval = swapDataSwapDel(req->data,req->datactx,
                req->intention_flags & INTENTION_DEL_ASYNC);
        break;
    default:
        retval = SWAP_ERR_DATA_FIN_FAIL;
        break;
    }
    req->errcode = retval;
}

void submitSwapRequest(int mode, int intention,
        uint32_t intention_flags, swapData* data, void *datactx,
        swapRequestFinishedCallback cb, void *pd, void *msgs, int idx) {
    swapRequest *req = swapRequestNew(intention,intention_flags,data,datactx,cb,pd,msgs);
    updateStatsSwapStart(req);
    if (mode == SWAP_MODE_ASYNC) {
        asyncSwapRequestSubmit(req, idx);
    } else {
        parallelSyncSwapRequestSubmit(req,  idx);
    }
}

swapRequest *swapRequestNew(int intention, uint32_t intention_flags, swapData *data, void *datactx,
        swapRequestFinishedCallback cb, void *pd, void *msgs) {
    swapRequest *req = zcalloc(sizeof(swapRequest));
    UNUSED(msgs);
    req->intention = intention;
    req->intention_flags = intention_flags;
    req->data = data;
    req->datactx = datactx;
    req->result = NULL;
    req->finish_cb = cb;
    req->finish_pd = pd;
    req->swap_memory = 0;
#ifdef SWAP_DEBUG
    req->msgs = msgs;
#endif
    return req;
}

void swapRequestFree(swapRequest *req) {
    if (req->result) decrRefCount(req->result);
    zfree(req);
}

#ifdef REDIS_TEST

void mockNotifyCallback(swapRequest *req, void *pd) {
    UNUSED(req),UNUSED(pd);
}

void initServer(void);
void initServerConfig(void);
void InitServerLast();
int swapExecTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    server.hz = 10;
    sds rawkey1 = sdsnew("rawkey1"), rawkey2 = sdsnew("rawkey2");
    sds rawval1 = sdsnew("rawval1"), rawval2 = sdsnew("rawval2");
    sds prefix = sdsnew("rawkey");
    robj *key1 = createStringObject("key1",4);
    robj *val1 = createStringObject("val1",4);
    initTestRedisDb();
    redisDb *db = server.db;

    TEST("exec: init") {
        initServerConfig();
        incrRefCount(val1);
        dbAdd(db,key1,val1);
        rocksInit();
        initStatsSwap();
    }

   TEST("exec: rio") {
       rocksdb_writebatch_t *wb;
       RIO _rio, *rio = &_rio;

       RIOInitPut(rio,rawkey1,rawval1);
       test_assert(doRIO(rio) == C_OK);

       RIOInitGet(rio,rawkey1);
       test_assert(doRIO(rio) == C_OK);
       test_assert(sdscmp(rio->get.rawval, rawval1) == 0);

       RIOInitDel(rio,rawkey1);
       test_assert(doRIO(rio) == C_OK);

       wb = rocksdb_writebatch_create();
       rocksdb_writebatch_put(wb,rawkey1,sdslen(rawkey1),rawval1,sdslen(rawval1));
       rocksdb_writebatch_put(wb,rawkey2,sdslen(rawkey2),rawval2,sdslen(rawval2));
       RIOInitWrite(rio,wb);
       test_assert(doRIO(rio) == C_OK);

       sds *rawkeys = zmalloc(sizeof(sds)*2);
       rawkeys[0] = rawkey1;
       rawkeys[1] = rawkey2;
       RIOInitMultiGet(rio,2,rawkeys);
       test_assert(doRIO(rio) == C_OK);
       test_assert(rio->multiget.numkeys == 2);
       test_assert(sdscmp(rio->multiget.rawvals[0],rawval1) == 0);
       test_assert(sdscmp(rio->multiget.rawvals[1],rawval2) == 0);

       RIOInitScan(rio,prefix);
       test_assert(doRIO(rio) == C_OK);
       test_assert(rio->scan.numkeys == 2);
       test_assert(sdscmp(rio->scan.rawvals[0],rawval1) == 0);
       test_assert(sdscmp(rio->scan.rawvals[1],rawval2) == 0);
   } 

   TEST("exec: swap-out") {
       val1 = lookupKey(db,key1,LOOKUP_NOTOUCH);
       swapData *data = createWholeKeySwapData(db,key1,val1,NULL,NULL);
       swapRequest *req = swapRequestNew(SWAP_OUT,0,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       executeSwapRequest(req);
       test_assert(req->errcode == 0);
       finishSwapRequest(req);
       test_assert(req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(lookupEvictKey(db,key1) != NULL);
   }

   TEST("exec: swap-in") {
       /* rely on data swap out to rocksdb by previous case */
       robj *evict1 = lookupEvictKey(db,key1);
       swapData *data = createWholeKeySwapData(db,key1,NULL,evict1,NULL);
       swapRequest *req = swapRequestNew(SWAP_IN,0,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       executeSwapRequest(req);
       test_assert(req->errcode == 0);
       finishSwapRequest(req);
       test_assert(req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) != NULL);
       test_assert(lookupEvictKey(db,key1) == NULL);
   } 

   TEST("exec: swap-del") {
       val1 = lookupKey(db,key1,LOOKUP_NOTOUCH);
       swapData *data = createWholeKeySwapData(db,key1,val1,NULL,NULL);
       swapRequest *req = swapRequestNew(SWAP_DEL,0,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       executeSwapRequest(req);
       test_assert(req->errcode == 0);
       finishSwapRequest(req);
       test_assert(req->errcode == 0);
       test_assert(lookupEvictKey(db,key1) == NULL);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
   }

   TEST("exec: swap-del.async, swap-in.del") {
       robj *evict1;
       incrRefCount(val1);
       dbAdd(db,key1,val1);
       /* out key1 */
       swapData *out_data = createWholeKeySwapData(db,key1,val1,NULL,NULL);
       swapRequest *out_req = swapRequestNew(SWAP_OUT,0,out_data,NULL,NULL,NULL,NULL);
       out_req->notify_cb = mockNotifyCallback;
       out_req->notify_pd = NULL;
       executeSwapRequest(out_req);
       test_assert(out_req->errcode == 0);
       finishSwapRequest(out_req);
       test_assert(out_req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert((evict1 = lookupEvictKey(db,key1)) != NULL);

       /* check rocksdb */
       int numkeys, action;
       sds *rawkeys;
       RIO _rio = {0}, *rio = &_rio;
       swapDataEncodeKeys(out_data,SWAP_IN,NULL,&action,&numkeys,&rawkeys);
       test_assert(numkeys == 1);
       RIOInitGet(rio, rawkeys[0]);
       doRIO(rio);
       test_assert(rio->get.rawval != NULL);
       sdsfree(rio->get.rawval);
       rio->get.rawval = NULL;

       /* del.async key1 */
       swapData *del_async_data = createWholeKeySwapData(db,key1,NULL,evict1,NULL);
       swapRequest *del_async_req = swapRequestNew(SWAP_DEL,INTENTION_DEL_ASYNC,del_async_data,NULL,NULL,NULL,NULL);
       del_async_req->notify_cb = mockNotifyCallback;
       del_async_req->notify_pd = NULL;
       executeSwapRequest(del_async_req);
       test_assert(del_async_req->errcode == 0);
       finishSwapRequest(del_async_req);
       test_assert(del_async_req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(lookupEvictKey(db,key1) != NULL);
       dbDeleteEvict(db,key1);

       doRIO(rio);
       test_assert(rio->get.rawval == NULL);
       sdsfree(rio->get.rawval);
       rio->get.rawval = NULL;

       /* out key1 again */
       executeSwapRequest(out_req);
       test_assert(out_req->errcode == 0);
       finishSwapRequest(out_req);
       test_assert(out_req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert((evict1 = lookupEvictKey(db,key1)) != NULL);

       doRIO(rio);
       test_assert(rio->get.rawval != NULL);
       sdsfree(rio->get.rawval);
       rio->get.rawval = NULL;

       /* in.del key1 */
       swapData *in_del_data = createWholeKeySwapData(db,key1,NULL,evict1,NULL);
       swapRequest *in_del_req = swapRequestNew(SWAP_DEL,INTENTION_DEL_ASYNC,in_del_data,NULL,NULL,NULL,NULL);
       in_del_req->notify_cb = mockNotifyCallback;
       in_del_req->notify_pd = NULL;
       executeSwapRequest(in_del_req);
       test_assert(in_del_req->errcode == 0);
       finishSwapRequest(in_del_req);
       test_assert(in_del_req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(lookupEvictKey(db,key1) != NULL);

       doRIO(rio);
       test_assert(rio->get.rawval == NULL);
       sdsfree(rio->get.rawval);
       rio->get.rawval = NULL;
   }


   return error;
}

#endif

