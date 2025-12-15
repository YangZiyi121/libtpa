/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>

#include "tperf.h"

static void process_conn(struct connection *conn)
{
	uint32_t events;
	int ret = 0;

	events = conn->events;
	conn->events = 0;

	/* Debug: show whether we ever get IN events for accepted server conns */
	if (ctx.server_debug && !conn->is_client) {
		printf("[server-debug] process_conn: sid=%d events=0x%x (IN=%d OUT=%d ERR=%d HUP=%d)\n",
		       conn->sid, events,
		       !!(events & TPA_EVENT_IN),
		       !!(events & TPA_EVENT_OUT),
		       !!(events & TPA_EVENT_ERR),
		       !!(events & TPA_EVENT_HUP));
	}

	/*
	 * Some TPA event paths behave like edge-trigger: if the peer sends data
	 * very quickly after connect/accept, the first readable transition can
	 * happen before we register/observe TPA_EVENT_IN, leaving us with only
	 * TPA_EVENT_OUT events and a stuck connection.
	 *
	 * For server-side request connections that haven't even parsed the
	 * fixed-size test_info header yet, opportunistically attempt a nonblocking
	 * read when we get OUT but no IN. This is safe (conn_on_read will stop at
	 * EAGAIN) and fixes the "server never receives 4096B requests" symptom.
	 */
	if (!conn->is_client &&
	    !(events & (TPA_EVENT_IN | TPA_EVENT_ERR | TPA_EVENT_HUP)) &&
	    (events & TPA_EVENT_OUT) &&
	    conn->pkt_idx == 0 &&
	    conn->info_off < sizeof(struct test_info)) {
		ret = conn_on_read(conn);
	}

	if (events & (TPA_EVENT_IN | TPA_EVENT_ERR | TPA_EVENT_HUP))
		ret = conn_on_read(conn);

	if (ret >= 0 && (events & (TPA_EVENT_OUT | TPA_EVENT_ERR | TPA_EVENT_HUP)))
		ret = conn_on_write(conn);

    if (ret < 0 || (events & (TPA_EVENT_ERR | TPA_EVENT_HUP)) || conn->to_close){
      if (ctx.server_debug && !conn->is_client) {
	      printf("[server-debug] closing sid=%d ret=%d events=0x%x to_close=%d\n",
		     conn->sid, ret, events, conn->to_close);
      }
      /* Only free reassembly_buf if this connection owns it.
       * Server response connections borrow the buffer from request connections,
       * so they should NOT free it (the request connection will free it). */
      if (!conn->is_client && !conn->is_server_response && conn->reassemble.reassembly_buf != NULL){
            free(conn->reassemble.reassembly_buf);
            conn->reassemble.reassembly_buf = NULL;
      }
          conn_close(conn);
    }
}

static void process_event_queue(struct test_thread *thread)
{
	struct connection *conn;
	int nr_event = thread->nr_event;

	/* to avoid dead loop */
	while (nr_event--) {
		conn = event_queue_pop(thread);

		process_conn(conn);
	}
}

int poll_and_process(struct test_thread *thread)
{
	struct tpa_event events[BATCH_SIZE];
	int nr_event;
	int i;

	nr_event = tpa_event_poll(thread->worker, events, BATCH_SIZE);
	if (nr_event < 0) {
		fprintf(stderr, "err_epoll_wait: %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; i < nr_event; i++)
		event_queue_add(events[i].data, events[i].events);

	process_event_queue(thread);

	return 0;
}
