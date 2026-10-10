/* workload.c - host workload generators: fill, seq, random, zipf, trace */
#include "types.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static long generated, target;
static int phase;               /* 0 = fill, 1 = steady, 2 = done */
static uint64_t seq_ctr;
static uint64_t fill_cursor;    /* next sector to write in fill phase */
static uint64_t seq_cursor;     /* lpn cursor for sequential steady */
static FILE *trace_f;
static uint64_t rng;

static uint32_t *zipf_cdf;      /* over lpn */
static int zipf_n;
static uint32_t zipf_span;

uint64_t wl_next_seq(void) { return ++seq_ctr; }
long wl_generated(void) { return generated; }

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static double rndf(void)
{
    return (rnd64() >> 11) * (1.0 / 9007199254740992.0); /* 0..1 */
}

void wl_init(void)
{
    int sps = cfg.page_size / cfg.sector_size;
    target = cfg.n_ios;
    seq_ctr = 0;
    generated = 0;
    fill_cursor = 0;
    seq_cursor = 0;
    rng = cfg.seed ? cfg.seed * 6364136223846793005ULL + 1 : 0x123456789ULL;
    phase = (cfg.fill && strcmp(cfg.workload, "seq") != 0) ? 0 : 1;
    if (cfg.fill && strcmp(cfg.workload, "seq") == 0)
        phase = 1;              /* pure sequential workload does its own walk */

    trace_f = NULL;
    if (strcmp(cfg.workload, "trace") == 0) {
        if (cfg.trace_path[0]) {
            trace_f = fopen(cfg.trace_path, "r");
            if (!trace_f)
                fprintf(stderr, "cannot open trace %s\n", cfg.trace_path);
        }
    }

    zipf_cdf = NULL;
    if (strcmp(cfg.workload, "zipf") == 0) {
        int u = ftl_user_lpn();
        zipf_cdf = (uint32_t *)malloc((size_t)u * sizeof(uint32_t));
        double acc = 0;
        for (int i = 0; i < u; i++)
            acc += pow((double)(i + 1), -cfg.zipf_s);
        double run = 0;
        for (int i = 0; i < u; i++) {
            run += pow((double)(i + 1), -cfg.zipf_s);
            double f = run / acc;
            uint32_t v = (uint32_t)(f * 4294967000.0);
            zipf_cdf[i] = v;
        }
        zipf_n = u;
        zipf_span = zipf_cdf[u - 1];
    }
    (void)sps;
}

int wl_done(void) { return phase == 2; }
int wl_in_fill(void) { return phase == 0; }

static uint32_t zipf_pick(void)
{
    uint32_t r = (uint32_t)(rnd64() >> 8);
    if (r > zipf_span)
        r = zipf_span;
    int lo = 0, hi = zipf_n - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (zipf_cdf[mid] < r)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (uint32_t)lo;
}

static void issue_one(void)
{
    int sps = cfg.page_size / cfg.sector_size;
    int u = ftl_user_lpn();
    int ios = cfg.io_sectors > 0 ? cfg.io_sectors : sps;

    if (phase == 0) {
        uint64_t tgt = (uint64_t)((double)u * sps * cfg.fill_pct / 100.0);
        if (fill_cursor >= tgt) {
            phase = 1;
            return;
        }
        uint64_t nlb = (uint64_t)ios;
        if (fill_cursor + nlb > tgt)
            nlb = tgt - fill_cursor;
        host_submit(CMD_WRITE, fill_cursor, (uint32_t)nlb, wl_next_seq());
        fill_cursor += nlb;
        return;
    }

    if (phase != 1)
        return;

    if (trace_f) {
        char line[128];
        while (fgets(line, sizeof(line), trace_f)) {
            char t;
            unsigned long long lba, nlb;
            if (line[0] == '#' || line[0] == '\n')
                continue;
            if (sscanf(line, "%c %llu %llu", &t, &lba, &nlb) != 3)
                continue;
            int type = (t == 'R' || t == 'r') ? CMD_READ :
                       (t == 'T' || t == 't') ? CMD_TRIM : CMD_WRITE;
            host_submit(type, lba, (uint32_t)nlb, wl_next_seq());
            generated++;
            if (generated >= target)
                phase = 2;
            return;
        }
        fclose(trace_f);
        trace_f = NULL;
        phase = 2;
        return;
    }

    int is_read = rndf() < cfg.read_ratio;
    uint32_t lpn;
    if (strcmp(cfg.workload, "seq") == 0) {
        lpn = (uint32_t)(seq_cursor++ % (uint64_t)u);
    } else if (strcmp(cfg.workload, "zipf") == 0) {
        lpn = zipf_pick();
    } else {
        lpn = (uint32_t)(rnd64() % (uint64_t)u);
    }
    uint64_t lba = (uint64_t)lpn * sps;
    uint32_t nlb = (uint32_t)(ios < sps ? ios : sps);
    host_submit(is_read ? CMD_READ : CMD_WRITE, lba, nlb, wl_next_seq());
    generated++;
    if (generated >= target)
        phase = 2;
}

void wl_kick(void)
{
    if (host_stopped())
        return;
    if (sim_state == ST_OFF || sim_state == ST_PLP)
        return;
    while (!wl_done() && host_outstanding() < cfg.queue_depth)
        issue_one();
}

void wl_ack_range(int first_lpn, int last_lpn, uint64_t seq)
{
    int u = ftl_user_lpn();
    int k = 0;
    for (int l = first_lpn; l <= last_lpn && l < u; l++) {
        ack_seq[l] = seq;
        k++;
    }
    if (cfg.trace_v && k)
        log_v("ACK lpn %d..%d seq=%llu\n", first_lpn, last_lpn,
              (unsigned long long)seq);
}

void wl_trim_ack(uint64_t lba, uint32_t nlb)
{
    int sps = cfg.page_size / cfg.sector_size;
    int u = ftl_user_lpn();
    int first = (int)(lba / (uint64_t)sps);
    int last = (int)((lba + nlb - 1) / (uint64_t)sps);
    if (last >= u)
        last = u - 1;
    for (int l = first; l <= last && l < u; l++)
        ack_seq[l] = 0;
}

static long completed;

void wl_on_complete(host_cmd_t *cmd)
{
    if (cmd->type == CMD_WRITE)
        wl_ack_range(cmd->first_lpn, cmd->last_lpn, cmd->seq);
    else if (cmd->type == CMD_TRIM)
        wl_trim_ack(cmd->lba, cmd->nlb);
    completed++;
    if (cfg.trace_v && (completed % 20000) == 0)
        fprintf(stderr, "progress: %ld cmds, sim %.1f ms, free=%d, gc=%llu\n",
                completed, g_now / 1e6, ftl_free_blocks(),
                (unsigned long long)st.gc_count);
    wl_kick();
}
