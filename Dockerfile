FROM ubuntu:latest

RUN apt update && apt install -y \
	cmake \
	clang-14 lld-14 \
	g++-12-riscv64-linux-gnu

ENV CXX=clang++-14

COPY lib /app/lib
COPY emulator/build.sh /app/emulator/build.sh
COPY emulator/CMakeLists.txt /app/emulator/CMakeLists.txt
COPY emulator/src /app/emulator/src
COPY binaries/measure_mips/fib.c /app/emulator/fib.c

# Emulator program
WORKDIR /app/emulator
RUN ./build.sh && cp /app/emulator/.build/rvlinux /app

# Faster emulator program (no C-extension)
WORKDIR /app/emulator/.build
RUN cmake .. -DRISCV_EXT_C=OFF -DRISCV_BINARY_TRANSLATION=ON && make -j6 && cp rvlinux /app/rvlinux-fast

# Clean up
RUN rm -rf /app/emulator/.build

# Example program
WORKDIR /app
RUN riscv64-linux-gnu-gcc-12 -march=rv32g -mabi=ilp32d -static -O2 -nostdlib -ffreestanding emulator/fib.c -o fib

# Provdide a path to your cli apps executable
WORKDIR /app
ENTRYPOINT [ "./rvlinux" ]
