#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>

struct DCSR {
    uint32_t prv       : 2;
    uint32_t step      : 1;
    uint32_t ebreakm   : 1;
    uint32_t ebreaks   : 1;
    uint32_t ebreaku   : 1;
    uint32_t stopcount : 1;
    uint32_t stoptime  : 1;
    uint32_t cause     : 4;
    uint32_t mprven    : 1;
    uint32_t nmip      : 1;
    uint32_t reserved  : 14;
    uint32_t xdebugver : 4;

    DCSR()
        : prv(3), step(0), ebreakm(0), ebreaks(0), ebreaku(0),
          stopcount(0), stoptime(0), cause(0), mprven(0),
          nmip(0), reserved(0), xdebugver(4) {}

    uint32_t to_u32() const {
        uint32_t value = 0;
        value |= (prv & 0x3);
        value |= (step & 0x1) << 2;
        value |= (ebreakm & 0x1) << 3;
        value |= (ebreaks & 0x1) << 4;
        value |= (ebreaku & 0x1) << 5;
        value |= (stopcount & 0x1) << 6;
        value |= (stoptime & 0x1) << 7;
        value |= (cause & 0xF) << 8;
        value |= (mprven & 0x1) << 12;
        value |= (nmip & 0x1) << 13;
        value |= (xdebugver & 0xF) << 28;
        return value;
    }

    void from_u32(uint32_t value) {
        prv       = value & 0x3;
        step      = (value >> 2) & 0x1;
        ebreakm   = (value >> 3) & 0x1;
        ebreaks   = (value >> 4) & 0x1;
        ebreaku   = (value >> 5) & 0x1;
        stopcount = (value >> 6) & 0x1;
        stoptime  = (value >> 7) & 0x1;
        cause     = (value >> 8) & 0xF;
        mprven    = (value >> 12) & 0x1;
        nmip      = (value >> 13) & 0x1;
        xdebugver = 4;
        reserved  = 0;
    }
};


class Warp {
public:
    // Constructor: Initializes a RISC-V warp (SIMD thread group) with default state.
    // Use case: Creates a warp instance representing a group of 32 threads that can be debugged.
    Warp();

    // Returns the current Program Counter (PC) value for the selected thread.
    // Use case: Used by debugger to read the current instruction address.
    uint32_t get_pc() const;
    
    // Sets the Program Counter (PC) to a new value for all threads.
    // Use case: Used by debugger to set a breakpoint or jump to a different address.
    void set_pc(uint32_t value);

    // Returns the value of a general-purpose register (x0-x31) for the selected thread.
    // Use case: Used by debugger to read CPU register values. x0 always returns 0 (read-only).
    uint32_t get_reg(size_t index) const;
    
    // Sets the value of a general-purpose register (x0-x31) for the selected thread.
    // Use case: Used by debugger to modify CPU register values. x0 writes are ignored (read-only).
    void set_reg(size_t index, uint32_t value);

    // Returns the currently selected thread index (0-31).
    // Use case: Used to query which thread's registers are being accessed.
    unsigned get_current_thread() const;

    // Sets the currently selected thread index (0-31). Thread selection affects register reads/writes.
    // Use case: Used to switch between threads for register access. Step operations still affect all threads.
    void set_current_thread(unsigned thread_id);

    // Returns the Debug Program Counter (DPC) - the PC value when entering debug mode.
    // Use case: Used to determine where execution was when the warp was halted.
    uint32_t get_dpc() const;
    
    // Sets the Debug Program Counter (DPC) value.
    // Use case: Used when entering debug mode to save the current PC.
    void set_dpc(uint32_t value);

    // Converts the Debug Control and Status Register (DCSR) to a 32-bit value.
    // Use case: Used to read the DCSR register via abstract commands.
    uint32_t dcsr_to_u32() const;
    
    // Updates the DCSR register from a 32-bit value.
    // Use case: Used to write the DCSR register via abstract commands (e.g., to enable step mode).
    void update_dcsr(uint32_t value);
    
    // Returns true if the warp is in single-step mode (DCSR.step = 1).
    // Use case: Used to determine if the warp should execute one instruction and then halt.
    bool is_step_mode() const;

    // Returns true if the warp is currently halted in debug mode.
    // Use case: Used to check warp state for debug status reporting.
    bool is_halted() const;
    
    // Sets the halted state of the warp (also updates running state).
    // Use case: Used to halt or resume the warp for debugging.
    void set_halted(bool value);

    // Returns true if the warp is currently running (not halted).
    // Use case: Used to check warp state for debug status reporting.
    bool is_running() const;

    // Returns the resume acknowledgment flag (indicates warp has resumed after resume request).
    // Use case: Used by debug module to report resume status to debugger.
    bool get_resumeack() const;

    // Sets the resume acknowledgment flag.
    // Use case: Used to acknowledge that a resume request has been processed.
    void set_resumeack(bool value);

    // Returns the "have reset" flag (indicates warp has been reset since last acknowledgment).
    // Use case: Used to report reset status to debugger.
    bool get_havereset() const;

    // Sets the "have reset" flag.
    // Use case: Used to mark that the warp has been reset.
    void set_havereset(bool value);

    // Enters debug mode: saves PC to DPC, sets halt cause, and halts the warp.
    // Use case: Called when a breakpoint, halt request, or exception occurs to stop execution.
    void enter_debug_mode(uint8_t cause, uint32_t next_pc);
    
    // Executes a single instruction step for all 32 threads (increments PC by 4 for RISC-V 32-bit instructions).
    // Use case: Used for single-step debugging to advance execution by one instruction for all threads.
    void step();

    // Enables or disables verbose logging for warp operations.
    // Use case: Used to control debug output during development and troubleshooting.
    static void set_verbose_logging(bool enable);
    
    // Returns the current verbose logging state.
    // Use case: Used to check if verbose logging is enabled.
    static bool verbose_logging();

private:
    static std::atomic<bool> verbose_flag;

    // PC for each of the 32 threads
    uint32_t pc[32];
    // Register file: 32 threads x 32 registers
    uint32_t regs[32][32];
    uint32_t dpc;
    DCSR dcsr;
    bool halted;
    bool running;
    bool resumeack;
    bool havereset;
    // Currently selected thread for register access (0-31)
    unsigned current_thread;
};

