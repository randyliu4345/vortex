#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <set>
#include "warp.h"

namespace vortex {
    class Emulator;
}


#define DM_DATA0           0x04
#define DM_DMCONTROL       0x10
#define DM_DMSTATUS        0x11
#define DM_HARTINFO        0x12
#define DM_ABSTRACTCS      0x16
#define DM_COMMAND         0x17
#define DM_ABSTRACTAUTO    0x18
#define DM_DMCONTROL2      0x1a
#define DM_AUTHDATA        0x30
#define DM_SBCS            0x38
#define DM_SBADDRESS0      0x39
#define DM_SBDATA0         0x3c


template<typename T>
static inline T set_field(T reg, T mask, T val) {
    return (reg & ~mask) | (val & mask);
}

template<typename T>
static inline T get_field(T reg, T mask) {
    return (reg & mask);
}

template<typename T>
static inline T set_field_pos(T reg, T mask, unsigned pos, unsigned val) {
    return set_field(reg, mask, static_cast<T>(val) << pos);
}

template<typename T>
static inline T get_field_pos(T reg, T mask, unsigned pos) {
    return (reg & mask) >> pos;
}


struct dmcontrol_t {
    bool dmactive;
    bool ndmreset;
    bool clrresethaltreq;
    bool setresethaltreq;
    bool hartreset;
    bool ackhavereset;
    bool resumereq;
    bool haltreq;
    unsigned hartsel;
    bool hasel;

    dmcontrol_t() : dmactive(true), ndmreset(false), clrresethaltreq(false),
                    setresethaltreq(false), hartreset(false), ackhavereset(false),
                    resumereq(false), haltreq(false), hartsel(0), hasel(false) {}
};


struct dmstatus_t {
    unsigned version;
    bool confstrptrvalid;
    bool hasresethaltreq;
    bool authbusy;
    bool authenticated;
    bool anyhalted;
    bool allhalted;
    bool anyrunning;
    bool allrunning;
    bool anyunavail;
    bool allunavail;
    bool anynonexistent;
    bool allnonexistent;
    bool anyresumeack;
    bool allresumeack;
    bool anyhavereset;
    bool allhavereset;

    bool impebreak;


    dmstatus_t() : version(2), confstrptrvalid(false), hasresethaltreq(false),
                   authbusy(false), authenticated(true),
                   anyhalted(false), allhalted(false),
                   anyrunning(false), allrunning(false),
                   anyunavail(false), allunavail(false),
                   anynonexistent(false), allnonexistent(false),
                   anyresumeack(false), allresumeack(true),
                   anyhavereset(false), allhavereset(false),
                   impebreak(false) {}
};


struct abstractcs_t {
    unsigned datacount;
    unsigned progbufsize;
    bool busy;
    unsigned cmderr;

    abstractcs_t() : datacount(1), progbufsize(0), busy(false), cmderr(0) {}
};

class DebugModule {
public:
    // Constructor: Initializes the RISC-V Debug Module with a simulated memory space.
    // Use case: Creates a debug module instance that implements the RISC-V Debug Specification 0.13.
    DebugModule(vortex::Emulator* emulator = nullptr, size_t mem_size = 4096);

    // Reads a value from a DMI (Debug Module Interface) register by address.
    // Use case: Called by JTAG DTM to read debug module registers (dmcontrol, dmstatus, abstractcs, etc.).
    bool dmi_read(unsigned address, uint32_t *value);
    
    // Writes a value to a DMI (Debug Module Interface) register by address.
    // Use case: Called by JTAG DTM to write debug module registers (dmcontrol, command, data0, etc.).
    bool dmi_write(unsigned address, uint32_t value);


    static void set_verbose_logging(bool enable);
    static bool verbose_logging();


    uint32_t direct_read_register(uint16_t regaddr);
    void direct_write_register(uint16_t regaddr, uint32_t value);
    bool read_memory_block(uint64_t addr, uint8_t* dest, size_t len) const;
    bool write_memory_block(uint64_t addr, const uint8_t* src, size_t len);
    // Halts the warp (SIMD thread group) and enters debug mode with the specified cause.
    // Use case: Called when debugger requests a halt or a breakpoint is hit.
    void halt_hart(uint8_t cause);
    
    // Resumes the warp execution, optionally in single-step mode.
    // Use case: Called when debugger requests resume or step execution.
    void resume_hart(bool single_step);
    
    // Returns true if the warp is currently halted.
    // Use case: Used to check warp state for status reporting.
    bool hart_is_halted() const;

    // Called periodically when JTAG is in Run-Test-Idle state.
    // Use case: Allows the debug module to process state updates during idle periods.
    void run_test_idle();


private:

    dmcontrol_t dmcontrol;
    dmstatus_t dmstatus;
    abstractcs_t abstractcs;

    vortex::Emulator* emulator_;
    Warp warp;



    static constexpr unsigned datacount = 1;
    uint32_t dmdata[datacount];
    uint32_t data1;  // DATA1 register (address 0x5)
    uint32_t data2;  // DATA2 register (address 0x6)
    uint32_t data3;  // DATA3 register (address 0x7)

    uint32_t& data0() { return dmdata[0]; }


    static constexpr unsigned progbufsize = 0;


    uint32_t command;


    bool resumereq_prev;


    std::vector<uint8_t> memory;
    
    // Temporary storage for Access Memory command address
    // OpenOCD sets address in DATA0, then data, then executes command
    uint32_t access_mem_addr;
    bool access_mem_addr_valid;


    void reset();
    void update_dmstatus();


    bool perform_abstract_command();
    void execute_command(uint32_t cmd);


    uint32_t read_register(uint16_t regaddr);
    void write_register(uint16_t regaddr, uint32_t val);


    uint32_t read_mem(uint64_t addr);
    void write_mem(uint64_t addr, uint32_t val);


    uint32_t read_dmcontrol();
    uint32_t read_dmstatus();
    uint32_t read_abstractcs();
    uint32_t read_data0();
    uint32_t read_authdata();

    bool write_dmcontrol(uint32_t value);
    bool write_command(uint32_t value);
    bool write_data0(uint32_t value);
    bool write_authdata(uint32_t value);
};
