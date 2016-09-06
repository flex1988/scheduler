#ifndef __SKIPLIST_H__
#define __SKIPLIST_H__

#define SKIPLIST_MAXLEVEL 32 /* Should be enough for 2^32 elements */
#define SKIPLIST_P 0.25      /* Skiplist P = 1/4 */
#define SKIP_OK 0
#define SKIP_ERR -1

#include "dict.h"
typedef struct skiplistNode
{
    void* obj;
    long long score;
    long long id;
    struct skiplistLevel
    {
        struct skiplistNode* forward;
        unsigned int span;
    } level[];
} skiplistNode;

typedef struct skiplist
{
    struct skiplistNode* header;
    int (*compare)(void* a, void* b);
    unsigned long length;
    int level;
    dict* dict;
} skiplist;

skiplist* createSkiplist(void);
skiplistNode* createSkiplistNode(int level, long long score, void* obj,
                                 long long id);
void freeSkiplist(skiplist* sl);
void freeSkiplistNode(skiplistNode* node);
skiplistNode* skiplistInsert(skiplist* sl, long long score, void* obj,
                             long long id);
int skiplistDelete(skiplist* sl, long long score, long long id);
int skiplistDeleteHeader(skiplist* sl);
#endif
