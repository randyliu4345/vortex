#include "warp.h"
#include <cstdio>
#include <cstdarg>
#include <atomic>

#ifndef WARP_LOGGING_ENABLED
static constexpr bool kWarpVerbose = false;
#else
static constexpr bool kWarpVerbose = true;
#endif

std::atomic<bool> g_warp_verbose{kWarpVerbose};

namespace {

void warp_log(const char* fmt, ...) {
    if (!g_warp_verbose.load(std::memory_order_relaxed)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

}

// Constructor: Initializes a RISC-V warp (SIMD thread group) with default state.
// Use case: Creates a warp instance representing a group of 32 threads that can be debugged.
Warp::Warp()
    : dpc(0),
      halted(true),
      running(false),
      resumeack(true),
      havereset(false),
      current_thread(0) {
    // Initialize all 32 threads' PCs to 0
    for (int i = 0; i < 32; i++) {
        pc[i] = 0;
    }
    // Initialize register file: 32 threads x 32 registers
    for (int thread = 0; thread < 32; thread++) {
        for (int reg = 0; reg < 32; reg++) {
            regs[thread][reg] = 0;
        }
    }
}

uint32_t Warp::get_pc() const {
    // Return selected thread's PC
    return pc[current_thread];
}

void Warp::set_pc(uint32_t value) {
    // Set PC for all 32 threads
    for (int i = 0; i < 32; i++) {
        pc[i] = value;
    }
}

uint32_t Warp::get_reg(size_t index) const {
    // Return selected thread's register value
    if (index >= 32) return 0;
    if (index == 0) return 0;  // x0 is always 0
    return regs[current_thread][index];
}

void Warp::set_reg(size_t index, uint32_t value) {
    // Set register for selected thread
    if (index >= 32) return;
    if (index == 0) return;  // x0 is read-only
    regs[current_thread][index] = value;
}

unsigned Warp::get_current_thread() const {
    return current_thread;
}

void Warp::set_current_thread(unsigned thread_id) {
    if (thread_id < 32) {
        current_thread = thread_id;
    }
}

uint32_t Warp::get_dpc() const {
    return dpc;
}

void Warp::set_dpc(uint32_t value) {
    dpc = value;
}

uint32_t Warp::dcsr_to_u32() const {
    return dcsr.to_u32();
}

void Warp::update_dcsr(uint32_t value) {
    dcsr.from_u32(value);
}

bool Warp::is_step_mode() const {
    return dcsr.step;
}

bool Warp::is_halted() const {
    return halted;
}

void Warp::set_halted(bool value) {
    halted = value;
    running = !value;
}

bool Warp::is_running() const {
    return running;
}

bool Warp::get_resumeack() const {
    return resumeack;
}

void Warp::set_resumeack(bool value) {
    resumeack = value;
}

bool Warp::get_havereset() const {
    return havereset;
}

void Warp::set_havereset(bool value) {
    havereset = value;
}

// Enters debug mode: saves PC to DPC, sets halt cause, and halts the warp.
// Use case: Called when a breakpoint, halt request, or exception occurs to stop execution.
void Warp::enter_debug_mode(uint8_t cause, uint32_t next_pc) {
    dpc = next_pc;
    dcsr.cause = cause & 0xF;
    set_halted(true);
    warp_log("[WARP] Entered Debug Mode: cause=%u, dpc=0x%08x\n", dcsr.cause, dpc);
}

// Executes a single instruction step for all 32 threads (increments PC by 4 for RISC-V 32-bit instructions).
// Use case: Used for single-step debugging to advance execution by one instruction for all threads.
void Warp::step() {
    // Step all 32 threads
    for (int i = 0; i < 32; i++) {
        pc[i] += 4;
    }
}

void Warp::set_verbose_logging(bool enable) {
    g_warp_verbose.store(enable, std::memory_order_relaxed);
}

bool Warp::verbose_logging() {
    return g_warp_verbose.load(std::memory_order_relaxed);
}

