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
./build/sim/simx/simx -d -p 9824 build/tests/kernel/fibonacci/fibonacci.bin
```

The simulator starts halted, waiting for a debugger connection.

### Step 2: Start OpenOCD

```bash
openocd -f vortex.cfg
```

**Note:** `vortex.cfg` uses port 9824. If using default port 9823, either:
- Start simulator with `-p 9824`, or
- Update `vortex.cfg` to use port 9823

### Step 3: Connect GDB

**For single-hart debugging (hart 0 only):**
```bash
riscv64-unknown-elf-gdb build/tests/kernel/fibonacci/fibonacci.elf
```

In GDB:
```
(gdb) target remote localhost:3333
(gdb) monitor reset halt
(gdb) set $pc = 0x80000000
(gdb) break main
(gdb) continue
```

**For multi-hart debugging (all 4 harts):**
```bash
riscv64-unknown-elf-gdb build/tests/kernel/vecadd/vecadd.elf -x gdb_multi_target.gdb
```

The script automatically connects to all 4 harts. See [Multi-Hart Debugging](#multi-hart-debugging) section below for details.

## Common GDB Commands

```bash
# Breakpoints
(gdb) break main
(gdb) break fibonacci
(gdb) break main.cpp:16

# Execution control
(gdb) continue          # Continue execution
(gdb) step             # Step into function
(gdb) next             # Step over function
(gdb) stepi            # Step one instruction
(gdb) nexti            # Next instruction

# Inspection
(gdb) print variable
(gdb) info registers
(gdb) x/10i $pc        # Disassemble 10 instructions
(gdb) x/s 0x80005740   # Print string at address

# Multi-hart debugging (when using gdb_multi_target.gdb script)
(gdb) info threads   # List all connected harts (inferiors 1-4)
(gdb) thread 2       # Switch to hart 1 (inferior 2)
(gdb) info registers   # View registers of current hart
(gdb) print $a0        # Print register value for current hart
```

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

## Example Session

### Single-Hart Debugging (Hart 0 Only)

```bash
# Terminal 1
./build/sim/simx/simx -d -p 9824 build/tests/kernel/fibonacci/fibonacci.bin

# Terminal 2
openocd -f vortex.cfg

# Terminal 3
riscv64-unknown-elf-gdb
(gdb) target remote localhost:3333
(gdb) stepi
(gdb) b *0x80000094
(gdb) continue
(gdb) i r
(gdb) continue
Continuing.
Program Stopped
```

### Multi-Hart Debugging (All 4 Harts)

```bash
# Terminal 1
./build/sim/simx/simx -d -p 9824 build/tests/kernel/vecadd/vecadd.bin

# Terminal 2
openocd -f vortex.cfg

# Terminal 3: Use the multi-target script
riscv64-unknown-elf-gdb build/tests/kernel/vecadd/vecadd.elf -x gdb_multi_target.gdb

# The script connects to all 4 harts automatically
(gdb) info inferiors
  Num  Description       Executable        
* 1    process 1         /path/to/vecadd.elf
  2    process 2         /path/to/vecadd.elf
  3    process 3         /path/to/vecadd.elf
  4    process 4         /path/to/vecadd.elf

(gdb) break vecadd_kernel
Breakpoint 1 at 0x80000148

(gdb) continue
# All harts will hit the breakpoint

# Examine different harts
(gdb) inferior 1
(gdb) print $a0
$1 = 0x12345678

(gdb) inferior 2
(gdb) print $a0
$2 = 0x87654321

(gdb) inferior 3
(gdb) print $a0
$3 = 0xabcdef00

(gdb) inferior 4
(gdb) print $a0
$4 = 0xfedcba98
```

## Multi-Hart Debugging

Vortex supports debugging all 4 hardware threads (harts) simultaneously using GDB's multi-inferior feature. This allows you to inspect the register state of each hart independently.

### Quick Start: Multi-Hart Debugging

**You must use the `gdb_multi_target.gdb` script to connect to all 4 harts:**

```bash
# Terminal 1: Start simulator
./build/sim/simx/simx -d -p 9824 build/tests/kernel/vecadd/vecadd.bin

# Terminal 2: Start OpenOCD (creates 4 targets)
openocd -f vortex.cfg

# Terminal 3: Connect GDB using the multi-target script
riscv64-unknown-elf-gdb build/tests/kernel/vecadd/vecadd.elf -x gdb_multi_target.gdb
```

The script automatically:
- Connects to all 4 OpenOCD targets (ports 3333-3336)
- Sets up each hart as a separate GDB inferior
- Configures the environment for multi-inferior debugging

### Why Multi-Inferior Instead of Threads?

In Vortex's SIMT model, all threads in a warp share the same PC and execution state. The standard `hwthread` RTOS approach only reports hart 0 as "active" to GDB, so other harts aren't visible as separate threads.

**Solution:** Create 4 separate OpenOCD targets (one per hart) and use GDB's multi-inferior feature to connect to all of them simultaneously.

### Switching Between Harts

Once connected via the script, use `inferior` commands to switch between harts:

```bash
(gdb) info inferiors           # List all connected harts
  Num  Description       Executable        
* 1    process 1         /path/to/vecadd.elf
  2    process 2         /path/to/vecadd.elf
  3    process 3         /path/to/vecadd.elf
  4    process 4         /path/to/vecadd.elf

(gdb) inferior 2               # Switch to hart 1 (inferior 2 = hart 1)
[Switching to inferior 2 [process 2]]

(gdb) info registers           # View registers for hart 1
a0             0x12345678    305419896
a1             0x87654321    2271560481
...

(gdb) inferior 1               # Switch back to hart 0
[Switching to inferior 1 [process 1]]

(gdb) info registers           # View registers for hart 0 (different values)
a0             0xabcdef00    2882400000
...
```

### Comparing Register Values Across Harts

```bash
# Print a0 for all harts
(gdb) inferior 1
(gdb) printf "Hart 0: a0 = 0x%x\n", $a0

(gdb) inferior 2
(gdb) printf "Hart 1: a0 = 0x%x\n", $a0

(gdb) inferior 3
(gdb) printf "Hart 2: a0 = 0x%x\n", $a0

(gdb) inferior 4
(gdb) printf "Hart 3: a0 = 0x%x\n", $a0
```

### Setting Breakpoints

Breakpoints apply to **all inferiors** by default:

```bash
(gdb) break vecadd_kernel
Breakpoint 1 at 0x80000148

# When any hart hits the breakpoint, GDB will stop all inferiors
(gdb) continue
```

### Understanding Inferiors vs Threads

In this setup:
- **Inferior** = Hart (hardware thread)
- **Thread** = Not used (each inferior has 1 thread)

| GDB Concept | Vortex Mapping |
|-------------|----------------|
| Inferior 1  | Hart 0         |
| Inferior 2  | Hart 1         |
| Inferior 3  | Hart 2         |
| Inferior 4  | Hart 3         |

### Important Notes

- **Use `inferior N`, not `thread N`**: Since harts are inferiors, use `inferior` commands
- **Use `info inferiors`, not `info threads`**: To list all connected harts
- **Shared PC**: All harts share the same PC (SIMT execution model), so they'll all be at the same instruction
- **Script required**: The `gdb_multi_target.gdb` script is required for multi-hart debugging
- **Known limitation**: You may see "Connection N does not support multi-target resumption" messages when using `continue` or `step`. These are harmless warnings - execution works correctly. GDB checks other inferiors but only resumes the current one.

### Manual Connection (Alternative)

If you prefer to connect manually instead of using the script:

```bash
riscv64-unknown-elf-gdb build/tests/kernel/vecadd/vecadd.elf

# Connect to hart 0 (inferior 1)
(gdb) target extended-remote localhost:3333

# Add and connect to hart 1 (inferior 2)
(gdb) add-inferior
(gdb) inferior 2
(gdb) target extended-remote localhost:3334

# Add and connect to hart 2 (inferior 3)
(gdb) add-inferior
(gdb) inferior 3
(gdb) target extended-remote localhost:3335

# Add and connect to hart 3 (inferior 4)
(gdb) add-inferior
(gdb) inferior 4
(gdb) target extended-remote localhost:3336

# View all connected harts
(gdb) info inferiors
```

## Features

- **Software breakpoints:** Implemented using `EBREAK` instructions
- **Single-step:** Full instruction-level stepping support
- **Program completion:** Automatically detected and reported to GDB
- **Multi-hart debugging:** Access all 4 harts simultaneously using the `gdb_multi_target.gdb` script
- **Per-hart register inspection:** View each hart's registers independently
- **RISC-V Debug Spec 0.13:** Full compliance with standard debug interface

## Files

- `vortex.cfg` - OpenOCD configuration (creates 4 targets on ports 3333-3336)
- `gdb_multi_target.gdb` - Helper script to connect to all 4 harts (required for multi-hart debugging)
- `test_multi_inferior.gdb` - Test script to verify all harts are accessible

## Additional Resources

- [RISC-V Debug Specification](https://github.com/riscv/riscv-debug-spec)
- [OpenOCD Documentation](http://openocd.org/doc/html/index.html)
- [GDB User Manual](https://sourceware.org/gdb/current/onlinedocs/gdb/)
