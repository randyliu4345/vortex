#ifndef DEBUG_MODULE_H
#define DEBUG_MODULE_H

#include <stdint.h>

// Debug Module Interface (DMI) for RISC-V debug specification
class debug_module_t {
public:
  debug_module_t();
  ~debug_module_t();

  // DMI (Debug Module Interface) read operation
  // Returns true on success, false on failure
  bool dmi_read(unsigned address, uint32_t *value);

  // DMI write operation
  // Returns true on success, false on failure
  bool dmi_write(unsigned address, uint32_t data);

  // Called when JTAG enters Run-Test/Idle state
  void run_test_idle();

  // Check if the debug module has halted the CPU
  bool is_halted() const { return halted_; }

  // Set halt state (called by debug commands)
  void set_halted(bool halted) { halted_ = halted; }

private:
  bool halted_;
};

#endif // DEBUG_MODULE_H
