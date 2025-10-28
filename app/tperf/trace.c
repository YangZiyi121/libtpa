/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "tperf.h"

/* definition moved to tperf.h */

static struct trace_entry *g_entries;
static size_t g_entries_len;

/* expected CSV header: app,sleep_time,request_size,response_size */
int trace_load(const char *path)
{
    FILE *fp;
    char line[256];

    if (!path)
        return -EINVAL;

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "trace_load: failed to open %s: %s\n", path, strerror(errno));
        return -errno;
    }

    /* free old */
    free(g_entries);
    g_entries = NULL;
    g_entries_len = 0;

    /* read header */
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -EINVAL;
    }

    size_t cap = 1024;
    g_entries = malloc(cap * sizeof(*g_entries));
    if (!g_entries) {
        fclose(fp);
        return -ENOMEM;
    }

    while (fgets(line, sizeof(line), fp)) {
        /* Trim newline */
        char *p = strchr(line, '\n');
        if (p) *p = '\0';

        /* Columns: app,sleep_time,request_size,response_size */
        char *saveptr = NULL;
        char *tok_app = strtok_r(line, ",", &saveptr);
        char *tok_sleep = strtok_r(NULL, ",", &saveptr);
        char *tok_req = strtok_r(NULL, ",", &saveptr);
        char *tok_resp = strtok_r(NULL, ",", &saveptr);
        if (!tok_app || !tok_sleep || !tok_req || !tok_resp)
            continue;

        if (g_entries_len == cap) {
            cap *= 2;
            struct trace_entry *newbuf = realloc(g_entries, cap * sizeof(*g_entries));
            if (!newbuf) {
                fclose(fp);
                free(g_entries);
                g_entries = NULL;
                g_entries_len = 0;
                return -ENOMEM;
            }
            g_entries = newbuf;
        }

        struct trace_entry *e = &g_entries[g_entries_len++];
        e->func = (uint32_t)strtoul(tok_app, NULL, 10);
        e->sleep_sec = strtod(tok_sleep, NULL);
        e->req_size = (uint32_t)strtoul(tok_req, NULL, 10);
        e->resp_size = (uint32_t)strtoul(tok_resp, NULL, 10);
    }

    fclose(fp);
    return 0;
}

const struct trace_entry *trace_get_entries(size_t *len_out)
{
    if (len_out)
        *len_out = g_entries_len;
    return g_entries;
}


