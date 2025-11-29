#include "debug_module.h"
#include "emulator.h"
#include <cstdarg>
#include <atomic>
#include <cstring>

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

} // namespace

void DebugModule::set_verbose_logging(bool enable) {
    g_debug_module_verbose.store(enable, std::memory_order_relaxed);
    //Hart::set_verbose_logging(enable);
}

bool DebugModule::verbose_logging() {
    return g_debug_module_verbose.load(std::memory_order_relaxed);
}

DebugModule::DebugModule(size_t mem_size)
    : emulator_(nullptr),
      halted_(false),
      resumeack_(true),  // Start with resumeack true (hart is running)
      havereset_(true),  // Hart has reset at startup
      command(0),
      memory(mem_size, 0)
{
    // Initialize data registers
    for (unsigned i = 0; i < datacount; i++) {
        dmdata[i] = 0;
    }
    
    reset();
}

void DebugModule::attach_emulator(vortex::Emulator* emulator)
{
    emulator_ = emulator;
    dm_log("[DM] Emulator attached\n");
}

void DebugModule::reset()
{
    // Initialize register state
    dmcontrol = dmcontrol_t();
    dmstatus = dmstatus_t();
    abstractcs = abstractcs_t();
    
    // Reset hart (this will reset all hart fields including resumeack, havereset)
    //hart = Hart();
    
    dmcontrol.dmactive = true;
    dmstatus.authenticated = true;
    dmstatus.authbusy = false;
    dmstatus.version = 2;  // Debug Module version 0.13 (version=2 per spec)
    
    // Ensure hart appears available (not nonexistent, not unavailable)
    dmstatus.allnonexistent = false;
    dmstatus.anynonexistent = false;
    dmstatus.allunavail = false;
    dmstatus.anyunavail = false;
    
    update_dmstatus();
    
    // Double-check authenticated is still true after update
    dmstatus.authenticated = true;
    dmstatus.authbusy = false;
}

void DebugModule::update_dmstatus()
{
    bool saved_authenticated = dmstatus.authenticated;
    bool saved_authbusy = dmstatus.authbusy;
    unsigned saved_version = dmstatus.version;
    
    // Update status fields based on hart state
    if (halted_) {
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
    
    // Hart exists and is available for our stub
    dmstatus.allnonexistent = false;
    dmstatus.anynonexistent = false;
    dmstatus.allunavail = false;
    dmstatus.anyunavail = false;
    
    // RESTORE authentication state
    dmstatus.authenticated = saved_authenticated;
    dmstatus.authbusy = saved_authbusy;
    dmstatus.version = saved_version;
}

bool DebugModule::dmi_read(unsigned address, uint32_t *value)
{
    switch (address) {
        case DM_DMCONTROL:
            *value = read_dmcontrol();
            break;
        case DM_DMSTATUS:
            update_dmstatus();
            *value = read_dmstatus();
            // Verify authenticated bit is set at bit 7 (per RISC-V Debug Spec)
            if ((*value & (1U << 7)) == 0) {
                dm_log("[DM] ERROR: authenticated bit (bit 7) not set! value=0x%x\n", *value);
                *value |= (1U << 7);  // Force it
            }
            dm_log("[DM] DMSTATUS: halted=%d, running=%d, resumeack=%d, havereset=%d\n",
                   dmstatus.allhalted, dmstatus.allrunning, dmstatus.allresumeack, dmstatus.allhavereset);
            break;
        case DM_HARTINFO:
            // Hart info: nscratch=1, dataaccess=1, datasize=datacount, dataaddr=0x380
            *value = (1 << 20) | (1 << 19) | (datacount << 16) | 0x380;
            break;
        case DM_ABSTRACTCS:
            *value = read_abstractcs();
            break;
        case DM_COMMAND:
            *value = 0;  // Command register is write-only
            break;
        case DM_ABSTRACTAUTO:
            *value = 0;  // Auto-execute disabled in stub
            break;
        case DM_DATA0:
            *value = read_data0();
            break;
        case DM_AUTHDATA:
            *value = read_authdata();
            break;
        default:
            *value = 0;
            dm_log("[DM] DMI READ  addr=0x%x -> 0x%x (unimplemented)\n", address, *value);
            return false;
    }
    
    dm_log("[DM] DMI READ  addr=0x%x -> 0x%x\n", address, *value);
    return true;
}

bool DebugModule::dmi_write(unsigned address, uint32_t value)
{
    printf("[DM] DMI WRITE addr=0x%x data=0x%x\n", address, value);
    
    // For stub: always allow writes (we're always authenticated)
    // Don't block writes based on authentication state
    
    switch (address) {
        case DM_DMCONTROL:
            return write_dmcontrol(value);
        case DM_COMMAND:
            return write_command(value);
        case DM_DATA0:
            return write_data0(value);
        case DM_AUTHDATA:
            return write_authdata(value);
        case DM_ABSTRACTAUTO:
            // Auto-execute not implemented in stub
            return true;
        case DM_ABSTRACTCS:
            // Clear command error if writing 1 to error bits
            if (value & (7 << 8)) {
                abstractcs.cmderr = 0;
            }
            return true;
        default:
            dm_log("[DM] DMI WRITE addr=0x%x unimplemented\n", address);
            return false;
    }
}

uint32_t DebugModule::read_dmcontrol()
{
    uint32_t result = 0;
    
    // Build DMCONTROL using direct bit manipulation (clearer and correct)
    if (dmcontrol.dmactive)       result |= (1U << 0);
    if (dmcontrol.ndmreset)       result |= (1U << 1);
    if (dmcontrol.clrresethaltreq) result |= (1U << 2);
    if (dmcontrol.setresethaltreq) result |= (1U << 3);
    // hartsel bits [9:6] and [25:16]
    result |= ((dmcontrol.hartsel & 0xF) << 6);      // hartsello [9:6]
    result |= ((dmcontrol.hartsel >> 4) << 16);       // hartselhi [25:16]
    if (dmcontrol.hartreset)      result |= (1U << 16);
    if (dmcontrol.hasel)          result |= (1U << 26);
    if (dmcontrol.ackhavereset)   result |= (1U << 28);
    if (dmcontrol.resumereq)      result |= (1U << 30);
    if (dmcontrol.haltreq)        result |= (1U << 31);
    
    printf("[DM] read_dmcontrol: dmactive=%d, result=0x%x\n", dmcontrol.dmactive, result);
    return result;
}

uint32_t DebugModule::read_dmstatus()
{
    // ALWAYS ensure authenticated is true for stub
    dmstatus.authenticated = true;
    dmstatus.authbusy = false;
    
    uint32_t result = 0;
    
    // Build dmstatus register value according to RISC-V Debug Spec 0.13.2
    // Bits [3:0] = version (2 = Debug Module 0.13)
    result |= (dmstatus.version & 0xf);
    
    // Bit 4 = confstrptrvalid
    if (dmstatus.confstrptrvalid) result |= (1U << 4);
    
    // Bit 5 = hasresethaltreq
    if (dmstatus.hasresethaltreq) result |= (1U << 5);
    
    // Bit 6 = authbusy (always 0 for stub)
    // Don't set it
    
    // Bit 7 = authenticated (CRITICAL for OpenOCD!) - FORCE IT TO 1
    result |= (1U << 7);  // Always set authenticated bit at bit 7
    
    // Bit 8 = anyhalted
    if (dmstatus.anyhalted) result |= (1U << 8);
    
    // Bit 9 = allhalted
    if (dmstatus.allhalted) result |= (1U << 9);
    
    // Bit 10 = anyrunning
    if (dmstatus.anyrunning) result |= (1U << 10);
    
    // Bit 11 = allrunning
    if (dmstatus.allrunning) result |= (1U << 11);
    
    // Bit 12 = anyunavail
    if (dmstatus.anyunavail) result |= (1U << 12);
    
    // Bit 13 = allunavail
    if (dmstatus.allunavail) result |= (1U << 13);
    
    // Bit 14 = anynonexistent
    if (dmstatus.anynonexistent) result |= (1U << 14);
    
    // Bit 15 = allnonexistent
    if (dmstatus.allnonexistent) result |= (1U << 15);
    
    // Bit 16 = anyresumeack
    if (dmstatus.anyresumeack) result |= (1U << 16);
    
    // Bit 17 = allresumeack
    if (dmstatus.allresumeack) result |= (1U << 17);
    
    // Bit 18 = anyhavereset
    if (dmstatus.anyhavereset) result |= (1U << 18);
    
    // Bit 19 = allhavereset
    if (dmstatus.allhavereset) result |= (1U << 19);
    
    // Bits [21:20] = reserved (0) - don't set
    
    // Bit 22 = impebreak
    if (dmstatus.impebreak) result |= (1U << 22);
    
    // Bits [31:23] = reserved (0) - don't set
    
    // Verify authenticated bit is set at bit 7
    if ((result & (1U << 7)) == 0) {
        dm_log("[DM] ERROR: authenticated bit (bit 7) not set! result=0x%x\n", result);
        result |= (1U << 7);  // Force it
    }
    
    return result;
}

uint32_t DebugModule::read_abstractcs()
{
    uint32_t result = 0;
    // ABSTRACTCS register layout:
    // [3:0]   datacount (number of data registers, read-only)
    // [7:4]   reserved
    // [10:8]  cmderr (command error)
    // [11]    reserved
    // [12]    busy
    // [23:13] reserved
    // [28:24] progbufsize (program buffer size, read-only)
    // [31:29] reserved
    result |= (abstractcs.datacount & 0xF);           // [3:0]
    result |= ((abstractcs.cmderr & 0x7) << 8);       // [10:8]
    if (abstractcs.busy) result |= (1U << 12);        // [12]
    result |= ((abstractcs.progbufsize & 0x1F) << 24); // [28:24]
    return result;
}

uint32_t DebugModule::read_data0()
{
    uint32_t value = dmdata[0];
    dm_log("[DM] DATA0 read: 0x%08x\n", value);
    return value;
}

uint32_t DebugModule::read_authdata()
{
    // Stub: return 0 (not implementing authentication protocol)
    return 0;
}

bool DebugModule::write_dmcontrol(uint32_t value)
{
    printf("[DM] write_dmcontrol: value=0x%x, current dmactive=%d\n", value, dmcontrol.dmactive);
    
    // If setting dmactive from 0 to 1, reset the module
    if (!dmcontrol.dmactive && (value & 1)) {
        printf("[DM] write_dmcontrol: transitioning dmactive 0->1, calling reset()\n");
        reset();
        printf("[DM] write_dmcontrol: after reset(), dmactive=%d\n", dmcontrol.dmactive);
    }
    
    // Extract fields
    dmcontrol.dmactive = (value & 0x1) != 0;
    printf("[DM] write_dmcontrol: after extraction, dmactive=%d\n", dmcontrol.dmactive);
    dmcontrol.ndmreset = (value & (0x1 << 1)) != 0;
    dmcontrol.clrresethaltreq = (value & (0x1 << 2)) != 0;
    dmcontrol.setresethaltreq = (value & (0x1 << 3)) != 0;
    dmcontrol.hartreset = (value & (0x1 << 16)) != 0;
    dmcontrol.ackhavereset = (value & (0x1 << 28)) != 0;
    dmcontrol.resumereq = (value & (0x1 << 30)) != 0;
    dmcontrol.haltreq = (value & (0x1 << 31)) != 0;
    
    // Extract hartsel (simplified)
    dmcontrol.hartsel = (value >> 6) & 0x3ff;
    dmcontrol.hasel = (value & (0x1 << 26)) != 0;
    
    // Don't force dmactive - let OpenOCD control it for proper reset handshake
    // dmactive is already extracted from value above
    
    // Handle halt request
    if (dmcontrol.haltreq) {
        halted_ = true;
        resumeack_ = false;
        printf("[DM] Halt requested - hart halted\n");
    }
    
    // Handle resume request
    if (dmcontrol.resumereq) {
        halted_ = false;
        resumeack_ = true;
        printf("[DM] Resume requested - hart running\n");
    }
    
    // Handle ackhavereset
    if (dmcontrol.ackhavereset) {
        havereset_ = false;
    }
    
    update_dmstatus();
    return true;
}

bool DebugModule::write_command(uint32_t value)
{
    command = value;
    dm_log("[DM] COMMAND written: 0x%08x\n", value);
    
    // For stub, execute command immediately if not busy
    if (!abstractcs.busy) {
        return perform_abstract_command();
    } else {
        abstractcs.cmderr = 1;  // BUSY error
        dm_log("[DM] COMMAND error: BUSY (cmderr=1)\n");
        return false;
    }
}

bool DebugModule::write_data0(uint32_t value)
{
    dmdata[0] = value;
    dm_log("[DM] DATA0 written: 0x%08x\n", value);
    return true;
}

bool DebugModule::write_authdata(uint32_t)
{
    // Stub: accept any authdata write and mark as authenticated
    dmstatus.authenticated = true;
    dmstatus.authbusy = false;
    return true;
}

bool DebugModule::perform_abstract_command()
{
    if (abstractcs.busy) {
        abstractcs.cmderr = 1;  // BUSY
        dm_log("[DM] COMMAND error: BUSY (cmderr=1)\n");
        return false;
    }
    
    // Extract command type (bits [31:24])
    unsigned cmdtype = (command >> 24) & 0xff;
    
    if (cmdtype == 0) {
        // Access register command
        abstractcs.busy = true;
        execute_command(command);
        abstractcs.busy = false;
        return true;
    } else {
        // Unsupported command type
        abstractcs.cmderr = 2;  // NOTSUP
        dm_log("[DM] COMMAND error: NOTSUP (cmderr=2), cmdtype=0x%02x\n", cmdtype);
        return false;
    }
}

void DebugModule::execute_command(uint32_t value)
{
    uint8_t cmdtype = (value >> 24) & 0xFF;
    if (cmdtype == 0) { // Access Register
        uint8_t aarsize = (value >> 20) & 0x7;
        bool postexec   = value & (1 << 18);
        bool transfer   = value & (1 << 17);
        bool write      = value & (1 << 16);
        uint16_t regaddr  = value & 0xFFFF;

        dm_log("[DM] EXECUTE COMMAND: Access Register, regaddr=0x%04x, write=%d, transfer=%d, postexec=%d, aarsize=%d\n",
               regaddr, write ? 1 : 0, transfer ? 1 : 0, postexec ? 1 : 0, aarsize);

        if (transfer) {
            if (write) {
                write_register(regaddr, data0());
            } else {
                data0() = read_register(regaddr);
            }
        }
        
        if (postexec) {
            // TODO: Reimplement with Vortex emulator
            //hart.step();
            //dm_log("[DM] STEP: PC incremented by 4, new PC=0x%08x\n", hart.get_pc());
            dm_log("[DM] STEP: postexec not yet implemented\n");
        }
    } else {
        abstractcs.cmderr = 1;
        dm_log("[DM] EXECUTE COMMAND error: unsupported cmdtype=0x%02x (cmderr=1)\n", cmdtype);
    }
}

uint32_t DebugModule::read_register(uint16_t regaddr)
{
    // For now, use wid=0, tid=0 (first warp, first thread)
    uint32_t wid = 0;
    uint32_t tid = 0;

    // General purpose registers (x0–x31) at addresses 0x1000–0x101F
    if (regaddr >= 0x1000 && regaddr <= 0x101F) {
        int gpr_index = regaddr - 0x1000;
        vortex::Word value = 0;
        if (emulator_) {
            emulator_->debug_read_register(wid, tid, gpr_index, &value, false);
            dm_log("[DM] READ REG  x%d (0x%04x) -> 0x%08x (via emulator)\n", gpr_index, regaddr, (uint32_t)value);
        } else {
            dm_log("[DM] READ REG  x%d (0x%04x) -> 0x%08x (no emulator)\n", gpr_index, regaddr, (uint32_t)value);
        }
        return static_cast<uint32_t>(value);
    }

    // Floating point registers (f0–f31) at addresses 0x1020–0x103F
    if (regaddr >= 0x1020 && regaddr <= 0x103F) {
        int fpr_index = regaddr - 0x1020;
        vortex::Word value = 0;
        if (emulator_) {
            emulator_->debug_read_register(wid, tid, fpr_index, &value, true);
            dm_log("[DM] READ REG  f%d (0x%04x) -> 0x%016llx (via emulator)\n", fpr_index, regaddr, (unsigned long long)value);
        } else {
            dm_log("[DM] READ REG  f%d (0x%04x) -> 0x%016llx (no emulator)\n", fpr_index, regaddr, (unsigned long long)value);
        }
        return static_cast<uint32_t>(value);
    }

    // Debug Control and Status Register (DCSR) at 0x7B0
    if (regaddr == 0x07b0 || regaddr == 0x7B0) {
        // Return default DCSR value for debug mode
        uint32_t value = (4 << 28) | (3 << 0);  // xdebugver=4, prv=3
        dm_log("[DM] READ REG  dcsr (0x7B0) -> 0x%08x (default)\n", value);
        return value;
    }

    // Debug Program Counter (DPC) at 0x7B1
    if (regaddr == 0x07b1 || regaddr == 0x7B1) {
        dm_log("[DM] READ REG  dpc (0x7B1) -> 0x%08x (stub)\n", 0);
        return 0;
    }

    // CSRs (0x0000–0x0FFF)
    if (regaddr >= 0x0000 && regaddr <= 0x0FFF) {
        dm_log("[DM] READ REG  csr[0x%03x] -> 0x%08x (stub)\n", regaddr, 0);
        return 0;
    }

    dm_log("[DM] READ REG unknown regaddr=0x%04x -> 0xdeadbeef\n", regaddr);
    return 0xDEADBEEF;
}

void DebugModule::write_register(uint16_t regaddr, uint32_t val)
{
    // For now, use wid=0, tid=0 (first warp, first thread)
    uint32_t wid = 0;
    uint32_t tid = 0;

    // General purpose registers (x0–x31) at addresses 0x1000–0x101F
    if (regaddr >= 0x1000 && regaddr <= 0x101F) {
        int gpr_index = regaddr - 0x1000;
        if (gpr_index == 0) {
            dm_log("[DM] WRITE REG x0 (0x%04x) <- 0x%08x (ignored, x0 is read-only)\n", regaddr, val);
            return;
        }
        if (emulator_) {
            emulator_->debug_write_register(wid, tid, gpr_index, val, false);
            dm_log("[DM] WRITE REG x%d (0x%04x) <- 0x%08x (via emulator)\n", gpr_index, regaddr, val);
        } else {
            dm_log("[DM] WRITE REG x%d (0x%04x) <- 0x%08x (no emulator)\n", gpr_index, regaddr, val);
        }
        return;
    }

    // Floating point registers (f0–f31) at addresses 0x1020–0x103F
    if (regaddr >= 0x1020 && regaddr <= 0x103F) {
        int fpr_index = regaddr - 0x1020;
        if (emulator_) {
            emulator_->debug_write_register(wid, tid, fpr_index, val, true);
            dm_log("[DM] WRITE REG f%d (0x%04x) <- 0x%08x (via emulator)\n", fpr_index, regaddr, val);
        } else {
            dm_log("[DM] WRITE REG f%d (0x%04x) <- 0x%08x (no emulator)\n", fpr_index, regaddr, val);
        }
        return;
    }

    // Debug Control and Status Register (DCSR) at 0x7B0
    if (regaddr == 0x07b0 || regaddr == 0x7B0) {
        dm_log("[DM] WRITE REG dcsr (0x7B0) <- 0x%08x (stub)\n", val);
        return;
    }

    // Debug Program Counter (DPC) at 0x7B1
    if (regaddr == 0x07b1 || regaddr == 0x7B1) {
        dm_log("[DM] WRITE REG dpc (0x7B1) <- 0x%08x (stub)\n", val);
        return;
    }

    // CSRs (0x0000–0x0FFF)
    if (regaddr >= 0x0000 && regaddr <= 0x0FFF) {
        dm_log("[DM] WRITE REG csr[0x%03x] <- 0x%08x (stub)\n", regaddr, val);
        return;
    }

    dm_log("[DM] WRITE REG unknown regaddr=0x%04x <- 0x%08x (ignored)\n", regaddr, val);
}

uint32_t DebugModule::read_mem(uint64_t addr)
{
    if (addr + 4 > memory.size()) {
        dm_log("[DM] READ MEM  addr=0x%llx -> OUT OF BOUNDS\n", (unsigned long long)addr);
        return 0;
    }
    uint32_t val = *(uint32_t*)&memory[addr];
    dm_log("[DM] READ MEM  addr=0x%llx -> 0x%x\n", (unsigned long long)addr, val);
    return val;
}

void DebugModule::write_mem(uint64_t addr, uint32_t val)
{
    if (addr + 4 > memory.size()) {
        dm_log("[DM] WRITE MEM addr=0x%llx <- 0x%x OUT OF BOUNDS\n", (unsigned long long)addr, val);
        return;
    }
    *(uint32_t*)&memory[addr] = val;
    dm_log("[DM] WRITE MEM addr=0x%llx <- 0x%x\n", (unsigned long long)addr, val);
}

uint32_t DebugModule::direct_read_register(uint16_t regaddr)
{
    return read_register(regaddr);
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

void DebugModule::halt_hart(uint8_t cause)
{
    dm_log("[DM] Halt requested (cause=%u): halted_=%d->1, resumeack_=%d->0\n", 
           cause, halted_, resumeack_);
    // TODO: Actually halt the emulator execution
    halted_ = true;
    resumeack_ = false;  // Clear resumeack when halted
    update_dmstatus();
}

void DebugModule::resume_hart(bool single_step)
{
    dm_log("[DM] Resume requested (single_step=%d)\n", single_step ? 1 : 0);
    // TODO: Actually resume emulator execution
    halted_ = false;
    resumeack_ = true;  // Acknowledge the resume
    
    if (single_step) {
        // TODO: Step one instruction then halt again
        dm_log("[DM] Single-step: will halt after one instruction (not yet implemented)\n");
    }
    update_dmstatus();
}

bool DebugModule::hart_is_halted() const
{
    return halted_;
}

void DebugModule::run_test_idle()
{
    // Called periodically when JTAG is in Run-Test-Idle
    // For stub, this is mostly a no-op, but could be used for state machine updates
}
