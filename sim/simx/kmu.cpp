// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kmu.h"
#include "debug.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace vortex;

Kmu::Kmu() {
  this->reset();
}

void Kmu::reset() {
  PC_           = 0;
  param_        = 0;
  block_dim_[0] = block_dim_[1] = block_dim_[2] = 1;
  grid_dim_[0]  = grid_dim_[1]  = grid_dim_[2]  = 1;
  lmem_size_    = 0;
  block_size_   = 0;
  warp_step_[0] = warp_step_[1] = warp_step_[2] = 1;
  // Reset to load-balanced behavior; the host bumps this via DCR before
  // start() if a pinned launch was requested.
  core_affinity_= KMU_AFFINITY_ANY;
  running_      = false;
  cta_id_       = 0;
  block_idx_[0] = block_idx_[1] = block_idx_[2] = 0;
  pending_launches_.clear();
}

void Kmu::dcr_write(uint32_t addr, uint32_t value) {
  switch (addr) {
  case VX_DCR_KMU_STARTUP_ADDR0: PC_    = (PC_    & ~uint64_t(0xFFFFFFFF)) | value; break;
  case VX_DCR_KMU_STARTUP_ADDR1: PC_    = (PC_    &  uint64_t(0xFFFFFFFF)) | (uint64_t(value) << 32); break;
  case VX_DCR_KMU_STARTUP_ARG0:  param_ = (param_ & ~uint64_t(0xFFFFFFFF)) | value; break;
  case VX_DCR_KMU_STARTUP_ARG1:  param_ = (param_ &  uint64_t(0xFFFFFFFF)) | (uint64_t(value) << 32); break;
  case VX_DCR_KMU_BLOCK_DIM_X:   block_dim_[0] = value; break;
  case VX_DCR_KMU_BLOCK_DIM_Y:   block_dim_[1] = value; break;
  case VX_DCR_KMU_BLOCK_DIM_Z:   block_dim_[2] = value; break;
  case VX_DCR_KMU_GRID_DIM_X:    grid_dim_[0]  = value; break;
  case VX_DCR_KMU_GRID_DIM_Y:    grid_dim_[1]  = value; break;
  case VX_DCR_KMU_GRID_DIM_Z:    grid_dim_[2]  = value; break;
  case VX_DCR_KMU_LMEM_SIZE:     lmem_size_    = value; break;
  case VX_DCR_KMU_BLOCK_SIZE:    block_size_   = value; break;
  case VX_DCR_KMU_WARP_STEP_X:   warp_step_[0] = value; break;
  case VX_DCR_KMU_WARP_STEP_Y:   warp_step_[1] = value; break;
  case VX_DCR_KMU_WARP_STEP_Z:   warp_step_[2] = value; break;
  case VX_DCR_KMU_CORE_AFFINITY: core_affinity_= value; break;
  default: break;
  }
}

void Kmu::start() {
  running_ = (block_size_ > 0)
           && (grid_dim_[0] > 0)
           && (grid_dim_[1] > 0)
           && (grid_dim_[2] > 0);
  if (running_) {
    cta_id_       = 0;
    block_idx_[0] = block_idx_[1] = block_idx_[2] = 0;
  }
}

void Kmu::arm_child(uint64_t pc,
                    uint64_t param,
                    const uint32_t grid_dim[3],
                    const uint32_t block_dim[3],
                    uint32_t block_size,
                    const uint32_t warp_step[3],
                    uint32_t lmem_size,
                    uint32_t core_affinity) {
  launch_state_t state;
  state.pc            = pc;
  state.param         = param;
  state.block_size    = block_size;
  state.lmem_size     = lmem_size;
  state.core_affinity = core_affinity;
  for (int i = 0; i < 3; ++i) {
    state.grid_dim[i]  = grid_dim[i];
    state.block_dim[i] = block_dim[i];
    state.warp_step[i] = warp_step[i];
  }
  this->arm_child(state);
}

void Kmu::arm_child(const launch_state_t& state) {
  PC_           = state.pc;
  param_        = state.param;
  for (int i = 0; i < 3; ++i) {
    grid_dim_[i]  = state.grid_dim[i];
    block_dim_[i] = state.block_dim[i];
    warp_step_[i] = state.warp_step[i];
  }
  block_size_    = state.block_size;
  lmem_size_     = state.lmem_size;
  core_affinity_ = state.core_affinity;
  this->start();
}

void Kmu::attach_mem_reader(uint32_t core_id, const mem_reader_t& mem_read) {
  mem_readers_[core_id] = mem_read;
}

void Kmu::request_child_launch(uint64_t desc_addr, uint32_t core_id) {
  auto it = mem_readers_.find(core_id);
  if (it == mem_readers_.end()) {
    std::cerr << "Error: KMU child launch from core #" << core_id
              << " has no attached memory reader" << std::endl;
    std::abort();
  }
  this->launch_child(desc_addr, it->second);
}

void Kmu::launch_child(uint64_t desc_addr, const mem_reader_t& mem_read) {
  // Layout must match vx_kmu_launch_desc_t in kernel/include/vx_launch.h.
  launch_state_t state = {};
  state.core_affinity = KMU_AFFINITY_ANY;

  mem_read(&state.pc,            desc_addr + 0,  sizeof(uint64_t));
  mem_read(&state.param,         desc_addr + 8,  sizeof(uint64_t));
  mem_read(state.grid_dim,       desc_addr + 16, sizeof(state.grid_dim));
  mem_read(state.block_dim,      desc_addr + 28, sizeof(state.block_dim));
  mem_read(&state.block_size,    desc_addr + 40, sizeof(uint32_t));
  mem_read(state.warp_step,      desc_addr + 44, sizeof(state.warp_step));
  mem_read(&state.lmem_size,     desc_addr + 56, sizeof(uint32_t));
  mem_read(&state.core_affinity, desc_addr + 60, sizeof(uint32_t));

  DP(3, "*** device kernel launch: pc=0x" << std::hex << state.pc
     << ", arg=0x" << state.param << std::dec
     << ", grid=[" << state.grid_dim[0] << "," << state.grid_dim[1] << "," << state.grid_dim[2] << "]"
     << ", block=[" << state.block_dim[0] << "," << state.block_dim[1] << "," << state.block_dim[2] << "]"
     << ", core_affinity=" << (state.core_affinity == KMU_AFFINITY_ANY
                                 ? std::string("ANY")
                                 : std::to_string(state.core_affinity)));

  if (!running_ && pending_launches_.empty()) {
    this->arm_child(state);
  } else {
    pending_launches_.push_back(state);
  }
}

bool Kmu::step(uint32_t core_id, kmu_req_t* req) {
  if (!running_) {
    if (pending_launches_.empty())
      return false;
    const auto& state = pending_launches_.front();
    if (state.core_affinity != KMU_AFFINITY_ANY && state.core_affinity != core_id)
      return false;
    this->arm_child(state);
    pending_launches_.pop_front();
  }
  // Affinity gate: only the pinned core (if any) is allowed to dequeue.
  // Other cores see the queue as empty even though it is still draining.
  if (core_affinity_ != KMU_AFFINITY_ANY && core_affinity_ != core_id)
    return false;

  req->PC           = PC_;
  req->param        = param_;
  req->cta_id       = cta_id_;
  req->block_idx[0] = block_idx_[0];
  req->block_idx[1] = block_idx_[1];
  req->block_idx[2] = block_idx_[2];
  req->block_dim[0] = block_dim_[0];
  req->block_dim[1] = block_dim_[1];
  req->block_dim[2] = block_dim_[2];
  req->grid_dim[0]  = grid_dim_[0];
  req->grid_dim[1]  = grid_dim_[1];
  req->grid_dim[2]  = grid_dim_[2];
  req->lmem_size    = lmem_size_;
  req->block_size   = block_size_;
  req->warp_step[0] = warp_step_[0];
  req->warp_step[1] = warp_step_[1];
  req->warp_step[2] = warp_step_[2];

  // Advance the CTA iterator (X-innermost, Z-outermost)
  ++cta_id_;
  uint32_t bx = block_idx_[0] + 1;
  if (bx == grid_dim_[0]) {
    block_idx_[0] = 0;
    uint32_t by = block_idx_[1] + 1;
    if (by == grid_dim_[1]) {
      block_idx_[1] = 0;
      uint32_t bz = block_idx_[2] + 1;
      if (bz == grid_dim_[2]) {
        block_idx_[2] = 0;
        running_ = false;
      } else {
        block_idx_[2] = bz;
      }
    } else {
      block_idx_[1] = by;
    }
  } else {
    block_idx_[0] = bx;
  }

  return true;
}
