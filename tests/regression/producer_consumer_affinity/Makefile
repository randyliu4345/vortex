ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := producer_consumer_affinity

SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp

VX_SRCS := $(SRC_DIR)/kernel.cpp

OPTS ?=

# Make L2 behavior explicit: bypass L1 I/D caches for this regression.
CONFIGS += -DICACHE_ENABLE=0 -DDCACHE_ENABLE=0

KERNEL_LIB := vortex2

REGRESSION_SIMX_ONLY := 1

include ../common.mk
