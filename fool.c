#include "fool.h"
#include "ae.h"
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3

typedef struct foolServer {
    pthread_t mainthread;
    int port;
    int fd;
    aeEventLoop* el;
    char neterr[1024];
    char* bindaddr;
    char* logfile;
} foolServer;

static foolServer server;

// prototype
static void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask);
static void redisLog(int level, const char* fmt, ...);

static void initServer()
{
    server.mainthread = pthread_self();
    server.el = aeCreateEventLoop();
    server.port = 6379;
    server.bindaddr = "127.0.0.1";
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    server.logfile = NULL;
    if (server.fd == -1) {
        sprintf(stderr, "fool tcp open err:%s", server.neterr);
        exit(1);
    }
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE, acceptHandler, NULL) == AE_ERR) {
        sprintf(stderr, "ae read err:%s", server.fd);
        exit(1);
    }
}

static void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask)
{
    int cport, cfd;
    char cip[128];

    cfd = anetAccept(server.neterr, fd, cip, &cport);
    if (cfd == AE_ERR) {
        redisLog(REDIS_VERBOSE, "Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_VERBOSE, "Accepted %s:%d", cip, cport);
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
