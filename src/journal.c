/* journal.c - write-ahead journal, checkpoints, power-loss protection,
 * recovery and post-recovery verification.
 *
 * Meta region (node0/ch0die0, blocks [0..meta_blocks)):
 *   [0, jnl_blocks)                     journal (circular, segment append)
 *   [jnl_blocks, jnl_blocks+ckpt_blks)  checkpoint slot A
 *   next ckpt_blks blocks               checkpoint slot B
 *
 * Page header (meta_img) + payload:
 *   JSEG = journal data page   JCMT = journal segment commit
 *   CKPT = checkpoint data     CKCM = checkpoint commit (fixed last page)
 */
#include "types.h"
#include <stdlib.h>
#include <string.h>

/* drain handshake with main.c */
int g_drain;
int g_flushing;

#define JBUF 8192

typedef struct jrec {
    uint32_t lpn;
    uint32_t ppa;
    uint64_t seq;
} jrec_t;

typedef struct pg_hdr {
    uint32_t magic;
    uint32_t count;             /* records/entries in this page */
    uint64_t seg_seq;
    uint64_t lsn0;              /* first record lsn (JSEG) / covered lsn (CK*) */
    uint64_t aux;
    uint32_t crc;
    uint32_t pad;
} pg_hdr_t;                     /* 48 bytes */

#define MG_JSEG 0x4A534547u
#define MG_JCMT 0x4A434D54u
#define MG_CKPT 0x434B5054u
#define MG_CKCM 0x434B434Du

/* ---------- layout ---------- */
static int jnl_blocks, ckpt_blocks, jnl_start, ckpt_a, ckpt_b;
static int epp;                 /* records per data page */
static int ckpt_epp;            /* entries per ckpt page */
static int slot_pages;          /* pages per checkpoint slot (incl. commit) */
static uint8_t *meta_img;
static int meta_pages;

static int hdr_ok(const pg_hdr_t *h, uint32_t magic);
static uint32_t crc32_buf(const void *p, int n);

static void *pg_ptr(int blk, int page)
{
    return meta_img + (((size_t)blk * cfg.pages_per_block + page) * cfg.page_size);
}
static uint32_t pg_ppa(int blk, int page)
{
    return PPA_MAKE(0, 0, blk, page);
}

/* ---------- journal writer ---------- */
static struct {
    jrec_t buf[JBUF];
    int nbuf;
    int writing;
    jrec_t *seg;
    int seg_n, seg_pages, cur;
    uint64_t seg_seq, lsn;
    int wr_blk, wr_page;        /* region-relative cursor */
    int erase_done;             /* current block already erased this pass */
    uint64_t covered_lsn;       /* records covered by last checkpoint */
    int wait_ckpt;
    unsigned gen;
} J;

static struct {
    int busy;
    jrec_t *snap;
    int nsnap, pages, cur;
    uint64_t lsn, slot_seq;
    int slot;                   /*0/1 */
    int erase_left, erased;
    unsigned gen;
} C;

typedef struct cbw {
    void (*cb)(void *);
    void *arg;
    struct cbw *next;
} cbw_t;
static cbw_t *jnl_w, *ckpt_w;

static cbw_t *cbw_add(cbw_t **l, void (*cb)(void *), void *arg)
{
    cbw_t *w = (cbw_t *)malloc(sizeof(cbw_t));
    w->cb = cb;
    w->arg = arg;
    w->next = *l;
    *l = w;
    return w;
}

static void cbw_run(cbw_t **l, unsigned gen, unsigned mygen)
{
    cbw_t *w = *l;
    *l = NULL;
    while (w) {
        cbw_t *n = w->next;
        if (gen == mygen)       /* abort chain if state was reset under us */
            w->cb(w->arg);
        free(w);
        w = n;
    }
}

/* ---------- init / layout ---------- */
void jnl_init(void)
{
    int ppb = cfg.pages_per_block;
    epp = (cfg.page_size - (int)sizeof(pg_hdr_t)) / (int)sizeof(jrec_t);
    ckpt_epp = (cfg.page_size - (int)sizeof(pg_hdr_t)) / (int)sizeof(jrec_t);
    int need_pages = (ftl_user_lpn() + ckpt_epp - 1) / ckpt_epp + 1;
    ckpt_blocks = (need_pages + ppb - 1) / ppb;
    if (ckpt_blocks < 1)
        ckpt_blocks = 1;
    jnl_blocks = cfg.meta_blocks - 2 * ckpt_blocks;
    if (jnl_blocks < 2) {
        fprintf(stderr, "FATAL: meta_blocks=%d too small (need >= %d)\n",
                cfg.meta_blocks, 2 * ckpt_blocks + 2);
        exit(2);
    }
    jnl_start = 0;
    ckpt_a = jnl_blocks;
    ckpt_b = jnl_blocks + ckpt_blocks;
    slot_pages = ckpt_blocks * ppb;
    meta_pages = cfg.meta_blocks * ppb;
    meta_img = (uint8_t *)calloc((size_t)meta_pages, cfg.page_size);

    memset(&J, 0, sizeof(J));
    memset(&C, 0, sizeof(C));
    log_v("journal: %d blocks, ckpt slot %d blocks (%d entries/page)\n",
          jnl_blocks, ckpt_blocks, ckpt_epp);
}

void jnl_reset_volatile(void)
{
    J.nbuf = 0;
    J.writing = 0;
    free(J.seg);
    J.seg = NULL;
    J.seg_n = J.seg_pages = J.cur = 0;
    J.wait_ckpt = 0;
    J.erase_done = 0;
    J.gen++;
    C.busy = 0;
    free(C.snap);
    C.snap = NULL;
    C.gen++;
    while (jnl_w) { cbw_t *n = jnl_w->next; free(jnl_w); jnl_w = n; }
    while (ckpt_w) { cbw_t *n = ckpt_w->next; free(ckpt_w); ckpt_w = n; }
}

/* ---------- crc32 ---------- */
static uint32_t crc32_buf(const void *p, int n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(c & 1));
    }
    return ~c;
}

static int hdr_ok(const pg_hdr_t *h, uint32_t magic)
{
    if (h->magic != magic)
        return 0;
    pg_hdr_t tmp = *h;
    uint32_t c = tmp.crc;
    tmp.crc = 0;
    return crc32_buf(&tmp, sizeof(tmp)) == c;
}

static void hdr_make(pg_hdr_t *h, uint32_t magic, uint32_t count,
                     uint64_t seg_seq, uint64_t lsn0, uint64_t aux)
{
    memset(h, 0, sizeof(*h));
    h->magic = magic;
    h->count = count;
    h->seg_seq = seg_seq;
    h->lsn0 = lsn0;
    h->aux = aux;
    h->crc = 0;
    h->crc = crc32_buf(h, sizeof(*h));
}

/* ---------- cursor ---------- */
static void cursor_advance(void)
{
    if (++J.wr_page >= cfg.pages_per_block) {
        J.wr_page = 0;
        J.wr_blk = (J.wr_blk + 1) % jnl_blocks;
    }
}

/* safety: may we erase/overwrite the block at the cursor? */
static int block_safe(void)
{
    int blk = jnl_start + J.wr_blk;
    for (int p = 0; p < cfg.pages_per_block; p++) {
        pg_hdr_t *h = (pg_hdr_t *)pg_ptr(blk, p);
        if (h->magic == MG_JSEG) {
            if (h->lsn0 + h->count > J.covered_lsn)
                return 0;       /* records not yet checkpointed */
        } else if (h->magic == MG_JCMT) {
            if (h->aux > J.covered_lsn)
                return 0;
        }
    }
    return 1;
}

/* ---------- segment writer ---------- */
static uint64_t seg_lsn0;
static void pump(void);
static void start_segment(void);

static void seg_abort(void)
{
    free(J.seg);
    J.seg = NULL;
    J.writing = 0;
    J.cur = J.seg_pages = J.seg_n = 0;
}

static void seg_erase_cb(int err, void *arg);
static void seg_page_cb(int err, void *arg);
static void seg_commit_cb(int err, void *arg);

static void write_seg_page(void)
{
    if (J.cur < J.seg_pages) {
        if (J.wr_page == 0 && !J.erase_done) {
            int blk = jnl_start + J.wr_blk;
            if (!block_safe()) {
                J.wait_ckpt = 1;
                ckpt_force();   /* wrap needs coverage now, bypass interval */
                return;         /* woken by checkpoint completion */
            }
            flash_submit(FOP_ERASE, pg_ppa(blk, 0), seg_erase_cb, NULL);
            return;
        }
        uint32_t lsn0 = (uint32_t)(seg_lsn0 + (uint64_t)J.cur * epp);
        int n = J.seg_n - J.cur * epp;
        if (n > epp)
            n = epp;
        pg_hdr_t *h = (pg_hdr_t *)pg_ptr(jnl_start + J.wr_blk, J.wr_page);
        hdr_make(h, MG_JSEG, (uint32_t)n, J.seg_seq, lsn0, 0);
        memcpy((uint8_t *)h + sizeof(pg_hdr_t),
               J.seg + (size_t)J.cur * epp, (size_t)n * sizeof(jrec_t));
        flash_submit(FOP_PROG, pg_ppa(jnl_start + J.wr_blk, J.wr_page),
                     seg_page_cb, NULL);
        return;
    }
    /* commit page */
    pg_hdr_t *h = (pg_hdr_t *)pg_ptr(jnl_start + J.wr_blk, J.wr_page);
    hdr_make(h, MG_JCMT, (uint32_t)J.seg_n, J.seg_seq, seg_lsn0,
             seg_lsn0 + J.seg_n);
    flash_submit(FOP_PROG, pg_ppa(jnl_start + J.wr_blk, J.wr_page),
                 seg_commit_cb, NULL);
}

static void seg_erase_cb(int err, void *arg)
{
    (void)arg;
    if (err < 0) { seg_abort(); return; }
    J.erase_done = 1;
    write_seg_page();
}

static void seg_page_cb(int err, void *arg)
{
    (void)arg;
    if (err < 0) { seg_abort(); return; }
    cursor_advance();
    J.erase_done = 0;
    J.cur++;
    write_seg_page();
}

static void seg_commit_cb(int err, void *arg)
{
    (void)arg;
    if (err < 0) { seg_abort(); return; }
    cursor_advance();
    J.writing = 0;
    free(J.seg);
    J.seg = NULL;
    st.jnl_segments++;
    pump();
}

static void pump(void)
{
    unsigned g = J.gen;
    if (J.writing)
        return;
    if (J.wait_ckpt)
        return;                 /* woken by checkpoint completion */
    if (J.nbuf > 0 &&
        (J.nbuf >= (int)cfg.jnl_flush_at || jnl_w)) {
        start_segment();
        return;
    }
    if (jnl_w && J.nbuf == 0)
        cbw_run(&jnl_w, J.gen, g);
}

static void start_segment(void)
{
    if (J.writing || J.nbuf == 0) {
        pump();
        return;
    }
    J.seg = (jrec_t *)malloc((size_t)J.nbuf * sizeof(jrec_t));
    memcpy(J.seg, J.buf, (size_t)J.nbuf * sizeof(jrec_t));
    seg_lsn0 = J.lsn - (uint64_t)J.nbuf;
    J.seg_n = J.nbuf;
    J.nbuf = 0;
    J.seg_pages = (J.seg_n + epp - 1) / epp;
    J.cur = 0;
    J.writing = 1;
    J.seg_seq++;
    ftl_jnl_retry_waiters();    /* buffer has space again */
    write_seg_page();
}

/* ---------- append / flush API ---------- */
void jnl_append(uint32_t lpn, uint32_t ppa, uint64_t seq)
{
    if (J.nbuf >= JBUF)
        return;                 /* guarded by jnl_can_accept() upstream */
    J.buf[J.nbuf].lpn = lpn;
    J.buf[J.nbuf].ppa = ppa;
    J.buf[J.nbuf].seq = seq;
    J.nbuf++;
    J.lsn++;
    st.jnl_records++;

    if (!C.busy && J.lsn - C.lsn >= cfg.ckpt_interval)
        ckpt_kick();
    if (!J.writing && J.nbuf >= (int)cfg.jnl_flush_at)
        start_segment();
}

int jnl_can_accept(void)
{
    return J.nbuf <= JBUF - 4096;
}

/* packed journal state for diagnostics: writing|wait_ckpt|C.busy|waiters|nbuf */
int jnl_dbg(void)
{
    return (J.writing ? 1 : 0) | (J.wait_ckpt ? 2 : 0) | (C.busy ? 4 : 0) |
           (jnl_w ? 8 : 0) | (J.nbuf > 0 ? 16 : 0);
}

void jnl_flush(void (*cb)(void *), void *arg)
{
    cbw_add(&jnl_w, cb, arg);
    pump();
}

/* ---------- checkpoint ---------- */

static void ckpt_page_cb(int err, void *arg);
static void ckpt_commit_cb(int err, void *arg);
static void ckpt_erase_cb(int err, void *arg);

static void ckpt_write_next(void)
{
    int slot_blk = (C.slot == 0) ? ckpt_a : ckpt_b;
    if (C.cur < C.pages) {
        int lsn_off = C.cur * ckpt_epp;
        int n = C.nsnap - lsn_off;
        if (n > ckpt_epp)
            n = ckpt_epp;
        pg_hdr_t *h = (pg_hdr_t *)pg_ptr(slot_blk + C.cur / cfg.pages_per_block,
                                         C.cur % cfg.pages_per_block);
        hdr_make(h, MG_CKPT, (uint32_t)n, C.slot_seq, C.lsn, 0);
        memcpy((uint8_t *)h + sizeof(pg_hdr_t), C.snap + lsn_off,
               (size_t)n * sizeof(jrec_t));
        flash_submit(FOP_PROG,
                     pg_ppa(slot_blk + C.cur / cfg.pages_per_block,
                            C.cur % cfg.pages_per_block),
                     ckpt_page_cb, NULL);
        return;
    }
    /* fixed commit page at the very end of the slot */
    int pos = slot_pages - 1;
    pg_hdr_t *h = (pg_hdr_t *)pg_ptr(slot_blk + pos / cfg.pages_per_block,
                                     pos % cfg.pages_per_block);
    hdr_make(h, MG_CKCM, (uint32_t)C.nsnap, C.slot_seq, C.lsn, C.lsn);
    flash_submit(FOP_PROG, pg_ppa(slot_blk + pos / cfg.pages_per_block,
                                  pos % cfg.pages_per_block),
                 ckpt_commit_cb, NULL);
}

static void ckpt_page_cb(int err, void *arg)
{
    (void)arg;
    if (err < 0) {              /* aborted: slot stays invalid (old slot wins) */
        C.busy = 0;
        free(C.snap);
        C.snap = NULL;
        return;
    }
    C.cur++;
    ckpt_write_next();
}

static void ckpt_commit_cb(int err, void *arg)
{
    (void)arg;
    if (err < 0) {
        C.busy = 0;
        free(C.snap);
        C.snap = NULL;
        return;
    }
    unsigned g = C.gen;
    C.busy = 0;
    free(C.snap);
    C.snap = NULL;
    J.covered_lsn = C.lsn;      /* atomic publish */
    J.wait_ckpt = 0;
    st.ckpt_count++;
    st.ckpt_pages += (uint64_t)C.pages + 1;
    cbw_run(&ckpt_w, C.gen, g);
    pump();                     /* journal may have been blocked on us */
    if (J.nbuf >= (int)cfg.jnl_flush_at || jnl_w)
        pump();
}

static void ckpt_erase_cb(int err, void *arg)
{
    (void)arg;
    if (err < 0) {
        C.busy = 0;
        free(C.snap);
        C.snap = NULL;
        return;
    }
    if (--C.erase_left > 0) {
        int slot_blk = (C.slot == 0) ? ckpt_a : ckpt_b;
        flash_submit(FOP_ERASE, pg_ppa(slot_blk + C.erased, 0), ckpt_erase_cb, NULL);
        C.erased++;
        return;
    }
    ckpt_write_next();
}

static void ckpt_start(void);

void ckpt_force(void)
{
    if (C.busy || sim_state == ST_OFF || sim_state == ST_PLP)
        return;
    if (ftl_dirty_count() == 0)
        return;                 /* nothing changed: covered_lsn already valid */
    ckpt_start();
}

void ckpt_kick(void)
{
    if (C.busy || sim_state == ST_OFF || sim_state == ST_PLP)
        return;
    if (J.lsn - C.lsn < cfg.ckpt_interval)
        return;                 /* rate-limited: only every ckpt_interval recs */
    ckpt_start();
}

static void ckpt_start(void)
{
    uint8_t *dirty = ftl_dirty_table();
    int u = ftl_user_lpn();
    int cap = (slot_pages - 1) * ckpt_epp;
    int n = 0;
    for (int i = 0; i < u; i++)
        if (dirty[i])
            n++;
    if (n == 0)
        return;                 /* nothing dirty: coverage already valid */
    if (n > cap) {
        fprintf(stderr,
                "FATAL: checkpoint slot too small (%d entries > %d). "
                "Increase meta_blocks.\n", n, cap);
        exit(3);
    }
    C.snap = (jrec_t *)malloc((size_t)n * sizeof(jrec_t));
    int k = 0;
    for (int i = 0; i < u; i++) {
        if (dirty[i]) {
            C.snap[k].lpn = (uint32_t)i;
            C.snap[k].ppa = l2p[i];
            C.snap[k].seq = map_seq[i];
            k++;
            dirty[i] = 0;       /* cleared at capture; later changes re-dirty */
        }
    }
    ftl_dirty_cleared_all();
    C.nsnap = n;
    C.pages = (n + ckpt_epp - 1) / ckpt_epp;
    C.lsn = J.lsn;              /* snapshot reflects every record so far */
    C.slot ^= 1;
    C.slot_seq++;
    C.cur = 0;
    C.busy = 1;

    int slot_blk = (C.slot == 0) ? ckpt_a : ckpt_b;
    C.erase_left = ckpt_blocks;
    C.erased = 0;
    flash_submit(FOP_ERASE, pg_ppa(slot_blk, 0), ckpt_erase_cb, NULL);
    C.erased = 1;
    if (cfg.trace_v)
        log_v("CKPT start slot=%d entries=%d lsn=%llu\n", C.slot, n,
              (unsigned long long)C.lsn);
}

/* ---------- recovery ---------- */
typedef struct {
    int pos;                    /* region-relative page of the commit */
    uint64_t seg_seq;
    uint32_t count;
} commit_ref_t;

static int commit_cmp(const void *a, const void *b)
{
    const commit_ref_t *x = (const commit_ref_t *)a;
    const commit_ref_t *y = (const commit_ref_t *)b;
    if (x->seg_seq < y->seg_seq)
        return -1;
    if (x->seg_seq > y->seg_seq)
        return 1;
    return 0;
}

void jnl_recover(void)
{
    int ppb = cfg.pages_per_block;
    uint64_t pages_read = 0;

    J.covered_lsn = 0;
    J.lsn = 0;
    J.seg_seq = 0;
    J.wr_blk = 0;
    J.wr_page = 0;

    /* 1. checkpoint: pick the slot with the highest sequence */
    int best = -1;
    pg_hdr_t best_h;
    memset(&best_h, 0, sizeof(best_h));
    for (int s = 0; s < 2; s++) {
        int slot_blk = s ? ckpt_b : ckpt_a;
        int pos = slot_pages - 1;
        pg_hdr_t *h = (pg_hdr_t *)pg_ptr(slot_blk + pos / ppb, pos % ppb);
        if (hdr_ok(h, MG_CKCM) &&
            (best < 0 || h->seg_seq > best_h.seg_seq)) {
            best = s;
            best_h = *h;
        }
    }
    if (best >= 0) {
        int slot_blk = best ? ckpt_b : ckpt_a;
        pages_read++;
        int np = ((int)best_h.count + ckpt_epp - 1) / ckpt_epp;
        for (int p = 0; p < np; p++) {
            pg_hdr_t *h = (pg_hdr_t *)pg_ptr(slot_blk + p / ppb, p % ppb);
            if (!hdr_ok(h, MG_CKPT))
                break;
            jrec_t *e = (jrec_t *)((uint8_t *)h + sizeof(pg_hdr_t));
            for (uint32_t i = 0; i < h->count; i++) {
                l2p[e[i].lpn] = e[i].ppa;
                map_seq[e[i].lpn] = e[i].seq;
            }
            pages_read++;
        }
        J.covered_lsn = best_h.lsn0;
        J.lsn = best_h.lsn0;
    }

    /* 2. collect committed journal segments, replay in segment order */
    int total = jnl_blocks * ppb;
    commit_ref_t *refs = (commit_ref_t *)malloc((size_t)total * sizeof(commit_ref_t));
    int nref = 0;
    for (int rel = 0; rel < jnl_blocks; rel++)
        for (int p = 0; p < ppb; p++) {
            pg_hdr_t *h = (pg_hdr_t *)pg_ptr(jnl_start + rel, p);
            if (h->magic == MG_JCMT && hdr_ok(h, MG_JCMT)) {
                refs[nref].pos = rel * ppb + p;
                refs[nref].seg_seq = h->seg_seq;
                refs[nref].count = h->count;
                nref++;
            }
        }
    qsort(refs, (size_t)nref, sizeof(commit_ref_t), commit_cmp);

    uint64_t last_seq = 0;
    int last_pos = -1;
    for (int r = 0; r < nref; r++) {
        int ndat = ((int)refs[r].count + epp - 1) / epp;
        int ok = 1;
        for (int k = 1; k <= ndat; k++) {
            int pos = refs[r].pos - k;
            if (pos < 0)
                pos += total;
            pg_hdr_t *h = (pg_hdr_t *)pg_ptr(jnl_start + pos / ppb, pos % ppb);
            if (h->magic != MG_JSEG || h->seg_seq != refs[r].seg_seq ||
                h->count == 0) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;           /* torn or overwritten segment */
        for (int k = 1; k <= ndat; k++) {
            int pos = refs[r].pos - k;
            if (pos < 0)
                pos += total;
            pg_hdr_t *h = (pg_hdr_t *)pg_ptr(jnl_start + pos / ppb, pos % ppb);
            jrec_t *e = (jrec_t *)((uint8_t *)h + sizeof(pg_hdr_t));
            for (uint32_t i = 0; i < h->count; i++) {
                if (e[i].seq > map_seq[e[i].lpn]) {
                    l2p[e[i].lpn] = e[i].ppa;
                    map_seq[e[i].lpn] = e[i].seq;
                }
            }
            pages_read++;
        }
        pages_read++;           /* commit page */
        if (refs[r].seg_seq > last_seq) {
            last_seq = refs[r].seg_seq;
            last_pos = refs[r].pos;
        }
    }
    J.seg_seq = last_seq;

    /* 3. write cursor: after the newest commit, rounded to a block start
     *    (blocks are erased on entry, so a partial block cannot be reused) */
    if (last_pos >= 0) {
        int pos = last_pos + 1;
        if (pos >= total)
            pos = 0;
        J.wr_blk = pos / ppb;
        J.wr_page = pos % ppb;
        if (J.wr_page != 0) {
            J.wr_page = 0;
            J.wr_blk++;
            if (J.wr_blk >= jnl_blocks)
                J.wr_blk = 0;
        }
    }

    C.lsn = J.covered_lsn;
    st.recovery_pages += pages_read;
    free(refs);

    if (cfg.trace_v)
        log_v("recover: ckpt=%d covered=%llu segs=%d pages=%llu cursor=%d/%d\n",
              best, (unsigned long long)J.covered_lsn, nref,
              (unsigned long long)pages_read, J.wr_blk, J.wr_page);
}

/* ---------- post-recovery verification (harness side) ---------- */
void verify_scan(void)
{
    int n = ftl_user_lpn();
    int shown = 0;
    st.verified_ok = 0;
    st.lost_writes = 0;
    st.corrupted = 0;
    memset(st.lost_first, 0, sizeof(st.lost_first));
    for (int i = 0; i < n; i++) {
        uint64_t a = ack_seq[i];
        if (!a)
            continue;           /* never acked, or trimmed after the write */
        uint32_t p = l2p[i];
        if (p == PPA_INVALID || seq_nand[pidx_of(p)] < a) {
            st.lost_writes++;
            if (shown < 10)
                st.lost_first[shown++] = (uint64_t)i;
        } else {
            st.verified_ok++;
        }
    }
    if (cfg.trace_v || st.lost_writes)
        fprintf(stderr, "verify: ok=%llu lost=%llu\n",
                (unsigned long long)st.verified_ok,
                (unsigned long long)st.lost_writes);
}

/* ---------- power-loss protection ---------- */
int plp_active(void)
{
    return sim_state == ST_PLP;
}

static void plp_jnl_done(void *arg);

/* after the cache is clean, wait for evicted-but-unprogrammed pages to
 * reach NAND (bounded by flush_out_max), then commit the journal */
static void plp_progress(void *arg)
{
    (void)arg;
    if (sim_state != ST_PLP)
        return;
    if (ftl_bg_out() > 0) {
        ev_push(g_now + 50 * US, plp_progress, NULL);
        return;
    }
    jnl_flush(plp_jnl_done, NULL);
}

static void plp_cache_done(void *arg)
{
    (void)arg;
    if (sim_state != ST_PLP)
        return;
    plp_progress(NULL);
}

static void plp_jnl_done(void *arg)
{
    (void)arg;
    if (sim_state != ST_PLP)
        return;
    power_hard_cut();           /* durable: cut immediately, budget not spent */
}

void plp_deadline_check(void *arg)
{
    (void)arg;
    if (sim_state != ST_PLP)
        return;
    st.plp_budget_hits++;
    ftl_cache_drop_all();       /* capacitor expired: SRAM contents are gone */
    power_hard_cut();
}

void plp_start(void)
{
    if (sim_state != ST_RUNNING)
        return;
    sim_state = ST_PLP;
    host_stop();
    ev_push(g_now + cfg.plp_budget_ns, plp_deadline_check, NULL);
    if (cfg.trace_v)
        log_v("PLP start budget=%lluus dirty=%d\n",
              (unsigned long long)(cfg.plp_budget_ns / US),
              ftl_cache_dirty());
    if (ftl_cache_flush_all(plp_cache_done, NULL) == 0)
        plp_cache_done(NULL);
}

void power_hard_cut(void)
{
    if (sim_state == ST_OFF)
        return;
    int was_plp = (sim_state == ST_PLP);
    sim_state = ST_OFF;
    host_stop();

    nand_purge();               /* outstanding ops fail: no commits */
    gc_abort();
    host_purge();               /* un-acked commands vanish */
    ev_purge();                 /* timers, completions, deadlines */
    bg_ev_invalidate();         /* stored timer handle is now dangling */
    jnl_reset_volatile();       /* uncommitted journal buffer is RAM: lost */
    if (!was_plp)
        ftl_cache_drop_all();   /* dirty cache lost without PLP */
    ftl_drop_waits();

    ftl_boot_recover();         /* checkpoint + journal + OOB scan */

    sim_state = ST_RUNNING;
    host_resume();
    wl_kick();
    gc_kick("recovery");
    main_on_cut();
    bg_timer_reschedule();
}
