ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := gnn_spmm

SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp

VX_SRCS := $(SRC_DIR)/kernel.cpp

OPTS ?=

# vortex2 = KMU-aware kernel runtime (matches dyn_launch_v1/bfs_v2 style).
KERNEL_LIB := vortex2

include ../common.mk
