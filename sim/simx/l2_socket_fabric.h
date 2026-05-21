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

#include <simobject.h>
#include <unordered_map>
#include <vector>
#include "cache_sim.h"

namespace vortex {

// Routes socket L1 misses to per-socket private L2 caches. Address bits select
// the home socket; cross-socket traffic pays Manhattan mesh delay (socket grid).
class L2SocketFabric : public SimObject<L2SocketFabric> {
public:
  struct Config {
    uint32_t num_sockets;
    uint32_t ports_per_socket;
    uint32_t socket_region_log2; // home = (addr >> socket_region_log2) % num_sockets
    bool     mesh_enable;
    uint8_t  mesh_width;
    uint8_t  mesh_hop_delay;
  };

  struct PerfStats {
    uint64_t mesh_hop_cycles = 0;
    uint64_t mesh_reqs_hops0 = 0;
    uint64_t mesh_reqs_hops1 = 0;
    uint64_t mesh_reqs_hops2 = 0;
    uint64_t mesh_reqs_hops3p = 0;

    PerfStats& operator+=(const PerfStats& rhs) {
      mesh_hop_cycles += rhs.mesh_hop_cycles;
      mesh_reqs_hops0 += rhs.mesh_reqs_hops0;
      mesh_reqs_hops1 += rhs.mesh_reqs_hops1;
      mesh_reqs_hops2 += rhs.mesh_reqs_hops2;
      mesh_reqs_hops3p += rhs.mesh_reqs_hops3p;
      return *this;
    }
  };

  std::vector<std::vector<SimChannel<MemReq>>> socket_req_in;
  std::vector<std::vector<SimChannel<MemRsp>>> socket_rsp_out;
  std::vector<SimChannel<MemReq>> l2_req_out;
  std::vector<SimChannel<MemRsp>> l2_rsp_in;

  L2SocketFabric(const SimContext& ctx, const char* name, const Config& config);

  void reset();

  void tick();

  PerfStats perf_stats() const;

  void reset_perf_stats();

private:
  struct route_t {
    uint32_t socket;
    uint32_t port;
  };

  Config config_;
  PerfStats perf_stats_;
  std::unordered_map<uint64_t, route_t> rsp_route_;
  std::unordered_map<uint64_t, uint32_t> rsp_mesh_delay_;

  uint32_t home_socket(uint64_t addr) const;
  uint32_t mesh_hops(uint32_t src, uint32_t dst) const;
  void count_hops(uint32_t hops);
};

} // namespace vortex
