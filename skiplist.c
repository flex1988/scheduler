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
