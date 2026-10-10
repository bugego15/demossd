/* nand.c - NAND timing model, channel/die/plane scheduler, flash op engine */
#include "types.h"
#include <stdlib.h>
#include <string.h>

/* per-plane device availability, per-channel bus availability */
static uint64_t *dev_free;      /* [ch][die][plane] */
static uint64_t *ch_free;       /* [ch] */
static int DIES, PLANES = 2;

/* outstanding ops (for power-cut purge) */
typedef struct fop {
    int type;
    uint32_t ppa;
    void (*cb)(int err, void *arg);
    void *arg;
    ev_t *ev;
    struct fop *next;
    struct fop *prev;           /* O(1) unlink on completion */
} fop_t;

static fop_t *fop_list;
static int purging;

static uint64_t xor_state;
static uint64_t node_prog[64];   /* per-node prog counts (diagnostics) */

uint64_t nand_node_prog(int node) { return node_prog[node]; }

static uint64_t rnd(void)
{
    xor_state ^= xor_state << 13;
    xor_state ^= xor_state >> 7;
    xor_state ^= xor_state << 17;
    return xor_state;
}

void nand_init(void)
{
    DIES = cfg.dies_per_ch;
    dev_free = (uint64_t *)calloc((size_t)cfg.channels * DIES * PLANES, sizeof(uint64_t));
    ch_free = (uint64_t *)calloc(cfg.channels, sizeof(uint64_t));
    fop_list = NULL;
    purging = 0;
    xor_state = cfg.seed ? cfg.seed : 0x9E3779B97F4A7C15ULL;
}

uint64_t nand_xfer_ns(void)
{
    /* page transfer on the NAND bus + command overhead */
    double bus_bps = (double)cfg.bus_mts * 1e6;   /* bytes/s (MT/s, 1 byte per transfer @ DDR) */
    uint64_t t = (uint64_t)(cfg.page_size / bus_bps * 1e9);
    return t + cfg.cmd_ns;
}

int nand_nppages(void)
{
    return cfg.channels * cfg.dies_per_ch * cfg.blocks_per_die * cfg.pages_per_block;
}

int nand_nblocks(void)
{
    return cfg.channels * cfg.dies_per_ch * cfg.blocks_per_die;
}

int pidx_of(uint32_t ppa)
{
    int ch = PPA_CH(ppa), die = PPA_DIE(ppa), b = PPA_BLK(ppa), p = PPA_PAGE(ppa);
    return ((ch * DIES + die) * cfg.blocks_per_die + b) * cfg.pages_per_block + p;
}

uint64_t nand_max_backlog(void)
{
    uint64_t m = 0;
    for (int i = 0; i < cfg.channels * DIES * PLANES; i++)
        if (dev_free[i] > g_now && dev_free[i] - g_now > m)
            m = dev_free[i] - g_now;
    return m;
}

int nand_busy_planes(void)
{
    int n = 0;
    for (int i = 0; i < cfg.channels * DIES * PLANES; i++)
        if (dev_free[i] > g_now)
            n++;
    return n;
}

int bidx_of(uint32_t ppa)
{
    int ch = PPA_CH(ppa), die = PPA_DIE(ppa), b = PPA_BLK(ppa);
    return (ch * DIES + die) * cfg.blocks_per_die + b;
}

uint32_t blk_base_of(int bidx)
{
    int per_die = cfg.blocks_per_die;
    int node = bidx / per_die;          /* ch*DIES + die */
    int b = bidx % per_die;
    return PPA_MAKE(node / DIES, node % DIES, b, 0);
}

static void fop_done(void *arg)
{
    fop_t *f = (fop_t *)arg;
    /* unlink O(1) via back pointer */
    if (f->prev)
        f->prev->next = f->next;
    else
        fop_list = f->next;
    if (f->next)
        f->next->prev = f->prev;
    if (!purging)
        f->cb(0, f->arg);
    free(f);
}

void flash_submit(int type, uint32_t ppa, void (*cb)(int err, void *arg), void *arg)
{
    int ch = PPA_CH(ppa), die = PPA_DIE(ppa);
    int plane = PPA_BLK(ppa) & 1;
    uint64_t xfer = (type == FOP_ERASE) ? 0 : nand_xfer_ns();
    uint64_t dev_t, end;
    uint64_t *df = &dev_free[(ch * DIES + die) * PLANES + plane];

    switch (type) {
    case FOP_READ: {
        uint64_t t_r = cfg.tr_ns;
        /* read-retry simulation: ECC strain costs an extra array read */
        if (cfg.ecc_rate > 0 && (rnd() % 1000000) < (uint64_t)(cfg.ecc_rate * 1e6)) {
            t_r += cfg.tr_ns;
            st.nand_retry++;
        }
        uint64_t d0 = g_now > *df ? g_now : *df;
        uint64_t d1 = d0 + t_r;
        uint64_t c0 = d1 > ch_free[ch] ? d1 : ch_free[ch];
        uint64_t c1 = c0 + xfer;
        *df = d1;               /* plane free after array read; bus serializes output */
        ch_free[ch] = c1;
        end = c1;
        st.nand_read++;
        break;
    }
    case FOP_PROG: {
        uint64_t c0 = g_now > ch_free[ch] ? g_now : ch_free[ch];
        uint64_t c1 = c0 + xfer;
        uint64_t d0 = c1 > *df ? c1 : *df;
        uint64_t d1 = d0 + cfg.tprog_ns;
        *df = d1;
        ch_free[ch] = c1;
        end = d1;
        st.nand_prog++;
        node_prog[ch * DIES + die]++;
        break;
    }
    default: {                   /* ERASE */
        uint64_t d0 = g_now > *df ? g_now : *df;
        uint64_t d1 = d0 + cfg.tbers_ns;
        *df = d1;
        end = d1;
        st.nand_erase++;
        break;
    }
    }
    (void)dev_t;

    fop_t *f = (fop_t *)malloc(sizeof(fop_t));
    f->type = type;
    f->ppa = ppa;
    f->cb = cb;
    f->arg = arg;
    f->next = fop_list;
    f->prev = NULL;
    if (fop_list)
        fop_list->prev = f;
    fop_list = f;
    f->ev = ev_push(end, fop_done, f);

    if (cfg.trace_v)
        log_v("FOP t=%d ppa=%08x end=%llu\n", type, ppa, (unsigned long long)end);
}

void nand_purge(void)
{
    purging = 1;
    while (fop_list) {
        fop_t *f = fop_list;
        fop_list = f->next;
        if (fop_list)
            fop_list->prev = NULL;
        if (f->ev)
            ev_cancel(f->ev);
        f->cb(-1, f->arg);      /* owners free their context, no state commit */
        free(f);
    }
    purging = 0;
    /* channel/plane availability resets with power */
    for (int i = 0; i < cfg.channels * DIES * PLANES; i++)
        dev_free[i] = g_now;
    for (int i = 0; i < cfg.channels; i++)
        ch_free[i] = g_now;
}
