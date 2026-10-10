/* host.c - host command layer: split into page I/Os, completion, purge */
#include "types.h"
#include <stdlib.h>

static host_cmd_t *out_list;    /* in-flight commands */
static host_cmd_t *free_list;
static int out_n;
static int stopped;

void host_init(void)
{
    out_list = NULL;
    free_list = NULL;
    out_n = 0;
    stopped = 0;
}

static host_cmd_t *cmd_alloc(void)
{
    if (free_list) {
        host_cmd_t *c = free_list;
        free_list = c->next;
        return c;
    }
    return (host_cmd_t *)calloc(1, sizeof(host_cmd_t));
}

int host_outstanding(void) { return out_n; }
void host_stop(void) { stopped = 1; }
int host_stopped(void) { return stopped; }

void host_resume(void) { stopped = 0; }

void host_submit(int type, uint64_t lba, uint32_t nlb, uint64_t seq)
{
    int sps = cfg.page_size / cfg.sector_size;   /* sectors per page */
    int u = ftl_user_lpn();

    host_cmd_t *c = cmd_alloc();
    c->type = type;
    c->lba = lba;
    c->nlb = nlb;
    c->seq = seq;
    c->submit_ns = g_now;
    c->acked = 0;
    c->left = 0;

    int first = (int)(lba / (uint64_t)sps);
    int last = (int)((lba + nlb - 1) / (uint64_t)sps);
    if (first >= u)
        first = u - 1;
    if (last >= u)
        last = u - 1;
    if (last < first)
        last = first;
    c->first_lpn = first;
    c->last_lpn = last;

    c->next = out_list;
    out_list = c;
    out_n++;

    if (type == CMD_TRIM) {
        st.host_trims++;
        ftl_trim_cmd(c, seq);
        return;
    }

    int stream = wl_in_fill() ? STREAM_COLD : STREAM_HOT;
    int npages = last - first + 1;
    c->left = npages;

    if (type == CMD_READ) {
        st.host_reads++;
        st.host_r_bytes += (uint64_t)nlb * cfg.sector_size;
        for (int l = first; l <= last; l++)
            ftl_read((uint32_t)l, c);
    } else {
        st.host_writes++;
        st.host_w_bytes += (uint64_t)nlb * cfg.sector_size;
        for (int l = first; l <= last; l++) {
            uint64_t p_start = (uint64_t)l * sps;
            uint64_t p_end = p_start + sps;
            int partial = (lba > p_start) || (lba + nlb < p_end);
            ftl_write((uint32_t)l, seq, stream, partial, c);
        }
    }
}

void host_cmd_done(host_cmd_t *cmd, int aborted)
{
    /* unlink from the in-flight list */
    host_cmd_t **pp = &out_list;
    while (*pp && *pp != cmd)
        pp = &(*pp)->next;
    if (*pp == cmd) {
        *pp = cmd->next;
        out_n--;
    }

    if (aborted) {
        st.cmds_aborted++;
        cmd->next = free_list;
        free_list = cmd;
        return;
    }

    uint64_t lat = g_now - cmd->submit_ns;
    if (cmd->type == CMD_READ) {
        st.rd_lat_n++;
        st.rd_lat_sum += lat;
        hist_add(&st.rd_hist, lat);
        if (lat > st.rd_lat_max)
            st.rd_lat_max = lat;
    } else if (cmd->type == CMD_WRITE) {
        st.wr_lat_n++;
        st.wr_lat_sum += lat;
        hist_add(&st.wr_hist, lat);
        if (lat > st.wr_lat_max)
            st.wr_lat_max = lat;
    } else {
        st.tr_lat_n++;
    }

    wl_on_complete(cmd);

    cmd->next = free_list;
    free_list = cmd;
}

void host_purge(void)
{
    host_cmd_t *c = out_list;
    out_list = NULL;
    while (c) {
        host_cmd_t *n = c->next;
        st.cmds_aborted++;
        c->next = free_list;
        free_list = c;
        c = n;
    }
    out_n = 0;
}
