#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#include "fool.h"
#include "ae.h"
#include "sds.h"
#include "zmalloc.h"

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
} foolClient;

static foolServer server;

// prototype
static void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask);
static void redisLog(int level, const char* fmt, ...);
static void readQueryFromCLient(aeEventLoop* el, int fd, void* privdata, int mask);
static void freeClient(foolClient* c);
static void processInputBuffer(foolClient* c);
static int processCommand(foolClient* c);
static robj* createObject(int type, void* ptr);

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

static int processCommand(foolClient* c) { redisLog(REDIS_VERBOSE, "process command\n"); }
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

int main(int argc, char** argv)
{
    initServer();
    redisLog(REDIS_NOTICE, "The server is now ready to accept connections on port %d", server.port);
    aeMain(server.el);
    return 0;
}

static robj* createObject(int type, void* ptr)
{
    robj* o;
    o->type = type;
    o->encoding = REDIS_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    return o;
}
