SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_CROSSCOMPILING 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

set(COMPILER_DIR $ENV{HOME}/xpack-riscv-none-embed-gcc-10.2.0-1.2)
set(GCC_VERSION 10.2.0)
set(LIBRARY_DIR ${COMPILER_DIR}/lib/gcc/riscv-none-embed/${GCC_VERSION} CACHE STRING "GCC libraries")

include_directories(SYSTEM
	${CMAKE_SOURCE_DIR}/libc/override
	${COMPILER_DIR}/riscv-none-embed/include/c++/${GCC_VERSION}
	${COMPILER_DIR}/riscv-none-embed/include/c++/${GCC_VERSION}/riscv-none-embed
	${COMPILER_DIR}/riscv-none-embed/include
	${COMPILER_DIR}/lib/gcc/riscv-none-embed/${GCC_VERSION}/include-fixed
	${COMPILER_DIR}/lib/gcc/riscv-none-embed/${GCC_VERSION}/include
)
