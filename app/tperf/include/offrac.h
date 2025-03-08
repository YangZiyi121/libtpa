/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Author: Yixi Chen <yixi.chen@kaust.edu.sa>
 */

#ifndef _OFFRAC_H_
#define _OFFRAC_H_

#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>

#define MAX_BUF_SIZE 20480

// Enum for offrac supporting functions
enum {
      TOPK = 1,
      CNN = 2,
      LOGIT = 3,
      NORM = 5,
};

//static int offrac_process(struct test_thread *thread, struct connection *conn, struct tpa_iovec *iov);

int topk(uint32_t *buf, int req_size, void* in_buf);
/* offrac_resp_t* offrac_minmax(uint32_t *buf, int size, int offrac_size, int offrac_args); */
/* offrac_resp_t* offrac_logit(uint32_t *buf, int size, int offrac_size, int offrac_args); */

// Handler interface for offrac functions

#endif
