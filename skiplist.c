#include <stdlib.h>
#include "skiplist.h"
#include "zmalloc.h"

skiplist* createSkiplist(void)
{
    skiplist* sl;
    int j;
    sl = zmalloc(sizeof(*sl));
    sl->level = 1;
    sl->length = 0;
    sl->header = createSkiplistNode(SKIPLIST_MAXLEVEL, 0, NULL);
    for (j = 0; j < SKIPLIST_MAXLEVEL; j++) {
        sl->header->level[j].forward = NULL;
        sl->header->level[j].span = 0;
    }
    return sl;
}

skiplistNode* createSkiplistNode(int level, double score, void* obj)
{
    skiplistNode* node = zmalloc(sizeof(*node) + level * sizeof(struct skiplistLevel));
    node->obj = obj;
    node->score = score;
    return node;
}

void freeSkiplist(skiplist* sl)
{
    skiplistNode *node = sl->header->level[0].forward, *next;
    zfree(sl->header);
    while (node) {
        next = node->level[0].forward;
        freeSkiplistNode(node);
        node = next;
    }
    zfree(sl);
}

void freeSkiplistNode(skiplistNode* node)
{
    // node->obj must not be referenced
    zfree(node->obj);
    zfree(node);
}

int skiplistRandomLevel(void)
{
    int level = 1;
    while ((random() & 0xFFFF) < (SKIPLIST_P * 0xFFFF))
        level += 1;
    return (level < SKIPLIST_MAXLEVEL) ? level : SKIPLIST_MAXLEVEL;
}
