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

#include "processor.h"
#include "processor_impl.h"

#include <simobject.h>

#include <cstdlib>
#include <execinfo.h>
#include <iostream>

using namespace vortex;

static void simx_print_backtrace() {
  void* addrs[64];
  int count = ::backtrace(addrs, int(std::size(addrs)));
  char** symbols = ::backtrace_symbols(addrs, count);
  if (symbols == nullptr)
    return;
  std::cerr << "Backtrace (" << count << " frames):" << std::endl;
  for (int i = 0; i < count; ++i) {
    std::cerr << "  " << symbols[i] << std::endl;
  }
  std::free(symbols);
}

ProcessorImpl::ProcessorImpl(const Arch& arch)
  : arch_(arch)
  , clusters_(arch.num_clusters())
{
  SimPlatform::instance().initialize();

	assert(PLATFORM_MEMORY_DATA_SIZE == MEM_BLOCK_SIZE);

  // create memory simulator
  memsim_ = MemSim::Create("dram", MemSim::Config{
    PLATFORM_MEMORY_NUM_BANKS,
    L3_MEM_PORTS,
    MEM_BLOCK_SIZE,
    MEM_CLOCK_RATIO
  });

  char sname[100];

  // create clusters
  for (uint32_t i = 0; i < arch.num_clusters(); ++i) {
    snprintf(sname, 100, "cluster%d", i);
    clusters_.at(i) = Cluster::Create(sname, i, this, arch);
  }

  // create L3 cache
  l3cache_ = CacheSim::Create("l3cache", CacheSim::Config{
    !L3_ENABLED,
    log2ceil(L3_CACHE_SIZE),  // C
    log2ceil(MEM_BLOCK_SIZE), // L
    log2ceil(L2_LINE_SIZE),   // W
    log2ceil(L3_NUM_WAYS),    // A
    log2ceil(L3_NUM_BANKS),   // B
    XLEN,                     // address bits
    L3_NUM_REQS,              // request size
    L3_MEM_PORTS,             // memory ports
    L3_WRITEBACK,             // write-back
    false,                    // write response
    L3_MSHR_SIZE,             // mshr size
    2,                        // pipeline latency
    false, 0, 0,              // mesh (L2 only)
  });

  // connect L3 core interfaces
  for (uint32_t i = 0; i < arch.num_clusters(); ++i) {
    for (uint32_t j = 0; j < L2_MEM_PORTS; ++j) {
      clusters_.at(i)->mem_req_out.at(j).bind(&l3cache_->core_req_in.at(i * L2_MEM_PORTS + j));
      l3cache_->core_rsp_out.at(i * L2_MEM_PORTS + j).bind(&clusters_.at(i)->mem_rsp_in.at(j));
    }
  }

  // connect L3 memory interfaces
  for (uint32_t i = 0; i < L3_MEM_PORTS; ++i) {
    l3cache_->mem_req_out.at(i).bind(&memsim_->mem_req_in.at(i));
    memsim_->mem_rsp_out.at(i).bind(&l3cache_->mem_rsp_in.at(i));
  }

  // set up memory profiling
  for (uint32_t i = 0; i < L3_MEM_PORTS; ++i) {
    memsim_->mem_req_in.at(i).tx_callback([&](const MemReq& req, uint64_t cycle){
      __unused (cycle);
      perf_mem_reads_  += !req.write;
      perf_mem_writes_ += req.write;
      perf_mem_pending_reads_ += !req.write;
    });
    memsim_->mem_rsp_out.at(i).tx_callback([&](const MemRsp&, uint64_t cycle){
      __unused (cycle);
      --perf_mem_pending_reads_;
    });
  }

#ifndef NDEBUG
  // dump device configuration
  std::cout << "CONFIGS:"
            << " num_threads=" << arch.num_threads()
            << ", num_warps=" << arch.num_warps()
            << ", num_cores=" << arch.num_cores()
            << ", num_clusters=" << arch.num_clusters()
            << ", socket_size=" << arch.socket_size()
            << ", local_mem_base=0x" << std::hex << arch.local_mem_base() << std::dec
            << ", num_barriers=" << arch.num_barriers()
            << std::endl;
#endif
  // reset the device
  this->reset();
}

ProcessorImpl::~ProcessorImpl() {
  SimPlatform::instance().finalize();
}

void ProcessorImpl::attach_ram(RAM* ram) {
  for (auto cluster : clusters_) {
    cluster->attach_ram(ram);
  }
}
#ifdef VM_ENABLE
void ProcessorImpl::set_satp(uint64_t satp) {
  for (auto cluster : clusters_) {
    cluster->set_satp(satp);
  }
}
#endif

int ProcessorImpl::run() {
  this->reset();
  kmu_.start();
  // SimPlatform::cycles() is 0 here; end-of-run value is recorded as last_run_sim_cycles_
  // after the tick loop exits (final ebreak stops all clusters).

  bool done;
  int exitcode = 0;
  do {
    SimPlatform::instance().tick();
    done = true;
    for (auto cluster : clusters_) {
      if (cluster->running()) {
        done = false;
        continue;
      }
      exitcode |= cluster->get_exitcode();
    }
    perf_mem_latency_ += perf_mem_pending_reads_;
  } while (!done);

  last_run_sim_cycles_ = SimPlatform::instance().cycles();
  return exitcode;
}

void ProcessorImpl::reset() {
  SimPlatform::instance().reset();
  perf_mem_reads_ = 0;
  perf_mem_writes_ = 0;
  perf_mem_latency_ = 0;
  perf_mem_pending_reads_ = 0;
}

int ProcessorImpl::dcr_write(uint32_t addr, uint32_t value) {
  // KMU DCRs are stored in the processor-level KMU and not broadcast to cores
  if (addr >= VX_DCR_KMU_STATE_BEGIN && addr < VX_DCR_KMU_STATE_END) {
    kmu_.dcr_write(addr, value);
    return 0;
  }
  // Performance counter reset request: bumps every perf counter we maintain
  // at the processor scope (DRAM, L3, per-cluster L2, per-socket L1 I/D)
  // back to zero so the next measurement window starts clean.
  if (addr == VX_DCR_BASE_PERF_RESET) {
    (void)value;
    this->reset_perf_stats();
    return 0;
  }
  for (auto& cluster : clusters_) {
    int ret = cluster->dcr_write(addr, value);
    if (ret != 0)
      return ret;
  }
  return 0;
}

int ProcessorImpl::dcr_read(uint32_t addr, uint32_t tag, uint32_t* value) {
  for (auto& cluster : clusters_) {
    int ret = cluster->dcr_read(addr, tag, value);
    if (ret != 0)
      return ret;
  }
  return 0;
}

ProcessorImpl::PerfStats ProcessorImpl::perf_stats() const {
  ProcessorImpl::PerfStats perf;
  perf.mem_reads   = perf_mem_reads_;
  perf.mem_writes  = perf_mem_writes_;
  perf.mem_latency = perf_mem_latency_;
  perf.l3cache     = l3cache_->perf_stats();
  perf.memsim      = memsim_->perf_stats();
  return perf;
}

void ProcessorImpl::print_mesh_l2_stats(const char* tag) const {
  uint64_t h0 = 0, h1 = 0, h2 = 0, h3p = 0, hop_cycles = 0;
  for (const auto& cluster : clusters_) {
#if L2_SOCKET_PRIVATE_ENABLED
    const auto& fab = cluster->l2_fabric().perf_stats();
    h0 += fab.mesh_reqs_hops0;
    h1 += fab.mesh_reqs_hops1;
    h2 += fab.mesh_reqs_hops2;
    h3p += fab.mesh_reqs_hops3p;
    hop_cycles += fab.mesh_hop_cycles;
#else
    const auto& l2 = cluster->perf_stats().l2cache;
    h0 += l2.mesh_reqs_hops0;
    h1 += l2.mesh_reqs_hops1;
    h2 += l2.mesh_reqs_hops2;
    h3p += l2.mesh_reqs_hops3p;
    hop_cycles += l2.mesh_hop_cycles;
#endif
  }
  const uint64_t total = h0 + h1 + h2 + h3p;
  if (total == 0 && hop_cycles == 0)
    return;
  const char* lbl = (tag && tag[0]) ? tag : "run";
  std::cerr << "MESH_L2_STATS tag=" << lbl
            << " reqs_total=" << total
            << " close_0hop=" << h0
            << " mid_1hop=" << h1
            << " far_2hop=" << h2
            << " hops3p=" << h3p
            << " mesh_hop_cycles=" << hop_cycles
            << std::endl;
}

void ProcessorImpl::reset_perf_stats() {
  perf_mem_reads_ = 0;
  perf_mem_writes_ = 0;
  perf_mem_latency_ = 0;
  // Note: perf_mem_pending_reads_ is mid-flight state, not a counter; do
  // not zero it or in-flight responses would underflow the latency sum.
  l3cache_->reset_perf_stats();
  memsim_->reset_perf_stats();
  for (auto& cluster : clusters_) {
    cluster->reset_perf_stats();
  }
}

///////////////////////////////////////////////////////////////////////////////

Processor::Processor(const Arch& arch)
  : impl_(new ProcessorImpl(arch))
{
#ifdef VM_ENABLE
  satp_ = NULL;
#endif
}

Processor::~Processor() {
  delete impl_;
#ifdef VM_ENABLE
  if (satp_ != NULL)
    delete satp_;
#endif
}

void Processor::attach_ram(RAM* mem) {
  impl_->attach_ram(mem);
}

void Processor::reset() {
  impl_->reset();
}

uint64_t Processor::last_run_sim_cycles() const {
  return impl_->last_run_sim_cycles();
}

void Processor::print_mesh_l2_stats(const char* tag) const {
  impl_->print_mesh_l2_stats(tag);
}

int Processor::run() {
  try {
    return impl_->run();
  } catch (const std::exception& e) {
    std::cerr << "Error: exception: " << e.what() << std::endl;
    if (std::getenv("SIMX_BACKTRACE") != nullptr) {
      simx_print_backtrace();
    }
  } catch (...) {
    std::cerr << "Error: unknown exception." << std::endl;
    if (std::getenv("SIMX_BACKTRACE") != nullptr) {
      simx_print_backtrace();
    }
  }
  return -1;
}

int Processor::dcr_write(uint32_t addr, uint32_t value) {
  return impl_->dcr_write(addr, value);
}

int Processor::dcr_read(uint32_t addr, uint32_t tag, uint32_t* value) {
  return impl_->dcr_read(addr, tag, value);
}

#ifdef VM_ENABLE
int16_t Processor::set_satp_by_addr(uint64_t base_addr) {
  uint16_t asid = 0;
  satp_ = new SATP_t (base_addr,asid);
  if (satp_ == NULL)
    return 1;
  uint64_t satp = satp_->get_satp();
  impl_->set_satp(satp);
  return 0;
}
bool Processor::is_satp_unset() {
  return (satp_== NULL);
}
uint8_t Processor::get_satp_mode() {
  assert (satp_!=NULL);
  return satp_->get_mode();
}
uint64_t Processor::get_base_ppn() {
  assert (satp_!=NULL);
  return satp_->get_base_ppn();
}
#endif
