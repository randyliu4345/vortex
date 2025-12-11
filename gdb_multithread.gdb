# GDB script for single-target multi-hart debugging
# 
# Usage: riscv64-unknown-elf-gdb -x gdb_single_target.gdb build/tests/kernel/vecadd/vecadd.elf
#
# Simple wrapper for reading hart registers with 'monitor reg force'

echo \n=== Single-target multi-hart debugging ===\n

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

set architecture riscv:rv64

# Connect to single target
target extended-remote localhost:3333
echo Connected to single target\n

# DMCONTROL register address (0x10 per RISC-V Debug Spec)
python
import gdb
import re

DM_DMCONTROL = 0x10
DMCONTROL_DMACTIVE = 1 << 0
DMCONTROL_HARTSELHI_SHIFT = 16
DMCONTROL_HARTSELLO_SHIFT = 6
_current_hart = 0

def set_hartsel(hart_id):
    """Set hartsel in dmcontrol register via DMI write"""
    # Read current dmcontrol value
    current_value = DMCONTROL_DMACTIVE
    try:
        result = gdb.execute("monitor riscv dmi_read 0x%x" % DM_DMCONTROL, to_string=True)
        match = re.search(r'0x([0-9a-fA-F]{8})', result, re.IGNORECASE)
        if match:
            current_value = int(match.group(1), 16)
    except:
        pass
    
    # Clear hartsel fields and set new value
    current_value &= ~(0x3ff << DMCONTROL_HARTSELHI_SHIFT)
    current_value &= ~(0x3ff << DMCONTROL_HARTSELLO_SHIFT)
    
    # Set hartselhi to hart_id
    new_value = current_value | (hart_id << DMCONTROL_HARTSELHI_SHIFT)
    new_value |= DMCONTROL_DMACTIVE
    
    # Write new value via DMI
    try:
        gdb.execute("monitor riscv dmi_write 0x%x 0x%08x" % (DM_DMCONTROL, new_value), to_string=True)
        return True
    except:
        return False

class SwitchHart(gdb.Command):
    """Switch to a different hart by setting hartsel"""
    
    def __init__(self):
        super(SwitchHart, self).__init__("hart", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        global _current_hart
        
        if not arg:
            print("Current hart: %d" % _current_hart)
            print("Usage: hart <hart_id>")
            print("  hart 0  - Switch to hart 0")
            print("  hart 1  - Switch to hart 1")
            print("  hart 2  - Switch to hart 2")
            print("  hart 3  - Switch to hart 3")
            return
        
        try:
            hart_id = int(arg)
            if hart_id < 0 or hart_id >= 4:
                print("Error: hart_id must be 0-3")
                return
            
            if hart_id == _current_hart:
                print("Already on hart %d" % hart_id)
                return
            
            if set_hartsel(hart_id):
                _current_hart = hart_id
                print("Switched to hart %d" % hart_id)
            else:
                print("Failed to switch to hart %d" % hart_id)
        except ValueError:
            print("Error: hart_id must be a number (0-3)")

class ReadReg(gdb.Command):
    """Read a register using 'monitor reg force'"""
    
    def __init__(self):
        super(ReadReg, self).__init__("rreg", gdb.COMMAND_USER)
    
    def invoke(self, arg, from_tty):
        if not arg:
            print("Usage: rreg <register_name>")
            print("  rreg pc   - Read PC register")
            print("  rreg a0   - Read a0 register")
            print("  rreg a2   - Read a2 register")
            return
        
        try:
            gdb.execute("monitor reg %s force" % arg)
        except Exception as e:
            print("Error reading register %s: %s" % (arg, e))

SwitchHart()
ReadReg()
end

echo \n=== Custom commands loaded ===\n
echo Commands:\n
echo   hart N          - Switch to hart N (0-3)\n
echo   rreg <name>     - Read register using 'monitor reg <name> force'\n
echo \n
echo Example:\n
echo   (gdb) hart 1    # Switch to hart 1\n
echo   (gdb) rreg a2   # Read a2 register\n
echo   (gdb) hart 2    # Switch to hart 2\n
echo   (gdb) rreg pc   # Read PC\n
echo \n
