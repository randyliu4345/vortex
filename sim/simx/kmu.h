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

#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include "VX_types.h"

namespace vortex {

class RAM;

static constexpr uint32_t KMU_AFFINITY_ANY = 0xFFFFFFFFu;

struct __attribute__((packed)) vx_kmu_launch_desc_t {
  uint64_t pc;
  uint64_t arg;
  uint32_t grid_dim[3];
  uint32_t block_dim[3];
  uint32_t block_size;
  uint32_t warp_step[3];
  uint32_t lmem_size;
  uint32_t core_affinity;
  uint32_t flags;
};

struct kmu_req_t {
  uint64_t PC;
  uint64_t param;
  uint32_t cta_id;
  uint32_t block_idx[3];
  uint32_t block_dim[3];
  uint32_t grid_dim[3];
  uint32_t lmem_size;
  uint32_t block_size;
  uint32_t warp_step[3];
};

class Kmu {
public:
  using mem_reader_t = std::function<void(void*, uint64_t, uint32_t)>;

  Kmu();

  void reset();

  void dcr_write(uint32_t addr, uint32_t value);

  void start();

  void arm_child(uint64_t pc,
                 uint64_t param,
                 const uint32_t grid_dim[3],
                 const uint32_t block_dim[3],
                 uint32_t block_size,
                 const uint32_t warp_step[3],
                 uint32_t lmem_size,
                 uint32_t core_affinity = KMU_AFFINITY_ANY);

  void attach_ram(RAM* ram);

  void attach_mem_reader(uint32_t core_id, const mem_reader_t& mem_read);

  void signal_launch_request(uint32_t core_id);

  bool running() const { return running_ || launch_pending_; }

  bool launch_pending() const { return launch_pending_; }

  void service_launch_queue() { this->try_drain_launch_queue(); }

  bool running_for_core(uint32_t core_id) const {
    if (!running_)
      return false;
    return (core_affinity_ == KMU_AFFINITY_ANY) || (core_affinity_ == core_id);
  }

  uint32_t core_affinity() const { return core_affinity_; }

  void notify_cta_complete();

  bool step(uint32_t core_id, kmu_req_t* req);

private:
  struct launch_state_t {
    uint64_t pc;
    uint64_t param;
    uint32_t grid_dim[3];
    uint32_t block_dim[3];
    uint32_t block_size;
    uint32_t warp_step[3];
    uint32_t lmem_size;
    uint32_t core_affinity;
  };

  uint64_t PC_;
  uint64_t param_;
  uint32_t block_dim_[3];
  uint32_t grid_dim_[3];
  uint32_t lmem_size_;
  uint32_t block_size_;
  uint32_t warp_step_[3];
  uint32_t core_affinity_;
  bool     running_;
  uint32_t cta_id_;
  uint32_t block_idx_[3];
  bool     launch_pending_;
  uint32_t launch_core_id_;
  RAM*     ram_;
  std::unordered_map<uint32_t, mem_reader_t> mem_readers_;

  uint32_t grid_total_ctas_;
  uint32_t grid_completed_ctas_;
  bool     grid_executing_;
  bool     active_grid_tail_;
  uint32_t tail_stream_outstanding_;

  void mem_read(void* data, uint64_t addr, uint32_t size, uint32_t core_id);
  void mem_write(const void* data, uint64_t addr, uint32_t size);
  void init_grid_tracking();
  void on_grid_complete();
  void try_drain_launch_queue();
  void launch_child(const vx_kmu_launch_desc_t& desc);
  void arm_child(const launch_state_t& state);
};

} // namespace vortex
