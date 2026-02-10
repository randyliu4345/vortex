# GDB script for single-target multi-hart debugging with warp support
# 
# Usage: riscv64-unknown-elf-gdb -x gdb_multithread_warp_debug.gdb build/tests/kernel/vecadd/vecadd.elf
#
# Includes debugging commands for investigating breakpoint/single-step bugs
# Supports both thread and warp selection

echo \n=== Single-target multi-hart debugging (with warp support) ===\n

# Set the executable file path
python
import gdb
try:
    exec_file = gdb.current_progspace().filename
    if exec_file is None:
        exec_file = "build/tests/kernel/vecadd/vecadd.elf"
        print("No executable specified, using default: " + exec_file)
        gdb.execute('file ' + exec_file)
    else:
        print("Using executable: " + exec_file)
    gdb.execute('set $exec_file = "' + exec_file + '"')
except:
    exec_file = "build/tests/kernel/vecadd/vecadd.elf"
    print("Using default executable: " + exec_file)
    gdb.execute('file ' + exec_file)
    gdb.execute('set $exec_file = "' + exec_file + '"')
end


# Connect to single target
target extended-remote localhost:3333
echo Connected to single target\n

# DMCONTROL register address (0x10 per RISC-V Debug Spec)
python
import gdb
import re

# DMI Register Addresses (RISC-V Debug Spec 0.13)
DM_DATA0       = 0x04
DM_DMCONTROL   = 0x10
DM_DMSTATUS    = 0x11
DM_ABSTRACTCS  = 0x16
DM_COMMAND     = 0x17
DM_NUM_CORES   = 0x1b
DM_NUM_THREADS = 0x1c
DM_THREAD_LANE = 0x1d
DM_WARP_SEL    = 0x1e

# DMCONTROL bit positions
DMCONTROL_DMACTIVE = 1 << 0
DMCONTROL_HARTSELHI_SHIFT = 16
DMCONTROL_HARTSELLO_SHIFT = 6

# DCSR bit positions
DCSR_STEP_BIT = 2
DCSR_CAUSE_SHIFT = 8
DCSR_CAUSE_MASK = 0xF

# Configuration: NUM_THREADS per warp (default 4, can be changed)
NUM_THREADS = 4

# Current selection state
_current_thread = 0
_current_warp = 0
_current_core = 0

def dmi_read(addr):
    """Read a DMI register and return its value"""
    try:
        result = gdb.execute("monitor riscv dmi_read 0x%x" % addr, to_string=True)
        match = re.search(r'0x([0-9a-fA-F]+)', result, re.IGNORECASE)
        if match:
            return int(match.group(1), 16)
    except:
        pass
    return None

def dmi_write(addr, value):
    """Write a DMI register"""
    try:
        gdb.execute("monitor riscv dmi_write 0x%x 0x%08x" % (addr, value), to_string=True)
        return True
    except:
        return False

def read_abstract_register(regaddr):
    """Read a register via abstract command (cmdtype=0)"""
    # Build Access Register command: cmdtype=0, transfer=1, aarsize=2 (32-bit)
    cmd = (0 << 24) | (2 << 20) | (1 << 17) | regaddr
    dmi_write(DM_COMMAND, cmd)
    return dmi_read(DM_DATA0)

def write_abstract_register(regaddr, value):
    """Write a register via abstract command (cmdtype=0)"""
    # Write value to DATA0 first
    dmi_write(DM_DATA0, value)
    # Build Access Register command: cmdtype=0, transfer=1, write=1, aarsize=2 (32-bit)
    cmd = (0 << 24) | (2 << 20) | (1 << 17) | (1 << 16) | regaddr
    dmi_write(DM_COMMAND, cmd)

def set_hartsel(hartsel_value):
    """Set hartsel in dmcontrol register via DMI write (hartsel maps to core_id)"""
    current_value = dmi_read(DM_DMCONTROL)
    if current_value is None:
        current_value = DMCONTROL_DMACTIVE
    
    # Clear hartsel fields and set new value
    current_value &= ~(0x3ff << DMCONTROL_HARTSELHI_SHIFT)
    current_value &= ~(0x3ff << DMCONTROL_HARTSELLO_SHIFT)
    
    # Set hartsel (maps to core_id, though only core 0 emulator is currently connected)
    new_value = current_value | (hartsel_value << DMCONTROL_HARTSELHI_SHIFT)
    new_value |= DMCONTROL_DMACTIVE
    
    return dmi_write(DM_DMCONTROL, new_value)

def set_coresel(core_id):
    """Set core selection via hartsel register (note: only core 0 emulator is currently connected)"""
    global _current_core
    _current_core = core_id
    return set_hartsel(core_id)

def set_threadsel(thread_id):
    """Set thread lane selection via DM_THREAD_LANE register"""
    global _current_thread
    _current_thread = thread_id
    return dmi_write(DM_THREAD_LANE, thread_id)

def set_warpsel(warp_id):
    """Set warp selection via DM_WARP_SEL register"""
    global _current_warp
    _current_warp = warp_id
    return dmi_write(DM_WARP_SEL, warp_id)

def decode_dcsr(value):
    """Decode DCSR register fields"""
    prv       = value & 0x3
    step      = (value >> 2) & 0x1
    ebreakm   = (value >> 3) & 0x1
    cause     = (value >> 8) & 0xF
    xdebugver = (value >> 28) & 0xF
    
    cause_names = {0: "none", 1: "ebreak", 2: "trigger", 3: "haltreq", 4: "step", 5: "resethalt"}
    cause_str = cause_names.get(cause, "unknown(%d)" % cause)
    
    return {
        'prv': prv,
        'step': step,
        'ebreakm': ebreakm,
        'cause': cause,
        'cause_str': cause_str,
        'xdebugver': xdebugver,
        'raw': value
    }

class SwitchThread(gdb.Command):
    """Switch to a different thread lane"""
    
    def __init__(self):
        super(SwitchThread, self).__init__("vx_thread", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp, _current_core
        
        if not arg:
            print("Current selection: core %d, warp %d, thread %d" % (_current_core, _current_warp, _current_thread))
            print("Note: Only core 0 emulator is currently connected to debug module")
            print("Usage: vx_thread <thread_id>")
            print("  vx_thread 0  - Switch to thread lane 0")
            print("  vx_thread 1  - Switch to thread lane 1")
            print("  vx_thread 2  - Switch to thread lane 2")
            print("  vx_thread 3  - Switch to thread lane 3")
            return
        
        try:
            thread_id = int(arg)
            if thread_id < 0 or thread_id >= NUM_THREADS:
                print("Error: thread_id must be 0-%d" % (NUM_THREADS - 1))
                return
            
            if thread_id == _current_thread:
                print("Already on thread %d" % thread_id)
                return
            
            if set_threadsel(thread_id):
                _current_thread = thread_id
                print("Switched to core %d, warp %d, thread %d" % (_current_core, _current_warp, _current_thread))
            else:
                print("Failed to switch to thread %d" % thread_id)
        except ValueError:
            print("Error: thread_id must be a number (0-%d)" % (NUM_THREADS - 1))

class SwitchWarp(gdb.Command):
    """Switch to a different warp by setting hartsel"""
    
    def __init__(self):
        super(SwitchWarp, self).__init__("vx_warp", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp, _current_core
        
        if not arg:
            print("Current selection: core %d, warp %d, thread %d" % (_current_core, _current_warp, _current_thread))
            print("Note: Only core 0 emulator is currently connected to debug module")
            print("Usage: vx_warp <warp_id>")
            print("  vx_warp 0  - Switch to warp 0 (keeping current thread)")
            print("  vx_warp 1  - Switch to warp 1 (keeping current thread)")
            print("  vx_warp 2  - Switch to warp 2 (keeping current thread)")
            print("  vx_warp 3  - Switch to warp 3 (keeping current thread)")
            return
        
        try:
            warp_id = int(arg)
            if warp_id < 0:
                print("Error: warp_id must be >= 0")
                return
            
            if warp_id == _current_warp:
                print("Already on warp %d" % warp_id)
                return
            
            if set_warpsel(warp_id):
                _current_warp = warp_id
                print("Switched to core %d, warp %d, thread %d" % (_current_core, _current_warp, _current_thread))
            else:
                print("Failed to switch to warp %d" % warp_id)
        except ValueError:
            print("Error: warp_id must be a number")

class PrintReg(gdb.Command):
    """Read a register using 'monitor reg force'"""
    
    def __init__(self):
        super(PrintReg, self).__init__("vx_print", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        if not arg:
            print("Usage: vx_print <register_name>")
            print("  vx_print pc   - Read PC register")
            print("  vx_print a0   - Read a0 register")
            print("  vx_print a2   - Read a2 register")
            return
        
        try:
            gdb.execute("monitor reg %s force" % arg)
        except Exception as e:
            print("Error reading register %s: %s" % (arg, e))

def probe_threads_warps():
    """Read the number of cores and threads from the backend via separate DMI registers"""
    global NUM_THREADS
    
    print("Reading core and thread configuration from backend...")
    
    # Read the configuration registers
    num_cores_val = dmi_read(DM_NUM_CORES)
    num_threads_val = dmi_read(DM_NUM_THREADS)
    
    if num_cores_val is not None and num_threads_val is not None:
        NUM_CORES = num_cores_val
        NUM_THREADS = num_threads_val
        
        if NUM_CORES > 0 and NUM_THREADS > 0:
            print("Backend reports: %d cores, %d threads per warp" % (NUM_CORES, NUM_THREADS))
            return True
        else:
            print("Backend reported invalid values: %d cores, %d threads, using default: %d threads" % 
                  (NUM_CORES, NUM_THREADS, NUM_THREADS))
            return False
    else:
        print("Failed to read configuration registers, using default: %d threads" % NUM_THREADS)
        return False

def read_register_by_name(reg_name):
    """Read a register by name using monitor reg command, return value or None"""
    try:
        result = gdb.execute("monitor reg %s force" % reg_name, to_string=True)
        # Try to extract hex value from output
        match = re.search(r'0x([0-9a-fA-F]+)', result, re.IGNORECASE)
        if match:
            return int(match.group(1), 16)
        # If no hex value, try to extract decimal
        match = re.search(r'(\d+)', result)
        if match:
            return int(match.group(1))
        return None
    except:
        return None

class PrintRegAll(gdb.Command):
    """Print a register for all threads in the current warp"""
    
    def __init__(self):
        super(PrintRegAll, self).__init__("vx_print_all", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp
        
        if not arg:
            print("Usage: vx_print_all <register_name>")
            print("  vx_print_all pc        - Print PC for all threads in current warp")
            print("  vx_print_all a0        - Print a0 for all threads in current warp")
            return
        
        # Parse arguments
        args = arg.split()
        reg_name = args[0]
        
        # Transparently read number of threads from debug module register
        num_threads_val = dmi_read(DM_NUM_THREADS)
        if num_threads_val is not None and num_threads_val > 0:
            num_threads = num_threads_val
        else:
            # Fallback to default if register read fails
            num_threads = NUM_THREADS
            print("Warning: Failed to read DM_NUM_THREADS, using default: %d" % num_threads)
        
        # Save current selection
        saved_thread = _current_thread
        saved_warp = _current_warp
        
        print("=" * 70)
        print("Register '%s' for all threads in warp %d (%d threads)" % (reg_name, saved_warp, num_threads))
        print("=" * 70)
        print("%-6s | %s" % ("Thread", "Value"))
        print("-" * 70)
        
        # Iterate through all threads in current warp
        for thread_id in range(num_threads):
            # Switch to this thread
            if set_threadsel(thread_id):
                _current_thread = thread_id
                
                # Read register
                reg_val = read_register_by_name(reg_name)
                
                if reg_val is not None:
                    print("%-6d | 0x%08x (%d)" % (thread_id, reg_val, reg_val))
                else:
                    print("%-6d | FAILED" % thread_id)
            else:
                print("%-6d | FAILED (thread selection)" % thread_id)
        
        print("-" * 70)
        
        # Restore original selection
        set_threadsel(saved_thread)
        _current_thread = saved_thread

# ============================================================================
# DEBUGGING COMMANDS FOR BREAKPOINT BUG INVESTIGATION
# ============================================================================

class InspectDCSR(gdb.Command):
    """Inspect DCSR register for current hart - check step bit and cause"""
    
    def __init__(self):
        super(InspectDCSR, self).__init__("dcsr", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp
        
        # Read DCSR (register address 0x7B0)
        dcsr_val = read_abstract_register(0x7B0)
        if dcsr_val is None:
            print("Failed to read DCSR")
            return
        
        fields = decode_dcsr(dcsr_val)
        
        print("=" * 50)
        print("DCSR for warp %d, thread %d: 0x%08x" % (_current_warp, _current_thread, dcsr_val))
        print("=" * 50)
        print("  step      = %d  %s" % (fields['step'], "<-- STUCK?" if fields['step'] else ""))
        print("  cause     = %d (%s)" % (fields['cause'], fields['cause_str']))
        print("  prv       = %d (privilege level)" % fields['prv'])
        print("  ebreakm   = %d" % fields['ebreakm'])
        print("  xdebugver = %d" % fields['xdebugver'])
        print("")
        
        if fields['step']:
            print("WARNING: step bit is SET - thread will halt after every instruction!")
            print("This could explain repeated breakpoint hits after single-step + continue.")

class InspectDPC(gdb.Command):
    """Inspect DPC register (Debug PC) for current hart"""
    
    def __init__(self):
        super(InspectDPC, self).__init__("dpc", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp
        
        # Read DPC (register address 0x7B1)
        dpc_val = read_abstract_register(0x7B1)
        if dpc_val is None:
            print("Failed to read DPC")
            return
        
        # Also read PC for comparison
        pc_val = read_abstract_register(0x1020)
        
        print("=" * 50)
        print("Debug PC for warp %d, thread %d" % (_current_warp, _current_thread))
        print("=" * 50)
        print("  DPC = 0x%08x" % dpc_val)
        if pc_val is not None:
            print("  PC  = 0x%08x" % pc_val)
            if dpc_val != pc_val:
                print("  NOTE: DPC and PC differ!")

class InspectAllDCSR(gdb.Command):
    """Inspect DCSR for all threads (0-3) in current warp to find stuck step bits"""
    
    def __init__(self):
        super(InspectAllDCSR, self).__init__("dcsr_all", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp
        saved_thread = _current_thread
        saved_warp = _current_warp
        
        print("=" * 60)
        print("DCSR for all threads in warp %d (checking for stuck step bits)" % saved_warp)
        print("=" * 60)
        print("Thread | DCSR       | step | cause        | Issue?")
        print("-" * 60)
        
        issues = []
        for thread_id in range(NUM_THREADS):
            set_threadsel(thread_id)
            dcsr_val = read_abstract_register(0x7B0)
            
            if dcsr_val is None:
                print(" %d     | FAILED     |  -   | -            |" % thread_id)
                continue
            
            fields = decode_dcsr(dcsr_val)
            issue = ""
            if fields['step']:
                issue = "STEP BIT STUCK!"
                issues.append(thread_id)
            
            print(" %d     | 0x%08x |  %d   | %s | %s" % (
                thread_id, dcsr_val, fields['step'], 
                fields['cause_str'].ljust(12), issue))
        
        print("-" * 60)
        
        if issues:
            print("\nWARNING: Thread(s) %s have step bit stuck!" % issues)
            print("This will cause infinite halt-after-every-instruction behavior.")
        else:
            print("\nAll step bits are clear.")
        
        # Restore original selection
        set_warpsel(saved_warp)
        set_threadsel(saved_thread)
        _current_warp = saved_warp
        _current_thread = saved_thread

class ClearStepBit(gdb.Command):
    """Clear the step bit in DCSR for current or specified thread"""
    
    def __init__(self):
        super(ClearStepBit, self).__init__("clear_step", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp
        
        if arg:
            try:
                thread_id = int(arg)
                if thread_id < 0 or thread_id >= NUM_THREADS:
                    print("Error: thread_id must be 0-%d" % (NUM_THREADS - 1))
                    return
                set_threadsel(thread_id)
                target_thread = thread_id
            except ValueError:
                print("Error: thread_id must be a number")
                return
        else:
            target_thread = _current_thread
        
        # Read current DCSR
        dcsr_val = read_abstract_register(0x7B0)
        if dcsr_val is None:
            print("Failed to read DCSR for warp %d, thread %d" % (_current_warp, target_thread))
            return
        
        old_step = (dcsr_val >> DCSR_STEP_BIT) & 1
        
        # Clear step bit (bit 2)
        new_dcsr = dcsr_val & ~(1 << DCSR_STEP_BIT)
        
        # Write back
        write_abstract_register(0x7B0, new_dcsr)
        
        # Verify
        verify_val = read_abstract_register(0x7B0)
        new_step = (verify_val >> DCSR_STEP_BIT) & 1 if verify_val else -1
        
        print("Warp %d, Thread %d DCSR step bit: %d -> %d" % (_current_warp, target_thread, old_step, new_step))
        if new_step == 0:
            print("Step bit cleared successfully!")
        else:
            print("WARNING: Step bit may not have cleared properly")

class PCCheck(gdb.Command):
    """Check PC consistency across thread/warp switches"""
    
    def __init__(self):
        super(PCCheck, self).__init__("pc_check", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp
        saved_thread = _current_thread
        saved_warp = _current_warp
        
        # Record PC before switch
        pc_before = read_abstract_register(0x1020)
        dpc_before = read_abstract_register(0x7B1)
        
        print("=" * 50)
        print("PC Consistency Check for warp %d, thread %d" % (saved_warp, saved_thread))
        print("=" * 50)
        print("Before switch:")
        print("  PC  = 0x%08x" % (pc_before or 0))
        print("  DPC = 0x%08x" % (dpc_before or 0))
        
        # Switch to another thread and back
        other_thread = (saved_thread + 1) % NUM_THREADS
        set_threadsel(other_thread)
        print("\nSwitched to warp %d, thread %d" % (_current_warp, other_thread))
        
        set_threadsel(saved_thread)
        _current_thread = saved_thread
        print("Switched back to warp %d, thread %d" % (_current_warp, saved_thread))
        
        # Record PC after switch
        pc_after = read_abstract_register(0x1020)
        dpc_after = read_abstract_register(0x7B1)
        
        print("\nAfter switch:")
        print("  PC  = 0x%08x" % (pc_after or 0))
        print("  DPC = 0x%08x" % (dpc_after or 0))
        
        print("\nComparison:")
        if pc_before == pc_after:
            print("  PC:  MATCH")
        else:
            print("  PC:  MISMATCH! 0x%08x -> 0x%08x" % (pc_before or 0, pc_after or 0))
        
        if dpc_before == dpc_after:
            print("  DPC: MATCH")
        else:
            print("  DPC: MISMATCH! 0x%08x -> 0x%08x" % (dpc_before or 0, dpc_after or 0))

class DebugState(gdb.Command):
    """Show complete debug state: DCSR, DPC, PC, DMSTATUS for current warp/thread"""
    
    def __init__(self):
        super(DebugState, self).__init__("debug_state", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_thread, _current_warp, _current_core
        
        print("=" * 60)
        print("Complete Debug State for core %d, warp %d, thread %d" % (_current_core, _current_warp, _current_thread))
        if _current_core != 0:
            print("Note: Only core 0 emulator is currently connected")
        print("=" * 60)
        
        # DCSR
        dcsr_val = read_abstract_register(0x7B0)
        if dcsr_val:
            fields = decode_dcsr(dcsr_val)
            print("\nDCSR: 0x%08x" % dcsr_val)
            print("  step=%d, cause=%s, prv=%d" % (fields['step'], fields['cause_str'], fields['prv']))
        
        # DPC
        dpc_val = read_abstract_register(0x7B1)
        if dpc_val is not None:
            print("\nDPC:  0x%08x" % dpc_val)
        
        # PC  
        pc_val = read_abstract_register(0x1020)
        if pc_val is not None:
            print("PC:   0x%08x" % pc_val)
        
        # DMSTATUS
        dmstatus = dmi_read(DM_DMSTATUS)
        if dmstatus:
            print("\nDMSTATUS: 0x%08x" % dmstatus)
            print("  anyhalted=%d, allhalted=%d" % ((dmstatus >> 8) & 1, (dmstatus >> 9) & 1))
            print("  anyrunning=%d, allrunning=%d" % ((dmstatus >> 10) & 1, (dmstatus >> 11) & 1))
            print("  anyresumeack=%d, allresumeack=%d" % ((dmstatus >> 16) & 1, (dmstatus >> 17) & 1))
        
        # DMCONTROL
        dmcontrol = dmi_read(DM_DMCONTROL)
        if dmcontrol:
            hartsel = (dmcontrol >> 16) & 0x3ff
            # hartsel now maps to core_id
            core_id = hartsel
            thread_lane = dmi_read(DM_THREAD_LANE)
            warp_sel = dmi_read(DM_WARP_SEL)
            print("\nDMCONTROL: 0x%08x" % dmcontrol)
            print("  core_id=%d (hartsel), warp=%d, thread=%d, dmactive=%d" % (core_id, warp_sel if warp_sel is not None else 0, thread_lane if thread_lane is not None else 0, dmcontrol & 1))

class BugRepro(gdb.Command):
    """Reproduce the breakpoint bug and capture state at each step"""
    
    def __init__(self):
        super(BugRepro, self).__init__("bug_repro", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        print("=" * 60)
        print("Breakpoint Bug Reproduction")
        print("=" * 60)
        print("\nThis will guide you through reproducing the bug.")
        print("Run each step manually and use 'debug_state' after each:\n")
        print("Step 1: Set a breakpoint")
        print("  (gdb) break <function>")
        print("  (gdb) continue")
        print("  (gdb) debug_state   # Record state at breakpoint")
        print("")
        print("Step 2: Single step")
        print("  (gdb) stepi")
        print("  (gdb) debug_state   # Check if step bit is set")
        print("")
        print("Step 3: Continue (this should fail)")
        print("  (gdb) continue")
        print("  (gdb) debug_state   # Check state when it stops again")
        print("  (gdb) dcsr_all      # Check ALL threads for stuck step bits")
        print("")
        print("Step 4: Try workaround - clear step bit manually")
        print("  (gdb) clear_step")
        print("  (gdb) continue      # This should work now")
        print("")
        print("Step 5: Alternative - switch threads/warps and continue")
        print("  (gdb) vx_thread 1")
        print("  (gdb) vx_warp 1")
        print("  (gdb) vx_thread 0")
        print("  (gdb) vx_warp 0")
        print("  (gdb) continue      # Does this work?")

# Register all commands
SwitchThread()
SwitchWarp()
PrintReg()
PrintRegAll()
InspectDCSR()
InspectDPC()
InspectAllDCSR()
ClearStepBit()
PCCheck()
DebugState()
BugRepro()
end

echo \n=== Custom commands loaded ===\n
echo \n
echo Basic Commands:\n
echo   vx_thread N     - Switch to thread N (0-3) in current warp\n
echo   vx_warp N        - Switch to warp N (0-3) keeping current thread\n
echo   vx_print <name>  - Read register using 'monitor reg <name> force'\n
echo   vx_print_all <name>\n
echo                    - Print register for all threads in current warp (reads thread count from debug module)\n
echo \n
echo Debug Investigation Commands:\n
echo   dcsr            - Show DCSR register (check step bit and cause)\n
echo   dpc             - Show DPC register (debug PC)\n
echo   dcsr_all        - Show DCSR for ALL threads in current warp (find stuck step bits)\n
echo   clear_step [N]  - Clear step bit in DCSR for thread N (default: current)\n
echo   pc_check        - Test PC consistency across thread switches\n
echo   debug_state     - Show complete debug state\n
echo   bug_repro       - Instructions to reproduce the breakpoint bug\n
echo \n
echo Quick Debug Workflow:\n
echo   1. (gdb) break <func>  # Set breakpoint\n
echo   2. (gdb) continue      # Hit breakpoint\n
echo   3. (gdb) stepi         # Single step\n
echo   4. (gdb) dcsr          # Check if step bit is stuck\n
echo   5. (gdb) continue      # If it fails...\n
echo   6. (gdb) dcsr_all      # Check all threads\n
echo   7. (gdb) clear_step    # Clear stuck step bit\n
echo   8. (gdb) continue      # Should work now\n
echo \n
echo Note: hartsel = core_id, use DM_THREAD_LANE and DM_WARP_SEL for thread/warp selection\n
echo Note: Only core 0 emulator is currently connected to debug module\n
echo \n
