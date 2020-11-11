# This is almost a straight copy of the default that idf.py will supply if we call 'idf.py gdb'
# the --gdbinit arg, but it's nice to have it neatly abstracted for any other changes I might
# want to make.
target remote :3333
symbol-file build/spot-check-embedded.elf
set remote hardware-watchpoint-limit 2
mon reset halt
flushregs
thb app_main
c
