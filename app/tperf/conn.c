/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>

#include "tperf.h"

struct connection *conn_create(struct test_thread *thread, int sid)
{
	struct connection *conn = zmalloc_assert(sizeof(struct connection));
	struct tpa_event event;

	event.events = TPA_EVENT_IN | TPA_EVENT_OUT;
	event.data = conn;
	tpa_event_ctrl(sid, TPA_EVENT_CTRL_ADD, &event);

	conn->sid = sid;
	conn->thread = thread;

	TAILQ_INSERT_TAIL(&thread->conn_list, conn, thread_node);
	thread->sid_mappings[sid] = conn;
	thread->nr_conn += 1;
	thread->stats->nr_conn_total += 1;

	return conn_get(conn);
}

void conn_close(struct connection *conn)
{
	struct test_thread *thread = conn->thread;

	if (conn->in_fpga_waiting_requests) {
		TAILQ_REMOVE(&thread->fpga_waiting_requests, conn, fpga_queue_node);
		conn->in_fpga_waiting_requests = 0;
	}

	if (conn->in_fpga_waiting_replies) {
		TAILQ_REMOVE(&thread->fpga_waiting_replies, conn, fpga_queue_node);
		conn->in_fpga_waiting_replies = 0;
	}

	if (conn->is_fpga_reply) {
		if (conn->fpga_requester) {
			conn->fpga_requester->fpga_reply_conn = NULL;
			conn->fpga_requester->fpga_ready = 0;
			/* In CPU open-connection mode (fpga_srv=0), the reply connection closing
			 * is expected after each response. The request connection should stay
			 * alive to send more requests. Only close requester in FPGA mode. */
			if (conn->fpga_srv == 1 && !conn->fpga_requester->to_close)
				conn->fpga_requester->to_close = 1;
			conn->fpga_requester = NULL;
		}
	} else if (conn->fpga_reply_conn) {
		conn->fpga_reply_conn->fpga_requester = NULL;
		conn->fpga_reply_conn->to_close = 1;
		conn->fpga_reply_conn = NULL;
	}

	if (conn->stats.bytes_read == 0 && conn->stats.bytes_write == 0)
		thread->stats->nr_zero_io_conn += 1;

	TAILQ_REMOVE(&thread->conn_list, conn, thread_node);
	thread->sid_mappings[conn->sid] = NULL;
	thread->nr_conn -= 1;
	if (conn->is_client && !conn->is_fpga_reply)
		thread->nr_client_conn -= 1;

	tpa_event_ctrl(conn->sid, TPA_EVENT_CTRL_DEL, NULL);
	tpa_close(conn->sid);

	conn_put(conn);
}
