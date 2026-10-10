/* types.h - shared types, config, stats for the SSD firmware simulator */
#ifndef SSD_TYPES_H
#define SSD_TYPES_H
#include <stdint.h>
#include <stdio.h>

/* ---------------- PPA packing: ch:28-31 die:24-27 blk:12-23 page:0-11 ---------------- */
#define PPA_INVALID 0xFFFFFFFFu
#define PPA_CH(p)      (((p) >> 28) & 0xF)
#define PPA_DIE(p)     (((p) >> 24) & 0xF)
#define PPA_BLK(p)     (((p) >> 12) & 0xFFF)
#define PPA_PAGE(p)    ((p) & 0xFFF)
#define PPA_MAKE(c,d,b,g) \
    (((uint32_t)(c) << 28) | ((uint32_t)(d) << 24) | ((uint32_t)(b) << 12) | (uint32_t)(g))

/* block states */
#define BLK_FREE   0
#define BLK_ACTIVE 1
#define BLK_CLOSED 2
#define BLK_META   3
#define BLK_BAD    4

/* flash op types */
#define FOP_READ  0
#define FOP_PROG  1
#define FOP_ERASE 2

/* allocation streams (hot/cold, hinted by workload) */
#define STREAM_COLD 0
#define STREAM_HOT  1
#define NSTREAM     2

/* host command types */
#define CMD_READ  0
#define CMD_WRITE 1
#define CMD_TRIM  2

/* simulation states */
#define ST_RUNNING 0
#define ST_PLP     1
#define ST_OFF     2
#define ST_DRAIN   3
#define ST_DONE    4

#define NS (1ULL)               /* ns */
#define US (1000ULL * NS)
#define MS (1000ULL * US)
#define SEC (1000ULL * MS)

/* ---------------- configuration ---------------- */
typedef struct cfg {
    /* geometry */
    int channels;
    int dies_per_ch;
    int blocks_per_die;
    int pages_per_block;
    int page_size;              /* bytes, power of two */
    int sector_size;
    double op_ratio;
    int meta_blocks;            /* reserved blocks on ch0/die0 (journal+ckpt) */
    double bad_block_pct;
    /* NAND timing (ns) */
    uint64_t tprog_ns;
    uint64_t tr_ns;
    uint64_t tbers_ns;
    int bus_mts;                /* NAND interface rate, MT/s */
    uint64_t cmd_ns;            /* per-command overhead */
    uint64_t cache_lat_ns;      /* SRAM cache access latency */
    /* policies */
    int cache_pages;            /* write cache capacity in pages, 0 = write-through */
    int cache_flush_pct;        /* flush when dirty% of capacity exceeds this */
    int queue_depth;
    int flush_out_max;           /* max outstanding cache->NAND flush pages */
    int plp;
    uint64_t plp_budget_ns;
    double ecc_rate;            /* per-read probability of read-retry */
    int gc_trigger_pct;         /* kick GC when free blocks below this % */
    int gc_victim_max_pct;      /* prefer victims with valid% below this */
    int gc_max_out;             /* max outstanding GC move ops */
    int wl_threshold;           /* static WL when max-min PE spread exceeds this */
    int endurance;              /* PE cycles for SMART percentage_used */
    uint64_t ckpt_interval;     /* journal records between checkpoints */
    uint64_t jnl_flush_at;      /* records per journal segment page batch */
    int reserve_blocks;         /* blocks reserved for GC destination */
    uint64_t seed;
    /* workload */
    char workload[32];          /* seq | rand | zipf | trace */
    char trace_path[256];
    int io_sectors;             /* host IO size in sectors */
    double read_ratio;
    double zipf_s;
    int fill;                   /* do a sequential fill phase first */
    double fill_pct;
    long n_ios;                 /* steady-state IO count */
    int verify;
    int quiet;
    int trace_v;                /* verbose trace of internal events */
    int power_cuts;             /* number of random power cuts */
    uint64_t cut_interval_ns;   /* spacing between cuts */
    uint64_t seed_cli;
} cfg_t;

/* ---------------- latency histogram ---------------- */
#define HIST_BUCKETS 512
typedef struct {
    uint32_t b[HIST_BUCKETS];
} hist_t;

/* ---------------- statistics ---------------- */
typedef struct stats {
    /* host */
    uint64_t host_reads, host_writes, host_trims;
    uint64_t host_r_bytes, host_w_bytes;
    uint64_t cmds_aborted;
    uint64_t rd_lat_n, rd_lat_sum, wr_lat_n, wr_lat_sum, tr_lat_n;
    hist_t rd_hist, wr_hist;
    uint64_t rd_lat_max, wr_lat_max;
    /* nand */
    uint64_t nand_read, nand_prog, nand_erase, nand_retry;
    uint64_t host_wr_pages;     /* user pages programmed by host path (post-cache) */
    uint64_t cache_wr_pages;    /* host writes absorbed by cache */
    /* gc / wl */
    uint64_t gc_count, gc_moved, gc_erased, gc_alloc_fail;
    uint64_t wl_count, wl_moved;
    uint64_t gc_busy_ns;
    /* cache */
    uint64_t cache_rd_hit, cache_rd_miss, cache_wr_hit, cache_wr_new;
    uint64_t cache_evict, cache_flushed, cache_dirty_peak;
    /* journal / checkpoint */
    uint64_t jnl_records, jnl_segments, jnl_bytes;
    uint64_t ckpt_count, ckpt_pages;
    /* plp / power */
    uint64_t power_cuts, plp_flushed, plp_dropped, plp_budget_hits;
    uint64_t recovery_count, recovery_ns, recovery_pages;
    /* integrity */
    uint64_t verified_ok, lost_writes, corrupted;
    uint64_t lost_first[10];
    /* wear */
    uint64_t pe_min, pe_max;
    double pe_sum;
    uint64_t pe_hist[16];
    uint64_t bad_blocks;
    /* timing */
    uint64_t sim_ns;
} stats_t;

/* ---------------- host command ---------------- */
struct page_io;
typedef struct host_cmd {
    int type;                   /* CMD_* */
    uint64_t lba;               /* sector address */
    uint32_t nlb;               /* number of sectors */
    uint64_t submit_ns;
    uint64_t seq;               /* write/trim sequence number */
    int first_lpn, last_lpn;    /* inclusive lpn range */
    int partial;                /* first/last page not fully covered */
    int left;                   /* outstanding page_ios */
    int acked;
    struct host_cmd *next;
} host_cmd_t;

/* ---------------- globals ---------------- */
extern cfg_t cfg;
extern stats_t st;
extern uint64_t g_now;
extern int sim_state;

/* ---------------- event queue (event.c) ---------------- */
typedef void (*ev_fn)(void *arg);
typedef struct ev {
    uint64_t t;
    uint64_t id;
    int cancelled;
    ev_fn fn;
    void *arg;
} ev_t;

void ev_init(void);
ev_t *ev_push(uint64_t t, ev_fn fn, void *arg);
void ev_cancel(ev_t *e);
int ev_empty(void);
uint64_t ev_peek(void);
ev_t *ev_pop(void);             /* pop next live event, NULL if empty */
void ev_run_one(void);
void ev_run_until(uint64_t t);  /* process events up to and including t */
void ev_purge(void);            /* drop every pending event (no callbacks) */

/* ---------------- nand layer (nand.c) ---------------- */
void nand_init(void);
void nand_purge(void);
uint64_t nand_xfer_ns(void);
void flash_submit(int type, uint32_t ppa, void (*cb)(int err, void *arg), void *arg);

/* linear indices */
int nand_nppages(void);         /* total physical pages */
int nand_nblocks(void);         /* total blocks */
uint64_t nand_max_backlog(void);/* max plane queue depth (ns beyond g_now) */
int nand_busy_planes(void);     /* planes with queued ops right now */
uint64_t nand_node_prog(int node);
int pidx_of(uint32_t ppa);
uint32_t blk_base_of(int bidx);
int bidx_of(uint32_t ppa);

/* ---------------- block table (shared, owned by ftl.c) ---------------- */
typedef struct blk_meta {
    uint32_t erase_cnt;
    uint32_t valid;             /* referenced user pages */
    uint32_t free_ptr;          /* next page to program */
    uint8_t state;              /* BLK_* */
} blk_meta_t;

extern blk_meta_t *blks;        /* [bidx] */
extern uint32_t *ppa2lpn;       /* [pidx], rebuilt from l2p on recovery */
extern uint64_t *seq_nand;      /* [pidx] OOB seq, 0 = never programmed */
extern uint32_t *l2p;           /* [lpn] */
extern uint64_t *map_seq;       /* [lpn] seq of last map op (write or trim) */
extern uint64_t *ack_seq;       /* [lpn] harness-side acked seq */

/* ---------------- FTL (ftl.c) ---------------- */
void ftl_format(void);
void ftl_boot_recover(void);    /* after power cut: rebuild volatile state */
void ftl_rebuild_from_map(void);
int ftl_user_lpn(void);
int ftl_free_blocks(void);
int ftl_alloc(int stream, uint32_t *ppa, int is_gc, int pin_node);
void ftl_free_block(uint32_t base);
void ftl_write(uint32_t lpn, uint64_t seq, int stream, int partial, host_cmd_t *cmd);
void ftl_read(uint32_t lpn, host_cmd_t *cmd);
void ftl_trim_cmd(host_cmd_t *cmd, uint64_t seq);
int ftl_map_commit(uint32_t lpn, uint32_t ppa, uint64_t seq, int is_gc);
void ftl_discard_page(uint32_t ppa);
int ftl_alloc_gc(uint32_t victim, uint32_t *ppa); /* dst pinned to victim node */
int ftl_reserve(void);
uint32_t ftl_map_get(uint32_t lpn);
uint64_t ftl_mapped_seq(uint32_t lpn);
int ftl_page_valid(uint32_t ppa);
/* write cache */
void ftl_cache_init(void);
int ftl_cache_probe(uint32_t lpn, uint64_t *seq);
int ftl_cache_insert(uint32_t lpn, uint64_t seq); /* -1 = throttled, retry later */
void ftl_cache_invalidate(uint32_t lpn);
void ftl_cache_drop_all(void);  /* lose dirty entries (power cut, no PLP) */
int ftl_cache_dirty(void);
void ftl_cache_bgflush(void);
int ftl_cache_flush_all(void (*done)(void *), void *arg); /* 1 if work started, cb when idle */
void ftl_cache_flush_page(uint32_t lpn, uint64_t seq, int cf);
void ftl_direct_start(struct page_io *pio);
void ftl_drop_waits(void);
void ftl_jnl_retry_waiters(void);
int ftl_bg_out(void);
uint8_t *ftl_dirty_table(void);
int ftl_dirty_count(void);
void ftl_dirty_cleared_all(void);
int ftl_wait_alloc(void);
int ftl_wait_jnl(void);
int ftl_wait_cache(void);
int ftl_pool_total(void);
int jnl_dbg(void);
void ftl_alloc_retry_waiters(void);
int ftl_recovering(void);

/* ---------------- GC / WL (gc.c) ---------------- */
void gc_init(void);
void gc_kick(const char *why);
void gc_abort(void);
void gc_on_free(void);
int gc_busy(void);
void wl_check(void);

/* ---------------- journal / ckpt / PLP (journal.c) ---------------- */
void jnl_init(void);
void jnl_append(uint32_t lpn, uint32_t ppa, uint64_t seq);
int jnl_can_accept(void);
void jnl_flush(void (*cb)(void *), void *arg);
void jnl_reset_volatile(void);
void jnl_recover(void);         /* load checkpoint + replay journal */
void ckpt_kick(void);
void ckpt_force(void);
void dirty_mark(uint32_t lpn);
void verify_scan(void);
void plp_deadline_check(void *arg);
int plp_active(void);
void plp_start(void);
void power_hard_cut(void);

/* drain handshake (defined in journal.c, reset by power_hard_cut) */
extern int g_drain;
extern int g_flushing;

/* main.c hooks */
void main_on_cut(void);

/* ---------------- host layer (host.c) ---------------- */
void host_init(void);
void host_submit(int type, uint64_t lba, uint32_t nlb, uint64_t seq);
void host_cmd_done(host_cmd_t *cmd, int aborted);
void host_purge(void);
int host_outstanding(void);
void host_stop(void);
void host_resume(void);
int host_stopped(void);

/* ---------------- workload (workload.c) ---------------- */
void wl_init(void);
int wl_done(void);
void wl_kick(void);             /* issue while depth allows */
void wl_on_complete(host_cmd_t *cmd);
int wl_in_fill(void);
uint64_t wl_next_seq(void);
void wl_ack_range(int first_lpn, int last_lpn, uint64_t seq);
void wl_trim_ack(uint64_t lba, uint32_t nlb);
long wl_generated(void);

/* ---------------- stats / report (stats.c) ---------------- */
void stats_init(void);
void hist_add(hist_t *h, uint64_t v);
uint64_t hist_pct(const hist_t *h, double p);
void stats_lat_add(int type, uint64_t lat);
void wear_scan(void);
void print_report(FILE *f);

/* ---------------- main helpers ---------------- */
void bg_timer_reschedule(void);
void bg_ev_invalidate(void);     /* stored timer handle freed by ev_purge */
void log_v(const char *fmt, ...);

#endif
