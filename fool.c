#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#include "fool.h"
#include "ae.h"
#include "sds.h"
#include "zmalloc.h"
#include "dict.h"
#include "adlist.h"
#include "util.h"

/* Error codes */
#define REDIS_OK 0
#define REDIS_ERR -1

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3

#define REDIS_IOBUF_LEN 1024
#define REDIS_REQUEST_MAX_SIZE (1024 * 1024 * 256) /* max bytes in inline command */
#define REDIS_MAX_WRITE_PER_EVENT (1024 * 64)

/* Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define REDIS_ENCODING_RAW 0    /* Raw representation */
#define REDIS_ENCODING_INT 1    /* Encoded as integer */
#define REDIS_ENCODING_ZIPMAP 2 /* Encoded as zipmap */
#define REDIS_ENCODING_HT 3     /* Encoded as an hash table */

/* Command flags */
#define REDIS_CMD_BULK 1   /* Bulk write command */
#define REDIS_CMD_INLINE 2 /* Inline command */
                           /* REDIS_CMD_DENYOOM reserves a longer comment: all the commands marked with
                              this flags will return an error when the 'maxmemory' option is set in the
                              config file and the server is using more than maxmemory bytes of memory.
                              In short this commands are denied on low memory conditions. */
#define REDIS_CMD_DENYOOM 4
#define REDIS_CMD_FORCE_REPLICATION 8 /* Force replication even if dirty is 0 */
#define REDIS_CMD_NUM 2

typedef struct foolObject {
    void* ptr;
    unsigned char type;
    unsigned char encoding;
    unsigned char storage;
    unsigned char vtypes;
    int refcount;
} robj;

typedef struct foolDb {
    dict* dict;
    int id;
} foolDb;

typedef struct foolServer {
    pthread_t mainthread;
    int port;
    int fd;
    int stat_connections;
    aeEventLoop* el;
    char neterr[1024];
    char* bindaddr;
    char* logfile;
    list* clients;
    foolDb* db;
} foolServer;

typedef struct foolClient {
    int fd;
    sds querybuf;
    int argc;
    robj** argv;
    list* reply;
    int sentlen;
    int multibulklen;
    int bulklen;
    foolDb* db;
} foolClient;

struct sharedObjectStruct {
    robj *crlf, *nullbulk, *wrongtypeerr;
} shared;

typedef void foolCommandProc(foolClient* c);
typedef struct foolCommand {
    char* name;
    foolCommandProc* proc;
    int arity;
    int flags;
};

static foolServer server;

// prototype
static void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask);
static void redisLog(int level, const char* fmt, ...);
static void call(foolClient* c, struct foolCommand* cmd);
static void readQueryFromCLient(aeEventLoop* el, int fd, void* privdata, int mask);
static void freeClient(foolClient* c);
static void processInputBuffer(foolClient* c);
static int processCommand(foolClient* c);
static robj* createObject(int type, void* ptr);
static void addReply(foolClient* c, robj* obj);
static void sendReplyToClient(aeEventLoop* el, int fd, void* privdata, int mask);
static robj* lookupKey(foolDb* db, robj* key);
static void addReplyBulk(foolClient* c, robj* obj);
static struct foolCommand* lookupCommand(char* cmd);
static int getGenericCommand(foolClient* c);
static void getCommand(foolClient* c);
static void setCommand(foolClient* c);
static void resetClient(foolClient* c);
static void addReplySds(foolClient* c, sds s);
static robj* lookupKeyReadOrReply(foolClient* c, robj* key, robj* reply);
static void createSharedObjects(void);
static void addReplyBulkLen(foolClient* c, robj* obj);
static robj* lookupKeyRead(foolDb* db, robj* key);
static unsigned int dictObjHash(const void* key);
static int dictObjKeyCompare(void* privdata, const void* key1, const void* key2);
static void dictRedisObjectDestructor(void* privdata, void* val);
static void decrRefCount(void* o);
static void freeStringObject(robj* o);
static int sdsDictKeyCompare(void* privdata, const void* key1, const void* key2);
static robj* getDecodedObject(robj* o);
static void incrRefCount(robj* o);
static void freeClientArgv(foolClient* c);
static void freeListObject(robj* o);

static struct foolCommand cmdTable[] = { { "get", getCommand, 2, REDIS_CMD_INLINE, NULL, 1, 1, 1 }, { "set", setCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM, NULL, 0, 0, 0 } };

static dictType dbDictType = { dictObjHash, NULL, NULL, dictObjKeyCompare, dictRedisObjectDestructor, dictRedisObjectDestructor };

static void initServer()
{
    server.mainthread = pthread_self();
    server.el = aeCreateEventLoop();
    server.port = 6379;
    server.bindaddr = "127.0.0.1";
    server.logfile = NULL;
    server.stat_connections = 0;
    server.db = zmalloc(sizeof(foolDb));
    server.db->dict = dictCreate(&dbDictType, NULL);
    robj* key = createObject(REDIS_STRING, sdsnew("hello"));
    robj* val = createObject(REDIS_STRING, sdsnew("world!"));
    dictAdd(server.db->dict, key, val);
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    server.clients = listCreate();
    if (server.fd == -1) {
        sprintf(stderr, "fool tcp open err:%s", server.neterr);
        exit(1);
    }
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE, acceptHandler, NULL) == AE_ERR) {
        sprintf(stderr, "ae read err:%s", server.fd);
        exit(1);
    }
}

int main(int argc, char** argv)
{
    initServer();
    redisLog(REDIS_NOTICE, "The server is now ready to accept connections on port %d", server.port);
    aeMain(server.el);
    return 0;
}

static void readQueryFromClient(aeEventLoop* el, int fd, void* privdata, int mask)
{

    foolClient* c = (foolClient*)privdata;
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

static void processInputBuffer(foolClient* c)
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

static int processCommand(foolClient* c)
{

    struct foolCommand* cmd;
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

static struct foolCommand* lookupCommand(char* cmd)
{
    for (int i = 0; i < REDIS_CMD_NUM; i++) {
        if (!strcasecmp(cmdTable[i].name, cmd))
            return &cmdTable[i];
    }
    return NULL;
}
static void call(foolClient* c, struct foolCommand* cmd) { cmd->proc(c); }

static void freeClient(foolClient* c)
{
    listRelease(c->reply);
    close(c->fd);
    zfree(c);
}

static foolClient* createClient(int fd)
{
    foolClient* c = zmalloc(sizeof(*c));

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

static void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask)
{
    int cport, cfd;
    char cip[128];
    foolClient* c;

    cfd = anetAccept(server.neterr, fd, cip, &cport);
    if (cfd == AE_ERR) {
        redisLog(REDIS_VERBOSE, "Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE, "Accepted %s:%d", cip, cport);
    if ((c = createClient(cfd)) == NULL) {
        redisLog(REDIS_WARNING, "Error allocating resources for the client");
        close(cfd);
        return;
    }

    server.stat_connections++;
}

static void redisLog(int level, const char* fmt, ...)
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

static robj* createObject(int type, void* ptr)
{
    robj* o;
    o = zmalloc(sizeof(*o));
    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    return o;
}

static void getCommand(foolClient* c) { getGenericCommand(c); }

static int getGenericCommand(foolClient* c)
{

    robj* o;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL)
        return REDIS_OK;

    if (o->type != REDIS_STRING) {
        addReply(c, shared.wrongtypeerr);
        return REDIS_ERR;
    }
    else {
        redisLog(REDIS_NOTICE, "%s\n", o->ptr);
        addReplyBulk(c, o);
        return REDIS_OK;
    }
}

static void addReply(foolClient* c, robj* obj)
{
    redisLog(REDIS_NOTICE, "%s\n", obj->ptr);
    if (listLength(c->reply) == 0 && aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR) {
        return;
    }
    listAddNodeTail(c->reply, getDecodedObject(obj));
}

static void sendReplyToClient(aeEventLoop* el, int fd, void* privdata, int mask)
{

    foolClient* c = privdata;
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

static robj* lookupKey(foolDb* db, robj* key)
{
    dictEntry* de = dictFind(db->dict, key);
    if (de) {
        return dictGetEntryVal(de);
    }
    else {
        return NULL;
    }
}

static void setCommand(foolClient* c) {}

static void resetClient(foolClient* c) { freeClientArgv(c); }

static void freeClientArgv(foolClient* c)
{
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
}

static void addReplySds(foolClient* c, sds s)
{
    robj* o = createObject(REDIS_STRING, s);
    addReply(c, o);
    decrRefCount(o);
}

static robj* lookupKeyReadOrReply(foolClient* c, robj* key, robj* reply)
{
    robj* o = lookupKeyRead(c->db, key);
    if (!o)
        addReply(c, reply);
    return o;
}

static void addReplyBulk(foolClient* c, robj* obj)
{
    addReplyBulkLen(c, obj);
    addReply(c, obj);
    addReply(c, shared.crlf);
}

static void addReplyBulkLen(foolClient* c, robj* obj)
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

static void createSharedObjects(void)
{
    shared.crlf = createObject(REDIS_STRING, sdsnew("\r\n"));
    shared.nullbulk = createObject(REDIS_STRING, sdsnew("$-1\r\n"));
    shared.wrongtypeerr = createObject(REDIS_STRING, sdsnew("-ERR Operation against a key holding the wrong kind of value\r\n"));
}

static robj* lookupKeyRead(foolDb* db, robj* key) { return lookupKey(db, key); }

static unsigned int dictObjHash(const void* key)
{
    const robj* o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

static int dictObjKeyCompare(void* privdata, const void* key1, const void* key2)
{
    const robj *o1 = key1, *o2 = key2;
    return sdsDictKeyCompare(privdata, o1->ptr, o2->ptr);
}

static void dictRedisObjectDestructor(void* privdata, void* val)
{
    if (val == NULL)
        return;
    decrRefCount(val);
}

static void decrRefCount(void* obj)
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

static void freeListObject(robj* o) { listRelease((list*)o->ptr); }

static void freeStringObject(robj* obj)
{
    if (obj->encoding == REDIS_ENCODING_RAW) {
        sdsfree(obj->ptr);
    }
}

static int sdsDictKeyCompare(void* privdata, const void* key1, const void* key2)
{
    int l1, l2;
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

static robj* getDecodedObject(robj* o)
{
    robj* dec;
    if (o->encoding == REDIS_ENCODING_RAW) {
        incrRefCount(o);
        return o;
    }
}

static void incrRefCount(robj* o) { o->refcount++; };
