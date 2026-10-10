/* ftl.c - FTL core: mapping, allocation, read/write paths, write cache */
#include "types.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---------- global tables (see types.h) ---------- */
blk_meta_t *blks;
uint32_t *ppa2lpn;
uint64_t *seq_nand;
uint32_t *l2p;
uint64_t *map_seq;
uint64_t *ack_seq;

static uint8_t *dirty_bit;      /* checkpoint dirty tracking */
static int user_lpn_count;
static int dirty_n;             /* number of set dirty bits */

/* free block pools, one stack per node (ch*dies+die) */
static uint32_t **pool;         /* [node] stack of bidx */
static int *pool_n;
static int node_count;
static int free_total;
static int reserve_blocks;

/* active append block per stream slot per node */
#define SLOT_GC NSTREAM         /* third slot: GC destinations */
#define NSLOTS  (NSTREAM + 1)
static uint32_t *active;        /* [slot*node_count + node] block base or INVALID */
static int rr_node;

/* wait lists */
typedef struct wait_ent {
    struct page_io *pio;
    struct wait_ent *next;
} wait_ent_t;
static wait_ent_t *wait_alloc;
static wait_ent_t *wait_jnl;
static wait_ent_t *wait_cache;  /* host writes waiting for cache room */

/* cache-flush-all batch tracking */
static void (*cf_done)(void *);
static void *cf_arg;
static int cf_out;
static int bg_out;              /* outstanding background flush page_ios */

/* ---------- page_io ---------- */
typedef struct page_io {
    int type;                   /* CMD_READ / CMD_WRITE */
    uint32_t lpn;
    uint64_t seq;
    int partial;
    int rmw;                    /* program needs a RMW read first */
    int bg;                     /* background flush (no host cmd) */
    int cf;                     /* belongs to flush-all batch */
    int gc_dest;                /* allow GC-style alloc (bypass reserve) */
    uint32_t ppa;               /* allocated dest */
    host_cmd_t *cmd;
} page_io_t;

/* ---------- helpers ---------- */
void log_v(const char *fmt, ...)
{
    if (!cfg.trace_v)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int node_of_bidx(int bidx) { return bidx / cfg.blocks_per_die; }

int ftl_user_lpn(void) { return user_lpn_count; }
int ftl_free_blocks(void) { return free_total; }
uint32_t ftl_map_get(uint32_t lpn) { return l2p[lpn]; }
int ftl_recovering(void) { return sim_state == ST_OFF; }

uint64_t ftl_mapped_seq(uint32_t lpn)
{
    uint32_t p = l2p[lpn];
    return p == PPA_INVALID ? 0 : seq_nand[p];
}

int ftl_page_valid(uint32_t ppa)
{
    uint32_t l = ppa2lpn[pidx_of(ppa)];
    return l != PPA_INVALID && l2p[l] == ppa;
}

void dirty_mark(uint32_t lpn); /* journal.c */

/* ---------- format / pool management ---------- */
static uint32_t fmt_rnd_state;
static uint32_t fmt_rnd(void)
{
    fmt_rnd_state ^= fmt_rnd_state << 13;
    fmt_rnd_state ^= fmt_rnd_state >> 7;
    fmt_rnd_state ^= fmt_rnd_state << 17;
    return fmt_rnd_state;
}

void ftl_format(void)
{
    int nblocks = nand_nblocks();
    int npp = nand_nppages();

    fmt_rnd_state = (uint32_t)(cfg.seed ? cfg.seed : 0xC0FFEEu);
    node_count = cfg.channels * cfg.dies_per_ch;

    blks = (blk_meta_t *)calloc(nblocks, sizeof(blk_meta_t));
    ppa2lpn = (uint32_t *)malloc(npp * sizeof(uint32_t));
    seq_nand = (uint64_t *)calloc(npp, sizeof(uint64_t));
    l2p = NULL;                 /* sized after user capacity computed */
    for (int i = 0; i < npp; i++)
        ppa2lpn[i] = PPA_INVALID;

    /* geometry: data blocks exclude meta region on node 0 */
    int meta = cfg.meta_blocks;
    if (meta < 4)
        meta = 4;
    if (meta > cfg.blocks_per_die - 8)
        meta = cfg.blocks_per_die / 4;
    cfg.meta_blocks = meta;

    int data_blocks = nblocks - meta;

    /* auto-grow the meta region so checkpoint slots can always hold a full
     * snapshot (journal.c enforces the same layout formula) */
    {
        int ppb = cfg.pages_per_block;
        int epp = (cfg.page_size - 48) / 16;   /* entries per ckpt page */
        int approx_lpn = (int)((double)data_blocks * ppb * (1.0 - cfg.op_ratio));
        int need_pages = (approx_lpn + epp - 1) / epp + 1;
        int ckpt_blks = (need_pages + ppb - 1) / ppb;
        if (ckpt_blks < 1)
            ckpt_blks = 1;
        int need_meta = 2 * ckpt_blks + 2;
        if (cfg.meta_blocks < need_meta) {
            if (need_meta > cfg.blocks_per_die - 8) {
                fprintf(stderr,
                        "FATAL: blocks_per_die=%d too small for meta region "
                        "(need %d); increase blocks.\n",
                        cfg.blocks_per_die, need_meta);
                exit(2);
            }
            cfg.meta_blocks = need_meta;
            meta = need_meta;
            data_blocks = nblocks - meta;
        }
    }

    double usable = (double)data_blocks * cfg.pages_per_block * (1.0 - cfg.op_ratio);
    user_lpn_count = (int)usable;
    /* lpn covers page_size bytes */
    l2p = (uint32_t *)malloc(user_lpn_count * sizeof(uint32_t));
    map_seq = (uint64_t *)calloc(user_lpn_count, sizeof(uint64_t));
    ack_seq = (uint64_t *)calloc(user_lpn_count, sizeof(uint64_t));
    dirty_bit = (uint8_t *)calloc(user_lpn_count, 1);

    for (int i = 0; i < user_lpn_count; i++)
        l2p[i] = PPA_INVALID;

    /* block states, bad blocks */
    pool = (uint32_t **)calloc(node_count, sizeof(uint32_t *));
    pool_n = (int *)calloc(node_count, sizeof(int));
    for (int n = 0; n < node_count; n++) {
        pool[n] = (uint32_t *)malloc(cfg.blocks_per_die * sizeof(uint32_t));
        pool_n[n] = 0;
    }

    for (int b = 0; b < nblocks; b++) {
        int node = node_of_bidx(b);
        if (node == 0 && b < cfg.meta_blocks) {
            blks[b].state = BLK_META;
            continue;
        }
        if (cfg.bad_block_pct > 0 &&
            (fmt_rnd() % 1000000) < (uint64_t)(cfg.bad_block_pct * 1e6)) {
            blks[b].state = BLK_BAD;
            st.bad_blocks++;
            continue;
        }
        blks[b].state = BLK_FREE;
        blks[b].free_ptr = 0;
        pool[node][pool_n[node]++] = (uint32_t)b;
        free_total++;
    }

    reserve_blocks = cfg.reserve_blocks;
    if (reserve_blocks < 2)
        reserve_blocks = 2;
    int maxr = data_blocks / 20;
    if (reserve_blocks > maxr)
        reserve_blocks = maxr;

    active = (uint32_t *)malloc(sizeof(uint32_t) * NSLOTS * node_count);
    for (int i = 0; i < NSLOTS * node_count; i++)
        active[i] = PPA_INVALID;
    rr_node = 0;

    st.pe_min = UINT64_MAX;
    log_v("FTL format: %d user pages, %d blocks, free=%d reserve=%d\n",
          user_lpn_count, data_blocks, free_total, reserve_blocks);
}

/* pop helper not needed: direct per-node pop used by ftl_alloc */

void ftl_free_block(uint32_t base)
{
    int b = bidx_of(base);
    for (uint32_t i = 0; i < (uint32_t)cfg.pages_per_block; i++) {
        int pi = pidx_of(base | i);
        seq_nand[pi] = 0;
        ppa2lpn[pi] = PPA_INVALID;
    }
    blks[b].erase_cnt++;
    blks[b].valid = 0;
    blks[b].free_ptr = 0;
    blks[b].state = BLK_FREE;
    int node = node_of_bidx(b);
    pool[node][pool_n[node]++] = (uint32_t)b;
    free_total++;
    ftl_alloc_retry_waiters();
    gc_on_free();
}

/* GC destinations stay on the victim's node: one cycle then pops at most
 * one pool block (instead of one per node when active[] is empty, e.g.
 * right after recovery), so a cycle can never exhaust the free pool. */
int ftl_alloc_gc(uint32_t victim, uint32_t *ppa)
{
    return ftl_alloc(SLOT_GC, ppa, 1, node_of_bidx(bidx_of(victim)));
}
int ftl_reserve(void) { return reserve_blocks; }

/* ---------- allocation ---------- */
static void close_if_full(int b)
{
    if (blks[b].free_ptr >= (uint32_t)cfg.pages_per_block) {
        blks[b].state = BLK_CLOSED;
    }
}

int ftl_alloc(int slot, uint32_t *ppa, int is_gc, int pin_node)
{
    if (free_total <= 0)
        return -1;
    if (!is_gc && !plp_active() && free_total <= reserve_blocks)
        return -1;

    int start;
    if (pin_node >= 0) {
        start = pin_node;       /* pinned caller (GC): no round-robin */
    } else {
        start = rr_node++;
        if (rr_node >= node_count)
            rr_node = 0;
    }

    /* 1. preferred node's active block: each (stream, node) pair keeps its
     * own active block so all dies/planes receive traffic */
    {
        uint32_t base = active[slot * node_count + start];
        if (base != PPA_INVALID) {
            int b = bidx_of(base);
            if (blks[b].free_ptr < (uint32_t)cfg.pages_per_block) {
                *ppa = base | blks[b].free_ptr++;
                blks[b].state = BLK_ACTIVE;
                close_if_full(b);
                if (blks[b].free_ptr >= (uint32_t)cfg.pages_per_block)
                    active[slot * node_count + start] = PPA_INVALID;
                return 0;
            }
            active[slot * node_count + start] = PPA_INVALID;
        }
    }

    /* 2. fresh block from the preferred node's pool */
    if (pool_n[start] > 0) {
        int b = pool[start][--pool_n[start]];
        uint32_t base = blk_base_of(b);
        free_total--;
        blks[b].state = BLK_ACTIVE;
        blks[b].free_ptr = 1;
        active[slot * node_count + start] = base;
        *ppa = base;
        close_if_full(b);
        return 0;
    }

    /* 3. any active block with free space (other nodes/streams) */
    for (int s = 0; s < NSLOTS; s++) {
        for (int n = 0; n < node_count; n++) {
            uint32_t base = active[s * node_count + n];
            if (base == PPA_INVALID)
                continue;
            int b = bidx_of(base);
            if (blks[b].free_ptr < (uint32_t)cfg.pages_per_block) {
                *ppa = base | blks[b].free_ptr++;
                close_if_full(b);
                if (blks[b].free_ptr >= (uint32_t)cfg.pages_per_block)
                    active[s * node_count + n] = PPA_INVALID;
                return 0;
            }
        }
    }

    /* 4. any node's pool */
    for (int n = 0; n < node_count; n++) {
        if (pool_n[n] <= 0)
            continue;
        int b = pool[n][--pool_n[n]];
        uint32_t base = blk_base_of(b);
        free_total--;
        blks[b].state = BLK_ACTIVE;
        blks[b].free_ptr = 1;
        active[slot * node_count + n] = base;
        *ppa = base;
        close_if_full(b);
        return 0;
    }

    (void)is_gc;
    return -1;
}

/* ---------- wait lists ---------- */
/* wait lists are FIFO: new arrivals go to the tail so the oldest waiter
 * cannot starve while fresh requests keep arriving at the head */
static void wait_push(wait_ent_t **list, page_io_t *pio)
{
    wait_ent_t *e = (wait_ent_t *)malloc(sizeof(wait_ent_t));
    e->pio = pio;
    e->next = NULL;
    if (!*list) {
        *list = e;
    } else {
        wait_ent_t *t = *list;
        while (t->next)
            t = t->next;
        t->next = e;
    }
}

/* put a just-popped waiter back at the head (it failed the precondition,
 * it keeps its place at the front of the queue) */
static void wait_requeue(wait_ent_t **list, page_io_t *pio)
{
    wait_ent_t *e = (wait_ent_t *)malloc(sizeof(wait_ent_t));
    e->pio = pio;
    e->next = *list;
    *list = e;
}

void ftl_alloc_retry_waiters(void)
{
    wait_ent_t *l = wait_alloc;
    wait_alloc = NULL;
    while (l) {
        wait_ent_t *n = l->next;
        ftl_direct_start(l->pio);
        free(l);
        l = n;
    }
}

static void jnl_retry_waiters(void)
{
    wait_ent_t *l = wait_jnl;
    wait_jnl = NULL;
    while (l) {
        wait_ent_t *n = l->next;
        ftl_direct_start(l->pio);
        free(l);
        l = n;
    }
}

void ftl_jnl_retry_waiters(void)
{
    jnl_retry_waiters();
}

uint8_t *ftl_dirty_table(void)
{
    return dirty_bit;
}

void dirty_mark(uint32_t lpn)
{
    if (lpn < (uint32_t)user_lpn_count && !dirty_bit[lpn]) {
        dirty_bit[lpn] = 1;
        dirty_n++;
    }
}

int ftl_dirty_count(void) { return dirty_n; }
void ftl_dirty_cleared_all(void) { dirty_n = 0; }

static int list_len(wait_ent_t *l)
{
    int n = 0;
    for (; l; l = l->next)
        n++;
    return n;
}
int ftl_wait_alloc(void) { return list_len(wait_alloc); }
int ftl_wait_jnl(void) { return list_len(wait_jnl); }
int ftl_wait_cache(void) { return list_len(wait_cache); }

int ftl_pool_total(void)
{
    int n = 0;
    for (int i = 0; i < node_count; i++)
        n += pool_n[i];
    return n;
}

void ftl_drop_waits(void)
{
    wait_ent_t *l;
    l = wait_alloc; wait_alloc = NULL;
    while (l) { wait_ent_t *n = l->next; free(l->pio); free(l); l = n; }
    l = wait_jnl; wait_jnl = NULL;
    while (l) { wait_ent_t *n = l->next; free(l->pio); free(l); l = n; }
    l = wait_cache; wait_cache = NULL;
    while (l) { wait_ent_t *n = l->next; free(l->pio); free(l); l = n; }
    cf_out = 0; cf_done = NULL;
    bg_out = 0;
}

/* ---------- page_io completion ---------- */
static void cache_waiters_retry(void);

static void cf_check(void)
{
    if (cf_out == 0 && cf_done) {
        void (*fn)(void *) = cf_done;
        void *a = cf_arg;
        cf_done = NULL;
        fn(a);
    }
}

static void pio_finish(page_io_t *p)
{
    /* host completion first: cf_check below may trigger a power cut and
     * host_purge() would free p->cmd under us otherwise */
    if (p->bg) {
        bg_out--;
        cache_waiters_retry();  /* flush budget freed: admit waiting writes */
    }
    if (p->cmd) {
        if (--p->cmd->left == 0)
            host_cmd_done(p->cmd, 0);
    }
    if (p->cf) {
        cf_out--;
        cf_check();
    }
    free(p);
}

static void pio_complete_ev(void *arg) { pio_finish((page_io_t *)arg); }

/* ---------- cache admission (with NAND flush backpressure) ---------- */
static void cache_admit(page_io_t *p)
{
    if (ftl_cache_insert(p->lpn, p->seq) < 0) {
        wait_push(&wait_cache, p);
        return;
    }
    ev_push(g_now + cfg.cache_lat_ns, pio_complete_ev, p);
}

static void cache_waiters_retry(void)
{
    while (wait_cache) {
        wait_ent_t *w = wait_cache;
        page_io_t *p = w->pio;
        wait_cache = w->next;
        free(w);
        if (ftl_cache_insert(p->lpn, p->seq) < 0) {
            wait_requeue(&wait_cache, p);   /* still throttled: keep place */
            break;
        }
        ev_push(g_now + cfg.cache_lat_ns, pio_complete_ev, p);
    }
}

static void pio_abort(page_io_t *p)
{
    /* power-cut abort: detach from host cmd (host_purge frees it) */
    if (p->bg) {
        if (plp_active())
            st.plp_dropped++;
        if (bg_out > 0)
            bg_out--;
    }
    p->cmd = NULL;
    p->cf = 0;
    free(p);
}

/* ---------- map commit ---------- */
int ftl_map_commit(uint32_t lpn, uint32_t ppa, uint64_t seq, int is_gc)
{
    uint32_t old = l2p[lpn];

    if (!is_gc) {
        if (seq <= map_seq[lpn]) {   /* stale write lost the race (or was trimmed) */
            ftl_discard_page(ppa);
            return 0;
        }
    } else {
        if (old == PPA_INVALID) {    /* trimmed while relocating */
            ftl_discard_page(ppa);
            return 0;
        }
    }

    l2p[lpn] = ppa;
    ppa2lpn[pidx_of(ppa)] = lpn;
    seq_nand[pidx_of(ppa)] = seq;
    map_seq[lpn] = seq;

    if (old != PPA_INVALID && old != ppa) {
        ppa2lpn[pidx_of(old)] = PPA_INVALID;
        int ob = bidx_of(old);
        if (blks[ob].valid)
            blks[ob].valid--;
    }
    blks[bidx_of(ppa)].valid++;
    dirty_mark(lpn);
    jnl_append(lpn, ppa, seq);
    if (!is_gc)
        st.host_wr_pages++;
    return 1;
}

void ftl_discard_page(uint32_t ppa)
{
    /* page stays physically programmed (seq_nand kept) but is unreferenced */
    ppa2lpn[pidx_of(ppa)] = PPA_INVALID;
}

/* ---------- write path ---------- */
static void direct_prog(page_io_t *p);

static void rmw_read_cb(int err, void *arg)
{
    page_io_t *p = (page_io_t *)arg;
    if (err < 0) { pio_abort(p); return; }
    direct_prog(p);
}

static void prog_cb(int err, void *arg)
{
    page_io_t *p = (page_io_t *)arg;
    if (err < 0) { pio_abort(p); return; }
    if (ftl_map_commit(p->lpn, p->ppa, p->seq, 0)) {
        /* committed */
    }
    pio_finish(p);
}

void ftl_direct_start(page_io_t *p)
{
    if (!jnl_can_accept()) {
        wait_push(&wait_jnl, p);
        return;
    }
    uint32_t ppa;
    if (ftl_alloc(p->lpn & 1 ? STREAM_HOT : STREAM_COLD, &ppa, p->gc_dest,
                  -1) < 0) {
        wait_push(&wait_alloc, p);
        gc_kick("alloc-empty");
        return;
    }
    p->ppa = ppa;

    uint32_t old = l2p[p->lpn];
    if (p->partial && old != PPA_INVALID) {
        flash_submit(FOP_READ, old, rmw_read_cb, p);
        return;
    }
    direct_prog(p);
}

static void direct_prog(page_io_t *p)
{
    flash_submit(FOP_PROG, p->ppa, prog_cb, p);
}

/* choose stream: hint from caller passed via lpn parity fallback */
static int stream_of_lpn(uint32_t lpn, int hinted)
{
    (void)lpn;
    return hinted;
}

void ftl_write(uint32_t lpn, uint64_t seq, int stream, int partial, host_cmd_t *cmd){
    (void)stream;               /* hot/cold derived from lpn in direct_start */
    page_io_t *p = (page_io_t *)calloc(1, sizeof(page_io_t));
    p->type = CMD_WRITE;
    p->lpn = lpn;
    p->seq = seq;
    p->partial = partial;
    p->cmd = cmd;

    if (cfg.cache_pages > 0) {
        /* write-back through the SRAM cache (admission throttled) */
        st.cache_wr_pages++;
        cache_admit(p);
        return;
    }
    /* cache disabled: direct program */
    p->bg = 0;
    ftl_direct_start(p);
    (void)stream_of_lpn;
}

/* background page flush spawned by the cache (eviction / bg / plp) */
int ftl_bg_out(void) { return bg_out; }

void ftl_cache_flush_page(uint32_t lpn, uint64_t seq, int cf)
{
    page_io_t *p = (page_io_t *)calloc(1, sizeof(page_io_t));
    p->type = CMD_WRITE;
    p->lpn = lpn;
    p->seq = seq;
    p->bg = 1;
    p->cf = cf;
    p->gc_dest = plp_active();   /* during PLP bypass the reserve */
    if (cf)
        cf_out++;
    bg_out++;
    ftl_direct_start(p);
}

/* ---------- read path ---------- */
static void read_cb(int err, void *arg)
{
    page_io_t *p = (page_io_t *)arg;
    if (err < 0) { pio_abort(p); return; }
    pio_finish(p);
}

void ftl_read(uint32_t lpn, host_cmd_t *cmd)
{
    page_io_t *p = (page_io_t *)calloc(1, sizeof(page_io_t));
    p->type = CMD_READ;
    p->lpn = lpn;
    p->cmd = cmd;

    uint64_t cseq;
    if (ftl_cache_probe(lpn, &cseq)) {
        st.cache_rd_hit++;
        ev_push(g_now + cfg.cache_lat_ns, pio_complete_ev, p);
        return;
    }
    st.cache_rd_miss++;
    uint32_t ppa = l2p[lpn];
    if (ppa == PPA_INVALID) {
        /* never written: constant latency, returns zeros */
        ev_push(g_now + cfg.cache_lat_ns, pio_complete_ev, p);
        return;
    }
    flash_submit(FOP_READ, ppa, read_cb, p);
}

/* ---------- trim ---------- */
static void trim_done_ev(void *arg)
{
    host_cmd_done((host_cmd_t *)arg, 0);
}

void ftl_trim_cmd(host_cmd_t *cmd, uint64_t seq)
{
    for (int l = cmd->first_lpn; l <= cmd->last_lpn; l++) {
        uint32_t lpn = (uint32_t)l;
        if (lpn >= (uint32_t)user_lpn_count)
            break;
        if (map_seq[lpn] >= seq)
            continue;
        uint32_t old = l2p[lpn];
        if (old != PPA_INVALID) {
            ppa2lpn[pidx_of(old)] = PPA_INVALID;
            int ob = bidx_of(old);
            if (blks[ob].valid)
                blks[ob].valid--;
        }
        l2p[lpn] = PPA_INVALID;
        map_seq[lpn] = seq;
        ftl_cache_invalidate(lpn);
        dirty_mark(lpn);
        jnl_append(lpn, PPA_INVALID, seq);
    }
    ev_push(g_now + US, trim_done_ev, cmd);
}

/* ---------- write cache ---------- */
typedef struct {
    int used;
    uint32_t lpn;
    uint64_t seq;
    uint64_t last;
} cent_t;

static cent_t *cache;
static int cache_cap;
static uint64_t cache_tick;

static int cache_hash(uint32_t lpn)
{
    lpn = lpn * 2654435761u;
    return (int)(lpn >> 8);
}

static int cache_find(uint32_t lpn)
{
    if (!cache || cache_cap == 0)
        return -1;
    for (int i = 0; i < cache_cap; i++)
        if (cache[i].used && cache[i].lpn == lpn)
            return i;
    return -1;
}

void ftl_cache_init(void)
{
    if (cfg.cache_pages <= 0) { cache = NULL; cache_cap = 0; return; }
    cache_cap = cfg.cache_pages;
    cache = (cent_t *)calloc(cache_cap, sizeof(cent_t));
    cache_tick = 1;
    (void)cache_hash;
}

int ftl_cache_probe(uint32_t lpn, uint64_t *seq)
{
    int i = cache_find(lpn);
    if (i < 0)
        return 0;
    cache[i].last = ++cache_tick;
    *seq = cache[i].seq;
    return 1;
}

static void cache_evict(int i);

int ftl_cache_insert(uint32_t lpn, uint64_t seq)
{
    int i = cache_find(lpn);
    if (i >= 0) {
        cache[i].seq = seq;
        cache[i].last = ++cache_tick;
        st.cache_wr_hit++;
        return 0;
    }
    int slot = -1;
    for (int k = 0; k < cache_cap; k++)
        if (!cache[k].used) { slot = k; break; }
    if (slot < 0) {
        /* evict LRU, but only while the NAND flush backlog has room:
         * otherwise dirty pages would pile up unbounded in the device queue */
        if (ftl_bg_out() >= cfg.flush_out_max)
            return -1;          /* throttled: caller retries, do not count yet */
        uint64_t best = UINT64_MAX;
        for (int k = 0; k < cache_cap; k++)
            if (cache[k].last < best) { best = cache[k].last; slot = k; }
        cache_evict(slot);
    }
    cache[slot].used = 1;
    cache[slot].lpn = lpn;
    cache[slot].seq = seq;
    cache[slot].last = ++cache_tick;
    st.cache_wr_new++;
    int dirty = 0;
    for (int k = 0; k < cache_cap; k++)
        if (cache[k].used)
            dirty++;
    if ((uint64_t)dirty > st.cache_dirty_peak)
        st.cache_dirty_peak = dirty;
    return 0;
}

static void cache_evict(int i)
{
    if (!cache[i].used)
        return;
    st.cache_evict++;
    ftl_cache_flush_page(cache[i].lpn, cache[i].seq, 0);
    cache[i].used = 0;
}

void ftl_cache_invalidate(uint32_t lpn)
{
    int i = cache_find(lpn);
    if (i >= 0)
        cache[i].used = 0;
}

void ftl_cache_drop_all(void)
{
    int n = 0;
    for (int k = 0; k < cache_cap; k++)
        if (cache[k].used) { cache[k].used = 0; n++; }
    if (n)
        st.plp_dropped += n;
}

int ftl_cache_dirty(void)
{
    int n = 0;
    for (int k = 0; k < cache_cap; k++)
        if (cache[k].used)
            n++;
    return n;
}

/* background flush: keep dirty below threshold, respecting flush backlog */
void ftl_cache_bgflush(void)
{
    if (cache_cap == 0)
        return;
    int dirty = ftl_cache_dirty();
    int limit = cache_cap * cfg.cache_flush_pct / 100;
    while (dirty > limit && ftl_bg_out() < cfg.flush_out_max) {
        uint64_t best = UINT64_MAX;
        int slot = -1;
        for (int k = 0; k < cache_cap; k++)
            if (cache[k].used && cache[k].last < best) {
                best = cache[k].last;
                slot = k;
            }
        if (slot < 0)
            break;
        cache_evict(slot);
        dirty--;
    }
    cache_waiters_retry();
}

int ftl_cache_flush_all(void (*done)(void *), void *arg)
{
    cf_done = done;
    cf_arg = arg;
    cf_out = 0;
    for (int k = 0; k < cache_cap; k++) {
        if (cache[k].used) {
            uint32_t lpn = cache[k].lpn;
            uint64_t seq = cache[k].seq;
            cache[k].used = 0;
            st.cache_evict++;
            st.cache_flushed++;
            if (plp_active())
                st.plp_flushed++;
            /* cf_out incremented inside ftl_cache_flush_page */
            ftl_cache_flush_page(lpn, seq, 1);
        }
    }
    if (cf_out == 0) {
        cf_done = NULL;
        return 0;               /* nothing to do: caller runs cb itself */
    }
    return 1;
}

/* ---------- recovery: rebuild volatile state from l2p + OOB ---------- */
void ftl_rebuild_from_map(void)
{
    int nblocks = nand_nblocks();

    /* programmed-page scan: free_ptr = last programmed index + 1 */
    for (int b = 0; b < nblocks; b++) {
        if (blks[b].state == BLK_META || blks[b].state == BLK_BAD)
            continue;
        uint32_t base = blk_base_of(b);
        uint32_t fp = 0;
        for (uint32_t i = 0; i < (uint32_t)cfg.pages_per_block; i++)
            if (seq_nand[pidx_of(base | i)])
                fp = i + 1;
        blks[b].free_ptr = fp;
        blks[b].valid = 0;
        if (fp == 0) {
            blks[b].state = BLK_FREE;
        } else {
            blks[b].state = BLK_CLOSED; /* partial blocks become GC candidates */
        }
    }

    /* rebuild pool */
    for (int n = 0; n < node_count; n++)
        pool_n[n] = 0;
    free_total = 0;
    for (int b = 0; b < nblocks; b++) {
        if (blks[b].state == BLK_FREE) {
            int node = node_of_bidx(b);
            pool[node][pool_n[node]++] = (uint32_t)b;
            free_total++;
        }
    }

    /* reverse map + valid counts from l2p */
    for (int i = 0; i < nand_nppages(); i++)
        ppa2lpn[i] = PPA_INVALID;
    for (uint32_t l = 0; l < (uint32_t)user_lpn_count; l++) {
        uint32_t p = l2p[l];
        if (p == PPA_INVALID)
            continue;
        ppa2lpn[pidx_of(p)] = l;
        blks[bidx_of(p)].valid++;
    }

    for (int i = 0; i < NSLOTS * node_count; i++)
        active[i] = PPA_INVALID;
    rr_node = 0;

    /* Re-open the emptiest partial block of each node into every slot:
     * their free pages stay usable (host append + GC destinations) instead
     * of being stranded until a later GC cycle reclaims the block. */
    for (int n = 0; n < node_count; n++) {
        uint32_t chosen[NSLOTS];
        int nchosen = 0;
        for (int s = 0; s < NSLOTS; s++) {
            int best = -1;
            for (int b = n * cfg.blocks_per_die;
                 b < (n + 1) * cfg.blocks_per_die; b++) {
                if (blks[b].state != BLK_CLOSED)
                    continue;
                uint32_t fp = blks[b].free_ptr;
                if (fp == 0 || fp >= (uint32_t)cfg.pages_per_block)
                    continue;       /* empty (in pool) or full */
                int dup = 0;
                for (int t = 0; t < nchosen; t++)
                    if (chosen[t] == (uint32_t)b) { dup = 1; break; }
                if (dup)
                    continue;
                if (best < 0 || fp < blks[best].free_ptr)
                    best = b;
            }
            if (best < 0)
                break;
            chosen[nchosen++] = (uint32_t)best;
            blks[best].state = BLK_ACTIVE;
            active[s * node_count + n] = blk_base_of(best);
        }
    }
}

/* ---------- boot recovery (called after a power cut) ---------- */
void ftl_boot_recover(void)
{
    st.recovery_count++;
    jnl_recover();              /* restores l2p + map_seq from meta NAND */

    uint64_t data_pages = (uint64_t)nand_nppages();
    ftl_rebuild_from_map();     /* OOB scan: free_ptr, pool, reverse map */

    st.recovery_pages += data_pages;
    /* time estimate: per-die serial prefix scan, dies in parallel; plus the
     * meta region read on node0 (journal + checkpoint) */
    uint64_t per_page = cfg.tr_ns + nand_xfer_ns();
    uint64_t t = (uint64_t)cfg.blocks_per_die * cfg.pages_per_block * per_page +
                 (uint64_t)cfg.meta_blocks * cfg.pages_per_block * per_page;
    g_now += t;
    st.recovery_ns += t;
    if (cfg.trace_v)
        log_v("recovery done: %llu pages, est %llums\n",
              (unsigned long long)st.recovery_pages,
              (unsigned long long)(t / MS));
}
