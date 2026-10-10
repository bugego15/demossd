/* event.c - virtual-time discrete event queue (min-heap) */
#include "types.h"
#include <stdlib.h>
#include <string.h>

#define EV_HEAP_MAX 4096

static ev_t **heap;
static int hcap = EV_HEAP_MAX;
static int hsize;
static uint64_t ev_seq;

uint64_t g_now;
uint64_t ev_total;              /* instrumentation: events ever pushed */

void ev_init(void)
{
    heap = (ev_t **)calloc(hcap, sizeof(ev_t *));
    hsize = 0;
    ev_seq = 1;
    g_now = 0;
}

static void heap_swap(int a, int b)
{
    ev_t *t = heap[a];
    heap[a] = heap[b];
    heap[b] = t;
}

static void heap_up(int i)
{
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p]->t <= heap[i]->t)
            break;
        heap_swap(p, i);
        i = p;
    }
}

static void heap_down(int i)
{
    for (;;) {
        int l = 2 * i + 1, r = l + 1, s = i;
        if (l < hsize && heap[l]->t < heap[s]->t)
            s = l;
        if (r < hsize && heap[r]->t < heap[s]->t)
            s = r;
        if (s == i)
            break;
        heap_swap(s, i);
        i = s;
    }
}

static void heap_grow(void)
{
    hcap *= 2;
    heap = (ev_t **)realloc(heap, hcap * sizeof(ev_t *));
}

ev_t *ev_push(uint64_t t, ev_fn fn, void *arg)
{
    ev_t *e = (ev_t *)malloc(sizeof(ev_t));
    ev_total++;
    if (t < g_now)
        t = g_now;
    e->t = t;
    e->id = ev_seq++;
    e->cancelled = 0;
    e->fn = fn;
    e->arg = arg;
    if (hsize == hcap)
        heap_grow();
    heap[hsize] = e;
    heap_up(hsize);
    hsize++;
    return e;
}

void ev_cancel(ev_t *e)
{
    if (e)
        e->cancelled = 1;       /* freed lazily on pop */
}

int ev_empty(void)
{
    return hsize == 0;
}

uint64_t ev_peek(void)
{
    return hsize ? heap[0]->t : UINT64_MAX;
}

ev_t *ev_pop(void)
{
    while (hsize > 0) {
        ev_t *e = heap[0];
        hsize--;
        heap[0] = heap[hsize];
        if (hsize)
            heap_down(0);
        if (!e->cancelled)
            return e;
        free(e);
    }
    return NULL;
}

void ev_run_one(void)
{
    ev_t *e = ev_pop();
    if (!e)
        return;
    g_now = e->t;
    e->fn(e->arg);
    free(e);
}

void ev_run_until(uint64_t t)
{
    while (hsize > 0 && ev_peek() <= t)
        ev_run_one();
}

void ev_purge(void)
{
    while (hsize > 0) {
        ev_t *e = heap[0];
        hsize--;
        heap[0] = heap[hsize];
        if (hsize)
            heap_down(0);
        free(e);
    }
}
