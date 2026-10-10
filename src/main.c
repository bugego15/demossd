/* main.c - CLI, configuration, initialization, main event loop */
#include "types.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

cfg_t cfg;
stats_t st;
int sim_state;

static int cuts_left;
static ev_t *bg_ev;
static FILE *report_fp;

/* ---------------- defaults ---------------- */
static void set_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.channels = 4;
    cfg.dies_per_ch = 4;
    cfg.blocks_per_die = 64;
    cfg.pages_per_block = 512;
    cfg.page_size = 16384;
    cfg.sector_size = 512;
    cfg.op_ratio = 0.07;
    cfg.meta_blocks = 8;
    cfg.bad_block_pct = 0.0;
    cfg.tprog_ns = 800 * 1000;  /* 800us TLC program */
    cfg.tr_ns = 70 * 1000;      /* 70us read */
    cfg.tbers_ns = 3000 * 1000; /* 3ms block erase */
    cfg.bus_mts = 1200;
    cfg.cmd_ns = 1000;
    cfg.cache_lat_ns = 5000;    /* 5us SRAM cache ack */
    cfg.cache_pages = 256;
    cfg.cache_flush_pct = 50;
    cfg.queue_depth = 32;
    cfg.flush_out_max = 128;    /* NAND flush backlog throttle */
    cfg.plp = 1;
    cfg.plp_budget_ns = 50 * 1000 * 1000; /* 50ms capacitor budget */
    cfg.ecc_rate = 0.0;
    cfg.gc_trigger_pct = 5;
    cfg.gc_victim_max_pct = 50;
    cfg.gc_max_out = 8;
    cfg.wl_threshold = 30;
    cfg.endurance = 3000;
    cfg.ckpt_interval = 65536;
    cfg.jnl_flush_at = 1024;
    cfg.reserve_blocks = 4;
    cfg.seed = 42;
    strcpy(cfg.workload, "rand");
    cfg.trace_path[0] = 0;
    cfg.io_sectors = 32;        /* one full page */
    cfg.read_ratio = 0.5;
    cfg.zipf_s = 0.99;
    cfg.fill = 1;
    cfg.fill_pct = 70;
    cfg.n_ios = 300000;
    cfg.verify = 1;
    cfg.quiet = 0;
    cfg.trace_v = 0;
    cfg.power_cuts = 0;
    cfg.cut_interval_ns = 20 * SEC;
}

/* ---------------- config file: key = value ---------------- */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                     e[-1] == '\t'))
        e--;
    *e = 0;
    return s;
}

static void cfg_set(const char *key, const char *val)
{
#define S(k, field) if (!strcmp(key, k)) { snprintf(field, sizeof(field), "%s", val); continue; }
#define I(k, field) if (!strcmp(key, k)) { field = (int)strtol(val, NULL, 0); continue; }
#define D(k, field) if (!strcmp(key, k)) { field = atof(val); continue; }
#define U(k, field) if (!strcmp(key, k)) { field = strtoull(val, NULL, 0); continue; }
    do {
    I("channels", cfg.channels)
    I("dies", cfg.dies_per_ch)
    I("blocks", cfg.blocks_per_die)
    I("pages_per_block", cfg.pages_per_block)
    I("page_size", cfg.page_size)
    I("sector_size", cfg.sector_size)
    D("op_ratio", cfg.op_ratio)
    I("meta_blocks", cfg.meta_blocks)
    D("bad_block_pct", cfg.bad_block_pct)
    U("tprog_ns", cfg.tprog_ns)
    U("tr_ns", cfg.tr_ns)
    U("tbers_ns", cfg.tbers_ns)
    I("bus_mts", cfg.bus_mts)
    U("cmd_ns", cfg.cmd_ns)
    U("cache_lat_ns", cfg.cache_lat_ns)
    I("cache_pages", cfg.cache_pages)
    I("cache_flush_pct", cfg.cache_flush_pct)
    I("queue_depth", cfg.queue_depth)
    I("flush_out_max", cfg.flush_out_max)
    I("plp", cfg.plp)
    U("plp_budget_ns", cfg.plp_budget_ns)
    D("ecc_rate", cfg.ecc_rate)
    I("gc_trigger_pct", cfg.gc_trigger_pct)
    I("gc_victim_max_pct", cfg.gc_victim_max_pct)
    I("gc_max_out", cfg.gc_max_out)
    I("wl_threshold", cfg.wl_threshold)
    I("endurance", cfg.endurance)
    U("ckpt_interval", cfg.ckpt_interval)
    U("jnl_flush_at", cfg.jnl_flush_at)
    I("reserve_blocks", cfg.reserve_blocks)
    U("seed", cfg.seed)
    S("workload", cfg.workload)
    S("trace", cfg.trace_path)
    I("io_sectors", cfg.io_sectors)
    D("read_ratio", cfg.read_ratio)
    D("zipf_s", cfg.zipf_s)
    I("fill", cfg.fill)
    D("fill_pct", cfg.fill_pct)
    /* n_ios: long */
    if (!strcmp(key, "n_ios")) { cfg.n_ios = strtol(val, NULL, 0); continue; }
    I("verify", cfg.verify)
    I("quiet", cfg.quiet)
    I("verbose", cfg.trace_v)
    I("power_cuts", cfg.power_cuts)
    U("cut_interval_ns", cfg.cut_interval_ns)
    fprintf(stderr, "config: unknown key '%s'\n", key);
    } while (0);
#undef S
#undef I
#undef D
#undef U
}

static void load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config file not found: %s\n", path);
        exit(1);
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        if (*s == '#' || *s == '\n' || *s == '\r')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (*key)
            cfg_set(key, val);
    }
    fclose(f);
}

/* ---------------- background timer ---------------- */
extern uint64_t ev_total;
static long bg_ticks;

static void bg_timer(void *arg)
{
    (void)arg;
    bg_ev = NULL;
    if (sim_state == ST_RUNNING) {
        ftl_cache_bgflush();
        gc_kick("bg");
        wl_check();
        ckpt_kick();
    }
    bg_ticks++;
    if (cfg.trace_v && (bg_ticks % 10000) == 0) {
        fprintf(stderr, "[nodes] prog/node:");
        for (int i = 0; i < cfg.channels * cfg.dies_per_ch; i++)
            fprintf(stderr, " %llu", (unsigned long long)nand_node_prog(i));
        fprintf(stderr, "\n");
    }
    if (cfg.trace_v && (bg_ticks % 1000) == 0)
        fprintf(stderr, "[t] sim=%.1fms st=%d ev=%llu out=%d wr=%llu prog=%llu "
                "ckpt=%llu jnlseg=%llu dirty=%d cache_d=%d bg_out=%d "
                "waitj=%d waita=%d waitc=%d free=%d pool=%d gcb=%d jnl=%x "
                "planes=%d backlog=%.1fms\n",
                g_now / 1e6, sim_state, (unsigned long long)ev_total,
                host_outstanding(), (unsigned long long)st.host_writes,
                (unsigned long long)st.nand_prog,
                (unsigned long long)st.ckpt_count,
                (unsigned long long)st.jnl_segments,
                ftl_dirty_count(), ftl_cache_dirty(), ftl_bg_out(),
                ftl_wait_jnl(), ftl_wait_alloc(), ftl_wait_cache(),
                ftl_free_blocks(), ftl_pool_total(), gc_busy(), jnl_dbg(),
                nand_busy_planes(),
                nand_max_backlog() / 1e6);
    bg_timer_reschedule();
}

void bg_timer_reschedule(void)
{
    if (bg_ev)
        ev_cancel(bg_ev);
    bg_ev = ev_push(g_now + MS, bg_timer, NULL);
}

/* after ev_purge() the stored timer event is freed: drop the handle */
void bg_ev_invalidate(void)
{
    bg_ev = NULL;
}

/* ---------------- power cut scheduling ---------------- */
static void cut_ev(void *arg);

static void schedule_next_cut(void)
{
    if (cuts_left <= 0)
        return;
    cuts_left--;
    /* spread the cuts over the run: effective interval = configured / N */
    uint64_t iv = cfg.cut_interval_ns;
    if (cfg.power_cuts > 0)
        iv /= (uint64_t)cfg.power_cuts;
    if (iv < 2)
        iv = 2;
    uint64_t jitter = iv / 2 + (uint64_t)(rand() % (int)(iv / 2 + 1));
    ev_push(g_now + jitter, cut_ev, NULL);
}

static void cut_ev(void *arg)
{
    (void)arg;
    st.power_cuts++;
    if (cfg.trace_v)
        log_v(">>> POWER CUT at %.1f ms (state=%d)\n", g_now / 1e6, sim_state);
    if (cfg.plp && sim_state == ST_RUNNING)
        plp_start();
    else
        power_hard_cut();
}

void main_on_cut(void)
{
    schedule_next_cut();
}

/* ---------------- graceful drain before verification ---------------- */
static void drain_cache_done(void *arg);
static void drain_jnl_done(void *arg);

static void drain_step(void *arg)
{
    (void)arg;
    if (g_drain != 1 || g_flushing)
        return;
    if (ftl_cache_dirty() > 0) {
        g_flushing = 1;
        if (ftl_cache_flush_all(drain_cache_done, NULL))
            return;             /* async: callback resumes */
        g_flushing = 0;
    }
    if (ftl_bg_out() > 0 || gc_busy()) {
        ev_push(g_now + MS, drain_step, NULL);
        return;
    }
    g_flushing = 1;
    jnl_flush(drain_jnl_done, NULL);
}

static void drain_cache_done(void *arg)
{
    (void)arg;
    g_flushing = 0;
    drain_step(NULL);
}

static void drain_jnl_done(void *arg)
{
    (void)arg;
    g_flushing = 0;
    g_drain = 2;
}

/* ---------------- usage ---------------- */
static void usage(const char *p)
{
    printf(
        "SSD firmware simulator\n"
        "usage: %s [options]\n"
        "  -c FILE   config file (default ssd.cfg)\n"
        "  -o FILE   write report to FILE (default stdout)\n"
        "  -w X      workload: rand | seq | zipf | trace\n"
        "  -n N      number of steady-state IOs\n"
        "  -q N      queue depth\n"
        "  -r F      read ratio 0..1\n"
        "  -s N      IO size in sectors\n"
        "  -t FILE   trace file (lines: 'R|W|T lba nlb')\n"
        "  -P N      inject N random power cuts\n"
        "  --no-plp  disable power-loss protection (cuts should lose data)\n"
        "  --no-fill skip the sequential fill phase\n"
        "  --no-verify  skip final integrity verification\n"
        "  --seed N  RNG seed\n"
        "  -v        verbose (internal event trace)\n"
        "  -h        help\n", p);
    exit(0);
}

int main(int argc, char **argv)
{
    const char *cfg_path = "ssd.cfg";
    const char *out_path = NULL;
    int have_cli = 0;

    set_defaults();
    srand(1);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            usage(argv[0]);
    }
    /* first pass: find -c so the file loads before other overrides */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "-c"))
            cfg_path = argv[i + 1];
    load_config(cfg_path);

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-c")) { i++; continue; }
        if (!strcmp(a, "-o") && i + 1 < argc) { out_path = argv[++i]; continue; }
        if (!strcmp(a, "-w") && i + 1 < argc) { snprintf(cfg.workload, 32, "%s", argv[++i]); have_cli = 1; continue; }
        if (!strcmp(a, "-n") && i + 1 < argc) { cfg.n_ios = strtol(argv[++i], NULL, 0); have_cli = 1; continue; }
        if (!strcmp(a, "-q") && i + 1 < argc) { cfg.queue_depth = atoi(argv[++i]); have_cli = 1; continue; }
        if (!strcmp(a, "-r") && i + 1 < argc) { cfg.read_ratio = atof(argv[++i]); have_cli = 1; continue; }
        if (!strcmp(a, "-s") && i + 1 < argc) { cfg.io_sectors = atoi(argv[++i]); have_cli = 1; continue; }
        if (!strcmp(a, "-t") && i + 1 < argc) { snprintf(cfg.trace_path, 256, "%s", argv[++i]); snprintf(cfg.workload, 32, "trace"); continue; }
        if (!strcmp(a, "-P") && i + 1 < argc) { cfg.power_cuts = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--seed") && i + 1 < argc) { cfg.seed = strtoull(argv[++i], NULL, 0); continue; }
        if (!strcmp(a, "--no-fill")) { cfg.fill = 0; continue; }
        if (!strcmp(a, "--no-verify")) { cfg.verify = 0; continue; }
        if (!strcmp(a, "--no-plp")) { cfg.plp = 0; continue; }
        if (!strcmp(a, "-v")) { cfg.trace_v = 1; continue; }
        fprintf(stderr, "unknown option: %s\n", a);
        usage(argv[0]);
    }
    (void)have_cli;

    if (out_path) {
        report_fp = fopen(out_path, "w");
        if (!report_fp) {
            fprintf(stderr, "cannot write %s\n", out_path);
            return 1;
        }
    }

    srand((unsigned)cfg.seed);

    /* ---- init sequence (mirrors real firmware boot) ---- */
    ev_init();
    stats_init();
    nand_init();
    ftl_format();
    ftl_cache_init();
    jnl_init();
    gc_init();
    host_init();
    wl_init();
    sim_state = ST_RUNNING;

    if (!cfg.quiet) {
        printf("ssdsim: %dCH x %ddie x %dblk x %dpg, %dKB page, user %.2f GB, "
               "workload=%s n=%ld q=%d\n",
               cfg.channels, cfg.dies_per_ch, cfg.blocks_per_die,
               cfg.pages_per_block, cfg.page_size / 1024,
               (double)ftl_user_lpn() * cfg.page_size / (1024.0 * 1024 * 1024),
               cfg.workload, cfg.n_ios, cfg.queue_depth);
    }

    bg_timer_reschedule();
    cuts_left = cfg.power_cuts;
    schedule_next_cut();
    wl_kick();

    /* ---- main event loop ---- */
    for (;;) {
        wl_kick();

        if (g_drain == 0 && wl_done() && host_outstanding() == 0 &&
            !g_flushing) {
            if (cfg.verify) {
                g_drain = 1;
                drain_step(NULL);
                continue;
            }
            break;
        }
        if (g_drain == 2 && !g_flushing)
            break;

        if (ev_empty()) {
            if (g_drain == 1)
                drain_step(NULL);
            if (ev_empty()) {
                if (wl_done() && host_outstanding() == 0)
                    break;
                fprintf(stderr, "stall: no events left with work pending\n");
                break;
            }
            continue;
        }
        ev_run_one();
    }

    /* ---- report ---- */
    if (cfg.verify)
        verify_scan();
    wear_scan();
    st.sim_ns = g_now;
    print_report(report_fp ? report_fp : stdout);
    if (report_fp)
        print_report(stdout);
    if (report_fp)
        fclose(report_fp);

    int bad = cfg.verify && st.lost_writes > 0;
    return bad ? 1 : 0;
}
