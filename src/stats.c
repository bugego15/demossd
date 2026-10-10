/* stats.c - latency histograms, wear scan, final report / SMART */
#include "types.h"
#include <math.h>
#include <string.h>

void stats_init(void)
{
    memset(&st, 0, sizeof(st));
    st.pe_min = UINT64_MAX;
}

void hist_add(hist_t *h, uint64_t v)
{
    int b;
    if (v < 8) {
        b = (int)v;
    } else {
        int o = 63 - __builtin_clzll(v);
        int sub = (int)((v >> (o - 3)) & 7);
        b = 8 + (o - 3) * 8 + sub;
    }
    if (b >= HIST_BUCKETS)
        b = HIST_BUCKETS - 1;
    h->b[b]++;
}

static uint64_t bucket_value(int b)
{
    if (b < 8)
        return (uint64_t)b;
    int o = (b - 8) / 8 + 3;
    int sub = (b - 8) % 8;
    uint64_t unit = 1ULL << (o - 3);
    return (1ULL << o) + (uint64_t)sub * unit + unit / 2;
}

uint64_t hist_pct(const hist_t *h, double p)
{
    uint64_t total = 0;
    for (int i = 0; i < HIST_BUCKETS; i++)
        total += h->b[i];
    if (!total)
        return 0;
    uint64_t want = (uint64_t)(total * p);
    if (want < 1)
        want = 1;
    uint64_t cum = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        cum += h->b[i];
        if (cum >= want)
            return bucket_value(i);
    }
    return bucket_value(HIST_BUCKETS - 1);
}

void stats_lat_add(int type, uint64_t lat)
{
    (void)type;
    (void)lat;
}

void wear_scan(void)
{
    int nblocks = nand_nblocks();
    st.pe_min = UINT64_MAX;
    st.pe_max = 0;
    st.pe_sum = 0;
    memset(st.pe_hist, 0, sizeof(st.pe_hist));
    uint64_t n = 0;
    for (int b = 0; b < nblocks; b++) {
        if (blks[b].state == BLK_META || blks[b].state == BLK_BAD)
            continue;
        uint64_t e = blks[b].erase_cnt;
        if (e < st.pe_min)
            st.pe_min = e;
        if (e > st.pe_max)
            st.pe_max = e;
        st.pe_sum += e;
        n++;
        int bucket = 0;
        if (e > 0) {
            bucket = (int)(log10((double)e)) + 1;
            if (bucket > 15)
                bucket = 15;
        }
        st.pe_hist[bucket]++;
    }
    if (n)
        st.pe_sum /= (double)n;
    else
        st.pe_min = 0;
}

#define KB (1024.0)
#define MB (KB * 1024.0)
#define GB (MB * 1024.0)

static void lat_line(FILE *f, const char *name, uint64_t n, uint64_t sum,
                     const hist_t *h, uint64_t mx)
{
    if (!n) {
        fprintf(f, "  %-10s (no samples)\n", name);
        return;
    }
    fprintf(f,
            "  %-10s n=%-8llu avg=%.1fus  p50=%.1fus  p90=%.1fus  "
            "p99=%.1fus  p999=%.1fus  max=%.1fus\n",
            name, (unsigned long long)n, (double)sum / n / 1000.0,
            hist_pct(h, 0.50) / 1000.0, hist_pct(h, 0.90) / 1000.0,
            hist_pct(h, 0.99) / 1000.0, hist_pct(h, 0.999) / 1000.0,
            mx / 1000.0);
}

void print_report(FILE *f)
{
    uint64_t sim_s_ms = g_now / MS;
    double sim_sec = g_now / 1e9;

    fprintf(f, "\n================ SSD FIRMWARE SIMULATOR REPORT ================\n");
    fprintf(f, "-- configuration ------------------------------------------------\n");
    fprintf(f, "  geometry     : %dCH x %ddie x %dblk x %dpg x %dKB "
               "= %.2f GB raw, %.2f GB user (%d pages)\n",
            cfg.channels, cfg.dies_per_ch, cfg.blocks_per_die,
            cfg.pages_per_block, cfg.page_size / 1024,
            (double)nand_nppages() * cfg.page_size / GB,
            (double)ftl_user_lpn() * cfg.page_size / GB, ftl_user_lpn());
    fprintf(f, "  op=%.0f%%  meta=%d blocks  bad_blocks=%llu  cache=%d pages (%.1f MB)  "
               "plp=%s budget=%.1fms\n",
            cfg.op_ratio * 100, cfg.meta_blocks,
            (unsigned long long)st.bad_blocks, cfg.cache_pages,
            cfg.cache_pages * cfg.page_size / MB, cfg.plp ? "on" : "off",
            cfg.plp_budget_ns / 1e6);
    fprintf(f, "  timing       : tPROG=%.0fus tR=%.0fus tBERS=%.1fms bus=%dMT/s  "
               "workload=%s\n",
            cfg.tprog_ns / 1000.0, cfg.tr_ns / 1000.0, cfg.tbers_ns / 1e6,
            cfg.bus_mts, cfg.workload);

    fprintf(f, "-- host --------------------------------------------------------\n");
    fprintf(f, "  reads=%llu (%.2f MB)  writes=%llu (%.2f MB)  trims=%llu  "
               "aborted=%llu\n",
            (unsigned long long)st.host_reads, st.host_r_bytes / MB,
            (unsigned long long)st.host_writes, st.host_w_bytes / MB,
            (unsigned long long)st.host_trims,
            (unsigned long long)st.cmds_aborted);
    if (sim_sec > 0)
        fprintf(f, "  bandwidth    : read %.1f MB/s, write %.1f MB/s "
                   "(sim time %.2fs, %llu ms)\n",
                st.host_r_bytes / sim_sec / MB, st.host_w_bytes / sim_sec / MB,
                sim_sec, (unsigned long long)sim_s_ms);
    lat_line(f, "read", st.rd_lat_n, st.rd_lat_sum, &st.rd_hist, st.rd_lat_max);
    lat_line(f, "write", st.wr_lat_n, st.wr_lat_sum, &st.wr_hist, st.wr_lat_max);

    fprintf(f, "-- NAND / FTL ---------------------------------------------------\n");
    fprintf(f, "  ops: read=%llu prog=%llu erase=%llu  read-retries=%llu\n",
            (unsigned long long)st.nand_read, (unsigned long long)st.nand_prog,
            (unsigned long long)st.nand_erase,
            (unsigned long long)st.nand_retry);
    double waf = st.host_wr_pages ?
        (double)st.nand_prog / (double)st.host_wr_pages : 0.0;
    fprintf(f, "  WAF (total nand_prog / host pages) = %.3f  "
               "host_pages=%llu gc_moved=%llu\n",
            waf, (unsigned long long)st.host_wr_pages,
            (unsigned long long)st.gc_moved);
    fprintf(f, "  GC: runs=%llu erased=%llu moved=%llu busy=%.1fms | "
               "WL: runs=%llu moved=%llu\n",
            (unsigned long long)st.gc_count, (unsigned long long)st.gc_erased,
            (unsigned long long)st.gc_moved, st.gc_busy_ns / 1e6,
            (unsigned long long)st.wl_count,
            (unsigned long long)st.wl_moved);

    fprintf(f, "-- write cache --------------------------------------------------\n");
    fprintf(f, "  rd hit=%llu miss=%llu  wr new=%llu hit=%llu  "
               "evicted=%llu flushed=%llu dirty_peak=%llu/%d\n",
            (unsigned long long)st.cache_rd_hit,
            (unsigned long long)st.cache_rd_miss,
            (unsigned long long)st.cache_wr_new,
            (unsigned long long)st.cache_wr_hit,
            (unsigned long long)st.cache_evict,
            (unsigned long long)st.cache_flushed,
            (unsigned long long)st.cache_dirty_peak, cfg.cache_pages);

    fprintf(f, "-- journal / checkpoint / PLP ----------------------------------\n");
    fprintf(f, "  journal: records=%llu segments=%llu  ckpt: count=%llu pages=%llu\n",
            (unsigned long long)st.jnl_records,
            (unsigned long long)st.jnl_segments,
            (unsigned long long)st.ckpt_count,
            (unsigned long long)st.ckpt_pages);
    fprintf(f, "  power: cuts=%llu  plp_flushed=%llu dropped=%llu budget_hits=%llu\n",
            (unsigned long long)st.power_cuts,
            (unsigned long long)st.plp_flushed,
            (unsigned long long)st.plp_dropped,
            (unsigned long long)st.plp_budget_hits);
    fprintf(f, "  recovery: count=%llu pages_scanned=%llu est_time=%.1fms\n",
            (unsigned long long)st.recovery_count,
            (unsigned long long)st.recovery_pages, st.recovery_ns / 1e6);

    if (cfg.verify) {
        fprintf(f, "-- integrity (post-run verification) ---------------------------\n");
        fprintf(f, "  acked writes verified OK : %llu\n",
                (unsigned long long)st.verified_ok);
        fprintf(f, "  LOST (acked but not durable): %llu\n",
                (unsigned long long)st.lost_writes);
        if (st.lost_writes) {
            fprintf(f, "  first lost LPNs:");
            for (int i = 0; i < 10 && st.lost_first[i]; i++)
                fprintf(f, " %llu", (unsigned long long)st.lost_first[i]);
            fprintf(f, "\n");
        }
    }

    fprintf(f, "-- wear / SMART -------------------------------------------------\n");
    fprintf(f, "  PE cycles: min=%llu avg=%.1f max=%llu  "
               "percentage_used=%.1f%% (endurance=%d)\n",
            (unsigned long long)st.pe_min, st.pe_sum,
            (unsigned long long)st.pe_max,
            cfg.endurance ? 100.0 * st.pe_max / cfg.endurance : 0.0,
            cfg.endurance);
    fprintf(f, "  PE distribution:");
    for (int i = 0; i < 16; i++)
        if (st.pe_hist[i]) {
            if (i == 0)
                fprintf(f, "  0:%llu", (unsigned long long)st.pe_hist[i]);
            else
                fprintf(f, "  1e%d:%llu", i - 1,
                        (unsigned long long)st.pe_hist[i]);
        }
    fprintf(f, "\n");
    fprintf(f, "  power_cycles=%llu  unsafe_shutdowns=%llu  "
               "data_units_written=%.1f MB (approx)\n",
            (unsigned long long)(st.recovery_count + 1),
            (unsigned long long)st.power_cuts, st.host_w_bytes / MB);

    fprintf(f, "================================================================\n");
}
