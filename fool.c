#include "fool.h"
#include "ae.h"
#include <pthread.h>

typedef struct foolServer {
    pthread_t mainthread;
    int port;
    int fd;
    aeEventLoop* el;
    char neterr[1024];
} foolServer;

static foolServer server;

static void initServer()
{
    server.mainthread = pthread_self();
    server.el = aeCreateEventLoop();
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
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
    aeMain(server.el);
    return 0;
}
