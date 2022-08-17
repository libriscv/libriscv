#
# C++17 RISC-V emulator library
#

option(RISCV_DEBUG  "Enable debugging features in the RISC-V machine" OFF)
option(RISCV_ICACHE "Enable instruction decoder cache" ON)
option(RISCV_EXT_A  "Enable RISC-V atomic instructions" ON)
option(RISCV_EXT_C  "Enable RISC-V compressed instructions" ON)
option(RISCV_EXT_F  "Enable RISC-V floating-point instructions" ON)
option(RISCV_EXT_V  "Enable RISC-V vector instructions" OFF)
option(RISCV_EXPERIMENTAL  "Enable experimental features" OFF)
option(RISCV_MEMORY_TRAPS  "Enable memory page traps" OFF)
option(RISCV_MULTIPROCESS  "Enable multiprocessing" ON)
option(RISCV_USE_RH_HASH "Enable robin-hood hashing for page tables" OFF)

if (RISCV_EXPERIMENTAL)
	option(RISCV_BINARY_TRANSLATION  "Enable binary translation" OFF)
	option(RISCV_FAST_SIMULATOR      "Enable fast simulator mode" ON)
	option(RISCV_FLAT_MEMORY         "Enable faster flat memory mode" OFF)
endif()

if (RISCV_USE_RH_HASH)
	include(FetchContent)
	FetchContent_Declare(
		robinhood
		GIT_REPOSITORY https://github.com/martinus/robin-hood-hashing.git
		CONFIGURE_COMMAND ""
		BUILD_COMMAND ""
		GIT_TAG master
		GIT_SHALLOW TRUE
		GIT_PROGRESS TRUE)

	FetchContent_GetProperties(robinhood)
	if (NOT robinhood_POPULATED)
		FetchContent_Populate(robinhood)
	endif()

	add_library(robinhood INTERFACE)
	target_include_directories(robinhood INTERFACE ${robinhood_SOURCE_DIR}/src/include)
	message(STATUS "robinhood sources: ${robinhood_SOURCE_DIR}")
endif()

set (SOURCES
		libriscv/cpu.cpp
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
endif()
if (RISCV_ICACHE)
	list(APPEND SOURCES
		libriscv/decoder_cache.cpp
	)
endif()
if (RISCV_ICACHE AND RISCV_EXPERIMENTAL)
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
if (RISCV_USE_RH_HASH)
	target_link_libraries(riscv PUBLIC robinhood)
	target_compile_definitions(riscv PUBLIC RISCV_USE_RH_HASH=1)
endif()
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
if (RISCV_ICACHE)
	target_compile_definitions(riscv PUBLIC RISCV_INSTR_CACHE=1)
endif()
if (RISCV_ICACHE AND RISCV_EXPERIMENTAL)
	message(WARNING "RISCV_INBOUND_JUMPS_ONLY disables virtual execute memory")
	target_compile_definitions(riscv PUBLIC RISCV_INBOUND_JUMPS_ONLY=1)
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
	target_compile_definitions(riscv PUBLIC RISCV_BINARY_TRANSLATION=1)
	target_compile_definitions(riscv PRIVATE RISCV_TRANSLATION_CACHE=1)
	target_link_libraries(riscv PUBLIC dl)
endif()
if (RISCV_FLAT_MEMORY)
	target_compile_definitions(riscv PUBLIC RISCV_FLAT_MEMORY=1)
endif()
if (RISCV_ICACHE AND RISCV_FAST_SIMULATOR)
	# The fast simulator is incompatible with RISCV_DEBUG because it
	# modifies the instruction stream.
	if (RISCV_DEBUG)
		message(WARNING "RISCV_FAST_SIMULATOR disabled by RISCV_DEBUG")
	else()
		target_compile_definitions(riscv PUBLIC RISCV_FAST_SIMULATOR=1)
	endif()
endif()
if (WIN32 OR MINGW_TOOLCHAIN)
	target_link_libraries(riscv PUBLIC wsock32 ws2_32)
endif()
