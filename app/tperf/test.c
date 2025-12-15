/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>

#include "tperf.h"
#include "offrac.h"

static void hexdump(const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if ((i & 0xF) == 0)
            printf("%04zx: ", i);
        printf("%02x ", data[i]);
        if (((i & 0xF) == 0xF) || i + 1 == len)
            printf("\n");
    }
}

static int printed_first_request = 1;
static size_t first_req_to_print = 1;
static size_t first_req_printed = 1;

static uint16_t make_fpga_func_header(uint32_t func)
{
    char digits[32];
    int len = snprintf(digits, sizeof(digits), "%u", func);
    if (len < 0)
        len = 0;

    /* Use the rightmost up to 4 characters, pad the left with 'F' */
    int start = len > 4 ? len - 4 : 0;
    int out_len = len - start;
    int pad = 4 - out_len;

    uint16_t header = 0;
    int pos = 0;

    /* left padding with 'F' */
    for (; pos < pad; pos++)
        header |= (uint16_t)0xF << ((3 - pos) * 4);

    /* copy digits as hex nibbles */
    for (int i = 0; i < out_len; i++, pos++) {
        char c = digits[start + i];
        uint16_t nibble;
        if (c >= '0' && c <= '9')
            nibble = (uint16_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            nibble = (uint16_t)(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'F')
            nibble = (uint16_t)(10 + (c - 'A'));
        else
            nibble = 0xF; /* fallback */

        header |= nibble << ((3 - pos) * 4);
    }

    return header;
}

static int read_test_info(struct connection *conn, struct tpa_iovec *iov, int size)
{
	uint32_t off = conn->info_off;
	int bytes_eaten = 0;
	int idx = 0;
	int len;
	int sum = 0;

	if (ctx.server_debug) {
		printf("[read_test_info] sid=%d, info_off=%u, size=%d\n", conn->sid, off, size);
	}

	/* if already parsed? */
	// if (off == sizeof(struct test_info))
	//	return 0;
	/*
	 * The 64B test_info header may arrive split across multiple iov entries
	 * (and/or only partially in this read). Walk iovs until we either finish
	 * the header or consume all bytes_read.
	 *
	 * NOTE: bytes_eaten counts bytes consumed from the stream (not per-iov),
	 * so read_test_data() can skip exactly bytes_eaten bytes even if the header
	 * ends in the middle of an iov.
	 */
	while (off < sizeof(struct test_info) && sum < size) {
		/* bytes available in this iov, but don't exceed total bytes_read (size) */
		len = iov[idx].iov_len;
		if (sum + len > size)
			len = size - sum;

		/* take at most remaining header bytes */
		int take = (int)MIN((size_t)len, sizeof(struct test_info) - off);
		if (take > 0) {
			memcpy(&conn->info_raw[off], iov[idx].iov_base, take);
			off += (uint32_t)take;
			bytes_eaten += take;
		}

		/* advance to next iov (even if take<len; remaining bytes are payload) */
		sum += len;
		idx += 1;
		/* safety: avoid running off the fixed iov array if size bookkeeping is off */
		if (idx >= BATCH_SIZE)
			break;
	}
	conn->info_off = off;

	if (off == sizeof(struct test_info)) {
		if (ctx.server_debug) {
			printf("[read_test_info] Header complete, calling init_server_conn for sid=%d\n", conn->sid);
		}
		init_server_conn(conn);
	}

	return bytes_eaten;
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
		      size_t remaining = conn->req_size - conn->reassemble.off;
		      size_t to_copy = MIN((size_t)len, remaining);
		      if (to_copy > 0) {
		          memcpy(conn->reassemble.reassembly_buf + conn->reassemble.off, base, to_copy);
		          conn->reassemble.off += to_copy;
		      }
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
	        if ((conn->read.off) != (conn->read.budget)) {
	            fprintf(stderr, "Client assertion failed: read.off=%lu, read.budget=%lu, diff=%ld, thread_id=%d\n", 
	                    conn->read.off, conn->read.budget, (long)(conn->read.off - conn->read.budget), conn->thread->id);
	        }
	        assert((conn->read.off) == (conn->read.budget));
		// /* Log each completed response for visibility */
		// printf("[client] received response: thread=%d sid=%d bytes=%lu\n",
		//        conn->thread->id, conn->sid, (unsigned long)conn->read.budget);
	        update_latency(conn);
		if (conn->test == TEST_CRR){
			conn->to_close = 1;
		}
		else if(conn->test == TEST_RR){
		  /* Set write budget for next request:
		   * FPGA mode uses message_size, CPU mode uses req_size + header */
		  if (conn->fpga_srv == 1) {
		  conn->write.budget = conn->message_size;
		  } else {
		    conn->write.budget = conn->req_size + sizeof(struct test_info);
		  }
		  event_queue_add(conn, TPA_EVENT_OUT);

		  /* In open-connection mode, re-enqueue for pairing with next reply connection */
		  if (ctx.fpga_reply_port != 0) {
		    fpga_request_enqueue(conn->thread, conn);
		    fpga_try_pair(conn->thread);
		  }
		}

	}

	/*
	 * re-open the write, when we are the
	 * - server: we just got the req; we need send the response; OR,
	 * - client and the test is RR: we just got the response,
	 *   let's start another request.
	 */
	if (!conn->is_client) {
	  if(conn->read.off < conn->req_size){
	    if (ctx.server_debug) {
	      printf("[server-debug] on_rr_read_done: waiting for more data, read.off=%zu, req_size=%u\n",
	             conn->read.off, conn->req_size);
	    }
	    conn->pkt_idx += 1;
	    return;
	  }
	  if (ctx.server_debug) {
	    printf("[server-debug] on_rr_read_done: request complete, read.off=%zu, req_size=%u\n",
	           conn->read.off, conn->req_size);
	  }
		assert(conn->read.off == conn->req_size);
		conn->reassemble.off = 0;
		conn->pkt_idx = 0;

		/* Check if we're in open-connection mode (open new connection for response) */
		if (ctx.server_response_port != 0) {
			/* Check if response connection already exists (from previous request) */
			if (conn->server_response_conn && conn->server_response_ready) {
				/* Reuse existing response connection */
				if (ctx.server_debug) {
					printf("[server-debug] Reusing response conn sid=%d for request sid=%d\n",
					       conn->server_response_conn->sid, conn->sid);
				}
				server_bind_pair(conn, conn->server_response_conn);
			} else {
				/* First request: Queue to open a new response connection */
				server_request_enqueue(conn->thread, conn);
			}
			/* Don't set write.budget here - the response conn will handle it */
		} else {
			/* Original behavior: respond on same connection */
			conn->write.budget = conn->response_size;
			event_queue_add(conn, TPA_EVENT_OUT);
		}
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

static void fpga_request_consume_bytes(struct connection *request_conn,
					 size_t len)
{
	size_t consumed = 0;

	while (consumed < len) {
		size_t remaining = len - consumed;
		size_t need = request_conn->read.budget - request_conn->read.off;
		size_t take = remaining;

		if (need > 0 && take > need)
			take = need;

		if (take == 0) {
			/* unexpected: nothing to consume, avoid tight loop */
			break;
		}

		UPDATE_STATS(request_conn, bytes_read, take);
		request_conn->read.off += take;
		consumed += take;

		if (request_conn->read.budget > 0 &&
		    request_conn->read.off == request_conn->read.budget)
			on_rr_read_done(request_conn);
	}
}

static int fpga_reply_on_read(struct connection *conn)
{
	struct connection *request = conn->fpga_requester;
	struct tpa_iovec iov[BATCH_SIZE];
	int bytes_read;
	int sum;
	int i;

	if (!request) {
		/*
		 * Not paired yet: do NOT drain data!
		 * Re-add to event queue so we'll be called again after pairing.
		 * The data remains in the socket buffer until we're paired.
		 */
		event_queue_add(conn, TPA_EVENT_IN);
		return 0;
	}

	while (1) {
		bytes_read = tpa_zreadv(conn->sid, iov, BATCH_SIZE);
		if (bytes_read < 0) {
			if (errno == EAGAIN)
				break;
			return -1;
		}
		if (bytes_read == 0)
			return -1;

		sum = 0;
		i = 0;
		while (sum < bytes_read) {
			int len = iov[i].iov_len;

			fpga_request_consume_bytes(request, len);

			iov[i].iov_read_done(iov[i].iov_base, iov[i].iov_param);
			sum += len;
			i++;
		}
	}

	return 0;
}

int conn_on_read(struct connection *conn)
{
	struct tpa_iovec iov[BATCH_SIZE];
	int bytes_read;
	int bytes_eaten;

	if (conn->is_fpga_reply)
		return fpga_reply_on_read(conn);

	while (1) {
        bytes_read = tpa_zreadv(conn->sid, iov, BATCH_SIZE);
		if (bytes_read < 0) {
			if (errno == EAGAIN)
				break;

			if (ctx.server_debug && !conn->is_client) {
				printf("[server-debug] conn_on_read: sid=%d tpa_zreadv error=%d (%s)\n",
				       conn->sid, errno, strerror(errno));
			}
			return -1;
		}

		if (bytes_read == 0) {
			if (ctx.server_debug && !conn->is_client) {
				printf("[server-debug] conn_on_read: sid=%d peer closed (bytes_read=0)\n",
				       conn->sid);
			}
			return -1;
		}
		bytes_eaten = 0;
		if (!conn->is_client && conn->pkt_idx == 0) {
		      bytes_eaten = read_test_info(conn, iov, bytes_read);
		      /*
		       * IMPORTANT: Don't run RR completion logic until the full
		       * test_info header is received (init_server_conn sets budgets).
		       *
		       * When the header arrives split across reads, conn->read.budget
		       * is still 0 here; calling on_read_done() would incorrectly
		       * treat the request as "complete" and can close/crash the conn.
		       */
		      if (conn->info_off < sizeof(struct test_info)) {
			      /* Consume and free all received buffers; no payload yet. */
			      bytes_eaten = bytes_read;
			      read_test_data(conn, iov, bytes_read, bytes_eaten);
			      continue;
		      }
		}
		read_test_data(conn, iov, bytes_read, bytes_eaten);

		if (ctx.server_debug && !conn->is_client) {
			printf("[server-debug] conn_on_read: bytes_read=%d, bytes_eaten=%d, read.off=%zu, read.budget=%zu, reassemble.off=%zu, req_size=%u\n",
			       bytes_read, bytes_eaten, conn->read.off, conn->read.budget, conn->reassemble.off, conn->req_size);
		}

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

static void heap_write_done(void *iov_base, void *iov_param)
{
	struct connection *conn = iov_param;
	free(iov_base);
	if (conn)
		conn_put(conn);
}

static int offrac_process(struct test_thread *thread, struct connection *conn, struct tpa_iovec *iov)
{
	struct mbuf *mbuf;
	int len;
	int nr_iov = 0;
	uint8_t *input_buf;

	if (ctx.server_debug) {
		printf("[offrac_process] CALLED: sid=%d, write.budget=%zu, req_size=%u, response_size=%u, func=%d\n",
		       conn->sid, conn->write.budget, conn->req_size, conn->response_size, conn->func);
	}

	mbuf = mbuf_alloc(thread->mbuf_pool);
	assert(mbuf != NULL);

	mbuf->private = conn_get(conn);

	len = MIN(conn->write.budget, MBUF_SIZE);
	
	/* Get input buffer: for response connections, read from the paired request connection */
	if (conn->is_server_response && conn->server_request_conn) {
		input_buf = conn->server_request_conn->reassemble.reassembly_buf;
	} else {
		input_buf = conn->reassemble.reassembly_buf;
	}

	/*
	 * Support function chaining for CPU path (Z=0):
	 * A multi-digit -F value like 12 means apply function 2 first, then 1.
	 * We interpret digits right-to-left: while (func>0) { apply func%10; func/=10; }
	 * Single-digit values preserve existing behavior.
	 */
	{
		int func_code = conn->func;
		/* Fast path: single known function id */
		if (func_code == SPARSE_TRANSFER || func_code == MAPID ||
		    func_code == LOGIT || func_code == NORM || func_code == TOPK) {
			if (ctx.server_debug && conn->pkt_idx == 0) {
				printf("[offrac_process] Fast path: func=%d, req_size=%u, len=%d, input_buf=%p, mbuf->data=%p\n",
				       func_code, conn->req_size, len, (void*)input_buf, (void*)mbuf->data);
			}
			if (func_code == SPARSE_TRANSFER) {
				sparse_transfer(mbuf->data, conn->req_size, input_buf);
			} else if (func_code == MAPID) {
				mapid(mbuf->data, conn->req_size, input_buf);
			} else if (func_code == LOGIT) {
				logit(mbuf->data, conn->req_size, input_buf);
			} else if (func_code == NORM) {
				norm(mbuf->data, conn->req_size, input_buf);
			} else if (func_code == TOPK) {
				topk(mbuf->data, conn->req_size, input_buf);
			}
		} else {
			/* Chaining path: ping-pong through two temporary buffers */
			/* Use heap allocation to avoid stack overflow for large request sizes */
			size_t buf_size = (conn->req_size > MBUF_SIZE) ? conn->req_size : MBUF_SIZE;
			uint8_t *tmp_a = (uint8_t *)malloc(buf_size);
			uint8_t *tmp_b = (uint8_t *)malloc(buf_size);
			
			/* Safety check - ensure buffers allocated successfully */
			
			if (tmp_a == NULL || tmp_b == NULL) {
				fprintf(stderr, "[offrac_process] Failed to allocate %zu bytes for chaining buffers\n", buf_size);
				if (tmp_a) free(tmp_a);
				if (tmp_b) free(tmp_b);
				/* Fallback: just copy input to output */
				memcpy(mbuf->data, input_buf, MIN(conn->req_size, MBUF_SIZE));
			} else {
				void *in_ptr = input_buf;
				void *out_ptr = tmp_a;
				int code = func_code;
				while (code > 0) {
					int digit = code % 10; /* rightmost digit first */
					switch (digit) {
					case 1: /* SPARSE_TRANSFER */
						sparse_transfer(out_ptr, conn->req_size, in_ptr);
						break;
					case 2: /* MAPID */
						mapid(out_ptr, conn->req_size, in_ptr);
						break;
					case 3: /* LOGIT */
						logit(out_ptr, conn->req_size, in_ptr);
						break;
					case 5: /* NORM */
						norm(out_ptr, conn->req_size, in_ptr);
						break;
					case 6: /* TOPK */
						topk(out_ptr, conn->req_size, in_ptr);
						break;
					case 7: /* CNN removed - no longer supported */
						/* CNN has been removed; pass-through */
						memcpy(out_ptr, in_ptr, MIN(conn->req_size, buf_size));
						break;
					default:
						/* Unknown step: pass-through */
						memcpy(out_ptr, in_ptr, MIN(conn->req_size, buf_size));
					}

					/* flip buffers for next stage */
					if (in_ptr == input_buf) {
						in_ptr = out_ptr;
						out_ptr = (out_ptr == (void *)tmp_a) ? (void *)tmp_b : (void *)tmp_a;
					} else {
						void *prev_out = in_ptr;
						in_ptr = out_ptr;
						out_ptr = prev_out;
					}
					code /= 10;
				}
				/* Final result is in in_ptr after the last swap; copy to mbuf->data */
				memcpy(mbuf->data, in_ptr, MIN(conn->req_size, MBUF_SIZE));
				
				/* Free the temporary buffers */
				free(tmp_a);
				free(tmp_b);
			}
		}
	}

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
    int budget = conn->write.budget;
	size_t off = 0;
	struct mbuf *mbuf;
	int nr_iov = 0;
	int len;
	uint8_t fpga_hdr[64];

	struct test_info *info = &conn->info;

	info->test = conn->test;
	info->integrity_enabled = conn->integrity_enabled;
	info->integrity_off = conn->integrity_off;
	info->enable_zwrite = conn->enable_zwrite;
	info->message_size = conn->message_size;
	info->response_size = conn->response_size;
	info->func = conn->func;
	info->req_size = conn->req_size;

	if (conn->fpga_srv == 1){
	      uint32_t request_size = conn->req_size;
	      uint16_t func_header = make_fpga_func_header(conn->func);
	      //printf("Function header: 0x%x (func=%d)\n", func_header, conn->func);
	      memset(fpga_hdr, 0xff, 64);
	      memcpy(&fpga_hdr[62], &func_header, sizeof(uint16_t)); // Bytes 62–63
	      memcpy(&fpga_hdr[56], &request_size, sizeof(uint32_t)); // Bytes 56–59
	}

	/*
	 * CPU mode: if total request (header+payload) exceeds one MBUF (4096),
	 * avoid multi-iov zero-copy writes. We've observed open-connection mode
	 * get stuck (server sees OUT-only then ERR/ECONNRESET) when the client
	 * uses multi-iov zwritev for these sizes.
	 *
	 * Workaround: build one contiguous buffer and force non-zero-copy
	 * (iov_phys=0) so libtpa copies and reliably transmits.
	 */
	if (conn->fpga_srv == 0 && budget > MBUF_SIZE) {
		uint8_t *buf = (uint8_t *)malloc((size_t)budget);
		if (!buf) {
			fprintf(stderr, "[setup_test_data] malloc(%d) failed\n", budget);
			return 0;
		}

		/* Fill payload pattern across the whole buffer first */
		{
			static const uint8_t pattern[4] = {0x07, 0x34, 0xB8, 0xE8};
			for (int j = 0; j < budget; j++)
				buf[j] = pattern[j & 3];
		}

		/* Write header at the start */
		if (conn->pkt_idx == 0) {
			if (conn->fpga_srv == 1) {
				memcpy(buf, fpga_hdr, sizeof(struct test_info));
			} else {
				memcpy(buf, info, sizeof(struct test_info));
			}
		}

		iov[0].iov_base = buf;
		iov[0].iov_len = (uint32_t)budget;
		iov[0].iov_phys = 0; /* force fallback copy */
		iov[0].iov_write_done = heap_write_done;
		iov[0].iov_param = conn_get(conn);
		return 1;
	}

	while (off < budget) {
		mbuf = mbuf_alloc(thread->mbuf_pool);
		assert(mbuf != NULL);

		mbuf->private = conn_get(conn);

		len = MIN(budget - off, MBUF_SIZE);

		//original payload: repeating 0x07 0x34 0xB8 0xE8 pattern
		{
			static const uint8_t pattern[4] = {0x07, 0x34, 0xB8, 0xE8};
			uint8_t *byte_data = (uint8_t *)mbuf->data;
			for (int j = 0; j < len; j++)
				byte_data[j] = pattern[j & 3];
		}
		//Initialize buffer with repeating pattern 0, 1, 2, 3 (as 32-bit integers)
		// uint32_t *int_data = (uint32_t *)mbuf->data;
		// int num_ints = len / sizeof(uint32_t);
		// for (int j = 0; j < num_ints; j++) {
		// 	int_data[j] = j % 4;  // 0, 1, 2, 3, 0, 1, 2, 3...
		// }
		
		// Handle remaining bytes if len is not divisible by 4
		// uint8_t *byte_data = (uint8_t *)mbuf->data;
		// for (int j = num_ints * sizeof(uint32_t); j < len; j++) {
		// 	byte_data[j] = (j / sizeof(uint32_t)) % 4;
		// }

		/* Only write header to the FIRST mbuf of the first request */
		if (conn->pkt_idx == 0 && off == 0){
		      if(conn->fpga_srv == 1){
			    memcpy(mbuf->data, fpga_hdr, sizeof(struct test_info));
		      }else{
			    memcpy(mbuf->data, info, sizeof(struct test_info));
		      }
		}

		/* Print the first request (header + payload) once, across chunks */
		if (conn->is_client && !printed_first_request) {
			if (first_req_printed == 0) {
				first_req_to_print = (conn->fpga_srv == 1) ? (size_t)conn->message_size
							    : (size_t)conn->req_size + sizeof(struct test_info);
				printf("[tperf] First request total=%zu bytes (header=%zu, payload=%zu)\n",
				       first_req_to_print,
				       (size_t)sizeof(struct test_info),
				       (conn->fpga_srv == 1) ? (size_t)(first_req_to_print - 64)
								   : (size_t)conn->req_size);
			}
			size_t remaining = first_req_to_print - first_req_printed;
			size_t to_dump = remaining < (size_t)len ? remaining : (size_t)len;
			if (to_dump > 0) {
				printf("[tperf] Dumping first request chunk: %zu bytes\n", to_dump);
				hexdump((const uint8_t *)mbuf->data, to_dump);
				first_req_printed += to_dump;
				if (first_req_printed >= first_req_to_print)
					printed_first_request = 1;
			}
		}

		// {
		// 	size_t payload_off = (conn->pkt_idx == 0) ? sizeof(struct test_info) : 0;
		// 	if (payload_off < (size_t)len) {
		// 		uint8_t *byte_data = (uint8_t *)mbuf->data;
		// 		const uint8_t pattern[4] = {0x25, 0xC8, 0x3C, 0x98};
		// 		size_t j;
		// 		size_t k = 0;
		// 		for (j = payload_off; j < (size_t)len; j++) {
		// 			byte_data[j] = pattern[k];
		// 			k = (k + 1) % 4;
		// 		}
		// 	}
		// }

		iov[nr_iov].iov_base = mbuf->data;
		iov[nr_iov].iov_len  = len;
		iov[nr_iov].iov_phys = conn->enable_zwrite;
		iov[nr_iov].iov_write_done = zwrite_done;
		iov[nr_iov].iov_param = mbuf;

		if (0)
		      integrity_fill(mbuf->data, len, conn->integrity_off + conn->stats.bytes_write + off);

		nr_iov += 1;
		off += len;
	}

	return nr_iov;
}

static void on_write_done(struct connection *conn, int bytes_write)
{
	UPDATE_STATS(conn, bytes_write, bytes_write);
	conn->write.off += bytes_write;
	/* Use Z (fpga_srv) to select CPU vs FPGA completion logic */
	if (conn->fpga_srv == 1) {
		/* FPGA */
		if ((conn->is_client && (conn->write.off < conn->req_size)) ||
		    (!conn->is_client && (conn->write.off < conn->write.budget))) {
			return;
		}
		if (conn->is_client) {
			assert(conn->write.off == conn->req_size);
		} else {
			assert(conn->write.off == conn->write.budget);
		}
	} else {
		/* CPU */
		if (conn->write.off < conn->write.budget)
			return;

		assert(conn->write.off == conn->write.budget);
	}


	/* disable futher writes unless we get the response */
	if ((conn->test == TEST_RR || conn->test == TEST_CRR))
		conn->write.budget = 0;
	
	/* For FPGA: after first request sent, wait for reply connection before next send */
	if (conn->is_client && conn->fpga_srv == 1 && 
	    !conn->fpga_reply_conn && conn->fpga_ready) {
		conn->fpga_ready = 0;
	}

	/* Server response connection: after sending response, prepare for next request */
	if (conn->is_server_response && conn->server_request_conn) {
		struct connection *request_conn = conn->server_request_conn;

		if (ctx.server_debug) {
			printf("[server-debug] Response sent on sid=%d, resetting request sid=%d for next request\n",
			       conn->sid, request_conn->sid);
		}

		/* Reset the request connection for the next request */
		/* Keep server_response_ready = 1 so connection can be reused! */
		request_conn->read.off = 0;
		request_conn->read.budget = request_conn->req_size;
		request_conn->reassemble.off = 0;
		request_conn->pkt_idx = 0;
		request_conn->info_off = 0; /* Reset to re-read test_info header for next request */

		/* Reset response connection state for next response */
		conn->write.off = 0;
		conn->write.budget = 0; /* Will be set when next request arrives */
		
		/* Keep response connection alive for reuse - don't close it!
		 * It will be reused for the next response. */
	}

	conn->last_ns = get_time_in_ns();
	conn->pkt_idx = 0;
	conn->write.off = 0;
}

int conn_on_write(struct connection *conn)
{
	struct test_thread *thread = conn->thread;
	int bytes_write;
	int nr_iov = 0;
	int i;

	/* Server response connection: on first TPA_EVENT_OUT, TCP handshake is complete.
	 * Now we can pair it with the request and send the response. */
	if (conn->is_server_response && !conn->server_response_ready && conn->server_request_conn) {
		struct connection *request_conn = conn->server_request_conn;
		
		if (ctx.server_debug) {
			printf("[server-debug] Response conn sid=%d TCP handshake complete, pairing with request sid=%d\n",
			       conn->sid, request_conn->sid);
		}
		
		conn->server_response_ready = 1;
		
		/* Now call server_bind_pair to set up the response data */
		server_bind_pair(request_conn, conn);
		
		/* Don't return - continue to send the response */
	}

	/* For FPGA (Z=1), wait until paired before sending.
	 * For CPU (Z=0), allow sending immediately (fpga_ready==1). */
	if (conn->is_client && conn->fpga_srv == 1 && !conn->fpga_ready) {
		event_queue_add(conn, TPA_EVENT_OUT);
		return 0;
	}

	while (conn->write.budget) {
		/* Support large messages: up to 64KB needs 16 iovs (64KB/4KB) */
		struct tpa_iovec iov[16];

		if (mbuf_pool_free_count(thread->mbuf_pool) * MBUF_SIZE < conn->write.budget) {
			if (ctx.fpga_debug && conn->is_client) {
				printf("[client-debug] Not enough mbufs: free_count=%u, need=%zu\n",
				       mbuf_pool_free_count(thread->mbuf_pool), conn->write.budget / MBUF_SIZE + 1);
			}
			event_queue_add(conn, TPA_EVENT_OUT);
			break;
		}

		if (ctx.fpga_debug && conn->is_client && conn->pkt_idx == 0) {
			printf("[client-debug] Sending request: write.budget=%zu, req_size=%u\n",
			       conn->write.budget, conn->req_size);
		}

		if (conn->is_client) {
		      nr_iov += setup_test_data(thread, conn, iov);
		} else {
		      nr_iov = offrac_process(thread, conn, iov);
		}

		bytes_write = tpa_zwritev(conn->sid, iov, nr_iov);
		
		if (ctx.fpga_debug && conn->is_client) {
			printf("[client-debug] tpa_zwritev: nr_iov=%d, bytes_write=%d, write.budget=%zu\n",
			       nr_iov, bytes_write, conn->write.budget);
		}
		
		conn->pkt_idx += 1;

		if (bytes_write < 0) {
			int err = errno;

			for (i = 0; i < nr_iov; i++)
				iov[i].iov_write_done(iov[i].iov_base, iov[i].iov_param);

			if (err == EAGAIN)
				break;

			return -1;
		}

		on_write_done(conn, bytes_write);
	}

	return 0;
}
