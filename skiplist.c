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

skiplistNode* skiplistInsert(skiplist* sl, double score, void* obj)
{
    skiplistNode *update[SKIPLIST_MAXLEVEL], *x;
    unsigned int rank[SKIPLIST_MAXLEVEL];
    int i, level;

    x = sl->header;
    for (i = sl->level - 1; i >= 0; i--) {
        rank[i] = i == (sl->level - 1) ? 0 : rank[i + 1];
        while (x->level[i].forward && x->level[i].forward->score < score) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    level = skiplistRandomLevel();
    if (level > sl->level) {
        for (i = sl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = sl->header;
            update[i]->level[i].span = sl->length;
        }
        sl->level = level;
    }

    x = createSkiplistNode(level, score, obj);
    for (i = 0; i < level; i++) {
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;

        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }

    for (i = level; i < sl->level; i++) {
        update[i]->level[i].span++;
    }
    sl->length++;
    return x;
}

void skiplistDeleteNode(skiplist* sl, skiplistNode* x, skiplistNode** update)
{
    int i;
    for (i = 0; i < sl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        }
        else {
            update[i]->level[i].span -= 1;
        }
    }
    while (sl->level > 1 && sl->header->level[sl->level - 1].forward == NULL)
        sl->level--;
    sl->length--;
}

int skiplistDelete(skiplist* sl, double score, void* obj)
{
    skiplistNode *update[SKIPLIST_MAXLEVEL], *x;
    int i;

    x = sl->header;
    for (i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && x->level[i].forward->score < score)
            x = x->level[i].forward;
        update[i] = x;
    }
    x = x->level[0].forward;
    if (x && score == x->score) {
        skiplistDeleteNode(sl, x, update);
        freeSkiplistNode(x);
        return 1;
    }
    return 0;
}
