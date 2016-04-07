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

typedef struct foolServer {
    pthread_t mainthread;
    int port;
    int fd;
    int stat_connections;
    aeEventLoop* el;
    char neterr[1024];
    char* bindaddr;
    char* logfile;
} foolServer;

typedef struct foolClient {
    int fd;
    sds querybuf;
    int argc;
    robj** argv;
    list* reply;
} foolClient;

typedef struct foolDb {
    dict* dict;
    int id;
} foolDb;

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

static struct foolCommand cmdTable[] = { { "get", getCommand, 2, REDIS_CMD_INLINE, NULL, 1, 1, 1 }, { "set", setCommand, 3, REDIS_CMD_BULK | REDIS_CMD_DENYOOM, NULL, 0, 0, 0 } };

static void initServer()
{
    server.mainthread = pthread_self();
    server.el = aeCreateEventLoop();
    server.port = 6379;
    server.bindaddr = "127.0.0.1";
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    server.logfile = NULL;
    server.stat_connections = 0;
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

again : {

    char* p = strchr(c->querybuf, '\n');
    size_t querylen;

    if (p) {
        sds query, *argv;
        int argc, j;

        query = c->querybuf;
        c->querybuf = sdsempty();
        querylen = 1 + (p - (query));
        if (sdslen(query) > querylen) {
            c->querybuf = sdscatlen(c->querybuf, query + querylen, sdslen(query) - querylen);
        }
        *p = '\0';
        if (*(p - 1) == '\r')
            *(p - 1) = '\0';
        sdsupdatelen(query);
        argv = sdssplitlen(query, sdslen(query), " ", 1, &argc);
        sdsfree(query);

        if (c->argv)
            zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*) * argc);
        for (j = 0; j < argc; j++) {
            if (sdslen(argv[j])) {
                c->argv[c->argc] = createObject(REDIS_STRING, argv[j]);
                c->argc++;
            }
            else {
                sdsfree(argv[j]);
            }
        }
        zfree(argv);
        if (c->argc) {
            // execute command
            if (processCommand(c) && sdslen(c->querybuf))
                goto again;
        }
        else {
            if (sdslen(c->querybuf))
                goto again;
        }
        return;
    }
    else if (sdslen(c->querybuf) >= REDIS_REQUEST_MAX_SIZE) {
        redisLog(REDIS_VERBOSE, "Client protocol error");
        freeClient(c);
        return;
    }
}
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
        addReplySds(c, sdscatprintf(sdsempty(), "", (char*)c->argv[0]->ptr));
        resetClient(c);
        return 1;
    }
    call(c, cmd);
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

static void freeClient(foolClient* c) { free(c); }

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

    if (aeCreateFileEvent(server.el, c->fd, AE_READABLE, readQueryFromClient, c) == AE_ERR) {
        freeClient(c);
        return NULL;
    }
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
    /*if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL)*/
    /*return REDIS_OK;*/

    /*    if (o->type != REDIS_STRING) {*/
    /*addReply(c, shared.wrongtypeerr);*/
    /*return REDIS_ERR;*/
    /*}*/
    /*else {*/
    addReplyBulk(c, o);
    return REDIS_OK;
    /*}*/
}

static void addReply(foolClient* c, robj* obj)
{
    if (aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR)
        return;
}
static void addReplyBulk(foolClient* c, robj* obj)
{
    // addReplyBulkLen(c, obj);
    addReply(c, obj);
    // addReply(c, shared.crlf);
}

static void sendReplyToClient(aeEventLoop* el, int fd, void* privdata, int mask)
{
    foolClient* c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    robj* o;
    char* msg = "-hello world!\r\n";
    nwritten = write(fd, msg, strlen(msg) * sizeof(char));
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

static void resetClient(foolClient* c) {}

static void addReplySds(foolClient* c, sds s)
{
    robj* o = createObject(REDIS_STRING, s);
    addReplyBulk(c, o);
}
