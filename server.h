#ifndef __SERVER_H__
#define __SERVER_H__

#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "ae.h"
#include "sds.h"
#include "zmalloc.h"
#include "dict.h"
#include "adlist.h"
#include "util.h"
#include "anet.h"

/* Object types */
#define OBJ_STRING 0
#define OBJ_LIST 1
#define OBJ_SET 2
#define OBJ_ZSET 3
#define OBJ_HASH 4

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
#define REDIS_CMD_NUM 3

#define TASK_ONCE 1
#define TASK_REPEAT 2

typedef struct taskObject {
    void* ptr;
    unsigned char type;
    unsigned char encoding;
    unsigned char storage;
    unsigned char vtypes;
    int refcount;
} robj;

typedef struct taskDb {
    dict* dict;
    int id;
} taskDb;

typedef struct taskServer {
    pthread_t mainthread;
    int port;
    int fd;
    int stat_connections;
    aeEventLoop* el;
    char neterr[1024];
    char* bindaddr;
    char* logfile;
    list* clients;
    taskDb* db;
} taskServer;

typedef struct taskClient {
    int fd;
    sds querybuf;
    int argc;
    robj** argv;
    list* reply;
    int sentlen;
    int multibulklen;
    int bulklen;
    taskDb* db;
} taskClient;

struct sharedObjectStruct {
    robj *crlf, *nullbulk, *wrongtypeerr, *ok,*notfound;
} shared;

typedef struct timeEventObject {
    int port;
    sds addr;
    int ttl;
    int type;
    list* message;
} timeEventObject;

typedef void taskCommandProc(taskClient* c);
typedef struct taskCommand {
    char* name;
    taskCommandProc* proc;
    int arity;
    int flags;
};

void acceptHandler(aeEventLoop* el, int fd, void* privdata, int mask);
void redisLog(int level, const char* fmt, ...);
void call(taskClient* c, struct taskCommand* cmd);
void readQueryFromCLient(aeEventLoop* el, int fd, void* privdata, int mask);
void freeClient(taskClient* c);
void processInputBuffer(taskClient* c);
int processCommand(taskClient* c);
robj* createObject(int type, void* ptr);
void addReply(taskClient* c, robj* obj);
void sendReplyToClient(aeEventLoop* el, int fd, void* privdata, int mask);
robj* lookupKey(taskDb* db, robj* key);
void addReplyBulk(taskClient* c, robj* obj);
struct taskCommand* lookupCommand(char* cmd);
int getGenericCommand(taskClient* c);
void getCommand(taskClient* c);
void setCommand(taskClient* c);
void rpcCommand(taskClient* c);
void resetClient(taskClient* c);
void addReplySds(taskClient* c, sds s);
robj* lookupKeyReadOrReply(taskClient* c, robj* key, robj* reply);
void createSharedObjects(void);
void addReplyBulkLen(taskClient* c, robj* obj);
robj* lookupKeyRead(taskDb* db, robj* key);
unsigned int dictObjHash(const void* key);
int dictObjKeyCompare(void* privdata, const void* key1, const void* key2);
void dictRedisObjectDestructor(void* privdata, void* val);
void decrRefCount(void* o);
void freeStringObject(robj* o);
int sdsDictKeyCompare(void* privdata, const void* key1, const void* key2);
robj* getDecodedObject(robj* o);
void incrRefCount(robj* o);
void freeClientArgv(taskClient* c);
void freeListObject(robj* o);
int setGenericCommand(taskClient* c, int nx, robj* key, robj* val, robj* expire);
int notifyWorker(struct aeEventLoop* eventLoop, long long id, void* clientData);
void callWorker(char* addr, int port, list* message);
void addReplyBulkList(list* l,robj* obj);
void addReplyBulkLenList(list *l,robj* obj);
void daemonize(void);
#endif
