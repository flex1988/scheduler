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
} foolClient;

static foolServer server;

// prototype
static void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask);
static void redisLog(int level, const char* fmt, ...);
static void readQueryFromCLient(aeEventLoop* el, int fd, void* privdata, int mask);
static void freeClient(foolClient* c);

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
    // processInputBuffer(c);
}

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
