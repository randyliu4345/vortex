ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

MESH_L2_DATA_H := $(ROOT_DIR)/mesh_l2_data.h

$(MESH_L2_DATA_H): $(VORTEX_HOME)/scripts/gen_mesh_l2_data.py
	python3 $(VORTEX_HOME)/scripts/gen_mesh_l2_data.py -o $@

PROJECT := mesh_l2_affinity

SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp

VX_SRCS := $(SRC_DIR)/kernel.cpp

$(SRC_DIR)/kernel.cpp $(SRC_DIR)/main.cpp: $(MESH_L2_DATA_H)

OPTS ?=

KERNEL_LIB := vortex2

REGRESSION_SIMX_ONLY := 1

include ../common.mk
