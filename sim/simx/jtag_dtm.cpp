#include "jtag_dtm.h"
#include <cstdio>

jtag_dtm_t::jtag_dtm_t(DebugModule* dm)
    : dm(dm),
      _tck(0), _tms(0), _tdi(0), _tdo(0),
      _state(TEST_LOGIC_RESET),
      ir(IR_IDCODE), dr(0), dr_length(1),
      abits(7), busy_stuck(false), dmi(0) {}

void jtag_dtm_t::reset() {
    _state = TEST_LOGIC_RESET;
    ir = IR_IDCODE;
    busy_stuck = false;
    dmi = 0;
}

// State transition table for the JTAG TAP
void jtag_dtm_t::set_pins(bool tck, bool tms, bool tdi) {
    static const jtag_state_t next[16][2] = {
        {RUN_TEST_IDLE, TEST_LOGIC_RESET},
        {RUN_TEST_IDLE, SELECT_DR_SCAN},
        {CAPTURE_DR, SELECT_IR_SCAN},
        {SHIFT_DR, EXIT1_DR},
        {SHIFT_DR, EXIT1_DR},
        {PAUSE_DR, UPDATE_DR},
        {PAUSE_DR, EXIT2_DR},
        {SHIFT_DR, UPDATE_DR},
        {RUN_TEST_IDLE, SELECT_DR_SCAN},
        {CAPTURE_IR, TEST_LOGIC_RESET},
        {SHIFT_IR, EXIT1_IR},
        {SHIFT_IR, EXIT1_IR},
        {PAUSE_IR, UPDATE_IR},
        {PAUSE_IR, EXIT2_IR},
        {SHIFT_IR, UPDATE_IR},
        {RUN_TEST_IDLE, SELECT_DR_SCAN}
    };

    // Rising edge: sample inputs
    if (!_tck && tck) {
        switch (_state) {
            case SHIFT_DR: dr >>= 1; dr |= (uint64_t)_tdi << (dr_length - 1); break;
            case SHIFT_IR: ir >>= 1; ir |= _tdi << 4; break;
            default: break;
        }
        _state = next[_state][_tms];
    }
    // Falling edge: trigger operations
    else if (_tck && !tck) {
        switch (_state) {
            case CAPTURE_DR: capture_dr(); break;
            case UPDATE_DR:  update_dr();  break;
            case SHIFT_DR:   _tdo = dr & 1; break;
            case SHIFT_IR:   _tdo = ir & 1; break;
            default: break;
        }
    }

    _tck = tck;
    _tms = tms;
    _tdi = tdi;
}

void jtag_dtm_t::capture_dr() {
    switch (ir) {
        case IR_IDCODE:     dr = 0xdeadbeef; dr_length = 32; break;
        case IR_DTMCONTROL: 
            // DTM Control register format:
            // [3:0]   = version (1)
            // [8:4]   = abits (address bits, 7 in our case)
            // [17:9]  = reserved (0)
            // [31:18] = dmistat (status, 0 = no error)
            // Value = (abits << 4) | version = (7 << 4) | 1 = 0x71
            dr = (abits << 4) | 1;
            dr_length = 32; 
            break;
        case IR_DBUS:       
            // For DMI reads, return the stored result from previous read
            // For DMI writes or new operations, start with 0
            dr = dmi;
            dr_length = abits + 34; 
            break;
        case IR_BYPASS:     dr = 0; dr_length = 1; break;
        default:            dr = 0; dr_length = 1; break;
    }
}

// Called after shifting is complete — performs DMI read/write
void jtag_dtm_t::update_dr() {
    if (ir == IR_DBUS) {
        uint32_t op   = dr & 0x3;
        uint32_t data = (dr >> 2) & 0xFFFFFFFF;
        uint32_t addr = (dr >> 34) & ((1 << abits) - 1);

        bool success = true;
        if (op == 1) { // READ
            uint32_t val = 0;
            success = dm->dmi_read(addr, &val);
            // Store the result for the next CAPTURE_DR
            // Format: [bits 1:0] = op (should be 0 for success), [bits 33:2] = data
            dmi = ((uint64_t)val << 2) | (success ? 0 : 3);
        } else if (op == 2) { // WRITE
            success = dm->dmi_write(addr, data);
            // After write, clear the read result for next operation
            dmi = success ? 0 : 3;
        } else {
            // No-op or error - clear read result
            dmi = 0;
        }

        busy_stuck = !success;
    }
}
