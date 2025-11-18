#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

// DMI Register Addresses (RISC-V Debug Spec 0.13)
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

// Field manipulation helpers (similar to Spike's set_field/get_field)
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

// DM Control register fields
struct dmcontrol_t {
    bool dmactive;      // bit 0: Debug Module active
    bool ndmreset;      // bit 1: Non-debug-module reset
    bool clrresethaltreq; // bit 2: Clear reset halt request
    bool setresethaltreq; // bit 3: Set reset halt request
    bool hartreset;     // bit 16: Hart reset request
    bool ackhavereset;  // bit 28: Acknowledge have reset
    bool resumereq;     // bit 30: Resume request
    bool haltreq;       // bit 31: Halt request
    unsigned hartsel;   // bits [25:16] for hartselhi, [9:6] for hartsello
    bool hasel;         // bit 26: Hart array select

    dmcontrol_t() : dmactive(true), ndmreset(false), clrresethaltreq(false),
                    setresethaltreq(false), hartreset(false), ackhavereset(false),
                    resumereq(false), haltreq(false), hartsel(0), hasel(false) {}
};

// DM Status register fields (per RISC-V Debug Spec 0.13.2)
struct dmstatus_t {
    unsigned version;        // bits [3:0]: Version (2 = Debug Module 0.13)
    bool confstrptrvalid;    // bit 4: Configuration string pointer valid
    bool hasresethaltreq;    // bit 5: Has reset halt request
    bool authbusy;           // bit 6: Authentication busy
    bool authenticated;      // bit 7: Authenticated (CRITICAL!)
    bool anyhalted;          // bit 8: Any selected hart halted
    bool allhalted;          // bit 9: All selected harts halted
    bool anyrunning;         // bit 10: Any selected hart running
    bool allrunning;         // bit 11: All selected harts running
    bool anyunavail;         // bit 12: Any selected hart unavailable
    bool allunavail;         // bit 13: All selected harts unavailable
    bool anynonexistent;     // bit 14: Any selected hart doesn't exist
    bool allnonexistent;     // bit 15: All selected harts don't exist
    bool anyresumeack;       // bit 16: Any selected hart acknowledges resume
    bool allresumeack;       // bit 17: All selected harts acknowledge resume
    bool anyhavereset;       // bit 18: Any selected hart has reset
    bool allhavereset;       // bit 19: All selected harts have reset
    // bits [21:20] = reserved (0)
    bool impebreak;          // bit 22: Implicit ebreak supported
    // bits [31:23] = reserved (0)

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

// Abstract Command and Status register fields
struct abstractcs_t {
    unsigned datacount;      // bits [12:8]: Number of data registers
    unsigned progbufsize;    // bits [23:16]: Program buffer size
    bool busy;               // bit 28: Abstract command busy
    unsigned cmderr;         // bits [10:8]: Command error (0 = none)

    abstractcs_t() : datacount(1), progbufsize(0), busy(false), cmderr(0) {}
};

class DebugModule {
public:
    DebugModule(size_t mem_size = 4096);
    
    // DMI interface (used by JTAG DTM)
    bool dmi_read(unsigned address, uint32_t *value);
    bool dmi_write(unsigned address, uint32_t value);

    // Logging control
    static void set_verbose_logging(bool enable);
    static bool verbose_logging();

    // Direct access helpers (used by GDB server)
    uint32_t direct_read_register(uint16_t regaddr);
    void direct_write_register(uint16_t regaddr, uint32_t value);
    bool read_memory_block(uint64_t addr, uint8_t* dest, size_t len) const;
    bool write_memory_block(uint64_t addr, const uint8_t* src, size_t len);
    void halt_hart(uint8_t cause);
    void resume_hart(bool single_step);
    bool hart_is_halted() const;

    // Called periodically when JTAG is in Run-Test-Idle
    void run_test_idle();

private:
    // Register state
    dmcontrol_t dmcontrol;
    dmstatus_t dmstatus;
    abstractcs_t abstractcs;
    
    // Hart (CPU core) - assuming 1 hart for now
    //Hart hart;
    
    // Data registers (for abstract commands)
    // DATA0 is stored in dmdata[0]
    static constexpr unsigned datacount = 1;
    uint32_t dmdata[datacount];
    
    // Helper to get DATA0 reference
    uint32_t& data0() { return dmdata[0]; }
    
    // Program buffer (not used in stub, but kept for structure)
    static constexpr unsigned progbufsize = 0;
    
    // Abstract command
    uint32_t command;
    
    // Simulated memory (for fake data reads/writes)
    std::vector<uint8_t> memory;
    
    // Authentication: stub always authenticates, no challenge needed
    
    // Helper functions
    void reset();
    void update_dmstatus();
    
    // Abstract command execution
    bool perform_abstract_command();
    void execute_command(uint32_t cmd);
    
    // Register access (for abstract commands)
    uint32_t read_register(uint16_t regaddr);
    void write_register(uint16_t regaddr, uint32_t val);
    
    // Memory access (simulated)
    uint32_t read_mem(uint64_t addr);
    void write_mem(uint64_t addr, uint32_t val);
    
    // Register read/write helpers
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