#include "debug_module.h"
#include <cstdarg>
#include <atomic>
#include <cstring>
#include "emulator.h"
#include <VX_config.h>
#include <bitmanip.h>

namespace {

std::atomic<bool> g_debug_module_verbose{false};

void dm_log(const char* fmt, ...) {
    if (!g_debug_module_verbose.load(std::memory_order_relaxed)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

}

// Helper function to decode warp_id and thread_id from hartsel
// Uses log2(NUM_THREADS) bits for thread_id, remaining bits for warp_id
static void decode_hartsel(unsigned hartsel, unsigned& warp_id, unsigned& thread_id) {
    constexpr unsigned thread_bits = vortex::log2ceil(NUM_THREADS);
    constexpr unsigned thread_mask = (1U << thread_bits) - 1;
    thread_id = hartsel & thread_mask;
    warp_id = hartsel >> thread_bits;
    // Clamp to valid ranges
    if (thread_id >= NUM_THREADS) {
        thread_id = 0;
    }
    if (warp_id >= NUM_WARPS) {
        warp_id = 0;
    }
}

// Enables or disables verbose logging for debug module operations.
// Use case: Used to control debug output during development and troubleshooting.
void DebugModule::set_verbose_logging(bool enable) {
    g_debug_module_verbose.store(enable, std::memory_order_relaxed);
}

bool DebugModule::verbose_logging() {
    return g_debug_module_verbose.load(std::memory_order_relaxed);
}

// Constructor: Initializes the RISC-V Debug Module with a simulated memory space.
// Use case: Creates a debug module instance that implements the RISC-V Debug Specification 0.13.
DebugModule::DebugModule(vortex::Emulator* emulator, size_t mem_size)
    : emulator_(emulator),
      halt_requested_(false),
      single_step_active_(false),
      debug_mode_enabled_(false),
      data1(0),
      data2(0),
      data3(0),
      command(0),
      resumereq_prev(false),
      memory(mem_size, 0),
      access_mem_addr(0)
{
    for (unsigned i = 0; i < datacount; i++) {
        dmdata[i] = 0;
    }

    reset();
}

// Resets the debug module to its initial state: clears all registers and resets the hart.
// Use case: Called when dmactive is set from 0 to 1, or during initialization.
void DebugModule::reset()
{
    dmcontrol = dmcontrol_t();
    dmstatus = dmstatus_t();
    abstractcs = abstractcs_t();

    // Initialize debug state
    dcsr_ = DCSR();
    dpc_ = 0;
    resumeack_ = false;
    havereset_ = false;
    is_halted_ = false;

    dmcontrol.dmactive = true;
    dmstatus.authenticated = true;
    dmstatus.authbusy = false;
    dmstatus.version = 2;

    dmstatus.allnonexistent = false;
    dmstatus.anynonexistent = false;
    dmstatus.allunavail = false;
    dmstatus.anyunavail = false;

    access_mem_addr = 0;
    access_mem_addr_valid = false;

    update_dmstatus();

    dmstatus.authenticated = true;
    dmstatus.authbusy = false;
}

// Updates the dmstatus register fields based on current hart state.
// Use case: Called before reading dmstatus to ensure it reflects current hart state (halted/running/etc.).
// Preserves authentication state which must remain true for OpenOCD compatibility.
void DebugModule::update_dmstatus()
{
    bool saved_authenticated = dmstatus.authenticated;
    bool saved_authbusy = dmstatus.authbusy;
    unsigned saved_version = dmstatus.version;

    // Check running state - emulator notifies us when program completes, so we just check our flags
    if (is_halted_ || halt_requested_) {
        dmstatus.allhalted = true;
        dmstatus.anyhalted = true;
        dmstatus.allrunning = false;
        dmstatus.anyrunning = false;
    } else {
        dmstatus.allhalted = false;
        dmstatus.anyhalted = false;
        dmstatus.allrunning = true;
        dmstatus.anyrunning = true;
    }

    dmstatus.allresumeack = resumeack_;
    dmstatus.anyresumeack = resumeack_;
    dmstatus.allhavereset = havereset_;
    dmstatus.anyhavereset = havereset_;

    // Check if selected hartsel (warp+thread) is valid
    unsigned warp_id, thread_id;
    decode_hartsel(dmcontrol.hartsel, warp_id, thread_id);
    bool hart_exists = (warp_id < NUM_WARPS) && (thread_id < NUM_THREADS);
    
    unsigned max_hartsel = NUM_THREADS * NUM_WARPS;
    bool hart_out_of_range = (dmcontrol.hartsel >= max_hartsel);
    
    dmstatus.allnonexistent = !hart_exists || hart_out_of_range;
    dmstatus.anynonexistent = !hart_exists || hart_out_of_range;
    dmstatus.allunavail = false;
    dmstatus.anyunavail = false;


    dmstatus.authenticated = saved_authenticated;
    dmstatus.authbusy = saved_authbusy;
    dmstatus.version = saved_version;
}

// Reads a value from a DMI (Debug Module Interface) register by address.
// Use case: Called by JTAG DTM to read debug module registers (dmcontrol, dmstatus, abstractcs, etc.).
// Returns true on success, false for unimplemented addresses.
bool DebugModule::dmi_read(unsigned address, uint32_t *value)
{
    switch (address) {
        case DM_DMCONTROL:
            *value = read_dmcontrol();
            break;
        case DM_DMSTATUS: {
            update_dmstatus();
            *value = read_dmstatus();
            // Log DMSTATUS reads to help debug thread discovery
            // dm_log("[DM] DMSTATUS read: hartsel=0x%x, anynonexistent=%d, value=0x%08x\n", 
            //       dmcontrol.hartsel, dmstatus.anynonexistent ? 1 : 0, *value);

            // Ensure authenticated bit (bit 7) is always set - critical for OpenOCD compatibility
            if ((*value & (1U << 7)) == 0) {
                dm_log("[DM] ERROR: authenticated bit (bit 7) not set! value=0x%x\n", *value);
                *value |= (1U << 7);
            }
            break;
        }
        case DM_HARTINFO:
            // Hart info: nscratch=1, dataaccess=1, datasize=datacount, dataaddr=0x380
            *value = (1 << 20) | (1 << 19) | (datacount << 16) | 0x380;
            break;
        case DM_ABSTRACTCS:
            *value = read_abstractcs();
            break;
        case DM_COMMAND:
            *value = 0;
            break;
        case DM_ABSTRACTAUTO:
            *value = 0;
            break;
        case DM_DATA0:
            *value = read_data0();
            break;
        case 0x5:  // DATA1
            *value = data1;
            dm_log("[DM] DATA1 read: 0x%08x\n", data1);
            break;
        case 0x6:  // DATA2
            *value = data2;
            break;
        case 0x7:  // DATA3
            *value = data3;
            break;
        case DM_AUTHDATA:
            *value = read_authdata();
            break;
        case DM_SBCS:
            // System Bus Control and Status: return 0 to indicate no system bus access available
            // This is optional functionality, so returning 0 is acceptable
            *value = 0;
            break;
        default:
            *value = 0;
            dm_log("[DM] DMI READ  addr=0x%x -> 0x%x (unimplemented)\n", address, *value);
            return false;
    }

    // dm_log("[DM] DMI READ  addr=0x%x -> 0x%x\n", address, *value);
    return true;
}

// Writes a value to a DMI (Debug Module Interface) register by address.
// Use case: Called by JTAG DTM to write debug module registers (dmcontrol, command, data0, etc.).
// Returns true on success, false for unimplemented addresses.
bool DebugModule::dmi_write(unsigned address, uint32_t value)
{
    // Skip logging dmcontrol writes (address 0x10) as they're too verbose
    if (address != DM_DMCONTROL) {
        dm_log("[DM] DMI WRITE addr=0x%x data=0x%x\n", address, value);
    }

    switch (address) {
        case DM_DMCONTROL:
            return write_dmcontrol(value);
        case DM_COMMAND:
            return write_command(value);
        case DM_DATA0:
            return write_data0(value);
        case 0x5:  // DATA1
            data1 = value;
            return true;
        case 0x6:  // DATA2
            data2 = value;
            return true;
        case 0x7:  // DATA3
            data3 = value;
            dm_log("[DM] DATA3 written: 0x%08x\n", value);
            return true;
        case DM_AUTHDATA:
            return write_authdata(value);
        case DM_ABSTRACTAUTO:
            // Auto-execute not implemented in stub
            return true;
        case DM_ABSTRACTCS:
            // Clear command error if writing 1 to error bits (bits [10:8])
            if (value & (7 << 8)) {
                abstractcs.cmderr = 0;
            }
            return true;
        case DM_SBCS:
            // System Bus Control and Status: accept writes but do nothing (no system bus access)
            return true;
        default:
            dm_log("[DM] DMI WRITE addr=0x%x unimplemented\n", address);
            return false;
    }
}

// Reads the dmcontrol register, encoding all control fields into a 32-bit value.
// Use case: Returns the current debug module control state (dmactive, haltreq, resumereq, hartsel, etc.).
uint32_t DebugModule::read_dmcontrol()
{
    uint32_t result = 0;
    result = set_field_pos<uint32_t>(result, 0x1U, 0, dmcontrol.dmactive ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x1U, 1, dmcontrol.ndmreset ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x1U, 2, dmcontrol.clrresethaltreq ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x1U, 3, dmcontrol.setresethaltreq ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x1U, 29, dmcontrol.hartreset ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x1U, 28, dmcontrol.ackhavereset ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x1U, 30, dmcontrol.resumereq ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x1U, 31, dmcontrol.haltreq ? 1U : 0U);


    // Encode hartsel as split fields per Debug Spec 0.13+
    unsigned hartselhi = 0;  // Leave hartselhi as 0 for simple encoding
    unsigned hartsello = dmcontrol.hartsel;  // Put hart ID in hartsello
    result = set_field_pos<uint32_t>(result, 0x3ffU << 6, 6, hartselhi);    // Bits [15:6]
    result = set_field_pos<uint32_t>(result, 0x3ffU << 16, 16, hartsello);  // Bits [25:16]
    result = set_field_pos<uint32_t>(result, 0x1U, 26, dmcontrol.hasel ? 1U : 0U);

    
    result |= 1;

    return result;
}

// Reads the dmstatus register, encoding all status fields into a 32-bit value per RISC-V Debug Spec 0.13.2.
// Use case: Returns the current debug module status (version, authenticated, halted/running state, etc.).
// Always ensures authenticated bit (bit 7) is set - critical for OpenOCD compatibility.
uint32_t DebugModule::read_dmstatus()
{

    dmstatus.authenticated = true;
    dmstatus.authbusy = false;

    uint32_t result = 0;



    result |= (dmstatus.version & 0xf);


    if (dmstatus.confstrptrvalid) result |= (1U << 4);


    if (dmstatus.hasresethaltreq) result |= (1U << 5);





    result |= (1U << 7);


    if (dmstatus.anyhalted) result |= (1U << 8);


    if (dmstatus.allhalted) result |= (1U << 9);


    if (dmstatus.anyrunning) result |= (1U << 10);


    if (dmstatus.allrunning) result |= (1U << 11);


    if (dmstatus.anyunavail) result |= (1U << 12);


    if (dmstatus.allunavail) result |= (1U << 13);


    if (dmstatus.anynonexistent) result |= (1U << 14);


    if (dmstatus.allnonexistent) result |= (1U << 15);


    if (dmstatus.anyresumeack) result |= (1U << 16);


    if (dmstatus.allresumeack) result |= (1U << 17);


    if (dmstatus.anyhavereset) result |= (1U << 18);


    if (dmstatus.allhavereset) result |= (1U << 19);




    if (dmstatus.impebreak) result |= (1U << 22);




    if ((result & (1U << 7)) == 0) {
        dm_log("[DM] ERROR: authenticated bit (bit 7) not set! result=0x%x\n", result);
        result |= (1U << 7);
    }

    return result;
}

// Reads the abstractcs register, encoding abstract command status fields.
// Use case: Returns abstract command status (datacount, progbufsize, busy flag, cmderr).
uint32_t DebugModule::read_abstractcs()
{
    uint32_t result = 0;
    result = set_field_pos<uint32_t>(result, 0x1fU << 8, 8, abstractcs.datacount);
    result = set_field_pos<uint32_t>(result, 0xffU << 16, 16, abstractcs.progbufsize);
    result = set_field_pos<uint32_t>(result, 0x1U, 28, abstractcs.busy ? 1U : 0U);
    result = set_field_pos<uint32_t>(result, 0x7U << 8, 8, abstractcs.cmderr);
    result = set_field_pos<uint32_t>(result, 0xfU << 24, 24, 1);
    return result;
}

// Reads the DATA0 register value (used for abstract command data transfer).
// Use case: Returns the value stored in DATA0, typically used to read register/memory values.
uint32_t DebugModule::read_data0()
{
    uint32_t value = dmdata[0];
    dm_log("[DM] DATA0 read: 0x%08x\n", value);
    return value;
}

// Reads the authdata register (authentication not implemented in stub).
// Use case: Returns 0 since authentication protocol is not implemented.
uint32_t DebugModule::read_authdata()
{
    return 0;
}

// Writes the dmcontrol register, updating control fields and processing requests (halt/resume).
// Use case: Called to control the debug module (halt/resume hart, select hart, reset, etc.).
// Processes haltreq and resumereq immediately, and resets module if dmactive transitions from 0 to 1.
bool DebugModule::write_dmcontrol(uint32_t value)
{
    // If setting dmactive from 0 to 1, reset the module
    if (!dmcontrol.dmactive && (value & 1)) {
        reset();
    }


    dmcontrol.dmactive = (value & 0x1) != 0;
    dmcontrol.ndmreset = (value & (0x1 << 1)) != 0;
    dmcontrol.clrresethaltreq = (value & (0x1 << 2)) != 0;
    dmcontrol.setresethaltreq = (value & (0x1 << 3)) != 0;
    dmcontrol.hartreset = (value & (0x1 << 29)) != 0;  // Bit 29 in Debug Spec 0.13
    dmcontrol.ackhavereset = (value & (0x1 << 28)) != 0;
    dmcontrol.resumereq = (value & (0x1 << 30)) != 0;
    dmcontrol.haltreq = (value & (0x1 << 31)) != 0;

    // hartselhi is bits [15:6], but we don't use it for single-hart systems
    unsigned hartselhi = (value >> 6) & 0x3ff;   // Bits [15:6]
    unsigned hartsello = (value >> 16) & 0x3ff;  // Bits [25:16]
    
    // Combine both fields for full 20-bit hartsel: (hartselhi << 10) | hartsello
    dmcontrol.hartsel = (hartselhi << 10) | hartsello;
    dmcontrol.hasel = (value & (0x1 << 26)) != 0;

    // Always keep dmactive set for stub (always active)
    dmcontrol.dmactive = true;

    // Handle halt request immediately (cause = 3 per spec)
    if (dmcontrol.haltreq) {
        halt_hart(3);
        dmcontrol.haltreq = false;
    }

    if (dmcontrol.resumereq && !resumereq_prev) {
        resumeack_ = false;
        resume_hart(false);
    }

    resumereq_prev = dmcontrol.resumereq;

    if (dmcontrol.ackhavereset) {
        havereset_ = false;
    }

    update_dmstatus();
    return true;
}

// Writes the abstract command register and executes the command if not busy.
// Use case: Called to execute abstract commands (e.g., access register, quick access).
// Returns false if busy, otherwise executes command and returns true.
bool DebugModule::write_command(uint32_t value)
{
    command = value;
    dm_log("[DM] COMMAND written: 0x%08x\n", value);


    // Execute command immediately if not busy (stub implementation)
    if (!abstractcs.busy) {
        return perform_abstract_command();
    } else {
        abstractcs.cmderr = 1;  // BUSY error
        dm_log("[DM] COMMAND error: BUSY (cmderr=1)\n");
        return false;
    }
}

// Writes the DATA0 register value (used for abstract command data transfer).
// Use case: Called to set data for abstract commands (e.g., register value to write).
bool DebugModule::write_data0(uint32_t value)
{
    dmdata[0] = value;
    return true;
}

// Writes the authdata register (authentication not implemented in stub).
// Use case: Accepts any authdata write and marks as authenticated (stub always authenticates).
bool DebugModule::write_authdata(uint32_t)
{
    dmstatus.authenticated = true;
    dmstatus.authbusy = false;
    return true;
}

// Performs the abstract command stored in the command register.
// Use case: Executes abstract commands (supports Access Register cmdtype=0 and Access Memory cmdtype=0x02).
// Returns false if busy or unsupported command type, sets cmderr accordingly.
bool DebugModule::perform_abstract_command()
{
    if (abstractcs.busy) {
        abstractcs.cmderr = 1;
        dm_log("[DM] COMMAND error: BUSY (cmderr=1)\n");
        return false;
    }


    unsigned cmdtype = (command >> 24) & 0xff;

    if (cmdtype == 0 || cmdtype == 0x02) {
        // Access Register (cmdtype=0) or Access Memory (cmdtype=0x02)
        abstractcs.busy = true;
        execute_command(command);
        abstractcs.busy = false;
        return true;
    } else {
        abstractcs.cmderr = 2;
        dm_log("[DM] COMMAND error: NOTSUP (cmderr=2), cmdtype=0x%02x\n", cmdtype);
        return false;
    }
}

// Executes an abstract command (supports Access Register and Access Memory commands).
// Use case: Processes abstract commands to read/write hart registers or memory, optionally with postexec step.
// Command format: [cmdtype][aarsize/aamsize][postexec][transfer][write][regaddr/aamaddress]
void DebugModule::execute_command(uint32_t value)
{
    uint8_t cmdtype = (value >> 24) & 0xFF;
    if (cmdtype == 0) {
        // Access Register command
        uint8_t aarsize = (value >> 20) & 0x7;
        bool postexec   = value & (1 << 18);
        bool transfer   = value & (1 << 17);
        bool write      = value & (1 << 16);
        uint16_t regaddr  = value & 0xFFFF;

        dm_log("[DM] EXECUTE COMMAND: Access Register, regaddr=0x%04x, write=%d, transfer=%d, postexec=%d, aarsize=%d\n",
               regaddr, write ? 1 : 0, transfer ? 1 : 0, postexec ? 1 : 0, aarsize);

        // Check aarsize compatibility with XLEN
        // aarsize: 2 = 32-bit, 3 = 64-bit, 4 = 128-bit
        // OpenOCD detects XLEN by trying a 64-bit read and checking if it fails
#if (XLEN == 32)
        if (aarsize == 3) {
            // 64-bit access not supported on XLEN=32
            abstractcs.cmderr = 2;  // NOT SUPPORTED
            dm_log("[DM] COMMAND error: 64-bit access (aarsize=3) not supported on XLEN=32 (cmderr=2)\n");
            return;
        }
#endif
        if (transfer) {
            if (write) {
                write_register(regaddr, data0());
            } else {
                vortex::reg_data_t reg_data = read_register(regaddr);
                if (aarsize == 3) {
                    // 64-bit read: split into DATA0 (lower) and DATA1 (upper)
                    data0() = reg_data.u32;
#if (XLEN == 64)
                    data1 = static_cast<uint32_t>(reg_data.u >> 32);
#else
                    data1 = 0;  // XLEN=32: upper 32 bits are always 0
#endif
                } else {
                    // 32-bit read: use lower 32 bits and clear DATA1
                    data0() = reg_data.u32;
                    data1 = 0;
                }
            }
        }

        if (postexec) {
            // Get PC from emulator for selected warp
            unsigned warp_id, thread_id;
            decode_hartsel(dmcontrol.hartsel, warp_id, thread_id);
            uint32_t pc = 0;
            if (emulator_ != nullptr) {
                auto& warp = emulator_->get_warp(warp_id);
                pc = static_cast<uint32_t>(warp.PC);
            }
            
            // Check for software breakpoint: if instruction at PC is EBREAK, halt
            uint32_t instruction = read_mem(pc);
            if (instruction == 0x00100073) {
                // EBREAK instruction - software breakpoint
                dm_log("[DM] Software breakpoint hit at 0x%08x (EBREAK), halting hart warp=%u\n", pc, warp_id);
                halt_hart(1);  // Cause 1 = ebreak instruction
                return;  // Don't execute the instruction
            }
            
            // Note: Step is handled by emulator, not here
            dm_log("[DM] STEP: PC=0x%08x (step handled by emulator)\n", pc);
        }
    } else if (cmdtype == 0x02) {
        // Access Memory command (per updated RISC-V Debug Spec)
        // Fields:
        // [31:24] cmdtype (0x02)
        // [23]    aamvirtual
        // [22:20] aamsize (0=8-bit, 1=16-bit, 2=32-bit, 3=64-bit)
        // [19]    aampostincrement
        // [18:17] 0
        // [16]    write (1=write, 0=read)
        // [15:14] target-specific-info
        // [13:0]  0

        uint8_t aamvirtual       = (value >> 23) & 0x1;   // currently unused
        uint8_t aamsize          = (value >> 20) & 0x7;
        bool    aampostincrement = ((value >> 19) & 0x1) != 0;
        bool    write            = ((value >> 16) & 0x1) != 0;
        (void)aamvirtual; // suppress unused warning for now

        size_t access_size = (aamsize == 0) ? 1 :
                             (aamsize == 1) ? 2 :
                             (aamsize == 2) ? 4 : 8;

        // Decide base address and remember where it came from so we can apply postincrement correctly.
        enum AddrSource {
            ADDR_NONE,
            ADDR_DATA2,
            ADDR_DATA1,
            ADDR_DATA0,
            ADDR_PREV
        } addr_src = ADDR_NONE;

        uint64_t mem_addr = 0;

        // Address translation for Access Memory commands
        // For XLEN=32: Address is in DATA1, DATA0 is data (never part of address)
        // For XLEN=64: More complex - may need to construct 64-bit address from multiple registers

#if (XLEN == 32)
        // XLEN=32: Simple address resolution
        // Priority: post-increment > DATA1 > fallback
        if (aampostincrement && access_mem_addr_valid) {
            mem_addr = access_mem_addr;
            addr_src = ADDR_PREV;
            dm_log("[DM] ADDR_TRANSLATE (32-bit): post-increment -> 0x%08lx\n", mem_addr);
        } else if (data1 != 0) {
            mem_addr = data1;
            addr_src = ADDR_DATA1;
            dm_log("[DM] ADDR_TRANSLATE (32-bit): DATA1=0x%08x -> 0x%08lx\n", data1, mem_addr);
        } else if (access_mem_addr_valid) {
            mem_addr = access_mem_addr;
            addr_src = ADDR_PREV;
            dm_log("[DM] ADDR_TRANSLATE (32-bit): previous -> 0x%08lx\n", mem_addr);
        } else {
            mem_addr = 0;
            addr_src = ADDR_NONE;
            dm_log("[DM] ADDR_TRANSLATE (32-bit): fallback -> 0x%08lx\n", mem_addr);
        }
#else
        // Determine if we're in 64-bit address space by checking PC/DPC
        // If PC >= 0x100000000, we're in 64-bit space and addresses >= 0x80000000 need translation
        // If PC < 0x100000000, we're in 32-bit space and addresses should be used as-is
        // This fixes the issue where kernels using 32-bit addresses (0x80000000) were incorrectly
        // translated to 64-bit addresses (0x180000000), causing wrong memory access.
        unsigned warp_id, thread_id;
        decode_hartsel(dmcontrol.hartsel, warp_id, thread_id);
        bool use_64bit_addr_space = false;
        if (emulator_ != nullptr) {
            auto& warp = emulator_->get_warp(warp_id);
            uint64_t pc = warp.PC;
            use_64bit_addr_space = (pc >= 0x100000000ULL);
        } else {
            // Fallback: DPC is 32-bit, so if emulator not available, assume 32-bit space (no translation)
            // This is safe because DPC < 0x100000000 by definition (it's uint32_t)
            use_64bit_addr_space = false;
        }
        
        // XLEN=64: Address resolution for 64-bit address space
        if (aampostincrement && access_mem_addr_valid) {
            mem_addr = access_mem_addr;
            addr_src = ADDR_PREV;
            dm_log("[DM] ADDR_TRANSLATE (64-bit): post-increment -> 0x%016lx\n", mem_addr);
        } else if (data2 != 0 && data2 >= 0x80000000 && use_64bit_addr_space) {
            // HACK: DATA2 >= 0x80000000 in 64-bit space -> assume upper 32 bits = 0x1
            // OpenOCD sends 0x80000xxx for address 0x180000xxx
            mem_addr = ((uint64_t)0x1 << 32) | (uint64_t)data2;
            addr_src = ADDR_DATA2;
            dm_log("[DM] ADDR_TRANSLATE (64-bit): DATA2>=0x80000000 -> 0x%016lx\n", mem_addr);
        } else if (data2 != 0) {
            // Low address (< 0x80000000) or not in 64-bit address space
            mem_addr = data2;
            addr_src = ADDR_DATA2;
            dm_log("[DM] ADDR_TRANSLATE (64-bit): DATA2 only -> 0x%016lx\n", mem_addr);
        } else {
            // Fallback: no address provided
            mem_addr = 0;
            addr_src = ADDR_NONE;
            dm_log("[DM] ADDR_TRANSLATE (64-bit): fallback -> 0x%016lx\n", mem_addr);
        }
#endif

        // Always perform one memory access per command.
            if (write) {
                // Write memory: DATA0 contains the data to write
                uint32_t write_data = data0();
                
            dm_log("[DM] Access Memory WRITE: addr=0x%016lx, data=0x%08x, size=%zu\n",
                   mem_addr, write_data, access_size);
                
                if (access_size == 1) {
                    uint32_t old_val = read_mem(mem_addr);
                    write_mem(mem_addr, (old_val & ~0xFF) | (write_data & 0xFF));
                } else if (access_size == 2) {
                // Detect compressed EBREAK (0x9002) being written - save original instruction
                if ((write_data & 0xFFFF) == 0x9002 && !has_breakpoint(mem_addr)) {
                    add_breakpoint(mem_addr);
                } else if ((write_data & 0xFFFF) != 0x9002 && has_breakpoint(mem_addr)) {
                    // Non-EBREAK written to breakpoint address - breakpoint is being removed
                    remove_breakpoint(mem_addr);
                }
                    uint32_t old_val = read_mem(mem_addr);
                    write_mem(mem_addr, (old_val & ~0xFFFF) | (write_data & 0xFFFF));
                } else if (access_size == 4) {
                // Detect EBREAK instruction (32-bit: 0x00100073 or compressed: 0x00009002) being written
                bool is_ebreak = (write_data == 0x00100073) || ((write_data & 0xFFFF) == 0x9002);
                if (is_ebreak && !has_breakpoint(mem_addr)) {
                    add_breakpoint(mem_addr);
                } else if (!is_ebreak && has_breakpoint(mem_addr)) {
                    // Non-EBREAK written to breakpoint address - breakpoint is being removed
                    remove_breakpoint(mem_addr);
                }
                    write_mem(mem_addr, write_data);
                } else {
                    dm_log("[DM] Access Memory: unsupported write size %zu\n", access_size);
                }
        } else {
            if (access_size == 1) {
                uint32_t read_val = read_mem(mem_addr);
                data0() = read_val & 0xFF;
            } else if (access_size == 2) {
                uint32_t read_val = read_mem(mem_addr);
                data0() = read_val & 0xFFFF;
            } else if (access_size == 4) {
                uint32_t read_val = read_mem(mem_addr);
                data0() = read_val;
                } else if (access_size == 8) {
                // 64-bit read: lower 32 bits in DATA0, upper 32 bits in DATA1
                uint32_t read_val_low = read_mem(mem_addr);
                uint32_t read_val_high = read_mem(mem_addr + 4);
                data0() = read_val_low;
                data1 = read_val_high;
                dm_log("[DM] Access Memory READ 64-bit: addr=0x%016lx, low=0x%08x, high=0x%08x\n",
                       mem_addr, read_val_low, read_val_high);
            } else {
                dm_log("[DM] Access Memory: unsupported read size %zu\n", access_size);
                data0() = 0;
            }
        }
            
        // Implement aampostincrement: advance the address and write it back to the same source.
        // For reads, don't overwrite the read data - only update the address tracking.
        uint64_t new_addr = mem_addr;
        if (aampostincrement) {
            new_addr = mem_addr + access_size;
            // For reads, we've already stored the data in DATA registers, so don't overwrite them
            // Only update the address tracking for the next access
            if (!write) {
                // Don't overwrite read data - just track the new address
                access_mem_addr = new_addr;
                access_mem_addr_valid = true;
            } else {
                // For writes, update the address in the appropriate DATA register
                switch (addr_src) {
                    case ADDR_DATA2:
                        // For 64-bit addresses, store upper 32 bits in DATA2, lower in DATA1
                        if (mem_addr > UINT32_MAX) {
                            data2 = static_cast<uint32_t>(new_addr >> 32);
                            data1 = static_cast<uint32_t>(new_addr);
                        } else {
                            data2 = static_cast<uint32_t>(new_addr);
                            data1 = 0;
                        }
                        break;
                    case ADDR_DATA1:
                        // For 64-bit addresses, store upper 32 bits in DATA1, lower in DATA0
                        if (mem_addr > UINT32_MAX) {
                            data1 = static_cast<uint32_t>(new_addr >> 32);
                            data0() = static_cast<uint32_t>(new_addr);
                        } else {
                            data1 = static_cast<uint32_t>(new_addr);
                        }
                        break;
                    case ADDR_DATA0:
                        data0() = static_cast<uint32_t>(new_addr);
                        break;
                    case ADDR_NONE:
                    case ADDR_PREV:
                    default:
                        // When address came from a previous implicit address sequence,
                        // follow the spec and leave the updated address in DATA0.
                        // For 64-bit addresses, store upper 32 bits in DATA1, lower in DATA0
                        if (mem_addr > UINT32_MAX) {
                            data1 = static_cast<uint32_t>(new_addr >> 32);
                            data0() = static_cast<uint32_t>(new_addr);
                        } else {
                            data0() = static_cast<uint32_t>(new_addr);
                        }
                        break;
                }
                access_mem_addr = new_addr;
                access_mem_addr_valid = true;
            }
        } else {
            access_mem_addr = mem_addr;
            access_mem_addr_valid = true;
        }
    } else {
        abstractcs.cmderr = 2;  // NOTSUP
        dm_log("[DM] COMMAND error: NOTSUP (cmderr=2), cmdtype=0x%02x\n", cmdtype);
    }
}

// Reads a hart register by abstract register address (used by access register commands).
// Returns full register value using reg_data_t naturally.
// Use case: Called during abstract command execution to read GPRs, PC, DCSR, DPC, or CSRs.
// Register address mapping: 0x1000-0x101F (GPRs), 0x1020 (PC), 0x1021-0x1040 (FPRs f0-f31), 0x7B0 (DCSR), 0x7B1 (DPC), 0x0000-0x0FFF/0xC000-0xFFFF (CSRs).
vortex::reg_data_t DebugModule::read_register(uint16_t regaddr)
{
    dm_log("[DM] read_register: regaddr=0x%04x, dmcontrol.hartsel=0x%x (%u)\n", regaddr, dmcontrol.hartsel, dmcontrol.hartsel);
    unsigned warp_id, thread_id;
    decode_hartsel(dmcontrol.hartsel, warp_id, thread_id);
    dm_log("[DM] read_register: decoded warp_id=%u, thread_id=%u\n", warp_id, thread_id);
    vortex::reg_data_t reg_data = {};
    
    // Emulator registers (GPRs, FPRs, PC) - use reg_data_t naturally
    if (emulator_ != nullptr) {
        auto& warp = emulator_->get_warp(warp_id);
        
        // General purpose registers (x0–x31) at addresses 0x1000–0x101F
        if (regaddr >= 0x1000 && regaddr <= 0x101F) {
            int gpr_index = regaddr - 0x1000;
            reg_data.u = warp.ireg_file.at(gpr_index).at(thread_id);
            if (thread_id != 0 || warp_id != 0) {
                dm_log("[DM] READ REG  x%d (0x%04x) warp=%u thread=%u -> 0x%016lx\n", gpr_index, regaddr, warp_id, thread_id, reg_data.u);
            }
            return reg_data;
        }
        
        // PC register (0x1020)
        if (regaddr == 0x1020) {
            reg_data.u = warp.PC;
            dm_log("[DM] READ REG  pc (0x1020) warp=%u -> 0x%016lx\n", warp_id, reg_data.u);
            return reg_data;
        }
        
        // Floating point registers (f0–f31) at addresses 0x1021–0x1040 (RISC-V Debug Spec)
        if (regaddr >= 0x1021 && regaddr <= 0x1040) {
            int fpr_index = regaddr - 0x1020; // Set w/ empirical testing, not sure why this works
            reg_data.u64 = warp.freg_file.at(fpr_index).at(thread_id);
            dm_log("[DM] READ REG  f%d (0x%04x) warp=%u thread=%u -> 0x%016lx\n", fpr_index, regaddr, warp_id, thread_id, reg_data.u64);
            return reg_data;
        }
        
        // DPC (0x7B1): return actual PC for 64-bit reconstruction
        if (regaddr == 0x07b1 || regaddr == 0x7B1) {
            reg_data.u = warp.PC;  // Return actual PC, not dpc_ (which is 32-bit)
            return reg_data;
        }
    }
    
    // DCSR (0x7B0): 32-bit register
    if (regaddr == 0x07b0 || regaddr == 0x7B0) {
        reg_data.u32 = dcsr_.to_u32();
        if (thread_id != 0) {
            dm_log("[DM] READ REG  dcsr (0x7B0) thread=%u -> 0x%08x\n", thread_id, reg_data.u32);
        }
        return reg_data;
    }
    
    // DPC (0x7B1): 32-bit register (handled above if emulator available)
    if (regaddr == 0x07b1 || regaddr == 0x7B1) {
        reg_data.u32 = dpc_;
        dm_log("[DM] READ REG  dpc (0x7B1) -> 0x%08x\n", reg_data.u32);
        return reg_data;
    }
    
    // CSRs: 32-bit registers
    if (regaddr <= 0x0FFF) {
        uint16_t csr_num = regaddr;
        if (csr_num == 0x0301) {
            reg_data.u32 = ((vortex::log2floor(XLEN) - 4) << 30) | MISA_STD;
            dm_log("[DM] READ REG  misa (0x0301) -> 0x%08x (RV%dIMAFD%s)\n", 
                   reg_data.u32, XLEN, (EXT_A_ENABLED ? "A" : ""));
            return reg_data;
        }
        if (csr_num == 0x0c22) {
            reg_data.u32 = 0;
            dm_log("[DM] READ REG  vlenb (0x0c22) -> 0x%08x (no vector support)\n", reg_data.u32);
            return reg_data;
        }
        dm_log("[DM] READ REG  csr[0x%03x] (0x%04x) -> 0x00000000\n", csr_num, regaddr);
        return reg_data;
    }
    
    if (regaddr >= 0xC000) {
        uint16_t csr_num = regaddr - 0xC000;
        if (csr_num == 0x0301) {
            reg_data.u32 = ((vortex::log2floor(XLEN) - 4) << 30) | MISA_STD;
            dm_log("[DM] READ REG  misa (0x%04x) -> 0x%08x (RV%dIMAFD%s)\n", 
                   regaddr, reg_data.u32, XLEN, (EXT_A_ENABLED ? "A" : ""));
            return reg_data;
        }
        if (csr_num == 0x0c22) {
            reg_data.u32 = 0;
            dm_log("[DM] READ REG  vlenb (0x%04x) -> 0x%08x (no vector support)\n", regaddr, reg_data.u32);
            return reg_data;
        }
        dm_log("[DM] READ REG  csr[0x%03x] (0x%04x) -> 0x00000000\n", csr_num, regaddr);
        return reg_data;
    }
    
    dm_log("[DM] READ REG unknown regaddr=0x%04x -> 0x00000000\n", regaddr);
    return reg_data;
}

void DebugModule::write_register(uint16_t regaddr, uint32_t val)
{
    unsigned warp_id, thread_id;
    decode_hartsel(dmcontrol.hartsel, warp_id, thread_id);
    
    if (regaddr >= 0x1000 && regaddr <= 0x101F) {
        int gpr_index = regaddr - 0x1000;
        if (gpr_index == 0) {
            dm_log("[DM] WRITE REG x0 (0x%04x) warp=%u thread=%u <- 0x%08x (ignored, x0 is read-only)\n", regaddr, warp_id, thread_id, val);
            return;
        }
        if (emulator_ != nullptr) {
            auto& warp = emulator_->get_warp(warp_id);
            vortex::reg_data_t reg_data;
            reg_data.u = static_cast<vortex::Word>(val);  // Zero-extend for XLEN=64
            warp.ireg_file.at(gpr_index).at(thread_id) = reg_data.u;
        }
        // Only log non-zero threads/warps to reduce verbosity
        if (thread_id != 0 || warp_id != 0) {
            dm_log("[DM] WRITE REG x%d (0x%04x) warp=%u thread=%u <- 0x%08x\n", gpr_index, regaddr, warp_id, thread_id, val);
        }
        return;
    }


    if (regaddr == 0x1020) {
        if (emulator_ != nullptr) {
            auto& warp = emulator_->get_warp(warp_id);
            warp.PC = val;
        }
        dm_log("[DM] WRITE REG pc (0x1020) warp=%u <- 0x%08x\n", warp_id, val);
        return;
    }

    // Floating point registers (f0–f31) at addresses 0x1021–0x1040 (RISC-V Debug Spec)
    if (regaddr >= 0x102a && regaddr <= 0x1040) {
        int fpr_index = regaddr - 0x1020; // Set w/ empirical testing, not sure why this works
        if (emulator_ != nullptr) {
            auto& warp = emulator_->get_warp(warp_id);
            // For 32-bit write, preserve upper 32 bits of existing value
            uint64_t old_value = warp.freg_file.at(fpr_index).at(thread_id);
            uint64_t new_value = (old_value & 0xFFFFFFFF00000000ULL) | static_cast<uint64_t>(val);
            warp.freg_file.at(fpr_index).at(thread_id) = new_value;
        }
        dm_log("[DM] WRITE REG f%d (0x%04x) warp=%u thread=%u <- 0x%08x\n", fpr_index, regaddr, warp_id, thread_id, val);
        return;
    }

    if (regaddr == 0x07b0 || regaddr == 0x7B0) {
        // Write DCSR (shared across all threads)
        dcsr_.from_u32(val);
        // Keep single_step_active_ in sync with dcsr_.step
        single_step_active_ = (dcsr_.step != 0);
        if (thread_id != 0 || warp_id != 0) {
            dm_log("[DM] WRITE REG dcsr (0x7B0) warp=%u thread=%u <- 0x%08x\n", warp_id, thread_id, val);
        }
        return;
    }

    if (regaddr == 0x07b1 || regaddr == 0x7B1) {
        dpc_ = val;
        dm_log("[DM] WRITE REG dpc (0x7B1) <- 0x%08x\n", val);
        return;
    }

    if (regaddr >= 0xC000) {
        dm_log("[DM] WRITE REG csr[0x%04x] (0x%04x) <- 0x%08x (ignored)\n", regaddr - 0xC000, regaddr, val);
        return;
    }

    dm_log("[DM] WRITE REG unknown regaddr=0x%04x <- 0x%08x (ignored)\n", regaddr, val);
}

uint32_t DebugModule::read_mem(uint64_t addr)
{
    // Pass 64-bit address directly - dcache_read accepts uint64_t
    uint32_t val = read_program_memory(addr);
    dm_log("[DM] READ MEM  addr=0x%016lx -> 0x%x\n", addr, val);
    return val;
}

void DebugModule::write_mem(uint64_t addr, uint32_t val)
{
    // Pass 64-bit address directly - dcache_write accepts uint64_t
    write_program_memory(addr, val);
    dm_log("[DM] WRITE MEM addr=0x%016lx <- 0x%x\n", addr, val);
}

uint32_t DebugModule::read_program_memory(uint64_t addr) const
{
    if (!emulator_) {
        return 0;
    }
    uint32_t value = 0;
    emulator_->dcache_read(&value, addr, sizeof(uint32_t));
    return value;
}

void DebugModule::write_program_memory(uint64_t addr, uint32_t value)
{
    if (!emulator_) {
        return;
    }
    emulator_->dcache_write(&value, addr, sizeof(uint32_t));
}


void DebugModule::direct_write_register(uint16_t regaddr, uint32_t value)
{
    write_register(regaddr, value);
}

bool DebugModule::read_memory_block(uint64_t addr, uint8_t* dest, size_t len) const
{
    if (addr + len > memory.size()) {
        return false;
    }
    std::memcpy(dest, memory.data() + addr, len);
    return true;
}

bool DebugModule::write_memory_block(uint64_t addr, const uint8_t* src, size_t len)
{
    if (addr + len > memory.size()) {
        return false;
    }
    std::memcpy(memory.data() + addr, src, len);
    return true;
}


// Halts the hart (CPU core) and enters debug mode with the specified cause.
// Use case: Called when debugger requests a halt or a breakpoint is hit.
// Cause values: 0=reserved, 1=ebreak, 2=trigger, 3=haltreq, 4=step, 5=resume after step, etc.
void DebugModule::halt_hart(uint8_t cause)
{
    dm_log("[DM] Halt requested - hart halted (cause=%u)\n", cause);
    // Enter debug mode: update DCSR for all threads
    // In SIMT model, all threads halt together, so set cause for all
    dcsr_.cause = cause & 0xF;
    is_halted_ = true;
    // Set halt flag so emulator will stop execution and update DPC
    set_halt_requested(true);
    update_dmstatus();
    // Log DCSR value after setting cause to verify encoding
    uint32_t dcsr_val = dcsr_.to_u32();
    uint8_t cause_field = (dcsr_val >> 8) & 0xF;
    dm_log("[DM] DCSR after halt: 0x%08x, cause field: 0x%x (should be 0x%x)\n", dcsr_val, cause_field, cause);
}

// Resumes the hart execution, optionally in single-step mode.
// Use case: Called when debugger requests resume or step execution.
// If single_step is true or hart is in step mode, executes one instruction then halts again.
void DebugModule::resume_hart(bool single_step)
{
    dm_log("[DM] Resume requested (single_step=%d)\n", single_step ? 1 : 0);
    is_halted_ = false;

    // Log current program state before resuming
    unsigned warp_id, thread_id;
    decode_hartsel(dmcontrol.hartsel, warp_id, thread_id);
    if (emulator_ != nullptr) {
        auto& warp = emulator_->get_warp(warp_id);
        uint64_t current_pc = warp.PC;
        uint32_t dpc = dpc_;
        dm_log("[DM] Resume state: PC=0x%016lx, DPC=0x%08x, halt_requested=%d\n", 
               current_pc, dpc, halt_requested_ ? 1 : 0);
        
        if (dcsr_.cause == 1) {
            uint64_t bp_addr = dpc_;
            if (bp_addr == 0) {
                bp_addr = current_pc;
            } else {
                // DPC is 32-bit, but we need the full 64-bit address
                // Use the upper 32 bits from the actual PC
                bp_addr = (current_pc & 0xFFFFFFFF00000000ULL) | bp_addr;
            }
            
        }
    }


    // Check if step flag is set (single DCSR shared across all threads)
    bool do_step = single_step || dcsr_.step;
    dm_log("[DM] Resume check: warp_id=%u thread_id=%u, single_step=%d, dcsr_.step=%d, do_step=%d\n", 
           warp_id, thread_id, single_step ? 1 : 0, dcsr_.step ? 1 : 0, do_step ? 1 : 0);
    if (do_step) {
        // Set single-step flag so emulator will execute one instruction then halt
        set_single_step_active(true);
        set_halt_requested(false);  // Clear halt to allow execution
        // No need to resume - just clearing halt_requested_ is enough
        dm_log("[DM] Single-step mode: halt_requested cleared, will execute one instruction\n");
        resumeack_ = true;
    } else {
        // Clear halt flag to allow continuous execution
        set_halt_requested(false);
        set_single_step_active(false);  // This also clears dcsr_.step
        // No need to resume - just clearing halt_requested_ is enough
        dm_log("[DM] Continuous execution resumed: halt_requested=%d, single_step_active=%d\n", 
               halt_requested_ ? 1 : 0, single_step_active_ ? 1 : 0);
        resumeack_ = true;
    }
    update_dmstatus();
}

bool DebugModule::hart_is_halted() const
{
    return is_halted_ || halt_requested_;
}

bool DebugModule::is_halt_requested() const
{
    return halt_requested_;
}

bool DebugModule::is_single_step_active() const
{
    return single_step_active_;
}

bool DebugModule::is_debug_mode_enabled() const
{
    return debug_mode_enabled_;
}

void DebugModule::set_halt_requested(bool halt)
{
    halt_requested_ = halt;
}

void DebugModule::set_single_step_active(bool step)
{
    single_step_active_ = step;
    dcsr_.step = step ? 1 : 0;  // Keep DCSR step bit in sync
}

void DebugModule::set_debug_mode_enabled(bool enabled)
{
    debug_mode_enabled_ = enabled;
}

bool DebugModule::has_breakpoint(uint64_t addr) const
{
    return software_breakpoints_.find(addr) != software_breakpoints_.end();
}

void DebugModule::add_breakpoint(uint64_t addr)
{
    if (has_breakpoint(addr)) {
        return; // Already has breakpoint
    }
    dm_log("[DM] Adding breakpoint at 0x%016lx\n", addr);
    // Read and store the original instruction (should be called before EBREAK is written)
    uint32_t original = read_program_memory(addr);
    software_breakpoints_[addr] = original;
}

void DebugModule::remove_breakpoint(uint64_t addr)
{
    auto it = software_breakpoints_.find(addr);
    if (it == software_breakpoints_.end()) {
        return; // No breakpoint at this address
    }
    dm_log("[DM] Removing breakpoint at 0x%016lx\n", addr);
    // Restore the original instruction
    write_program_memory(addr, it->second);
    software_breakpoints_.erase(it);
}

uint32_t DebugModule::get_original_instruction(uint64_t addr) const
{
    auto it = software_breakpoints_.find(addr);
    if (it != software_breakpoints_.end()) {
        return it->second;  // Return stored original instruction
    }
    return 0;  // No breakpoint at this address
}

// Notification from emulator when program completes naturally
void DebugModule::notify_program_completed(uint32_t final_pc)
{
    // Only process if we weren't already explicitly halted
    if (!is_halted_ && !halt_requested_) {
        dm_log("[DM] Program completed naturally at PC=0x%08x, halting hart\n", final_pc);
        
        // Update DPC to final PC
        direct_write_register(0x7B1, final_pc);
        
        is_halted_ = true;
        set_halt_requested(true);
        dcsr_.cause = 1;  // Use ebreak cause (generic halt)
    }
}

// Called periodically when JTAG is in Run-Test-Idle state.
// Use case: Allows the debug module to process state updates during idle periods.
// When the hart is running continuously, this simulates instruction execution by advancing PC
// and checking for breakpoints.
void DebugModule::run_test_idle()
{
    // run_test_idle is called periodically during JTAG Run-Test-Idle state
    // The emulator handles actual instruction execution via step()
    // We just need to check if we're halted or running
    // Note: We don't execute instructions here - that's handled by the emulator
    
    // Only log occasionally to avoid spam (every 1000 calls)
    static uint64_t log_counter = 0;
    if (!is_halted_ && !halt_requested_) {
        if ((log_counter++ % 1000) == 0 && emulator_ != nullptr) {
            unsigned warp_id, thread_id;
            decode_hartsel(dmcontrol.hartsel, warp_id, thread_id);
            auto& warp = emulator_->get_warp(warp_id);
            uint32_t pc = static_cast<uint32_t>(warp.PC);
            dm_log("[DM] run_test_idle: hart running, warp=%u PC=0x%08x\n", warp_id, pc);
        }
    } else {
        // Only log occasionally when halted too
        if ((log_counter++ % 1000) == 0) {
        dm_log("[DM] run_test_idle: hart is halted, nothing to do\n");
        }
    }
}

