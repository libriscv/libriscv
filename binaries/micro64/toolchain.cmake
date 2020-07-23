SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_CROSSCOMPILING 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

set(COMPILER_DIR $ENV{HOME}/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.3.0-1.1.1/.content)
include_directories(SYSTEM
	${COMPILER_DIR}/riscv-none-embed/include/c++/8.3.0
	${COMPILER_DIR}/riscv-none-embed/include/c++/8.3.0/riscv-none-embed
	${COMPILER_DIR}/riscv-none-embed/include
	${COMPILER_DIR}/lib/gcc/riscv-none-embed/8.3.0/include-fixed
	${COMPILER_DIR}/lib/gcc/riscv-none-embed/8.3.0/include
)
