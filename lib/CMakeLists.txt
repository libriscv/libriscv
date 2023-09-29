#
# C++17 RISC-V emulator library
#

# DEBUG allows memory alignment checks and other things
option(RISCV_DEBUG  "Enable extra checks in the RISC-V machine" OFF)
# Enable and disable various RISC-V instruction
# set extensions. Not recommended to disable any.
option(RISCV_EXT_A  "Enable RISC-V atomic instructions" ON)
option(RISCV_EXT_C  "Enable RISC-V compressed instructions" ON)
option(RISCV_EXT_V  "Enable RISC-V vector instructions" OFF)
# Enable 128-bit architecture emulation
option(RISCV_128I   "Enable RV128G" OFF)
# EXPERIMENTAL enables some high-performance interpreter
# features that may be unstable.
option(RISCV_EXPERIMENTAL  "Enable experimental features" OFF)
# MEMORY_TRAPS allows you to trap writes to uncacheable
# pages in memory. Cached pages can only be trapped once.
option(RISCV_MEMORY_TRAPS  "Enable memory page traps" ON)
# MULTIPROCESS enables experimental features that allow
# executing RISC-V guest functions in parallel.
option(RISCV_MULTIPROCESS  "Enable multiprocessing" OFF)
# SUPERVISOR enables full-system emulation. WIP.
option(RISCV_SUPERVISOR  "Enable supervisor mode" OFF)

option(RISCV_EASTL "Enable fast EASTL containers" OFF)

set(THREADED_IS_DEFAULT OFF)
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
	OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(THREADED_IS_DEFAULT ON)
endif()
# Threaded simulation uses computed goto, and is not supported
# on all compilers/systems.
option(RISCV_THREADED "Enable fastest simulation mode" THREADED_IS_DEFAULT)

if (RISCV_EXPERIMENTAL)
	option(RISCV_BINARY_TRANSLATION  "Enable exp. binary translation" OFF)
	option(RISCV_TAILCALL_DISPATCH   "Enable exp. tailcall dispatch" OFF)
endif()

set (SOURCES
		libriscv/cpu.cpp
		libriscv/debug.cpp
		libriscv/decode_bytecodes.cpp
		libriscv/decoder_cache.cpp
		libriscv/machine.cpp
		libriscv/memory.cpp
		libriscv/memory_elf.cpp
		libriscv/memory_rw.cpp
		libriscv/multiprocessing.cpp
		libriscv/native_libc.cpp
		libriscv/native_threads.cpp
		libriscv/posix/minimal.cpp
		libriscv/posix/signals.cpp
		libriscv/posix/threads.cpp
		libriscv/posix/socket_calls.cpp
		libriscv/rv32i.cpp
		libriscv/rv64i.cpp
		libriscv/serialize.cpp
		libriscv/util/crc32c.cpp
	)
if (MINGW_TOOLCHAIN OR MINGW)
	# TODO: Port this to MSVC/Clang-cl
	list(APPEND SOURCES
		libriscv/win32/system_calls.cpp
	)
elseif(WIN32)
	# TODO: Port system calls to WIN32
elseif(UNIX)
	list(APPEND SOURCES
		libriscv/linux/system_calls.cpp
	)
endif()
if (RISCV_THREADED)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
	 AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0
	 AND RISCV_TAILCALL_DISPATCH)
	 	# Experimental tail-call dispatch
		list(APPEND SOURCES
			libriscv/tailcall_dispatch.cpp
		)
	else()
		list(APPEND SOURCES
			libriscv/threaded_dispatch.cpp
		)
	endif()
else()
	list(APPEND SOURCES
		libriscv/bytecode_dispatch.cpp
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

if (NOT WIN32 OR MINGW_TOOLCHAIN)
	target_compile_options(riscv PRIVATE -Wall -Wextra)
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
if (RISCV_EXT_V)
	target_compile_definitions(riscv PUBLIC RISCV_EXT_VECTOR=32)
endif()

if (RISCV_128I)
	target_compile_definitions(riscv PUBLIC RISCV_128BIT_ISA=1)
	target_sources(riscv PUBLIC
		${CMAKE_CURRENT_LIST_DIR}/libriscv/rv128i.cpp
	)
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


if (RISCV_EASTL)
	# EASTL submodule
	find_package(Git REQUIRED)

	if(NOT EXISTS EASTL/CMakeLists.txt)
	execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --depth 1 -- EASTL
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		COMMAND_ERROR_IS_FATAL ANY)
	endif()
	add_subdirectory(EASTL)
	target_compile_definitions(EASTL PUBLIC EASTL_EXCEPTIONS_ENABLED=1)

	target_link_libraries(riscv PUBLIC EASTL)
	target_compile_definitions(riscv PUBLIC EASTL_ENABLED=1)
endif()
