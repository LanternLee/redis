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

/* ----------------------------- swaps result ----------------------------- */
/* Prepare the getKeyRequestsResult struct to hold numswaps, either by using
 * the pre-allocated swaps or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 */

void copyKeyRequest(keyRequest *dst, keyRequest *src) {
    if (src->key) incrRefCount(src->key);
    dst->key = src->key;
    if (src->num_subkeys > 0)  {
        dst->subkeys = zmalloc(sizeof(robj*)*src->num_subkeys);
        for (int i = 0; i < src->num_subkeys; i++) {
            if (src->subkeys[i]) incrRefCount(src->subkeys[i]);
            dst->subkeys[i] = src->subkeys[i];
        }
    }
    dst->num_subkeys = src->num_subkeys;
    dst->level = src->level;
    dst->cmd_intention = src->cmd_intention;
    dst->cmd_intention_flags = src->cmd_intention_flags;
    dst->dbid = src->dbid;
}

void moveKeyRequest(keyRequest *dst, keyRequest *src) {
    dst->key = src->key;
    src->key = NULL;
    dst->subkeys = src->subkeys;
    src->subkeys = NULL;
    dst->num_subkeys = src->num_subkeys;
    src->num_subkeys = 0;
    dst->level = src->level;
    dst->cmd_intention = src->cmd_intention;
    dst->cmd_intention_flags = src->cmd_intention_flags;
    dst->dbid = src->dbid;
}

void keyRequestDeinit(keyRequest *key_request) {
    if (key_request == NULL) return;
    if (key_request->key) decrRefCount(key_request->key);
    key_request->key = NULL;
    for (int i = 0; i < key_request->num_subkeys; i++) {
        if (key_request->subkeys[i])
            decrRefCount(key_request->subkeys[i]);
        key_request->subkeys[i] = NULL;
    }
    zfree(key_request->subkeys);
    key_request->subkeys = NULL;
    key_request->num_subkeys = 0;
}

void getKeyRequestsPrepareResult(getKeyRequestsResult *result, int num) {
	/* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the
     * pre-allocated stack buffer here. */
	if (!result->key_requests) {
		serverAssert(!result->num);
		result->key_requests = result->buffer;
	}

	/* Resize if necessary */
	if (num > result->size) {
		if (result->key_requests != result->buffer) {
			/* We're not using a static buffer, just (re)alloc */
			result->key_requests = zrealloc(result->key_requests,
                    num*sizeof(keyRequest));
		} else {
			/* We are using a static buffer, copy its contents */
			result->key_requests = zmalloc(num*sizeof(keyRequest));
			if (result->num) {
				memcpy(result->key_requests,result->buffer,
                        result->num*sizeof(keyRequest));
            }
		}
		result->size = num;
	}
}

/* Note that key&subkeys ownership moved */
void getKeyRequestsAppendResult(getKeyRequestsResult *result, int level,
        robj *key, int num_subkeys, robj **subkeys, int cmd_intention,
        int cmd_intention_flags, int dbid) {
    if (result->num == result->size) {
        int newsize = result->size + 
            (result->size > 8192 ? 8192 : result->size);
        getKeyRequestsPrepareResult(result, newsize);
    }

    keyRequest *key_request = &result->key_requests[result->num++];
    key_request->level = level;
    key_request->key = key;
    key_request->num_subkeys = num_subkeys;
    key_request->subkeys = subkeys;
    key_request->cmd_intention = cmd_intention;
    key_request->cmd_intention_flags = cmd_intention_flags;
    key_request->dbid = dbid;
}

void releaseKeyRequests(getKeyRequestsResult *result) {
    for (int i = 0; i < result->num; i++) {
        keyRequest *key_request = result->key_requests + i;
        keyRequestDeinit(key_request);
    }
}

void getKeyRequestsFreeResult(getKeyRequestsResult *result) {
    if (result && result->key_requests != result->buffer) {
        zfree(result->key_requests);
    }
}

/* NOTE that result.{key,subkeys} are ONLY REFS to client argv (since client
 * outlives getKeysResult if no swap action happend. key, subkey will be 
 * copied (using incrRefCount) when async swap acutally proceed. */
static void getSingleCmdKeyRequests(client *c, getKeyRequestsResult *result) {
    struct redisCommand *cmd = c->cmd;

    if (cmd->getkeyrequests_proc == NULL) {
        int i, numkeys;
        getKeysResult keys = GETKEYS_RESULT_INIT;
        /* whole key swaping, swaps defined by command arity. */
        numkeys = getKeysFromCommand(cmd,c->argv,c->argc,&keys);
        getKeyRequestsPrepareResult(result,result->num+numkeys);
        for (i = 0; i < numkeys; i++) {
            robj *key = c->argv[keys.keys[i]];

            incrRefCount(key);
            getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,key,0,NULL,
                    cmd->intention,cmd->intention_flags,KEYREQUESTS_DBID);
        }
        getKeysFreeResult(&keys); 
    } else if (cmd->flags & CMD_MODULE) {
        // TODO support module
    } else {
        cmd->getkeyrequests_proc(cmd,c->argv,c->argc,result);
    }
}

void getKeyRequests(client *c, getKeyRequestsResult *result) {
    getKeyRequestsPrepareResult(result, MAX_KEYREQUESTS_BUFFER);

    if ((c->flags & CLIENT_MULTI) && 
            !(c->flags & (CLIENT_DIRTY_CAS | CLIENT_DIRTY_EXEC)) &&
            c->cmd->proc == execCommand) {
        /* if current is EXEC, we get swaps for all queue commands. */
        robj **orig_argv;
        int i, orig_argc;
        struct redisCommand *orig_cmd;

        orig_argv = c->argv;
        orig_argc = c->argc;
        orig_cmd = c->cmd;
        for (i = 0; i < c->mstate.count; i++) {
            c->argc = c->mstate.commands[i].argc;
            c->argv = c->mstate.commands[i].argv;
            c->cmd = c->mstate.commands[i].cmd;
            getSingleCmdKeyRequests(c, result);
        }
        c->argv = orig_argv;
        c->argc = orig_argc;
        c->cmd = orig_cmd;
    } else {
        getSingleCmdKeyRequests(c, result);
    }
}

int getKeyRequestsNone(struct redisCommand *cmd, robj **argv, int argc,
        getKeyRequestsResult *result) {
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(result);
    return 0;
}

/* Used by flushdb/flushall to get global scs(similar to table lock). */
int getKeyRequestsGlobal(struct redisCommand *cmd, robj **argv, int argc,
        getKeyRequestsResult *result) {
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    getKeyRequestsAppendResult(result,REQUEST_LEVEL_SVR,NULL,0,NULL,
            cmd->intention,cmd->intention_flags,KEYREQUESTS_DBID);
    return 0;
}

void expiredCommand(client *c) {
    addReply(c, shared.ok);
}

#define GETKEYS_RESULT_SUBKEYS_INIT_LEN 8
#define GETKEYS_RESULT_SUBKEYS_LINER_LEN 1024

robj** extractRequestArgs(robj **argv, int argc, int start, int end, int step, int* len) {
    int i, realstart, realend, num = 0;
    robj **rst;
    realstart = start >= 0 ? start : argc + start;
    realend = end >= 0 ? end : argc + end;

    if (realstart > realend) {
        *len = 0;
        return NULL;
    }

    rst = zmalloc((((realend - realstart)/step) + 1) * sizeof(robj*));
    for (i = realstart; i <= realend && i < argc; i+=step) {
        rst[num++] = argv[i];
    }
    *len = num;
    return rst;
}

int getKeyRequestsSingleKeyWithSubkeys(struct redisCommand *cmd, robj **argv,
        int argc, struct getKeyRequestsResult *result,
        int key_index, int first_subkey, int last_subkey, int subkey_step) {
    int i, num = 0, capacity = GETKEYS_RESULT_SUBKEYS_INIT_LEN;
    robj *key, **subkeys = NULL;
    UNUSED(cmd);

    subkeys = zmalloc(capacity*sizeof(robj*));
    getKeyRequestsPrepareResult(result,result->num+1);

    key = argv[key_index];
    incrRefCount(key);
    
    if (last_subkey < 0) last_subkey += argc;
    for (i = first_subkey; i <= last_subkey; i += subkey_step) {
        robj *subkey = argv[i];
        if (num >= capacity) {
            if (capacity < GETKEYS_RESULT_SUBKEYS_LINER_LEN)
                capacity *= 2;
            else
                capacity += GETKEYS_RESULT_SUBKEYS_LINER_LEN;

            subkeys = zrealloc(subkeys, capacity*sizeof(robj*));
        }
        incrRefCount(subkey);
        subkeys[num++] = subkey;
    }
    getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,key,num,subkeys,
            cmd->intention,cmd->intention_flags,KEYREQUESTS_DBID);

    return 0;
}

int getKeyRequestsSinterstore(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    getKeyRequestsPrepareResult(result, result->num + argc);
    incrRefCount(argv[1]);
    getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,argv[1], 0, NULL,
                               SWAP_IN, INTENTION_IN_DEL,KEYREQUESTS_DBID);
    for(int i = 2; i < argc; i++) {
        incrRefCount(argv[i]);
        getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,argv[i], 0, NULL,
                                   SWAP_IN,0,KEYREQUESTS_DBID);
    }

    return 0;
}

int getKeyRequestsHset(struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(cmd,argv,argc,result,1,2,-1,2);
}

int getKeyRequestsHmget(struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(cmd,argv,argc,result,1,2,-1,1);
}


int getKeyRequestSmembers(struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(cmd,argv,argc,result,1,2,-1,1);
}

int getKeyRequestSmove(struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result) {
    robj** subkeys;
    getKeyRequestsPrepareResult(result, result->num + 2);

    incrRefCount(argv[1]);
    incrRefCount(argv[3]);
    subkeys = zmalloc(sizeof(robj*));
    subkeys[0] = argv[3];
    getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,argv[1], 1, subkeys,
                               SWAP_IN, INTENTION_IN_DEL,KEYREQUESTS_DBID);

    incrRefCount(argv[2]);
    incrRefCount(argv[3]);
    subkeys = zmalloc(sizeof(robj*));
    subkeys[0] = argv[3];
    getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,argv[2], 1, subkeys,
                               SWAP_IN, 0,KEYREQUESTS_DBID);

    return 0;
}



/* zset type */
int getKeyRequestsZScore(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(cmd,argv,argc,result,1,2,-1,1);
}

int getKeyRequestsZincrby(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(cmd, argv, argc, result, 1, 3, -1, 2);
}

int getKeyRequestsZAdd(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    int first_score = 2;
    while(first_score < argc) {
        char *opt = argv[first_score]->ptr;
        if (
            strcasecmp(opt,"nx") != 0 &&
            strcasecmp(opt,"xx") != 0 &&
            strcasecmp(opt,"ch") != 0 &&
            strcasecmp(opt,"incr") != 0 &&
            strcasecmp(opt,"gt") != 0 &&
            strcasecmp(opt,"lt") != 0 
        ) {
            break;
        }
        first_score++;
    }
    return getKeyRequestsSingleKeyWithSubkeys(cmd, argv, argc, result, 1, first_score + 1, -1, 2);
}

int getKeyRequestsZunionInterDiffGeneric(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result, int op) {
    long long setnum;
    int numkeysIndex = 2;
    if (getLongLongFromObject(argv[2], &setnum) != C_OK) {
        return C_ERR;
    }
    if (setnum < 1 || setnum + 3 > argc) {
        return C_ERR;
    }
    
    getKeyRequestsPrepareResult(result,result->num+ setnum + 1);
    incrRefCount(argv[1]);
    getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,argv[1],0,NULL,
            SWAP_IN, INTENTION_IN_DEL,KEYREQUESTS_DBID);
    for(long long i = 0; i < setnum; i++) {        
        incrRefCount(argv[i + 3]);
        getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,argv[i + 3], 0, NULL,
            SWAP_IN,0,KEYREQUESTS_DBID);
        
    }
    
    
    
    return C_OK;
}

int getKeyRequestsZunionstore(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZunionInterDiffGeneric(cmd, argv, argc, result, SET_OP_UNION);
}

int getKeyRequestsZinterstore(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZunionInterDiffGeneric(cmd, argv, argc, result, SET_OP_INTER);
}
int getKeyRequestsZdiffstore(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZunionInterDiffGeneric(cmd, argv, argc, result, SET_OP_DIFF);
}

#define ZMIN -1
#define ZMAX 1
int getKeyRequestsZpopGeneric(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result, int flags) {
    getKeyRequestsPrepareResult(result,result->num+ argc - 2);
    for(int i = 1; i < argc - 1; i++) {
        incrRefCount(argv[i]);
        getKeyRequestsAppendResult(result, REQUEST_LEVEL_KEY, argv[i], 0, NULL, SWAP_IN, INTENTION_IN_DEL, 0);
    }
    return C_OK;
}

int getKeyRequestsZpopMin(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZpopGeneric(cmd, argv, argc, result, ZMIN);  
}

int getKeyRequestsZpopMax(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsZpopGeneric(cmd, argv, argc, result, ZMAX);    
}

int getKeyRequestsZrangestore(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    getKeyRequestsPrepareResult(result,result->num+ 2);
    for(int i = 1; i < 3; i++) {
        incrRefCount(argv[i]);
        getKeyRequestsAppendResult(result, REQUEST_LEVEL_KEY, argv[i], 0, NULL, SWAP_IN, INTENTION_IN_DEL, 0);
    }
    return C_OK;
}

/** geo **/
int getKeyRequestsGeoAdd(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    int first_score = 2;
    while(first_score < argc) {
        char *opt = argv[first_score]->ptr;
        if (
            strcasecmp(opt,"nx") != 0 &&
            strcasecmp(opt,"xx") != 0 &&
            strcasecmp(opt,"ch") != 0 
        ) {
            break;
        }
        first_score++;
    }
    return getKeyRequestsSingleKeyWithSubkeys(cmd, argv, argc, result, 1, first_score + 2, -1, 3);
}

int getKeyRequestsGeoDist(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(cmd,argv,argc,result,1,2,-2,1);
}

int getKeyRequestsGeoHash(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    return getKeyRequestsSingleKeyWithSubkeys(cmd,argv,argc,result,1,2,-1,1);
}

int getKeyRequestsGeoRadius(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    robj* storekey = NULL;
    for(int i =0; i < argc; i++) {
        if (!strcasecmp(argv[i]->ptr, "store") && (i+1) < argc) {
            storekey = argv[i+1];
            i++;
        } else if(!strcasecmp(argv[i]->ptr, "storedist") && (i+1) < argc) {
            storekey = argv[i+1];
            i++;
        }
    }
    getKeyRequestsPrepareResult(result,result->num+ 2);
    incrRefCount(argv[1]);
    getKeyRequestsAppendResult(result, REQUEST_LEVEL_KEY, argv[1], 0, NULL, SWAP_IN, 0, 0);
    if (storekey != NULL) {
        incrRefCount(storekey);
        getKeyRequestsAppendResult(result, REQUEST_LEVEL_KEY, storekey, 0, NULL, SWAP_IN, INTENTION_IN_DEL, 0);
    }
    return C_OK;
}

int getKeyRequestsGeoSearchStore(struct redisCommand *cmd, robj **argv, int argc, struct getKeyRequestsResult *result) {
    
    getKeyRequestsPrepareResult(result,result->num+ 2);
    incrRefCount(argv[1]);
    getKeyRequestsAppendResult(result, REQUEST_LEVEL_KEY, argv[1], 0, NULL, SWAP_IN, INTENTION_IN_DEL, 0);
    
    incrRefCount(argv[2]);
    getKeyRequestsAppendResult(result, REQUEST_LEVEL_KEY, argv[2], 0, NULL, SWAP_IN, 0, 0);
    return C_OK;
}

#ifdef REDIS_TEST

void rewriteResetClientCommandCString(client *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        char *a = va_arg(ap, char*);
        argv[j] = createStringObject(a, strlen(a));
    }
    replaceClientCommandVector(c, argc, argv);
    va_end(ap);
}

void initServerConfig(void);
int swapCmdTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    client *c;

    TEST("cmd: init") {
        initServerConfig();
        ACLInit();
        server.hz = 10;
        c = createClient(NULL);
    }

    TEST("cmd: no key") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,1,"PING");
        getKeyRequests(c,&result);
        test_assert(result.num == 0);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    } 

    TEST("cmd: single key") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,2,"GET","KEY");
        getKeyRequests(c,&result);
        test_assert(result.num == 1);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: multiple keys") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        getKeyRequests(c,&result);
        test_assert(result.num == 2);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: multi/exec") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,1,"PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"SET","KEY3","VAL3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 3);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(result.key_requests[0].subkeys == NULL);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        test_assert(result.key_requests[1].subkeys == NULL);
        test_assert(!strcmp(result.key_requests[2].key->ptr, "KEY3"));
        test_assert(result.key_requests[2].subkeys == NULL);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: hash subkeys") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,5,"HMGET","KEY","F1","F2","F3");
        getKeyRequests(c,&result);
        test_assert(result.num == 1);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY"));
        test_assert(result.key_requests[0].num_subkeys == 3);
        test_assert(!strcmp(result.key_requests[0].subkeys[0]->ptr, "F1"));
        test_assert(!strcmp(result.key_requests[0].subkeys[1]->ptr, "F2"));
        test_assert(!strcmp(result.key_requests[0].subkeys[2]->ptr, "F3"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: multi/exec hash subkeys") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,1,"PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,3,"MGET","KEY1","KEY2");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,5,"HMGET","HASH","F1","F2","F3");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 3);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "KEY1"));
        test_assert(result.key_requests[0].subkeys == NULL);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "KEY2"));
        test_assert(result.key_requests[1].subkeys == NULL);
        test_assert(!strcmp(result.key_requests[2].key->ptr, "HASH"));
        test_assert(result.key_requests[2].num_subkeys == 3);
        test_assert(!strcmp(result.key_requests[2].subkeys[0]->ptr, "F1"));
        test_assert(!strcmp(result.key_requests[2].subkeys[1]->ptr, "F2"));
        test_assert(!strcmp(result.key_requests[2].subkeys[2]->ptr, "F3"));
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: dispatch swap sequentially for reentrant-key request") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        rewriteResetClientCommandCString(c,4,"MGET", "K1", "K2", "K1");
        getKeyRequests(c,&result);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
    }

    TEST("cmd: dispatch swap sequentially for reentrant-key request (multi)") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,3,"HMGET","HASH", "F1");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,2,"DEL","HASH");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 2);
        test_assert(!strcmp(result.key_requests[0].key->ptr, "HASH"));
        test_assert(result.key_requests[0].num_subkeys == 1);
        test_assert(!strcmp(result.key_requests[1].key->ptr, "HASH"));
        test_assert(result.key_requests[1].subkeys == NULL);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    TEST("cmd: dispatch swap sequentially with db/svr request") {
        getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
        c->flags |= CLIENT_MULTI;
        rewriteResetClientCommandCString(c,1,"PING");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"FLUSHDB");
        queueMultiCommand(c);
        rewriteResetClientCommandCString(c,1,"EXEC");
        getKeyRequests(c,&result);
        test_assert(result.num == 1);
        test_assert(result.key_requests[0].key == NULL);
        releaseKeyRequests(&result);
        getKeyRequestsFreeResult(&result);
        discardTransaction(c);
    }

    return error;
}

#endif

