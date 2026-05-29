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
#include "mem.h"
#include <VX_config.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace vortex;

namespace {
constexpr uint32_t VX_LAUNCH_FLAG_TAIL = 1u << 0;
}

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
  running_      = false;
  cta_id_       = 0;
  block_idx_[0] = block_idx_[1] = block_idx_[2] = 0;
  launch_pending_           = false;
  launch_core_id_           = 0;
  ram_                      = nullptr;
  grid_total_ctas_          = 0;
  grid_completed_ctas_      = 0;
  grid_executing_           = false;
  active_grid_tail_         = false;
  tail_stream_outstanding_  = 0;
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
  default: break;
  }
}

void Kmu::init_grid_tracking() {
  grid_total_ctas_     = grid_dim_[0] * grid_dim_[1] * grid_dim_[2];
  grid_completed_ctas_ = 0;
  grid_executing_      = false;
}

void Kmu::on_grid_complete() {
  grid_executing_ = false;
  if (active_grid_tail_ && tail_stream_outstanding_ > 0)
    --tail_stream_outstanding_;
  this->try_drain_launch_queue();
}

void Kmu::start() {
  active_grid_tail_ = false;
  this->init_grid_tracking();
  running_ = (block_size_ > 0)
           && (grid_dim_[0] > 0)
           && (grid_dim_[1] > 0)
           && (grid_dim_[2] > 0);
  if (running_) {
    cta_id_       = 0;
    block_idx_[0] = block_idx_[1] = block_idx_[2] = 0;
  } else if (grid_total_ctas_ == 0) {
    this->on_grid_complete();
  }
}

void Kmu::arm_child(uint64_t pc,
                    uint64_t param,
                    const uint32_t grid_dim[3],
                    const uint32_t block_dim[3],
                    uint32_t block_size,
                    const uint32_t warp_step[3],
                    uint32_t lmem_size) {
  PC_           = pc;
  param_        = param;
  for (int i = 0; i < 3; ++i) {
    grid_dim_[i]  = grid_dim[i];
    block_dim_[i] = block_dim[i];
    warp_step_[i] = warp_step[i];
  }
  block_size_   = block_size;
  lmem_size_    = lmem_size;
  this->init_grid_tracking();
  running_ = (block_size_ > 0)
           && (grid_dim_[0] > 0)
           && (grid_dim_[1] > 0)
           && (grid_dim_[2] > 0);
  if (running_) {
    cta_id_       = 0;
    block_idx_[0] = block_idx_[1] = block_idx_[2] = 0;
  } else if (grid_total_ctas_ == 0) {
    this->on_grid_complete();
  }
}

void Kmu::attach_ram(RAM* ram) {
  ram_ = ram;
}

void Kmu::attach_mem_reader(uint32_t core_id, const mem_reader_t& mem_read) {
  mem_readers_[core_id] = mem_read;
}

void Kmu::mem_read(void* data, uint64_t addr, uint32_t size, uint32_t core_id) {
  auto it = mem_readers_.find(core_id);
  if (it != mem_readers_.end()) {
    it->second(data, addr, size);
    return;
  }
  if (ram_ != nullptr) {
    ram_->read((uint8_t*)data, addr, size);
    return;
  }
  std::cerr << "Error: KMU memory read from core #" << core_id
            << " has no attached memory reader" << std::endl;
  std::abort();
}

void Kmu::mem_write(const void* data, uint64_t addr, uint32_t size) {
  assert(ram_ != nullptr && "KMU launch queue drain requires direct RAM access");
  ram_->enable_acl(false);
  ram_->write((const uint8_t*)data, addr, size);
  ram_->enable_acl(true);
}

void Kmu::signal_launch_request(uint32_t core_id) {
  launch_core_id_ = core_id;
  launch_pending_ = true;
}

void Kmu::notify_cta_complete() {
  if (grid_total_ctas_ == 0)
    return;
  ++grid_completed_ctas_;
  if (grid_completed_ctas_ >= grid_total_ctas_) {
    grid_total_ctas_ = 0;
    this->on_grid_complete();
  }
}

void Kmu::try_drain_launch_queue() {
  // Wait until all CTAs of the current grid retire before arming a queued launch.
  if (running_ || grid_executing_ || !launch_pending_)
    return;

  const uint64_t queue_addr = LAUNCH_QUEUE_BASE;
  const uint32_t core_id = launch_core_id_;

  while (!running_ && launch_pending_) {
    uint32_t head = 0;
    uint32_t tail = 0;
    this->mem_read(&head, queue_addr + 0, sizeof(uint32_t), core_id);
    this->mem_read(&tail, queue_addr + 4, sizeof(uint32_t), core_id);
    if (head == tail) {
      launch_pending_ = false;
      return;
    }

    vx_kmu_launch_desc_t desc;
    static_assert(sizeof(desc) == 64, "layout must match vx_kmu_launch_entry_t");
    uint32_t idx = head % LAUNCH_QUEUE_SIZE;
    uint64_t entry_addr = queue_addr + 8 + uint64_t(idx) * sizeof(desc);
    this->mem_read(&desc, entry_addr, sizeof(desc), core_id);

    if ((desc.flags & VX_LAUNCH_FLAG_TAIL) && tail_stream_outstanding_ > 0)
      return;

    this->launch_child(desc);

    ++head;
    this->mem_write(&head, queue_addr + 0, sizeof(uint32_t));
  }
}

void Kmu::launch_child(const vx_kmu_launch_desc_t& desc) {
  uint32_t grid_dim[3];
  uint32_t block_dim[3];
  uint32_t warp_step[3];
  std::memcpy(grid_dim, desc.grid_dim, sizeof(grid_dim));
  std::memcpy(block_dim, desc.block_dim, sizeof(block_dim));
  std::memcpy(warp_step, desc.warp_step, sizeof(warp_step));
  DP(3, "*** device kernel launch: pc=0x" << std::hex << desc.pc
     << ", arg=0x" << desc.arg << std::dec
     << ", grid=[" << grid_dim[0] << "," << grid_dim[1] << "," << grid_dim[2] << "]"
     << ", block=[" << block_dim[0] << "," << block_dim[1] << "," << block_dim[2] << "]"
     << ", tail=" << ((desc.flags & VX_LAUNCH_FLAG_TAIL) != 0));
  active_grid_tail_ = (desc.flags & VX_LAUNCH_FLAG_TAIL) != 0;
  if (active_grid_tail_)
    ++tail_stream_outstanding_;
  this->arm_child(desc.pc, desc.arg, grid_dim, block_dim,
                  desc.block_size, warp_step, desc.lmem_size);
}

bool Kmu::step(kmu_req_t* req) {
  if (!running_) {
    this->try_drain_launch_queue();
    return false;
  }

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
        if (grid_total_ctas_ == 0) {
          this->on_grid_complete();
        } else {
          grid_executing_ = true;
        }
        this->try_drain_launch_queue();
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
