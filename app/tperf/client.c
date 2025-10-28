/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <errno.h>

#include "tperf.h"


volatile int client_shutdown = 0;
static int trace_debug = 0;

static struct connection *create_client_conn(struct test_thread *thread, int sid)
{
	struct connection *conn;
	int message_size = ctx.message_size;
	int response_size = ctx.response_size;

	conn = conn_create(thread, sid);

	conn->is_client = 1;
	conn->test = ctx.test;
	conn->integrity_enabled = ctx.integrity_enabled;
	conn->integrity_off = get_time_in_ns();
	conn->enable_zwrite = ctx.enable_zwrite;
	conn->message_size = message_size;
	conn->response_size = response_size;
	conn->func = ctx.func;
	conn->req_size = ctx.req_size;
	conn->fpga_srv = ctx.fpga_srv;
	conn->pkt_idx = 0;
	conn->req_cpl = 0;
	conn->header_sent = 0;
	conn->cnn.copy_limit = 0;
	conn->cnn.draining = 0;
	conn->cnn.drain_remaining = 0;

	switch (conn->test) {
	case TEST_READ:
		conn->read.budget  = message_size;
		conn->write.budget = 0;
		break;

	case TEST_WRITE:
		conn->read.budget  = 0;
		conn->write.budget = message_size;
		break;

	case TEST_RR:
	case TEST_CRR:
		conn->last_ns = get_time_in_ns();
		conn->read.budget  = response_size;
		conn->write.budget = 0;//message_size;
		break;

	case TEST_RW:
		conn->read.budget  = message_size;
		conn->write.budget = message_size;
		break;
	}

	return conn;
}

static void bootstrap_test(struct test_thread *thread)
{
	int sid;

	while (thread->nr_conn < ctx.nr_conn_per_thread) {
		sid = tpa_connect_to(ctx.server, ctx.port, NULL);
		if (sid < 0) {
			if (ctx.trace_enabled && trace_debug)
				printf("debug: connect failed, sid=%d errno=%d\n", sid, errno);
			break;
		}

		create_client_conn(thread, sid);
		if (ctx.trace_enabled && trace_debug)
			printf("debug: connection created, sid=%d nr_conn=%lu\n", sid, thread->nr_conn);
	}
}

static void *client_test_loop(void *arg)
{
	struct test_thread *thread = arg;
	struct tpa_worker *worker;

	worker = tpa_worker_init();
	if (!worker) {
		fprintf(stderr, "failed to init worker: %s\n", strerror(errno));
		return NULL;
	}
	thread->worker = worker;
	int send_first_pack = 1;

	/* initialize per-thread trace cursor */
	if (ctx.trace_enabled) {
		size_t len = 0;
		const struct trace_entry *entries = trace_get_entries(&len);
		thread->trace_entries = entries;
		thread->trace_len = len;
		thread->trace_index = 0;
		
		/* Check for debug flag via environment variable */
		if (getenv("TPERF_TRACE_DEBUG")) {
			trace_debug = 1;
		}
	}

	while (!client_shutdown) {
		bootstrap_test(thread);

		/* If the connection was dropped before the first send, allow re-kicking */
		if (ctx.trace_enabled && thread->trace_index < thread->trace_len && thread->nr_conn == 0 && send_first_pack == 0)
			send_first_pack = 1;
		
		tpa_worker_run(thread->worker);

		struct connection *c;

		TAILQ_FOREACH(c, &thread->conn_list, thread_node) {
			if (!ctx.trace_enabled) {
				if (c->req_cpl == 0 && send_first_pack == 1) {
					c->write.budget = c->message_size;
					event_queue_add(c, TPA_EVENT_OUT);
					send_first_pack = 0;
				} else if(c->req_cpl == 1 && send_first_pack == 0) {
					c->req_cpl = 0;
					c->write.budget = c->message_size;
					event_queue_add(c, TPA_EVENT_OUT);
				}
				continue;
			}

			/* trace playback mode: send entries sequentially and exit when done */
			uint64_t now = get_time_in_ns();

            /* First packet: set budget and queue OUT to kick first send */
			if (send_first_pack == 1 && thread->trace_index < thread->trace_len) {
				const struct trace_entry *e = &thread->trace_entries[thread->trace_index];
				c->func = e->func;
				/* In trace mode: message_size from -m, req/resp from CSV */
				c->message_size = ctx.message_size;
				c->req_size = (int)e->req_size;
				c->response_size = (int)e->resp_size;
				c->read.budget = c->response_size;
				c->header_sent = 0;
				/* write.budget should be req_size; header is included in first mbuf */
				c->write.budget = c->req_size;
				/* log send */
				printf("trace_send func=%u msg_bytes=%d req_bytes=%d resp_bytes=%d sleep_sec=%.6f\n",
				       e->func, c->message_size, c->req_size, c->response_size, e->sleep_sec);
				if (trace_debug) {
					printf("debug: first_send sid=%d write_budget=%zu write_off=%zu req_cpl=%d\n",
					       c->sid, c->write.budget, c->write.off, c->req_cpl);
				}
				fflush(stdout);
                /* Kick the first send; write path will retry on ENOTCONN if needed */
                event_queue_add(c, TPA_EVENT_OUT);
				send_first_pack = 0;
				uint64_t delta = (uint64_t)(e->sleep_sec * 1e9);
				c->next_send_ns = now + delta;
				thread->trace_index += 1;
				continue;
			}

			/* After each response, schedule the next entry when its delay expires */
            if (c->req_cpl == 1) {
				if (thread->trace_index >= thread->trace_len) {
					/* finished all entries */
					client_shutdown = 1;
					continue;
				}

                if (now >= c->next_send_ns) {
					c->req_cpl = 0;
					const struct trace_entry *e = &thread->trace_entries[thread->trace_index];
					c->func = e->func;
					/* In trace mode: message_size from -m, req/resp from CSV */
					c->message_size = ctx.message_size;
					c->req_size = (int)e->req_size;
					c->response_size = (int)e->resp_size;
				c->read.budget = c->response_size;
				c->header_sent = 0;
				/* write.budget should be req_size; header is included in first mbuf */
				c->write.budget = c->req_size;
				/* log send */
				printf("trace_send func=%u msg_bytes=%d req_bytes=%d resp_bytes=%d sleep_sec=%.6f sid=%d\n",
				       e->func, c->message_size, c->req_size, c->response_size, e->sleep_sec, c->sid);
				if (trace_debug) {
					printf("debug: scheduling send sid=%d write_budget=%zu req_cpl=%d\n",
					       c->sid, c->write.budget, c->req_cpl);
				}
				fflush(stdout);
                event_queue_add(c, TPA_EVENT_OUT);

					uint64_t delta = (uint64_t)(e->sleep_sec * 1e9);
					c->next_send_ns = now + delta;
					thread->trace_index += 1;
                }
				/* Don't queue OUT events when waiting for response (write.budget == 0) */
			}
		}

		if (poll_and_process(thread) < 0)
			break;
	}
	printf("exiting client: %d\n", thread->id);

	if (thread->log){
		char outfile[64];
		snprintf(outfile, sizeof(outfile), "%s/hugepage_thread_%lu.txt", thread->log_dir, (unsigned long)thread->id);
		FILE *fout = fopen(outfile, "w");
		if (!fout) {
			perror("fopen");
			for (int i=0; i<=thread->curr_hugepg;i++)
				munmap(thread->hugepg, HUGEPAGE_SIZE);
			return NULL;
		}
		for (int i = 0; i <= thread->curr_hugepg; i++) {
			size_t to_write = (i == thread->curr_hugepg) ? thread->hugepg_off : HUGEPAGE_SIZE_COMMIT;
			fwrite(thread->hugepg[i], 1, to_write, fout);
		}

		fclose(fout);
		for (int i=0;i<=thread->curr_hugepg; i++)
			munmap(thread->hugepg[i], HUGEPAGE_SIZE);
	}
	return NULL;
}

int tperf_client(void)
{
	spawn_test_threads(client_test_loop);

    struct thread_stats last_stats[ctx.nr_thread];
    memset(last_stats, 0, sizeof(last_stats));

    int loop = 0;

	if (!ctx.trace_enabled) {
		while (ctx.duration-- > 0 ) {
			sleep(1);
			show_stats_once(loop++, last_stats);
		}
		//show_stats();

		client_shutdown = 1;
	} else {
		/* Trace mode: run until the client thread finishes the CSV */
		while (!client_shutdown) {
			sleep(1);
			/* show_stats_once is a no-op in trace mode */
		}
	}

	for (int i = 0; i < ctx.nr_thread; i++)
	  pthread_join(ctx.tid[i], NULL);

	return 0;
}
