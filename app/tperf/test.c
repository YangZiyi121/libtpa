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

	/* if already parsed? */
	// if (off == sizeof(struct test_info))
	//	return 0;
	while (off < sizeof(struct test_info)) {
		len = MIN(iov[idx].iov_len, sizeof(struct test_info) - off);
		memcpy(&conn->info_raw[off], iov[idx].iov_base, len);

		off += len;
		bytes_eaten += len;

		size -= len;
		if (size == 0)
			break;
	}
	conn->info_off = off;

	if (off == sizeof(struct test_info))
		init_server_conn(conn);

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
	        update_latency(conn);
		if (conn->test == TEST_CRR){
			conn->to_close = 1;
		}
		else if(conn->test == TEST_RR){
		  conn->write.budget = conn->message_size;
		  event_queue_add(conn, TPA_EVENT_OUT);
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
	    conn->pkt_idx += 1;
	    return;
	  }
		assert(conn->read.off == conn->req_size);
		conn->reassemble.off = 0;
		conn->pkt_idx = 0;
		conn->write.budget = conn->response_size;
		event_queue_add(conn, TPA_EVENT_OUT);
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
	int bytes_eaten;

	while (1) {
        bytes_read = tpa_zreadv(conn->sid, iov, BATCH_SIZE);
		if (bytes_read < 0) {
			if (errno == EAGAIN)
				break;

			return -1;
		}

		if (bytes_read == 0)
			return -1;
		bytes_eaten = 0;
		if (!conn->is_client && conn->pkt_idx == 0) {
		      bytes_eaten = read_test_info(conn, iov, bytes_read);
		}
		read_test_data(conn, iov, bytes_read, bytes_eaten);

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
		    func_code == LOGIT || func_code == NORM || func_code == TOPK || func_code == CNN) {
			if (func_code == SPARSE_TRANSFER) {
				sparse_transfer(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
			} else if (func_code == MAPID) {
				mapid(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
			} else if (func_code == LOGIT) {
				logit(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
			} else if (func_code == NORM) {
				norm(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
			} else if (func_code == TOPK) {
				topk(mbuf->data, conn->req_size, conn->reassemble.reassembly_buf);
			} else {
				/* CNN unsupported in CPU path here unless initialized elsewhere */
			}
		} else {
			/* Chaining path: ping-pong through two temporary buffers */
			uint8_t tmp_a[MBUF_SIZE];
			uint8_t tmp_b[MBUF_SIZE];
			void *in_ptr = conn->reassemble.reassembly_buf;
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
				case 7: /* CNN (requires TF init elsewhere) */
					/* Not applied unless properly initialized; skip by copying */
					memcpy(out_ptr, in_ptr, MIN(conn->req_size, MBUF_SIZE));
					break;
				default:
					/* Unknown step: pass-through */
					memcpy(out_ptr, in_ptr, MIN(conn->req_size, MBUF_SIZE));
				}

				/* flip buffers for next stage */
				if (in_ptr == conn->reassemble.reassembly_buf) {
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

		if (conn->pkt_idx == 0){
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

	while (conn->write.budget) {
		struct tpa_iovec iov[1];

		if (mbuf_pool_free_count(thread->mbuf_pool) * MBUF_SIZE < conn->write.budget) {
			event_queue_add(conn, TPA_EVENT_OUT);
			break;
		}

		if (conn->is_client) {
		      nr_iov += setup_test_data(thread, conn, iov);
		} else {
		      nr_iov = offrac_process(thread, conn, iov);
		}

		bytes_write = tpa_zwritev(conn->sid, iov, nr_iov);
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
