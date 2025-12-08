# GDB script to connect to all 4 targets (harts) as separate inferiors
# 
# Usage: riscv64-unknown-elf-gdb -x gdb_multi_target.gdb build/tests/kernel/vecadd/vecadd.elf
#        OR
#        riscv64-unknown-elf-gdb -x gdb_multi_target.gdb
#        (if you don't specify the .elf, you'll need to set EXEC_FILE below)
#
# This connects GDB to all 4 OpenOCD targets simultaneously:
#   - Inferior 1 -> hart 0 (localhost:3333)
#   - Inferior 2 -> hart 1 (localhost:3334)
#   - Inferior 3 -> hart 2 (localhost:3335)
#   - Inferior 4 -> hart 3 (localhost:3336)

echo \n=== Connecting to all 4 targets ===\n

# Set the executable file path
# This will use the file passed to GDB, or you can set it manually here
python
import gdb
try:
    # Get the current executable from GDB
    exec_file = gdb.current_progspace().filename
    if exec_file is None:
        # No file loaded, use default
        exec_file = "build/tests/kernel/vecadd/vecadd.elf"
        print("No executable specified, using default: " + exec_file)
        gdb.execute('file ' + exec_file)
    else:
        print("Using executable: " + exec_file)
    # Store it as a convenience variable
    gdb.execute('set $exec_file = "' + exec_file + '"')
except:
    exec_file = "build/tests/kernel/vecadd/vecadd.elf"
    print("Using default executable: " + exec_file)
    gdb.execute('file ' + exec_file)
    gdb.execute('set $exec_file = "' + exec_file + '"')
end

set architecture riscv:rv64

# Connect to target 0 (default inferior 1)
target extended-remote localhost:3333
echo Connected to inferior 1 (hart 0)\n

# Add inferior 2 and connect to target 1
add-inferior
inferior 2
python gdb.execute('file ' + exec_file)
target extended-remote localhost:3334
echo Connected to inferior 2 (hart 1)\n

# Add inferior 3 and connect to target 2
add-inferior
inferior 3
python gdb.execute('file ' + exec_file)
target extended-remote localhost:3335
echo Connected to inferior 3 (hart 2)\n

# Add inferior 4 and connect to target 3
add-inferior
inferior 4
python gdb.execute('file ' + exec_file)
target extended-remote localhost:3336
echo Connected to inferior 4 (hart 3)\n

# Show all inferiors
echo \n
info inferiors

# Switch back to inferior 1 (hart 0)
inferior 1

echo \n=== All 4 harts connected! ===\n
echo \n
echo Commands:\n
echo   info threads        - List all connected harts\n
echo   thread N            - Switch to hart N-1 (e.g. thread 2 = hart 1)\n
echo   info registers        - Show registers for current hart\n
echo   print $a0             - Print register value\n
echo \n
echo Example workflow:\n
echo   (gdb) thread 1      # Switch to hart 0\n
echo   (gdb) info registers  # View hart 0 registers\n
echo   (gdb) thread 2      # Switch to hart 1\n
echo   (gdb) print $a0       # View hart 1's a0 register\n
echo \n

