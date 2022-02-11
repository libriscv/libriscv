export DEBUG=1
source build.sh
echo "=================================="
echo " Start emulator with DEBUG=1 now. "
echo "=================================="
riscv64-unknown-linux-gnu-gdb -x debug.gdb
