# Vortex Debug Mode with GDB

This guide explains how to debug Vortex programs using GDB and OpenOCD with the RISC-V debug interface.

## Prerequisites

- Built simulator: `make -C build/sim/simx`
- OpenOCD installed
- RISC-V GDB (part of RISC-V toolchain)

## Quick Start: Debugging Fibonacci

### Step 1: Start Simulator in Debug Mode

```bash
cd /vortex
./build/sim/simx/simx -d build/tests/kernel/fibonacci/fibonacci.bin
```

The simulator starts halted, waiting for a debugger connection.

### Step 2: Start OpenOCD

```bash
openocd -f vortex.cfg
```

### Step 3: Connect GDB

**For single-hart debugging (hart 0 only):**
```bash
riscv64-unknown-elf-gdb build/tests/kernel/fibonacci/fibonacci.elf
```

## Common GDB Commands

```bash
# Execution control
(gdb) continue          # Continue execution
(gdb) step             # Step into function
(gdb) next             # Step over function
(gdb) stepi            # Step one instruction
(gdb) nexti            # Next instruction

# Inspection
(gdb) print t4
(gdb) info registers
(gdb) x/10i $pc        # Disassemble 10 instructions
(gdb) x/s 0x80005740   # Print string at address

```

## Multi-Threading Support

Vortex supports debugging multiple hardware threads (harts) simultaneously using the single-target approach:

```bash
# Terminal 1: Start simulator
./build/sim/simx/simx -d build/tests/kernel/vecadd/vecadd.bin

# Terminal 2: Start OpenOCD
openocd -f vortex.cfg

# Terminal 3: Connect GDB with single-target script
riscv64-unknown-elf-gdb build/tests/kernel/vecadd/vecadd.elf -x gdb_multithread.gdb
```

**Available Commands:**
- `hart N` - Switch to hart N (0-3)
  ```bash
  (gdb) hart 1    # Switch to hart 1
  (gdb) hart 2    # Switch to hart 2
  ```
- `rreg <name>` - Read register for the current hart
  ```bash
  (gdb) rreg pc   # Read PC register
  (gdb) rreg a0   # Read a0 register
  (gdb) rreg a2   # Read a2 register
  ```

**Example Usage:**
```bash
(gdb) b vecadd_kernel      # Set breakpoint
(gdb) c           #continue to breakpoint
(gdb) hart 0      # Switch to hart 0
(gdb) rreg pc     # Read PC for hart 0
(gdb) hart 1      # Switch to hart 1
(gdb) rreg a2     # Read a0 for hart 1
(gdb) delete 1    # delete breakpoint for continuous execution
(gdb) continue    # Continue execution (all harts resume)
```

**Note:** This approach uses a single OpenOCD target and switches between harts using the `hartsel` register. Execution control (continue, step, etc.) affects all harts simultaneously since they share the same PC in SIMT execution.

## Command-Line Options

```bash
./build/sim/simx/simx [options] <program.bin>

Options:
  -d              Enable debug mode
  -p <port>       Remote bitbang port (default: 9823)
  -c <cores>      Number of cores
  -w <warps>      Number of warps per core
  -t <threads>    Number of threads per warp
```

## Key Addresses (Fibonacci Binary)

| Address | Function/Data |
|---------|---------------|
| 0x80000000 | `_start` (entry point) |
| 0x80000094 | `fibonacci()` |
| 0x80000114 | `main()` |
| 0x800001ac | `init_regs()` (final PC) |
| 0x80005740 | `"fibonacci(%d) = %d\n"` |
| 0x80005754 | `"Passed!\n"` |
| 0x8000575c | `"Failed! value=%d, expected=%d\n"` |

## Troubleshooting

**OpenOCD can't connect:**
- Verify simulator is running with `-d` flag
- Check port numbers match (default 9823, config uses 9824)
- Check simulator output for "Remote bitbang server ready"

**GDB shows PC at 0x800001ac:**
- Program already completed
- Use `monitor reset halt` and `set $pc = 0x80000000` to restart

**Breakpoints not working:**
- Ensure address is in executable memory (0x80000000+)
- Verify program hasn't already passed that address

## Features

- **Software breakpoints:** Implemented using `EBREAK` instructions
- **Single-step:** Full instruction-level stepping support
- **Program completion:** Automatically detected and reported to GDB
- **Multi-hart debugging:** Access all 4 harts simultaneously using the `gdb_multithread.gdb` script with `hart` and `rreg` commands
- **Per-hart register inspection:** View each hart's registers independently
- **RISC-V Debug Spec 0.13:** Full compliance with standard debug interface

## Files

- `vortex.cfg` - OpenOCD configuration for single-target multi-hart debugging
- `gdb_multithread.gdb` - Helper script that provides `hart` and `rreg` commands for multi-hart debugging

## Additional Resources

- [RISC-V Debug Specification](https://github.com/riscv/riscv-debug-spec)
- [OpenOCD Documentation](http://openocd.org/doc/html/index.html)
- [GDB User Manual](https://sourceware.org/gdb/current/onlinedocs/gdb/)
