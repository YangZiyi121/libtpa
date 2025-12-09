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
#define IMAGE_SIZE (64 * 64 * 3)
#define IDMAP_SIZE 65536

// Enum for offrac supporting functions
enum {
      // SPARSE_TRANSFER = 0xfff1,
      // LOGIT = 0xfff3,
      // NORM = 0xfff5,
      // TOPK = 0xfff6,
      SPARSE_TRANSFER = 0x01,
      MAPID = 0x02,
      LOGIT =0x03,
      NORM = 0x05,  
      TOPK = 0x06,
      // CNN removed - no longer supported
};

int sparse_transfer(void* out_buf, int req_size, void* in_buf);
int norm(void* out_buf, int req_size, void* in_buf);
int logit(void* out_buf, int req_size, void* in_buf);
int topk(void* out_buf, int req_size, void* in_buf);
int mapid(void* out_buf, int req_size, void* in_buf);

// Global variables for mapid functionality
extern uint16_t *g_idmap;
extern int g_idmap_loaded;

// Function to initialize idmap from COE file
void idmap_init_from_coe_once(void);

#endif
