#include "server.h"

taskServer server;
// prototype

struct taskCommand cmdTable[] = { { "get", getCommand, 2, REDIS_CMD_INLINE, NULL, 1, 1, 1 }, { "rpc", rpcCommand, 5, REDIS_CMD_BULK, NULL, 0, 0, 0 }, { "del", delCommand, 2, REDIS_CMD_INLINE, NULL, 1, 1, 1 } };

dictType dbDictType = { dictObjHash, NULL, NULL, dictObjKeyCompare, dictRedisObjectDestructor, dictRedisObjectDestructor };

void initServer()
{
    server.mainthread = pthread_self();
    server.el = aeCreateEventLoop(1024 * 10);
    server.port = 6379;
    server.bindaddr = "127.0.0.1";
    server.logfile = NULL;
    server.stat_connections = 0;
    server.db = zmalloc(sizeof(taskDb));
    server.db->dict = dictCreate(&dbDictType, NULL);
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    server.clients = listCreate();
    createSharedObjects();
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "task tcp open err:%s", server.neterr);
        exit(1);
    }
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE, acceptHandler, NULL) == AE_ERR) {
        redisLog(REDIS_WARNING, "ae read err:%s", server.fd);
        exit(1);
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcasecmp(argv[1], "--daemonize") == 0)
        daemonize();
    initServer();
    redisLog(REDIS_NOTICE, "The server is now ready to accept connections on port %d", server.port);
    aeMain(server.el);
    return 0;
}

void daemonize(void)
{
    int fd;
    if (fork() != 0)
        exit(0);

    setsid();

    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
    }
}

void readQueryFromClient(aeEventLoop* el, int fd, void* privdata, int mask)
{

    taskClient* c = (taskClient*)privdata;
    char buf[REDIS_IOBUF_LEN];
    int nread;

    nread = read(fd, buf, REDIS_IOBUF_LEN);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        }
        else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }
    else if (nread == 0) {
        redisLog(REDIS_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }
    if (nread) {
        c->querybuf = sdscatlen(c->querybuf, buf, nread);
    }
    else {
        return;
    }
    processInputBuffer(c);
}

void processInputBuffer(taskClient* c)
{
    char* newline = NULL;
    int pos = 0, ok;
    long long ll;

    if (c->multibulklen == 0) {
        newline = strchr(c->querybuf, '\r');
        ok = string2ll(c->querybuf + 1, newline - (c->querybuf + 1), &ll);
        pos = (newline - c->querybuf) + 2;
        c->multibulklen = ll;

        if (c->argv)
            zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*) * c->multibulklen);
    }

    while (c->multibulklen) {
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf + pos, '\r');
            if (newline - (c->querybuf) > ((signed)sdslen(c->querybuf) - 2))
                break;
            ok = string2ll(c->querybuf + pos + 1, newline - (c->querybuf + pos + 1), &ll);
            pos += newline - (c->querybuf + pos) + 2;
            c->bulklen = ll;
        }
        if (sdslen(c->querybuf) - pos < (unsigned)(c->bulklen + 2)) {
            break;
        }
        else {
            c->argv[c->argc++] = createObject(OBJ_STRING, sdsnewlen(c->querybuf + pos, c->bulklen));
            pos += c->bulklen + 2;
            c->bulklen = -1;
            c->multibulklen--;
        }
    }
    /* Trim to pos */
    if (pos)
        sdsrange(c->querybuf, pos, -1);
    processCommand(c);
}

int processCommand(taskClient* c)
{
    struct taskCommand* cmd;
    if (!strcasecmp(c->argv[0]->ptr, "quit")) {
        freeClient(c);
        return 0;
    }
    cmd = lookupCommand(c->argv[0]->ptr);

    if (!cmd) {
        addReplySds(c, sdscatprintf(sdsempty(), "-ERR unknown command '%s'\r\n", (char*)c->argv[0]->ptr));
        resetClient(c);
        return 1;
    }
    call(c, cmd);
    resetClient(c);
    return 1;
}

struct taskCommand* lookupCommand(char* cmd)
{
    for (int i = 0; i < REDIS_CMD_NUM; i++) {
        if (!strcasecmp(cmdTable[i].name, cmd))
            return &cmdTable[i];
    }
    return NULL;
}
void call(taskClient* c, struct taskCommand* cmd) { cmd->proc(c); }

void freeClient(taskClient* c)
{
    listRelease(c->reply);
    close(c->fd);
    zfree(c);
}

taskClient* createClient(int fd)
{
    taskClient* c = zmalloc(sizeof(*c));

    anetNonBlock(NULL, fd);
    anetTcpNoDelay(NULL, fd);
    if (!c)
        return NULL;

    c->fd = fd;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->sentlen = 0;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->db = server.db;
    c->reply = listCreate();
    listSetFreeMethod(c->reply, decrRefCount);

    if (aeCreateFileEvent(server.el, c->fd, AE_READABLE, readQueryFromClient, c) == AE_ERR) {
        freeClient(c);
        return NULL;
    }
    listAddNodeTail(server.clients, c);
    return c;
}

void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask)
{
    int cport, cfd;
    char cip[128];
    taskClient* c;

    cfd = anetAccept(server.neterr, fd, cip, &cport);
    if (cfd == AE_ERR) {
        redisLog(REDIS_VERBOSE, "Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE, "Accepted %s:%d %d", cip, cport, cfd);
    if ((c = createClient(cfd)) == NULL) {
        redisLog(REDIS_WARNING, "Error allocating client: %s", strerror(errno));
        close(cfd);
        return;
    }

    server.stat_connections++;
}

void redisLog(int level, const char* fmt, ...)
{
    va_list ap;
    FILE* fp;

    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile, "a");
    if (!fp)
        return;

    va_start(ap, fmt);
    char* c = ".-*#";
    char buf[64];
    time_t now;

    now = time(NULL);

    strftime(buf, 64, "%d %b %H:%M:%S", localtime(&now));
    fprintf(fp, "[%d] %s %c ", (int)getpid(), buf, c[level]);
    vfprintf(fp, fmt, ap);
    fprintf(fp, "\n");
    fflush(fp);

    va_end(ap);

    if (server.logfile)
        fclose(fp);
}

robj* createObject(int type, void* ptr)
{
    robj* o;
    o = zmalloc(sizeof(*o));
    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    return o;
}

void getCommand(taskClient* c) { getGenericCommand(c); }
void delCommand(taskClient* c) { delGenericCommand(c); }

int getGenericCommand(taskClient* c)
{
    robj* o;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL)
        return REDIS_OK;

    if (o->type != REDIS_STRING) {
        addReply(c, shared.wrongtypeerr);
        return REDIS_ERR;
    }
    else {
        addReplyBulk(c, o);
        return REDIS_OK;
    }
}

int delGenericCommand(taskClient* c)
{
    long long timeId = atoi(c->argv[1]->ptr);
    aeDeleteTimeEvent(server.el, timeId);
    addReply(c, shared.ok);
    return REDIS_OK;
}

void addReply(taskClient* c, robj* obj)
{
    if (listLength(c->reply) == 0 && aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR) {
        return;
    }
    listAddNodeTail(c->reply, getDecodedObject(obj));
}

void addReplytoWorker(timeEventObject* tobj, robj* obj) { addReplyBulkList(tobj->message, obj); }

void sendReplyToClient(aeEventLoop* el, int fd, void* privdata, int mask)
{
    taskClient* c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    robj* o;

    while (listLength(c->reply)) {
        o = listNodeValue(listFirst(c->reply));
        objlen = sdslen(o->ptr);

        if (objlen == 0) {
            listDelNode(c->reply, listFirst(c->reply));
            continue;
        }
        nwritten = write(fd, ((char*)o->ptr) + c->sentlen, objlen - c->sentlen);
        if (nwritten <= 0)
            break;
        c->sentlen += nwritten;
        totwritten += nwritten;
        if (c->sentlen == objlen) {
            listDelNode(c->reply, listFirst(c->reply));
            c->sentlen = 0;
        }
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT)
            break;
    }

    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        }
        else {
            redisLog(REDIS_VERBOSE, "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }

    if (listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
    }
}

robj* lookupKey(taskDb* db, robj* key)
{
    dictEntry* de = dictFind(db->dict, key);
    if (de) {
        return dictGetEntryVal(de);
    }
    else {
        return NULL;
    }
}

void setCommand(taskClient* c) { setGenericCommand(c, 0, c->argv[1], c->argv[2], NULL); }

void callWorker(char* addr, int port, list* msg)
{
    char* err = zmalloc(100 * sizeof(char));
    int fd;
    if ((fd = anetTcpConnect(err, addr, port)) == ANET_ERR) {
        redisLog(REDIS_ERR, "connect error: %s\n", err);
        zfree(err);
        return;
    }

    listIter* iter = listGetIterator(msg, AL_START_HEAD);
    listNode* node = NULL;
    int nwritten = 0, totwritten = 0, objlen, sentlen = 0;
    robj* o;

    while (node = listNext(iter)) {
        o = listNodeValue(node);

        objlen = sdslen(o->ptr);
        if (objlen == 0) {
            continue;
        }
        nwritten = write(fd, ((char*)o->ptr) + sentlen, objlen - sentlen);
        if (nwritten <= 0)
            break;
        sentlen += nwritten;
        totwritten += nwritten;
        if (sentlen == objlen) {
            sentlen = 0;
        }
    }
    close(fd);
}

int setGenericCommand(taskClient* c, int nx, robj* key, robj* val, robj* expire)
{
    if (dictFind(c->db->dict, key) != NULL) {
        dictReplace(c->db->dict, key, val);
        incrRefCount(val);
        addReply(c, shared.ok);
        return REDIS_ERR;
    }
    else {
        sds copy = sdsdup(key->ptr);
        dictAdd(c->db->dict, createObject(REDIS_STRING, copy), val);
        incrRefCount(val);
        addReply(c, shared.ok);
        return REDIS_OK;
    }
}

void resetClient(taskClient* c) { freeClientArgv(c); }

void freeClientArgv(taskClient* c)
{
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
}

void addReplySds(taskClient* c, sds s)
{
    robj* o = createObject(REDIS_STRING, s);
    addReply(c, o);
    decrRefCount(o);
}

void addReplyList(list* l, robj* obj) { listAddNodeTail(l, getDecodedObject(obj)); }

robj* lookupKeyReadOrReply(taskClient* c, robj* key, robj* reply)
{
    robj* o = lookupKeyRead(c->db, key);
    if (!o)
        addReply(c, reply);
    return o;
}

void addReplyBulk(taskClient* c, robj* obj)
{
    addReplyBulkLen(c, obj);
    addReply(c, obj);
    addReply(c, shared.crlf);
}

void addReplyBulkList(list* l, robj* obj)
{
    addReplyBulkLenList(l, obj);
    addReplyList(l, obj);
    addReplyList(l, shared.crlf);
}

void addReplyBulkLen(taskClient* c, robj* obj)
{
    size_t len, intlen;
    char buf[128];
    if (obj->encoding == REDIS_ENCODING_RAW) {
        len = sdslen(obj->ptr);
    }
    else {
        long n = (long)obj->ptr;
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while ((n = n / 10) != 0) {
            len++;
        }
    }
    buf[0] = '$';
    intlen = ll2string(buf + 1, sizeof(buf) - 1, (long long)len);
    buf[intlen + 1] = '\r';
    buf[intlen + 2] = '\n';
    addReplySds(c, sdsnewlen(buf, intlen + 3));
}

void addReplyBulkLenList(list* l, robj* obj)
{
    size_t len, intlen;
    char buf[128];

    if (obj->encoding == REDIS_ENCODING_RAW) {
        len = sdslen(obj->ptr);
    }
    else {
        long n = (long)obj->ptr;
        len = 1;
        if (n < 0) {
            len++;
            n = -n;
        }
        while ((n = n / 10) != 0) {
            len++;
        }
    }
    buf[0] = '$';
    intlen = ll2string(buf + 1, sizeof(buf) - 1, (long long)len);
    buf[intlen + 1] = '\r';
    buf[intlen + 2] = '\n';
    addReplyList(l, createObject(REDIS_STRING, sdsnewlen(buf, intlen + 3)));
}

void createSharedObjects(void)
{
    shared.crlf = createObject(REDIS_STRING, sdsnew("\r\n"));
    shared.nullbulk = createObject(REDIS_STRING, sdsnew("$-1\r\n"));
    shared.wrongtypeerr = createObject(REDIS_STRING, sdsnew("-Operation against a key holding the wrong kind of value\r\n"));
    shared.ok = createObject(REDIS_STRING, sdsnew("+OK\r\n"));
    shared.notfound = createObject(REDIS_STRING, sdsnew("-key not found\r\n"));
}

robj* lookupKeyRead(taskDb* db, robj* key) { return lookupKey(db, key); }

unsigned int dictObjHash(const void* key)
{
    const robj* o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

int dictObjKeyCompare(void* privdata, const void* key1, const void* key2)
{
    const robj *o1 = key1, *o2 = key2;
    return sdsDictKeyCompare(privdata, o1->ptr, o2->ptr);
}

void dictRedisObjectDestructor(void* privdata, void* val)
{
    if (val == NULL)
        return;
    decrRefCount(val);
}

void decrRefCount(void* obj)
{
    robj* o = obj;
    if (--(o->refcount) == 0) {
        switch (o->type) {
        case REDIS_STRING:
            freeStringObject(o);
            break;
        case REDIS_LIST:
            freeListObject(o);
            break;
        }
        zfree(o);
    }
}

void freeListObject(robj* o) { listRelease((list*)o->ptr); }

void freeStringObject(robj* obj)
{
    if (obj->encoding == REDIS_ENCODING_RAW) {
        sdsfree(obj->ptr);
    }
}

int sdsDictKeyCompare(void* privdata, const void* key1, const void* key2)
{
    int l1, l2;
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

robj* getDecodedObject(robj* o)
{
    robj* dec;
    if (o->encoding == REDIS_ENCODING_RAW) {
        incrRefCount(o);
        return o;
    }
}

void incrRefCount(robj* o) { o->refcount++; }

void rpcCommand(taskClient* c)
{
    timeEventObject* obj = zmalloc(sizeof(timeEventObject));
    char* split = strchr(c->argv[3]->ptr, ':');
    obj->port = atoi(split + 1);
    obj->addr = sdsnewlen(c->argv[3]->ptr, split - (char*)c->argv[3]->ptr);

    long long milliseconds;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    milliseconds = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    long long eventMilliSeconds = atoll(c->argv[2]->ptr);
    if (eventMilliSeconds > milliseconds)
        obj->ttl = eventMilliSeconds - milliseconds;
    else
        obj->ttl = eventMilliSeconds;

    obj->message = listCreate();
    obj->type = strcasecmp("once", c->argv[1]->ptr) == 0 ? TASK_ONCE : TASK_REPEAT;
    robj* buf = createObject(REDIS_STRING, sdsdup(c->argv[4]->ptr));
    addReplytoWorker(obj, getDecodedObject(buf));
    long long timeId;
    char time[33];
    if ((timeId = aeCreateTimeEvent(server.el, obj->ttl, notifyWorker, obj, NULL)) == AE_ERR) {
        redisLog(REDIS_NOTICE, "redis create task failed\n");
    }
    sprintf(time, "%lld", timeId);
    addReply(c, createObject(REDIS_STRING, sdscatprintf(sdsempty(), "+OK timeEventId:%s\r\n", time)));
}

int notifyWorker(struct aeEventLoop* eventLoop, long long id, void* clientData)
{
    timeEventObject* obj = clientData;
    callWorker(obj->addr, obj->port, obj->message);
    if (obj->type != TASK_ONCE) {
        return obj->ttl;
    }
    listRelease(obj->message);
    zfree(obj);
    return AE_NOMORE;
}
