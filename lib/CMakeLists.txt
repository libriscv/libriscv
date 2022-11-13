#
# C++17 RISC-V emulator library
#

# DEBUG allows printing, stepping, breaking from
# a terminal so that you can step through programs.
# This feature should only be enabled when
# the terminal is used to debug problems.
option(RISCV_DEBUG  "Enable debugging features in the RISC-V machine" OFF)
# FASTSIM analyzes the code in order to interpret it
# faster than normally possible.
option(RISCV_FASTSIM "Enable fast simulation mode" ON)
# Enable and disable various RISC-V instruction
# set extensions. Not recommended to disable any.
option(RISCV_EXT_A  "Enable RISC-V atomic instructions" ON)
option(RISCV_EXT_C  "Enable RISC-V compressed instructions" ON)
option(RISCV_EXT_F  "Enable RISC-V floating-point instructions" ON)
option(RISCV_EXT_V  "Enable RISC-V vector instructions" OFF)
# EXPERIMENTAL enables some high-performance interpreter
# features that may be unstable.
option(RISCV_EXPERIMENTAL  "Enable experimental features" OFF)
# MEMORY_TRAPS allows you to trap writes to uncacheable
# pages in memory. Cached pages can only be trapped once.
option(RISCV_MEMORY_TRAPS  "Enable memory page traps" OFF)
# MULTIPROCESS enables experimental features that allow
# executing RISC-V guest functions in parallel.
option(RISCV_MULTIPROCESS  "Enable multiprocessing" OFF)

if (RISCV_EXPERIMENTAL)
	option(RISCV_BINARY_TRANSLATION  "Enable exp. binary translation" OFF)
	option(RISCV_FLAT_MEMORY         "Enable exp. faster flat memory mode" OFF)
endif()

set (SOURCES
		libriscv/cpu.cpp
		libriscv/decoder_cache.cpp
		libriscv/machine.cpp
		libriscv/memory.cpp
		libriscv/memory_rw.cpp
		libriscv/multiprocessing.cpp
		libriscv/native_libc.cpp
		libriscv/native_threads.cpp
		libriscv/posix_signals.cpp
		libriscv/posix_threads.cpp
		libriscv/socket_calls.cpp
		libriscv/rv32i.cpp
		libriscv/rv64i.cpp
		libriscv/rv128i.cpp
		libriscv/serialize.cpp
		libriscv/util/crc32c.cpp
	)
if (WIN32 OR MINGW_TOOLCHAIN)
	list(APPEND SOURCES
		libriscv/win32/system_calls.cpp
	)
else()
	list(APPEND SOURCES
		libriscv/linux/system_calls.cpp
	)
endif()
if (RISCV_DEBUG)
	list(APPEND SOURCES
		libriscv/debug.cpp
	)
	# Debugging must happen on unmodified instructions
	set(RISCV_EXPERIMENTAL OFF)
endif()
if (RISCV_FASTSIM AND RISCV_EXPERIMENTAL)
	list(APPEND SOURCES
		libriscv/decoder_rewriter.cpp
	)
endif()
if (RISCV_BINARY_TRANSLATION)
	list(APPEND SOURCES
		libriscv/tr_api.cpp
		libriscv/tr_compiler.cpp
		libriscv/tr_emit.cpp
		libriscv/tr_translate.cpp
	)
endif()

add_library(riscv ${SOURCES})
target_compile_features(riscv PUBLIC cxx_std_17)
target_include_directories(riscv PUBLIC .)
target_compile_options(riscv PRIVATE -Wall -Wextra)
if (RISCV_DEBUG)
	target_compile_definitions(riscv PUBLIC RISCV_DEBUG=1)
endif()
if (RISCV_EXT_A)
	target_compile_definitions(riscv PUBLIC RISCV_EXT_ATOMICS=1)
endif()
if (RISCV_EXT_C)
	target_compile_definitions(riscv PUBLIC RISCV_EXT_COMPRESSED=1)
endif()
if (RISCV_EXT_F)
	target_compile_definitions(riscv PUBLIC RISCV_EXT_FLOATS=1)
endif()
if (RISCV_EXT_V)
	target_compile_definitions(riscv PUBLIC RISCV_EXT_VECTOR=32)
endif()

# The fast simulator is incompatible with RISCV_DEBUG (for now)
if (RISCV_DEBUG)
	message(WARNING "RISCV_FAST_SIMULATOR disabled by RISCV_DEBUG")
elseif (RISCV_FASTSIM)
	target_compile_definitions(riscv PUBLIC RISCV_FAST_SIMULATOR=1)
	message(WARNING "RISCV_INBOUND_JUMPS_ONLY disables virtual execute memory")
	target_compile_definitions(riscv PUBLIC RISCV_INBOUND_JUMPS_ONLY=1)
endif()

if (RISCV_FASTSIM AND RISCV_EXPERIMENTAL)
	# The rewriter changes instruction bytes and is incompatible
	# with a decoder that reads instructions from execute-memory.
	target_compile_definitions(riscv PUBLIC RISCV_DECODER_REWRITER=1)
endif()
if (RISCV_MULTIPROCESS)
	find_package(Threads REQUIRED)
	target_link_libraries(riscv PUBLIC Threads::Threads)
	target_compile_definitions(riscv PUBLIC RISCV_MULTIPROCESS=1)
endif()
if (RISCV_MEMORY_TRAPS)
	target_compile_definitions(riscv PUBLIC RISCV_MEMORY_TRAPS=1)
endif()
if (RISCV_BINARY_TRANSLATION)
	if (RISCV_DEBUG)
		message(WARNING "RISCV_BINARY_TRANSLATION disabled by RISCV_DEBUG")
	else()
		target_compile_definitions(riscv PUBLIC RISCV_BINARY_TRANSLATION=1)
		target_compile_definitions(riscv PRIVATE RISCV_TRANSLATION_CACHE=1)
		target_link_libraries(riscv PUBLIC dl)
	endif()
endif()
if (RISCV_FLAT_MEMORY)
	target_compile_definitions(riscv PUBLIC RISCV_FLAT_MEMORY=1)
endif()
if (WIN32 OR MINGW_TOOLCHAIN)
	target_link_libraries(riscv PUBLIC wsock32 ws2_32)
endif()
