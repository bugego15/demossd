/* gc.c - garbage collection + static wear leveling state machine */
#include "types.h"
#include <stdlib.h>

typedef struct gc_move {
    uint32_t lpn;
    uint32_t src;
    uint32_t dst;
    uint64_t seq;
    struct gc_move *next;
} gc_move_t;

static struct {
    int busy;
    int mode;                   /* 0 = GC, 1 = static WL */
    uint32_t victim;            /* block base ppa */
    int scan;                   /* next page index to examine */
    int moves_out;              /* outstanding read+prog ops */
    uint64_t t0;
} G;

static gc_move_t *pending;      /* moves waiting for a destination page */

static void gc_submit_moves(void);
static void gc_prog_cb(int err, void *arg);
static void gc_erase_cb(int err, void *arg);
static void gc_after_moves(void);
static void gc_after_jnl(void *arg);

void gc_init(void)
{
    G.busy = 0;
    G.mode = 0;
    G.moves_out = 0;
    G.scan = 0;
    pending = NULL;
}

int gc_busy(void) { return G.busy; }

void gc_abort(void)
{
    G.busy = 0;
    G.moves_out = 0;
    G.scan = 0;
    while (pending) {
        gc_move_t *n = pending->next;
        free(pending);
        pending = n;
    }
}

/* ---------- victim selection ---------- */
static int victim_select(int wl_mode, uint32_t *out)
{
    int nblocks = nand_nblocks();
    int ppb = cfg.pages_per_block;
    uint32_t thr_pages =
        (uint32_t)((uint64_t)ppb * (uint64_t)cfg.gc_victim_max_pct / 100);

    int best = -1, best_under = 0;
    uint32_t best_v = 0, best_pe = 0;

    for (int b = 0; b < nblocks; b++) {
        if (blks[b].state != BLK_CLOSED)
            continue;           /* FREE / ACTIVE / META / BAD never victimized */
        uint32_t v = blks[b].valid;

        if (!wl_mode) {
            /* Greedy: fewest valid pages; prefer victims under the threshold */
            int under = (v <= thr_pages);
            if (best < 0) {
                best = b; best_under = under; best_v = v;
            } else if (under && !best_under) {
                best = b; best_under = 1; best_v = v;
            } else if (under == best_under && v < best_v) {
                best = b; best_v = v;
            }
        } else {
            /* static WL: block holding data with the lowest erase count */
            if (v == 0 || v >= (uint32_t)ppb)
                continue;
            if (best < 0 ||
                blks[b].erase_cnt < best_pe ||
                (blks[b].erase_cnt == best_pe && v < best_v)) {
                best = b;
                best_pe = blks[b].erase_cnt;
                best_v = v;
            }
        }
    }
    if (best < 0)
        return 0;
    *out = blk_base_of(best);
    return 1;
}

/* ---------- move pipeline ---------- */
static void gc_read_cb(int err, void *arg);

static void gc_submit_moves(void)
{
    int ppb = cfg.pages_per_block;
    int max_out = cfg.gc_max_out > 0 ? cfg.gc_max_out : 8;

    while (G.moves_out < max_out && G.scan < ppb) {
        uint32_t ppa = G.victim | (uint32_t)G.scan;
        G.scan++;
        uint32_t lpn = ppa2lpn[pidx_of(ppa)];
        if (lpn == PPA_INVALID || l2p[lpn] != ppa)
            continue;           /* already stale */
        gc_move_t *m = (gc_move_t *)malloc(sizeof(gc_move_t));
        m->lpn = lpn;
        m->src = ppa;
        m->dst = PPA_INVALID;
        m->seq = seq_nand[pidx_of(ppa)];
        m->next = NULL;
        G.moves_out++;
        flash_submit(FOP_READ, ppa, gc_read_cb, m);
    }
}

static void gc_read_cb(int err, void *arg)
{
    gc_move_t *m = (gc_move_t *)arg;
    if (err < 0) {
        free(m);
        if (G.moves_out > 0)
            G.moves_out--;
        return;
    }
    uint32_t dst;
    if (ftl_alloc_gc(G.victim, &dst) < 0) {
        G.moves_out--;
        m->next = pending;
        pending = m;
        return;
    }
    m->dst = dst;
    flash_submit(FOP_PROG, dst, gc_prog_cb, m);
}

static void gc_prog_cb(int err, void *arg)
{
    gc_move_t *m = (gc_move_t *)arg;
    if (err < 0) {
        free(m);
        if (G.moves_out > 0)
            G.moves_out--;
        return;
    }
    /* only relocate if the mapping still points at the page we read */
    if (l2p[m->lpn] == m->src) {
        ftl_map_commit(m->lpn, m->dst, m->seq, 1);
        st.gc_moved++;
        if (G.mode)
            st.wl_moved++;
    } else {
        ftl_discard_page(m->dst);
    }
    free(m);
    if (G.moves_out > 0)
        G.moves_out--;

    if (G.scan >= cfg.pages_per_block && G.moves_out == 0 && !pending)
        gc_after_moves();
    else
        gc_submit_moves();
}

/* ---------- finish: journal barrier, then erase ---------- */
static void gc_after_moves(void)
{
    if (!G.busy)
        return;
    /* relocation records must hit NAND before the victim is erased */
    jnl_flush(gc_after_jnl, NULL);
}

static void gc_after_jnl(void *arg)
{
    (void)arg;
    if (!G.busy)
        return;
    flash_submit(FOP_ERASE, G.victim, gc_erase_cb, NULL);
}

static void gc_erase_cb(int err, void *arg)
{
    (void)arg;
    if (err < 0) {
        G.busy = 0;
        return;
    }
    if (G.mode)
        st.wl_count++;
    else {
        st.gc_count++;
        st.gc_erased++;
    }
    st.gc_busy_ns += g_now - G.t0;
    uint32_t victim = G.victim;
    G.busy = 0;
    ftl_free_block(victim);     /* zeroes pages, pushes to pool, wakes waiters */
    gc_kick("cycle");
}

/* ---------- start / kick ---------- */
static void gc_start(int mode, uint32_t victim)
{
    G.busy = 1;
    G.mode = mode;
    G.victim = victim;
    G.scan = 0;
    G.t0 = g_now;
    if (blks[bidx_of(victim)].valid == 0) {
        gc_after_moves();       /* pure garbage: barrier + erase only */
        return;
    }
    gc_submit_moves();
}

void gc_kick(const char *why)
{
    if (G.busy || sim_state != ST_RUNNING)
        return;
    int data_blocks = nand_nblocks() - cfg.meta_blocks;
    int trigger = data_blocks * cfg.gc_trigger_pct / 100;
    int floor = ftl_reserve() + 1;
    if (trigger < floor)
        trigger = floor;
    if (ftl_free_blocks() > trigger)
        return;
    /* during the final drain, only reclaim when allocation is starved,
     * otherwise GC restarts itself endlessly and drain never terminates */
    if (g_drain == 1 && ftl_free_blocks() > ftl_reserve() + 1)
        return;
    uint32_t victim;
    if (!victim_select(0, &victim))
        return;
    if (cfg.trace_v)
        log_v("GC start (%s) victim=%08x valid=%u free=%d\n", why, victim,
              blks[bidx_of(victim)].valid, ftl_free_blocks());
    gc_start(0, victim);
}

/* called when a block returns to the pool (or a move needs a page) */
void gc_on_free(void)
{
    while (pending) {
        uint32_t dst;
        if (ftl_alloc_gc(G.victim, &dst) < 0)
            break;
        gc_move_t *m = pending;
        pending = m->next;
        m->dst = dst;
        G.moves_out++;
        flash_submit(FOP_PROG, dst, gc_prog_cb, m);
    }
    if (G.busy && !pending && G.scan >= cfg.pages_per_block && G.moves_out == 0)
        gc_after_moves();
    if (!G.busy)
        gc_kick("free");
}

/* ---------- static wear leveling ---------- */
void wl_check(void)
{
    if (G.busy || g_drain == 1 || sim_state != ST_RUNNING)
        return;
    int nblocks = nand_nblocks();
    uint64_t mn = UINT64_MAX, mx = 0;
    for (int b = 0; b < nblocks; b++) {
        if (blks[b].state == BLK_META || blks[b].state == BLK_BAD)
            continue;
        uint64_t e = blks[b].erase_cnt;
        if (e < mn) mn = e;
        if (e > mx) mx = e;
    }
    if (mn == UINT64_MAX || mx < mn)
        return;
    if ((int)(mx - mn) < cfg.wl_threshold)
        return;
    if (ftl_free_blocks() < 8)
        return;                 /* need somewhere to move data to */
    uint32_t victim;
    if (!victim_select(1, &victim))
        return;
    if (cfg.trace_v)
        log_v("WL start spread=%llu victim=%08x pe=%u\n",
              (unsigned long long)(mx - mn), victim,
              blks[bidx_of(victim)].erase_cnt);
    gc_start(1, victim);
}
