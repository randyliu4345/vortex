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

#ifndef VX_E2E_EVAL_H
#define VX_E2E_EVAL_H

#include <stdint.h>

// Modeled PCIe: fixed driver/OS/handshake cost per transaction (cycles @ 1 GHz core clock).
#define PCIE_FIXED_LATENCY 50000ULL

// Bandwidth model: one SimX cycle of delay per PCIE_BANDWIDTH_LATENCY_BYTES transferred (16 GB/s @ 1 GHz).
#define PCIE_BANDWIDTH_LATENCY_BYTES 16ULL
#define PCIE_BANDWIDTH_LATENCY 1ULL

#ifdef __cplusplus
extern "C" {
#endif

// Enable via environment variable VORTEX_E2E_EVAL=1 (checked at first use).
int vx_e2e_eval_enabled(void);

// Reset host-side PCIe counters and SimX GPU cycle window (call before timed BFS region).
void vx_e2e_eval_reset(void);

// Host-to-device copy: one PCIe transaction plus byte-count for bandwidth term.
void vx_e2e_record_pcie_copy(uint64_t byte_count);

// Host kernel submission (vx_start / vx_start_g): one PCIe transaction.
void vx_e2e_record_pcie_launch(void);

// SimX: cumulative simulated cycles for one completed processor run (SimPlatform ticks).
void vx_e2e_on_sim_run_end(uint64_t run_sim_cycles);

// Print human-readable summary and write CSV under VORTEX_E2E_EVAL_DIR (default: e2e_eval_output).
void vx_e2e_eval_finalize(void);

#ifdef __cplusplus
}

namespace vortex {
// Computes Pure_GPU_Cycles, Modeled_PCIe_Delay, Total_E2E_Cycles; prints and writes CSV.
void print_e2e_evaluation();
} // namespace vortex

#endif

#endif // VX_E2E_EVAL_H
