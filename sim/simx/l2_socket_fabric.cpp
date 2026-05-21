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

#include "l2_socket_fabric.h"
#include "debug.h"

using namespace vortex;

L2SocketFabric::L2SocketFabric(const SimContext& ctx,
                               const char* name,
                               const Config& config)
  : SimObject<L2SocketFabric>(ctx, name)
  , socket_req_in(config.num_sockets, std::vector<SimChannel<MemReq>>(config.ports_per_socket, this))
  , socket_rsp_out(config.num_sockets, std::vector<SimChannel<MemRsp>>(config.ports_per_socket, this))
  , l2_req_out(config.num_sockets, this)
  , l2_rsp_in(config.num_sockets, this)
  , config_(config) {
}

void L2SocketFabric::reset() {
  perf_stats_ = PerfStats{};
  rsp_route_.clear();
  rsp_mesh_delay_.clear();
}

void L2SocketFabric::reset_perf_stats() {
  perf_stats_ = PerfStats{};
}

uint32_t L2SocketFabric::home_socket(uint64_t addr) const {
  const uint32_t n = config_.num_sockets;
  if (n <= 1)
    return 0;
  const uint64_t slot = addr >> config_.socket_region_log2;
  return static_cast<uint32_t>(slot % n);
}

uint32_t L2SocketFabric::mesh_hops(uint32_t src, uint32_t dst) const {
  if (!config_.mesh_enable || src == dst)
    return 0;
  const uint32_t w = config_.mesh_width;
  const uint32_t h = (config_.num_sockets + w - 1) / w;
  const uint32_t sx = src % w, sy = src / w;
  const uint32_t dx = dst % w, dy = dst / w;
  if (sy >= h || dy >= h)
    return sx > dx ? sx - dx : dx - sx;
  return (sx > dx ? sx - dx : dx - sx) + (sy > dy ? sy - dy : dy - sy);
}

void L2SocketFabric::count_hops(uint32_t hops) {
  if (hops == 0)
    ++perf_stats_.mesh_reqs_hops0;
  else if (hops == 1)
    ++perf_stats_.mesh_reqs_hops1;
  else if (hops == 2)
    ++perf_stats_.mesh_reqs_hops2;
  else
    ++perf_stats_.mesh_reqs_hops3p;
}

void L2SocketFabric::tick() {
  const uint32_t ns = config_.num_sockets;
  const uint32_t np = config_.ports_per_socket;

  // Return responses from home L2 to requesting socket.
  for (uint32_t home = 0; home < ns; ++home) {
    auto& l2_rsp = l2_rsp_in.at(home);
    if (l2_rsp.empty())
      continue;
    auto& rsp = l2_rsp.peek();
    auto it = rsp_route_.find(rsp.uuid);
    if (it == rsp_route_.end())
      continue;
    const route_t rt = it->second;
    auto& sock_rsp = socket_rsp_out.at(rt.socket).at(rt.port);
    if (sock_rsp.full())
      continue;
    uint32_t delay = 0;
    auto dit = rsp_mesh_delay_.find(rsp.uuid);
    if (dit != rsp_mesh_delay_.end()) {
      delay = dit->second;
      perf_stats_.mesh_hop_cycles += delay;
    }
    if (!sock_rsp.try_send(rsp, delay))
      continue;
    DT(3, this->name() << "-socket-rsp: sock=" << rt.socket << " port=" << rt.port
                       << " home=" << home << " " << rsp);
    l2_rsp.pop();
    rsp_route_.erase(it);
    if (dit != rsp_mesh_delay_.end())
      rsp_mesh_delay_.erase(dit);
  }

  // Forward socket requests to home L2.
  for (uint32_t s = 0; s < ns; ++s) {
    for (uint32_t p = 0; p < np; ++p) {
      auto& sock_req = socket_req_in.at(s).at(p);
      if (sock_req.empty())
        continue;
      auto& req = sock_req.peek();
      const uint32_t home = home_socket(req.addr);
      auto& l2_req = l2_req_out.at(home);
      if (l2_req.full())
        continue;
      const uint32_t hops = mesh_hops(s, home);
      const uint32_t delay = hops * config_.mesh_hop_delay;
      if (config_.mesh_enable) {
        count_hops(hops);
        if (delay != 0)
          perf_stats_.mesh_hop_cycles += delay;
        rsp_mesh_delay_[req.uuid] = delay;
      }
      rsp_route_[req.uuid] = route_t{s, p};
      if (!l2_req.try_send(req, delay))
        continue;
      DT(3, this->name() << "-l2-req: sock=" << s << " home=" << home
                         << " hops=" << hops << " " << req);
      sock_req.pop();
    }
  }
}

L2SocketFabric::PerfStats L2SocketFabric::perf_stats() const {
  return perf_stats_;
}
