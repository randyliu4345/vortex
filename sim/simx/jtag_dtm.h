#pragma once
#include <cstdint>
#include "debug_module.h"

enum jtag_state_t {
    TEST_LOGIC_RESET,
    RUN_TEST_IDLE,
    SELECT_DR_SCAN,
    CAPTURE_DR,
    SHIFT_DR,
    EXIT1_DR,
    PAUSE_DR,
    EXIT2_DR,
    UPDATE_DR,
    SELECT_IR_SCAN,
    CAPTURE_IR,
    SHIFT_IR,
    EXIT1_IR,
    PAUSE_IR,
    EXIT2_IR,
    UPDATE_IR
};

class jtag_dtm_t {
public:
    jtag_dtm_t(debug_module_t* dm);

    void reset();
    void set_pins(bool tck, bool tms, bool tdi);
    bool tdo() const { return _tdo; }
    jtag_state_t state() const { return _state; }

private:
    debug_module_t* dm;

    bool _tck, _tms, _tdi, _tdo;
    jtag_state_t _state;
    uint32_t ir;
    uint64_t dr;
    unsigned dr_length;
    const unsigned abits;
    bool busy_stuck;
    uint64_t dmi;  // Store DMI read result for next scan

    // IR instruction codes
    static constexpr uint32_t IR_IDCODE     = 0x01;
    static constexpr uint32_t IR_DTMCONTROL = 0x10;
    static constexpr uint32_t IR_DBUS       = 0x11;
    static constexpr uint32_t IR_BYPASS     = 0x1F;

    void capture_dr();
    void update_dr();
};
