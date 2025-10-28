# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
# Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>

SRCS := tperf.c
SRCS += client.c
SRCS += server.c
SRCS += test.c
SRCS += conn.c
SRCS += integrity.c
SRCS += options.c
SRCS += utils.c
SRCS += mbuf.c
SRCS += event.c
SRCS += stats.c
SRCS += offrac.c
SRCS += trace.c

APP  := tperf

TF := true
TF_LIB_PATH ?= $(HOME)/.anaconda/envs/cnn_offrac/lib/tensorflow

include ../app.mk

# Ensure runtime can find TensorFlow without LD_LIBRARY_PATH
ifeq ($(TF),true)
LDFLAGS += -Wl,-rpath,$(TF_LIB_PATH)/lib
endif
