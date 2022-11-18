#
# C++17 RISC-V emulator library
#

# DEBUG allows memory alignment checks and other things
option(RISCV_DEBUG  "Enable extra checks in the RISC-V machine" OFF)
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
# SUPERVISOR enables full-system emulation. WIP.
option(RISCV_SUPERVISOR  "Enable supervisor mode" OFF)

if (RISCV_EXPERIMENTAL)
	option(RISCV_DECODER_COMPRESS    "Enable compressed decoder data" ON)
	option(RISCV_BINARY_TRANSLATION  "Enable exp. binary translation" OFF)
endif()

set (SOURCES
		libriscv/cpu.cpp
		libriscv/debug.cpp
		libriscv/decoder_cache.cpp
		libriscv/machine.cpp
		libriscv/memory.cpp
		libriscv/memory_rw.cpp
		libriscv/multiprocessing.cpp
		libriscv/native_libc.cpp
		libriscv/native_threads.cpp
		libriscv/posix/signals.cpp
		libriscv/posix/threads.cpp
		libriscv/posix/socket_calls.cpp
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

if (RISCV_FASTSIM)
	target_compile_definitions(riscv PUBLIC RISCV_FAST_SIMULATOR=1)
	message(WARNING "RISCV_INBOUND_JUMPS_ONLY disables virtual execute memory")
	target_compile_definitions(riscv PUBLIC RISCV_INBOUND_JUMPS_ONLY=1)
endif()

if (RISCV_FASTSIM AND RISCV_EXPERIMENTAL AND NOT RISCV_DEBUG)
	# The rewriter changes instruction bytes and is incompatible
	# with a decoder that reads instructions from execute-memory.
	target_compile_definitions(riscv PUBLIC RISCV_DECODER_REWRITER=1)
	if (RISCV_DECODER_COMPRESS)
		# Reduce decoder structure to 12 bytes
		target_compile_definitions(riscv PUBLIC RISCV_DECODER_COMPRESS=1)
		# Compressed instructions require extra decoding data
		if (NOT RISCV_EXT_C)
			# Reduces decoder structure to 8 bytes
			#target_compile_definitions(riscv PUBLIC RISCV_SUPER_COMPRESSED=1)
		endif()
	endif()
endif()
if (RISCV_MULTIPROCESS)
	find_package(Threads REQUIRED)
	target_link_libraries(riscv PUBLIC Threads::Threads)
	target_compile_definitions(riscv PUBLIC RISCV_MULTIPROCESS=1)
endif()
if (RISCV_MEMORY_TRAPS)
	target_compile_definitions(riscv PUBLIC RISCV_MEMORY_TRAPS=1)
endif()
if (RISCV_SUPERVISOR)
	target_compile_definitions(riscv PUBLIC RISCV_SUPERVISOR_MODE=1)
endif()
if (RISCV_BINARY_TRANSLATION)
	target_compile_definitions(riscv PUBLIC RISCV_BINARY_TRANSLATION=1)
	target_compile_definitions(riscv PRIVATE RISCV_TRANSLATION_CACHE=1)
	target_link_libraries(riscv PUBLIC dl)
endif()
if (WIN32 OR MINGW_TOOLCHAIN)
	target_link_libraries(riscv PUBLIC wsock32 ws2_32)
endif()
