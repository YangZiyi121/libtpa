/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>

#include "tperf.h"
#include "offrac.h"

/* Server-side response connection queue helpers (similar to FPGA client) */
void server_request_enqueue(struct test_thread *thread, struct connection *conn)
{
	if (conn->in_server_waiting_requests)
		return;

	TAILQ_INSERT_TAIL(&thread->server_waiting_requests, conn, server_queue_node);
	conn->in_server_waiting_requests = 1;
}

void server_response_enqueue(struct test_thread *thread, struct connection *conn)
{
	if (conn->in_server_waiting_responses)
		return;

	TAILQ_INSERT_TAIL(&thread->server_waiting_responses, conn, server_queue_node);
	conn->in_server_waiting_responses = 1;
}

static struct connection *server_request_pop(struct test_thread *thread)
{
	struct connection *conn = TAILQ_FIRST(&thread->server_waiting_requests);

	if (conn) {
		TAILQ_REMOVE(&thread->server_waiting_requests, conn, server_queue_node);
		conn->in_server_waiting_requests = 0;
	}

	return conn;
}

static struct connection *server_response_pop(struct test_thread *thread)
{
	struct connection *conn = TAILQ_FIRST(&thread->server_waiting_responses);

	if (conn) {
		TAILQ_REMOVE(&thread->server_waiting_responses, conn, server_queue_node);
		conn->in_server_waiting_responses = 0;
	}

	return conn;
}

/* Helper to extract chain function digits from -F XY
 * Returns: first_digit (to process locally), second_digit (to forward)
 * For single digit, first_digit = digit, second_digit = 0 */
static void extract_chain_digits(int func, int *first_digit, int *second_digit)
{
	if (func < 10) {
		/* Single digit: process locally, no forward */
		*first_digit = func;
		*second_digit = 0;
	} else {
		/* Multi-digit: first digit is tens place, second is ones place */
		*first_digit = func / 10;  /* e.g., 21 -> 2 */
		*second_digit = func % 10; /* e.g., 21 -> 1 */
	}
}

void server_bind_pair(struct connection *request, struct connection *response)
{
	request->server_response_conn = response;
	request->server_response_ready = 1;

	response->is_server_response = 1;
	response->server_request_conn = request;
	response->test = request->test;
	response->response_size = request->response_size;
	response->req_size = request->req_size;
	response->message_size = request->message_size;
	response->enable_zwrite = request->enable_zwrite;

	/* Chain mode: extract function to forward from second digit */
	if (ctx.chain_mode && request->func >= 10) {
		int first_digit, second_digit;
		extract_chain_digits(request->func, &first_digit, &second_digit);
		response->func = second_digit;  /* Forward with this function ID */
		if (ctx.server_debug) {
			printf("[server-debug] Chain mode: request func=%d, process=%d, forward func=%d\n",
			       request->func, first_digit, second_digit);
		}
	} else {
		response->func = request->func;
	}

	/* Don't copy the buffer pointer - response will read from request connection.
	 * This prevents any double-free issues. */
	response->reassemble.reassembly_buf = NULL;
	response->reassemble.off = 0;

	/* Set write budget to response size.
	 * In chain mode, add header size since we prepend test_info. */
	if (ctx.chain_mode) {
		response->write.budget = request->response_size + sizeof(struct test_info);
		if (ctx.server_debug) {
			printf("[server-debug] Chain mode: write.budget=%zu (response_size=%u + header=%zu)\n",
			       response->write.budget, request->response_size, sizeof(struct test_info));
		}
	} else {
		response->write.budget = request->response_size;
	}
	response->write.off = 0;
	response->read.budget = 0;
	response->read.off = 0;

	/* Trigger the write on the response connection */
	event_queue_add(response, TPA_EVENT_OUT);

	if (ctx.server_debug) {
		printf("[server-debug] Paired request sid=%d with response sid=%d\n",
		       request->sid, response->sid);
	}
}

void server_try_pair(struct test_thread *thread)
{
	while (!TAILQ_EMPTY(&thread->server_waiting_requests) &&
	       !TAILQ_EMPTY(&thread->server_waiting_responses)) {
		struct connection *request = server_request_pop(thread);
		struct connection *response = server_response_pop(thread);

		server_bind_pair(request, response);
	}
}

/* Open a new connection to send response (open-connection mode) */
static void open_response_connection(struct test_thread *thread, struct connection *request_conn)
{
	int response_port;
	int sid;
	struct connection *response_conn;

	/* Use the port offset that was determined when this connection was accepted.
	 * This maintains 1:1 mapping between request and response ports. */
	response_port = ctx.server_response_port + request_conn->server_port_offset;

	if (ctx.server_debug) {
		printf("[server-debug] thread %d opening response connection to %s:%d for request sid=%d\n",
		       thread->id, ctx.server_response_addr, response_port, request_conn->sid);
	}

	sid = tpa_connect_to(ctx.server_response_addr, response_port, NULL);
	if (sid < 0) {
		fprintf(stderr, "[server] failed to connect to %s:%d for response: %s\n",
			ctx.server_response_addr, response_port, strerror(errno));
		/* Re-queue the request to retry later */
		server_request_enqueue(thread, request_conn);
		return;
	}

	/* Create the response connection */
	response_conn = conn_create(thread, sid);
	if (response_conn == NULL) {
		fprintf(stderr, "[server] failed to create response connection\n");
		server_request_enqueue(thread, request_conn);
		return;
	}

	response_conn->is_server_response = 1;
	response_conn->server_response_ready = 0; /* Will be set to 1 when connection is established */

	thread->stats->server_response_conns++;

	if (ctx.server_debug) {
		printf("[server-debug] thread %d created response conn sid=%d, waiting for TCP handshake (total=%lu)\n",
		       thread->id, sid, (unsigned long)thread->stats->server_response_conns);
	}

	/* Store the request connection reference so we can pair them later
	 * when the response connection becomes established (TPA_EVENT_OUT) */
	response_conn->server_request_conn = request_conn;
	request_conn->server_response_conn = response_conn;
	
	/* The connection will be paired and data sent when TPA_EVENT_OUT fires,
	 * indicating the TCP handshake is complete */
}

/* Process pending response connections - open new connection for each waiting request */
static void process_pending_responses(struct test_thread *thread)
{
	struct connection *request_conn;

	/* Process one request at a time to avoid overwhelming */
	request_conn = server_request_pop(thread);
	if (request_conn == NULL)
		return;

	/* Open a new connection to send the response */
	open_response_connection(thread, request_conn);
}

void init_server_conn(struct connection *conn)
{
	int message_size = conn->info.message_size;
	int response_size = conn->info.response_size;

	conn->test = conn->info.test;
	conn->integrity_enabled = conn->info.integrity_enabled;
	conn->integrity_off = conn->info.integrity_off;
	conn->enable_zwrite = conn->info.enable_zwrite;
	conn->message_size = message_size;
	conn->response_size = response_size;
	conn->func = conn->info.func;

	/* Allocate or reallocate reassembly buffer if needed */
	if (conn->reassemble.reassembly_buf == NULL) {
		/* First time: allocate new buffer */
		conn->reassemble.reassembly_buf = (uint8_t *)malloc(conn->info.req_size);
	} else if (conn->req_size != conn->info.req_size) {
		/* Size changed: reallocate */
		free(conn->reassemble.reassembly_buf);
		conn->reassemble.reassembly_buf = (uint8_t *)malloc(conn->info.req_size);
	}
	/* else: reuse existing buffer */
	
	conn->req_size = conn->info.req_size;
	conn->pkt_idx = 0;
	conn->reassemble.off = 0;

	/* Initialize idmap if MAPID is present in chained -F (e.g., 12 includes 2) */
	{
		int f = conn->func;
		int has_mapid = 0;
		while (f > 0) {
			if ((f % 10) == MAPID) { /* MAPID has enum value 2 */
				has_mapid = 1;
				break;
			}
			f /= 10;
		}
		if (has_mapid) {
			/* Initialize the idmap from COE file once */
			idmap_init_from_coe_once();
		}
	}	


	if (conn->reassemble.reassembly_buf == NULL) {
	      printf("Reassembly buffer Memory allocation failed!\n");
	      exit(1);
	}

	switch (conn->test) {
	case TEST_READ:
		conn->read.budget  = 0;
		conn->write.budget = message_size;
		event_queue_add(conn, TPA_EVENT_OUT);
		break;

	case TEST_WRITE:
		conn->read.budget  = message_size;
		conn->write.budget = 0;
		break;

	case TEST_RW:
		event_queue_add(conn, TPA_EVENT_OUT);
		conn->write.budget = message_size;
		conn->read.budget  = message_size;
		break;

	case TEST_RR:
	case TEST_CRR:
		/* For CPU server, read the actual request payload size (req_size).
		 * The client sends req_size bytes of payload (after the test_info header). */
		conn->read.budget  = conn->req_size;
		conn->write.budget = 0; /* write only after we got the req */
		break;
	}
}

/* Store port offset in listener's data field - we'll retrieve this on accept */
static int *port_offset_data[256];  /* Global array to store port offsets */

/* Start listener for a specific thread (per-thread port in open-connection mode) */
static void start_server_thread_listener(struct test_thread *thread)
{
	struct tpa_sock_opts opts;
	int port;
	int sid;
	int i;

	memset(&opts, 0, sizeof(opts));

	/* In open-connection mode with multiple ports, only thread 0 creates the listeners.
	 * Use listen_scaling=1 so all threads can accept from all ports.
	 * This allows flexible port:thread mapping. */
	if (ctx.server_response_port != 0) {
		/* Only thread 0 creates all the listeners */
		if (thread->id != 0)
			return;
		
		/* Use listen_scaling=1 so all threads can accept connections from any port */
		opts.listen_scaling = 1;
		
		/* Create listener for each port */
		for (i = 0; i < ctx.nr_ports; i++) {
			port = ctx.port + i;
			
			/* Store port offset in data field so we can retrieve it on accept */
			port_offset_data[i] = (int *)malloc(sizeof(int));
			*port_offset_data[i] = i;
			opts.data = port_offset_data[i];
			
			sid = tpa_listen_on(ctx.local, port, &opts);
			if (sid < 0) {
				fprintf(stderr, "failed to listen on port %d\n", port);
				exit(1);
			}
			
			if (ctx.server_debug) {
				printf("[server-debug] thread 0 created listener on port %d (offset=%d, data=%p)\n", 
				       port, i, opts.data);
			}
		}
		
		if (ctx.server_response_addr != NULL) {
			printf("[server] Open-connection mode enabled:\n");
			printf("[server]   Threads: %d\n", ctx.nr_thread);
			printf("[server]   Request ports: %d-%d (%d ports)\n", 
			       ctx.port, ctx.port + ctx.nr_ports - 1, ctx.nr_ports);
			printf("[server]   Response ports: %d-%d (%d ports)\n", 
			       ctx.server_response_port, ctx.server_response_port + ctx.nr_ports - 1, ctx.nr_ports);
			printf("[server]   All threads can accept from all ports (listen_scaling=1)\n");
		}
	} else {
		/* Non-open-connection mode: shared listener with scaling */
		opts.listen_scaling = 1;
		opts.data = NULL;
		port = ctx.port;
		/* Only thread 0 creates the shared listener */
		if (thread->id != 0)
			return;
		
		sid = tpa_listen_on(ctx.local, port, &opts);
		if (sid < 0) {
			fprintf(stderr, "failed to listen on port %d\n", port);
			exit(1);
		}
		
		if (ctx.server_debug) {
			printf("[server-debug] thread %d listening on port %d (shared)\n", thread->id, port);
		}
	}
}

static void accept_socks(struct test_thread *thread)
{
	int sid[BATCH_SIZE];
	int nr_sock;
	int i;
	struct connection *conn;
	struct tpa_sock_info sock_info;

	nr_sock = tpa_accept_burst(thread->worker, sid, BATCH_SIZE);
	for (i = 0; i < nr_sock; i++) {
		conn = conn_create(thread, sid[i]);
		
		/* In open-connection mode, get port offset from listener's data field */
		if (ctx.server_response_port != 0) {
			if (tpa_sock_info_get(sid[i], &sock_info) == 0 && sock_info.data != NULL) {
				/* Retrieve port offset that was stored in listener opts.data */
				conn->server_port_offset = *(int *)sock_info.data;
				
				if (ctx.server_debug) {
					printf("[server-debug] Accepted connection on port %d (offset=%d from data field)\n",
					       ctx.port + conn->server_port_offset, conn->server_port_offset);
				}
				
				/* Sanity check */
				if (conn->server_port_offset < 0 || conn->server_port_offset >= ctx.nr_ports) {
					fprintf(stderr, "[server-error] Invalid port offset %d from data field (expected 0-%d)\n",
						conn->server_port_offset, ctx.nr_ports - 1);
					conn->server_port_offset = 0;
				}
			} else {
				fprintf(stderr, "[server-warning] Could not get port offset for sid %d (data=%p)\n", 
					sid[i], sock_info.data);
				conn->server_port_offset = 0; /* Default to first port */
			}
		} else {
			conn->server_port_offset = 0;
		}
	}
}

static void *server_thread_loop(void *arg)
{
	struct test_thread *thread = arg;
	struct tpa_worker *worker;

	worker = tpa_worker_init();
	if (!worker) {
		fprintf(stderr, "failed to init worker: %s\n", strerror(errno));
		return NULL;
	}
	thread->worker = worker;

	/* Each thread starts its own listener (in open-connection mode)
	 * or only thread 0 starts a shared listener (in normal mode) */
	start_server_thread_listener(thread);

	while (1) {
		tpa_worker_run(thread->worker);

		accept_socks(thread);

		/* Process pending response connections (open-connection mode) */
		if (ctx.server_response_port != 0) {
			process_pending_responses(thread);
		}

		poll_and_process(thread);
	}
	return NULL;
}

int tperf_server(void)
{
	spawn_test_threads(server_thread_loop);

	while (1)
		sleep(1);

	return 0;
}
