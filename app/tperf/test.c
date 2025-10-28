/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>

#include "tperf.h"
#include "offrac.h"

/* Set to 1 to enable trace debug prints */
static int trace_debug = 0; 

static int read_test_info(struct connection *conn, struct tpa_iovec *iov, int size)
{
    uint32_t off = conn->info_off;
    int idx = 0;
    int iov_off = 0;
    int total_eaten = 0;
    int remain = (int)sizeof(struct test_info) - (int)off;

    /* if already parsed? */
    if (remain <= 0)
        return 0;

    while (remain > 0 && total_eaten < size) {
        if (idx >= BATCH_SIZE)
            break;

        int avail = iov[idx].iov_len - iov_off;
        if (avail <= 0) {
            idx += 1;
            iov_off = 0;
            continue;
        }

        int to_copy = MIN(avail, remain);
        memcpy(&conn->info_raw[off], (char *)iov[idx].iov_base + iov_off, to_copy);

        off += to_copy;
        iov_off += to_copy;
        total_eaten += to_copy;
        remain -= to_copy;
    }

    conn->info_off = off;
    if (off == sizeof(struct test_info))
        init_server_conn(conn);

    return total_eaten;
}

static void read_test_data(struct connection *conn, struct tpa_iovec *iov,
			   int bytes_read, int bytes_eaten)
{
	char *base;
	int len;
	int sum = 0;
	int i = 0;

	while (sum < bytes_read) {
		base = iov[i].iov_base;
		len  = iov[i].iov_len;

		if (sum < bytes_eaten) {
			/* is this iov completely consumed? */
			if (sum + len < bytes_eaten)
				goto next;

		base = iov[i].iov_base + bytes_eaten - sum;
		len -= bytes_eaten - sum;
	}

		if (!conn->is_client && conn->reassemble.reassembly_buf != NULL && conn->reassemble.off < conn->req_size){
		      memcpy((conn->reassemble.reassembly_buf + conn->reassemble.off), base, len);
		      conn->reassemble.off += len;
		}
		if (0) // disable integrity check for now
		      integrity_verify(base, len, conn->integrity_off + conn->stats.bytes_read);

		UPDATE_STATS(conn, bytes_read, len);

	next:
		iov[i].iov_read_done(iov[i].iov_base, iov[i].iov_param);

		sum += iov[i].iov_len;
		i += 1;
	}

	conn->read.off += bytes_read - bytes_eaten;
}

static void on_rr_read_done(struct connection *conn)
{

	/* we got the respose: the request is done */
	if (conn->is_client) {
	        assert((conn->read.off) == (conn->read.budget));
	        update_latency(conn);
		if (ctx.trace_enabled) {
			/* log response */
			printf("trace_resp bytes=%zu\n", conn->read.budget);
			fflush(stdout);
		}
		if (conn->test == TEST_CRR){
			conn->to_close = 1;
		}
		else if(conn->test == TEST_RR){
			// indicate the a req is completed
			conn->write.budget = 0;
			conn->read.budget = 0;  /* Reset read budget to prevent re-reading */
			conn->req_cpl = 1;
		}

	}

	/*
	 * re-open the write, when we are the
	 * - server: we just got the req; we need send the response; OR,
	 * - client and the test is RR: we just got the response,
	 *   let's start another request.
	 */
	if (!conn->is_client) {
	  printf("[server] on_rr_read_done: read.off=%zu req_size=%u pkt_idx=%u\n",
	         conn->read.off, conn->req_size, conn->pkt_idx);
	  fflush(stdout);
	  if(conn->read.off < conn->req_size){
	    printf("[server] waiting for more data: read.off=%zu < req_size=%u, incrementing pkt_idx\n",
	           conn->read.off, conn->req_size);
	    fflush(stdout);
	    conn->pkt_idx += 1;
	    return;
	  }
	        assert(conn->read.off == conn->req_size);
		printf("[server] full request received, sending response\n");
		fflush(stdout);
		conn->pkt_idx = 0;
		conn->write.budget = conn->response_size;
		event_queue_add(conn, TPA_EVENT_OUT);
		
		/* Reset for next request on persistent connection */
		conn->info_off = 0;
	}

	conn->read.off = 0;
}

static void on_read_done(struct connection *conn)
{
      if (conn->read.off < conn->read.budget){
        		return;
      }
	if ((conn->test == TEST_RR || conn->test == TEST_CRR)) {
		on_rr_read_done(conn);
	} else {
		/* doesn't really matter here */
		conn->read.off -= conn->read.budget;
	}
}

int conn_on_read(struct connection *conn)
{
	struct tpa_iovec iov[BATCH_SIZE];
	int bytes_read;

	while (1) {
		bytes_read = tpa_zreadv(conn->sid, iov, BATCH_SIZE);
		if (bytes_read < 0) {
			if (errno == EAGAIN)
				break;

			return -1;
		}

		if (bytes_read == 0)
			return -1;
		
		if (!conn->is_client) {
			printf("[server] read bytes=%d pkt_idx=%u read.off=%zu read.budget=%zu info_off=%u\n",
			       bytes_read, conn->pkt_idx, conn->read.off, conn->read.budget, conn->info_off);
			fflush(stdout);
		}
		
		if (!conn->is_client && conn->info_off < sizeof(struct test_info)) {
		      printf("[server] parsing header info_off=%u\n", conn->info_off);
		      fflush(stdout);
		      read_test_info(conn, iov, bytes_read);
		}
		read_test_data(conn, iov, bytes_read, 0);

		on_read_done(conn);
	}

	return 0;
}

static void zwrite_done(void *iov_base, void *iov_param)
{
	struct mbuf *mbuf = iov_param;
	struct connection *conn = mbuf->private;

	mbuf_put(mbuf);
	conn_put(conn);
}

static int offrac_process(struct test_thread *thread, struct connection *conn, struct tpa_iovec *iov)
{
	struct mbuf *mbuf;
	int len;
	int nr_iov = 0;

	mbuf = mbuf_alloc(thread->mbuf_pool);
	assert(mbuf != NULL);

	mbuf->private = conn_get(conn);

	len = MIN(conn->write.budget, MBUF_SIZE);
	// set buff to some random value

	if (conn->func == TOPK){
	      topk(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
	} else if (conn->func == LOGIT){
	      logit(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
	} else if (conn->func == NORM){
	      norm(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
	}
	#ifdef TF_ENABLED
	else if (conn->func == CNN){
	  cnn(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf, &conn->tf_obj);
	}
	#endif

	iov[nr_iov].iov_base = mbuf->data;
	iov[nr_iov].iov_len  = len;
	iov[nr_iov].iov_phys = conn->enable_zwrite;
	iov[nr_iov].iov_write_done = zwrite_done;
	iov[nr_iov].iov_param = mbuf;

	nr_iov = 1;
	return nr_iov;
}

static int setup_test_data(struct test_thread *thread, struct connection *conn, struct tpa_iovec *iov)
{
	/* total_bytes is payload only (req_size includes the 64-byte header) */
	size_t total_payload_bytes = conn->req_size - sizeof(struct test_info);
	size_t payload_sent = conn->write.off;
	size_t payload_remaining = (payload_sent < total_payload_bytes) ? (total_payload_bytes - payload_sent) : 0;
	struct mbuf *mbuf;
	int nr_iov = 0;
	int len;
	uint8_t header_buf[sizeof(struct test_info)];
	const uint8_t *header_src;

	struct test_info *info = &conn->info;


	info->test = conn->test;
	info->integrity_enabled = conn->integrity_enabled;
	info->integrity_off = conn->integrity_off;
	info->enable_zwrite = conn->enable_zwrite;
	/* In trace mode, set message_size to req_size to match trace semantics */
	info->message_size = conn->message_size;
	info->response_size = conn->response_size;
	info->func = conn->func;
	info->req_size = conn->req_size;
	

	if (conn->fpga_srv == 1) {
		uint32_t request_size = conn->req_size;
		uint16_t func = (uint16_t)conn->func;

		memset(header_buf, 0xff, sizeof(header_buf));
		memcpy(&header_buf[62], &func, sizeof(uint16_t));
		memcpy(&header_buf[56], &request_size, sizeof(uint32_t));
		header_src = header_buf;
	} else {
		memcpy(header_buf, info, sizeof(header_buf));
		header_src = header_buf;
	}

	/* If both header and payload are fully sent, nothing to do */
	if (conn->header_sent == sizeof(struct test_info) && payload_remaining == 0)
		return 0;

	/* Cap this batch by available mbufs to avoid stalling on large requests */
	{
		int free_mbufs = mbuf_pool_free_count(thread->mbuf_pool);
		size_t max_send = (size_t)free_mbufs * MBUF_SIZE;

		if (max_send == 0)
			return 0;
		/* We may need to send remaining header bytes plus payload */
		/* Conservatively bound payload by max_send; header bytes are small */
		if (payload_remaining > max_send)
			payload_remaining = max_send;
	}

	mbuf = mbuf_alloc(thread->mbuf_pool);
	assert(mbuf != NULL);

	mbuf->private = conn_get(conn);

	/* We may have partially sent the header if the first write was short. */
	int off = 0;
	if (conn->header_sent < sizeof(struct test_info)) {
		/* Copy remaining header bytes first */
		size_t header_remaining = sizeof(struct test_info) - conn->header_sent;
		size_t header_copy = MIN((size_t)MBUF_SIZE, header_remaining);
		memcpy((uint8_t*)mbuf->data + off, header_src + conn->header_sent, header_copy);
		off += header_copy;
	}

	/* Fill payload after header (or from start if header fully sent) */
	size_t payload_space = (off < MBUF_SIZE) ? (MBUF_SIZE - off) : 0;
	size_t payload_bytes = MIN(payload_space, payload_remaining);
	if (payload_bytes > 0)
		memset((uint8_t*)mbuf->data + off, 0x9f, payload_bytes);

	len = off + payload_bytes;

	if (trace_debug) {
		printf("debug: setup pkt_%u len=%d header_sent=%zu payload_bytes=%zu payload_remaining=%zu\n",
		       conn->pkt_idx, len, conn->header_sent, payload_bytes, payload_remaining);
		fflush(stdout);
	}

	iov[nr_iov].iov_base = mbuf->data;
	iov[nr_iov].iov_len  = len;
	iov[nr_iov].iov_phys = conn->enable_zwrite;
	iov[nr_iov].iov_write_done = zwrite_done;
	iov[nr_iov].iov_param = mbuf;

	nr_iov = 1;

	return nr_iov;
}

static void on_write_done(struct connection *conn, int bytes_write)
{
    UPDATE_STATS(conn, bytes_write, bytes_write);
    
	/* For client, split written bytes between remaining header and payload */
	if (conn->is_client) {
		int written = bytes_write;
		if (conn->header_sent < sizeof(struct test_info)) {
			size_t header_remaining = sizeof(struct test_info) - conn->header_sent;
			size_t header_advance = MIN((size_t)written, header_remaining);
			conn->header_sent += header_advance;
			written -= (int)header_advance;
		}
		if (written > 0)
			conn->write.off += (size_t)written;

		if (trace_debug) {
			printf("debug: on_write_done pkt_%u bytes_write=%d header_sent=%zu payload_off=%zu/%u\n",
			       conn->pkt_idx, bytes_write, conn->header_sent, conn->write.off, conn->req_size);
			fflush(stdout);
		}
    } else {
		conn->write.off += bytes_write;
	}

    if (conn->is_client) {
		size_t expected_payload = conn->req_size - sizeof(struct test_info);
		if (conn->write.off < expected_payload || conn->header_sent < sizeof(struct test_info)) {
			/* More data to send - queue another write event */
			if (trace_debug) {
				printf("debug: partial send, queue next write header=%zu/64 off=%zu/%zu budget=%zu\n",
				       conn->header_sent, conn->write.off, expected_payload, conn->write.budget);
				fflush(stdout);
			}
			event_queue_add(conn, TPA_EVENT_OUT);
			return;
		}

		assert(conn->write.off == expected_payload && conn->header_sent == sizeof(struct test_info));
    } else {
        /* Server: response write path */
        if (conn->write.off < conn->write.budget)
            return;

        assert(conn->write.off == conn->write.budget);
    }
	/* disable futher writes unless we get the response */
	if ((conn->test == TEST_RR || conn->test == TEST_CRR))
		conn->write.budget = 0;

	conn->last_ns = get_time_in_ns();
	conn->pkt_idx = 0;
	conn->write.off = 0;
	if (conn->is_client)
		conn->header_sent = 0;
}

int conn_on_write(struct connection *conn)
{
	struct test_thread *thread = conn->thread;
	int bytes_write;
	int nr_iov = 0;
	int i;

	if (ctx.trace_enabled && conn->is_client && trace_debug) {
		printf("debug: conn_on_write called sid=%d budget=%zu off=%zu\n", 
		       conn->sid, conn->write.budget, conn->write.off);
		fflush(stdout);
	}

    while (conn->write.budget) {
		struct tpa_iovec iov[BATCH_SIZE];
		nr_iov = 0;

        if (mbuf_pool_free_count(thread->mbuf_pool) == 0) {
            if (ctx.trace_enabled && conn->is_client && trace_debug) {
                printf("debug: no free mbufs, requeueing OUT\n");
                fflush(stdout);
            }
            event_queue_add(conn, TPA_EVENT_OUT);
            break;
        }

		if (conn->is_client) {
		      nr_iov = setup_test_data(thread, conn, iov);
		} else {
		      nr_iov = offrac_process(thread, conn, iov);
		}

        if (nr_iov == 0) {
            if (ctx.trace_enabled && conn->is_client && trace_debug) {
                printf("debug: nr_iov=0, requeueing OUT budget=%zu off=%zu\n",
                       conn->write.budget, conn->write.off);
                fflush(stdout);
            }
            /* Could not prepare any buffers this round */
            event_queue_add(conn, TPA_EVENT_OUT);
            break;
        }

        if (ctx.trace_enabled && conn->is_client && trace_debug) {
			printf("debug: calling tpa_zwritev sid=%d nr_iov=%d\n", conn->sid, nr_iov);
			fflush(stdout);
		}

        bytes_write = tpa_zwritev(conn->sid, iov, nr_iov);

		if (ctx.trace_enabled && conn->is_client && trace_debug) {
			printf("debug: tpa_zwritev returned %d errno=%d\n", bytes_write, errno);
			fflush(stdout);
		}

        if (bytes_write < 0) {
			int err = errno;

			for (i = 0; i < nr_iov; i++)
				iov[i].iov_write_done(iov[i].iov_base, iov[i].iov_param);

            if (err == EAGAIN || err == ENOTCONN) {
                /* Connection not ready; requeue OUT and retry later */
                event_queue_add(conn, TPA_EVENT_OUT);
                break;
            }

			return -1;
		}

        if (bytes_write == 0) {
            /* Treat zero write as not-ready; avoid tight loop */
            if (ctx.trace_enabled && conn->is_client && trace_debug) {
                printf("debug: tpa_zwritev returned 0, requeue OUT\n");
                fflush(stdout);
            }
            event_queue_add(conn, TPA_EVENT_OUT);
            break;
        }

        /* count this successful send as one packet for header placement logic */
        conn->pkt_idx += 1;

		on_write_done(conn, bytes_write);
	}

	return 0;
}
