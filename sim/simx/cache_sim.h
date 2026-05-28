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
#include "mem_sim.h"

namespace vortex {

// L2 mesh stats: one histogram sample per successful core->bank xbar transfer.
struct MeshHopBucket {
	uint64_t mesh_hop_cycles = 0;
	uint64_t mesh_reqs_hops0 = 0;
	uint64_t mesh_reqs_hops1 = 0;
	uint64_t mesh_reqs_hops2 = 0;
	uint64_t mesh_reqs_hops3 = 0;
	uint64_t mesh_reqs_hops4 = 0;

	uint64_t reqs_total() const {
		return mesh_reqs_hops0 + mesh_reqs_hops1 + mesh_reqs_hops2
		     + mesh_reqs_hops3 + mesh_reqs_hops4;
	}

	MeshHopBucket& operator+=(const MeshHopBucket& rhs) {
		mesh_hop_cycles += rhs.mesh_hop_cycles;
		mesh_reqs_hops0 += rhs.mesh_reqs_hops0;
		mesh_reqs_hops1 += rhs.mesh_reqs_hops1;
		mesh_reqs_hops2 += rhs.mesh_reqs_hops2;
		mesh_reqs_hops3 += rhs.mesh_reqs_hops3;
		mesh_reqs_hops4 += rhs.mesh_reqs_hops4;
		return *this;
	}

	void record(uint32_t hops, uint32_t hop_delay_cycles) {
		const uint32_t delay = hops * hop_delay_cycles;
		mesh_hop_cycles += delay;
		if (hops == 0)
			++mesh_reqs_hops0;
		else if (hops == 1)
			++mesh_reqs_hops1;
		else if (hops == 2)
			++mesh_reqs_hops2;
		else if (hops == 3)
			++mesh_reqs_hops3;
		else
			++mesh_reqs_hops4;
	}

	void reset() {
		mesh_hop_cycles = 0;
		mesh_reqs_hops0 = 0;
		mesh_reqs_hops1 = 0;
		mesh_reqs_hops2 = 0;
		mesh_reqs_hops3 = 0;
		mesh_reqs_hops4 = 0;
	}
};

class CacheSim : public SimObject<CacheSim> {
public:
	struct Config {
		bool    bypass;         // cache bypass
		uint8_t C;              // log2 cache size
		uint8_t L;              // log2 line size
		uint8_t W;              // log2 word size
		uint8_t A;              // log2 associativity
		uint8_t B;              // log2 number of banks
		uint8_t addr_width;     // word address bits
		uint8_t num_inputs;     // number of inputs
		uint8_t mem_ports;      // memory ports
		bool    write_back;     // is write-back
		bool    write_reponse;  // enable write response
		uint16_t mshr_size;     // MSHR buffer size
		uint8_t latency;        // pipeline latency
		bool    mesh_enable;    // L2 mesh hop latency (simx)
		uint8_t mesh_width;     // mesh columns (banks / width = rows)
		uint8_t mesh_hop_delay; // extra cycles per Manhattan hop
		bool    coarse_mapping_mode; // page-granular bank mapping enable
		uint8_t coarse_page_log2;    // page size = 1 << coarse_page_log2
	};

	struct PerfStats {
		uint64_t reads = 0;
		uint64_t writes = 0;
		uint64_t read_misses = 0;
		uint64_t write_misses = 0;
		uint64_t evictions = 0;
		uint64_t bank_stalls = 0;
		uint64_t mshr_stalls = 0;
		uint64_t mem_latency = 0;
		MeshHopBucket mesh;
		// Per global core_id (successful xbar transfers only).
		std::unordered_map<uint32_t, MeshHopBucket> mesh_by_core;

		PerfStats& operator+=(const PerfStats& rhs) {
			this->reads += rhs.reads;
			this->writes += rhs.writes;
			this->read_misses += rhs.read_misses;
			this->write_misses += rhs.write_misses;
			this->evictions += rhs.evictions;
			this->bank_stalls += rhs.bank_stalls;
			this->mshr_stalls += rhs.mshr_stalls;
			this->mem_latency += rhs.mem_latency;
			this->mesh += rhs.mesh;
			for (const auto& kv : rhs.mesh_by_core)
				this->mesh_by_core[kv.first] += kv.second;
			return *this;
		}
	};

	std::vector<SimChannel<MemReq>> core_req_in;
	std::vector<SimChannel<MemRsp>> core_rsp_out;
	std::vector<SimChannel<MemReq>> mem_req_out;
	std::vector<SimChannel<MemRsp>> mem_rsp_in;

	CacheSim(const SimContext& ctx, const char* name, const Config& config);
	~CacheSim();

	void reset();

	void tick();

	PerfStats perf_stats() const;

	// Zero out all per-bank performance counters in-place without touching
	// cache state (tags/LRU/MSHRs). Useful to delimit measurement windows
	// (e.g. ignore the host->device upload's compulsory misses).
	void reset_perf_stats();

	// Runtime L2 bank mapping control:
	// mode=0 -> fine-grained line interleaving (default)
	// mode=1 -> coarse page-granular interleaving.
	void set_bank_mapping(bool coarse_mode, uint8_t coarse_page_log2);

private:
	class Impl;
	Impl* impl_;
};

}