/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>

#include "tperf.h"

struct ctx ctx;

int main(int argc, char **argv)
{
	parse_options(argc, argv);
	integrity_init();

    if (ctx.trace_enabled) {
        int rc = trace_load(ctx.trace_file);
        if (rc < 0) {
            fprintf(stderr, "failed to load trace '%s' (%d)\n", ctx.trace_file, rc);
            return 1;
        }
    }

	if (ctx.is_client)
		return tperf_client();

	return tperf_server();
}
