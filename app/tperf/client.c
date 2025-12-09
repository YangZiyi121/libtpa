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

static void start_fpga_reply_listener(struct test_thread *thread)
{
	struct tpa_sock_opts opts;
	int port;
	int listen_sid;

	memset(&opts, 0, sizeof(opts));
	/*
	 * Each thread listens on a dedicated reply port (base + id).  Keep the
	 * listener bound to the worker that created it so RSS won't redirect all
	 * replies to thread 0.
	 */
	opts.listen_scaling = 0;

	port = ctx.fpga_reply_port + thread->id;

	listen_sid = tpa_listen_on(ctx.fpga_reply_addr, port, &opts);
	if (listen_sid < 0) {
		fprintf(stderr, "failed to listen for fpga replies on %s:%d: %s\n",
			ctx.fpga_reply_addr, port, strerror(errno));
		exit(1);
	}

	if (ctx.fpga_debug) {
		printf("[fpga-debug] thread %d binding FPGA reply listener sid=%d to %s:%d (worker slot %d)\n",
		       thread->id, listen_sid, ctx.fpga_reply_addr, port, thread->id);
	}
}

static void fpga_request_enqueue(struct test_thread *thread, struct connection *conn)
{
	if (conn->in_fpga_waiting_requests)
		return;

	TAILQ_INSERT_TAIL(&thread->fpga_waiting_requests, conn, fpga_queue_node);
	conn->in_fpga_waiting_requests = 1;
}

static void fpga_reply_enqueue(struct test_thread *thread, struct connection *conn)
{
	if (conn->in_fpga_waiting_replies)
		return;

	TAILQ_INSERT_TAIL(&thread->fpga_waiting_replies, conn, fpga_queue_node);
	conn->in_fpga_waiting_replies = 1;
}

static struct connection *fpga_request_pop(struct test_thread *thread)
{
	struct connection *conn = TAILQ_FIRST(&thread->fpga_waiting_requests);

	if (conn) {
		TAILQ_REMOVE(&thread->fpga_waiting_requests, conn, fpga_queue_node);
		conn->in_fpga_waiting_requests = 0;
	}

	return conn;
}

static struct connection *fpga_reply_pop(struct test_thread *thread)
{
	struct connection *conn = TAILQ_FIRST(&thread->fpga_waiting_replies);

	if (conn) {
		TAILQ_REMOVE(&thread->fpga_waiting_replies, conn, fpga_queue_node);
		conn->in_fpga_waiting_replies = 0;
	}

	return conn;
}

static void fpga_bind_pair(struct connection *request,
			    struct connection *reply)
{
	request->fpga_reply_conn = reply;
	request->fpga_ready = 1;

	reply->is_fpga_reply = 1;
	reply->fpga_requester = request;
	reply->fpga_srv = request->fpga_srv;
	reply->test = request->test;
	reply->response_size = request->response_size;
	reply->read.budget = request->read.budget;
	reply->write.budget = 0;

	/* Mark warmup done once paired - stats will start counting now */
	if (!request->fpga_warmup_done) {
		request->fpga_warmup_done = 1;
	}
}

static void fpga_try_pair(struct test_thread *thread)
{
	while (!TAILQ_EMPTY(&thread->fpga_waiting_requests) &&
	       !TAILQ_EMPTY(&thread->fpga_waiting_replies)) {
		struct connection *request = fpga_request_pop(thread);
		struct connection *reply = fpga_reply_pop(thread);

		fpga_bind_pair(request, reply);
	}
}

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
	conn->fpga_ready = 1;
	conn->fpga_warmup_done = 0;

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
		/* Set write budget based on target: FPGA (Z=1) uses message_size; CPU uses req_size + header */
		if (conn->fpga_srv == 1) {
			/* FPGA version */
			conn->write.budget = conn->message_size;
		} else {
			/* CPU version: send payload of req_size plus the 64-byte test_info header */
			conn->write.budget = conn->req_size + sizeof(struct test_info);
		}
		break;

	case TEST_RW:
		conn->read.budget  = message_size;
		conn->write.budget = message_size;
		break;
	}

	thread->nr_client_conn += 1;

	if (ctx.fpga_srv == 1 && (conn->test == TEST_RR || conn->test == TEST_CRR)) {
		/* Mark as ready to send first request, which triggers FPGA connection */
		conn->fpga_ready = 1;
		fpga_request_enqueue(thread, conn);
		fpga_try_pair(thread);
	}

	return conn;
}

static void bootstrap_test(struct test_thread *thread)
{
	int sid;
	int server_port = ctx.port + thread->id;

	if (server_port <= 0 || server_port >= 65536) {
		fprintf(stderr, "invalid per-thread server port: %d\n", server_port);
		return;
	}

	while (thread->nr_client_conn < ctx.nr_conn_per_thread) {
		sid = tpa_connect_to(ctx.server, server_port, NULL);
		if (sid < 0)
			break;

		create_client_conn(thread, sid);
	}
}

static void accept_fpga_replies(struct test_thread *thread)
{
	int sid[BATCH_SIZE];
	int nr_sock;
	int i;

	nr_sock = tpa_accept_burst(thread->worker, sid, BATCH_SIZE);
	if (nr_sock <= 0)
		return;

	thread->stats->fpga_reply_accepts += nr_sock;
	if (ctx.fpga_debug) {
		printf("[fpga-debug] thread %d accepted %d FPGA reply sockets on port %d (total=%lu)\n",
		       thread->id, nr_sock, ctx.fpga_reply_port + thread->id,
		       (unsigned long)thread->stats->fpga_reply_accepts);
	}

	for (i = 0; i < nr_sock; i++) {
		struct connection *conn = conn_create(thread, sid[i]);

		conn->fpga_ready = 1;
		conn->is_fpga_reply = 1;
		conn->fpga_srv = ctx.fpga_srv;
		conn->write.budget = 0;
		/*
		 * Set read.budget to response_size immediately so that if data
		 * arrives before pairing, fpga_reply_on_read() can buffer it.
		 * The pairing will copy this to the request connection.
		 */
		conn->read.budget = ctx.response_size;

		fpga_reply_enqueue(thread, conn);
		fpga_try_pair(thread);
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

	if (ctx.fpga_srv == 1) {
		start_fpga_reply_listener(thread);
		
		/* Wait for all threads to bind their reply listeners before sending requests */
		pthread_barrier_wait(&ctx.fpga_barrier);
		
		if (thread->id == 0 && ctx.fpga_debug) {
			printf("[fpga-debug] All %d FPGA reply listeners ready, starting requests\n", ctx.nr_thread);
		}
	}

	while (!client_shutdown) {
		bootstrap_test(thread);

		tpa_worker_run(thread->worker);

		if (ctx.fpga_srv == 1)
			accept_fpga_replies(thread);

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
	show_stats();

	client_shutdown = 1;

	for (int i = 0; i < ctx.nr_thread; i++)
	  pthread_join(ctx.tid[i], NULL);

	/* Clean up barrier */
	if (ctx.fpga_srv == 1) {
		pthread_barrier_destroy(&ctx.fpga_barrier);
	}

	return 0;
}
