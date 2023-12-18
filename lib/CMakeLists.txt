#
# C++20 RISC-V emulator library
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
# Enable Floating-point Control and Status Register emulation
option(RISCV_FCSR   "Enable FCSR emulation" OFF)
# EXPERIMENTAL enables some high-performance interpreter
# features that may be unstable.
option(RISCV_EXPERIMENTAL  "Enable experimental features" OFF)
# MEMORY_TRAPS allows you to trap writes to uncacheable
# pages in memory. Cached pages can only be trapped once.
option(RISCV_MEMORY_TRAPS  "Enable memory page traps" ON)
# BINARY_TRANSLATION will make libriscv generate portable C-code,
# and invoke a system compiler to execute a program much faster.
option(RISCV_BINARY_TRANSLATION  "Enable exp. binary translation" OFF)
# FLAT_RW_ARENA simplifies the program arena, making the heap area always read-write.
# Memory before the heap and outside of the arena behaves like before.
option(RISCV_FLAT_RW_ARENA       "Enable performant flat read-write arena" ON)

set(THREADED_IS_DEFAULT OFF)
# Threaded simulation uses computed goto, and is not supported
# on all compilers/systems.
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang"
	OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	option(RISCV_THREADED "Enable fastest simulation mode" ON)
else()
	option(RISCV_THREADED "Enable fastest simulation mode" OFF)
endif()

# TAILCALL_DISPATCH enables clang-based compilers to use musttail dispatch.
option(RISCV_TAILCALL_DISPATCH   "Enable exp. tailcall dispatch" OFF)

if (RISCV_EXPERIMENTAL)
	# MULTIPROCESS enables experimental features that allow
	# executing RISC-V guest functions in parallel using a custom API.
	option(RISCV_MULTIPROCESS        "Enable multiprocessing" OFF)
endif()
if (RISCV_EXPERIMENTAL AND RISCV_BINARY_TRANSLATION)
	# LIBTCC will embed the TCC compiler library, using it for binary translation.
	option(RISCV_LIBTCC              "Enable binary translation with libtcc" OFF)
endif()

set (SOURCES
		libriscv/cpu.cpp
		libriscv/debug.cpp
		libriscv/decode_bytecodes.cpp
		libriscv/decoder_cache.cpp
		libriscv/machine.cpp
		libriscv/machine_defaults.cpp
		libriscv/memory.cpp
		libriscv/memory_elf.cpp
		libriscv/memory_mmap.cpp
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
if (MINGW_TOOLCHAIN OR MINGW OR WIN32)
	list(APPEND SOURCES
		libriscv/win32/system_calls.cpp
	)
elseif(UNIX)
	list(APPEND SOURCES
		libriscv/linux/system_calls.cpp
	)
endif()
if (RISCV_THREADED OR RISCV_TAILCALL_DISPATCH)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
	 AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0
	 AND RISCV_TAILCALL_DISPATCH)
	 	# Experimental tail-call dispatch
		message(STATUS "libriscv: Tail-call dispatch enabled")
		list(APPEND SOURCES
			libriscv/tailcall_dispatch.cpp
		)
	else()
		message(STATUS "libriscv: Threaded dispatch enabled")
		list(APPEND SOURCES
			libriscv/threaded_dispatch.cpp
		)
	endif()
else()
	message(STATUS "libriscv: Switch-based dispatch enabled")
	list(APPEND SOURCES
		libriscv/bytecode_dispatch.cpp
	)
endif()
if (RISCV_BINARY_TRANSLATION)
	list(APPEND SOURCES
		libriscv/tr_api.cpp
		libriscv/tr_emit.cpp
		libriscv/tr_translate.cpp
	)
	if (RISCV_LIBTCC)
		list(APPEND SOURCES libriscv/tr_tcc.cpp)
	else()
		list(APPEND SOURCES libriscv/tr_compiler.cpp)
	endif()
endif()

configure_file(libriscv_settings.h.in ${CMAKE_CURRENT_BINARY_DIR}/libriscv_settings.h)
configure_file(libriscv.pc.in ${CMAKE_CURRENT_BINARY_DIR}/libriscv.pc)

add_library(riscv ${SOURCES})
target_compile_features(riscv PUBLIC cxx_std_20)
target_include_directories(riscv PUBLIC .)
target_include_directories(riscv PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

if (NOT WIN32 OR MINGW_TOOLCHAIN)
	target_compile_options(riscv PRIVATE -Wall -Wextra)
endif()

if (RISCV_128I)
	target_sources(riscv PUBLIC
		${CMAKE_CURRENT_LIST_DIR}/libriscv/rv128i.cpp
	)
endif()

if (RISCV_MULTIPROCESS)
	find_package(Threads REQUIRED)
	target_link_libraries(riscv PUBLIC Threads::Threads)
endif()
if (RISCV_BINARY_TRANSLATION)
	target_link_libraries(riscv PUBLIC dl)
endif()
if (WIN32 OR MINGW_TOOLCHAIN)
	target_link_libraries(riscv PUBLIC wsock32 ws2_32)
endif()


if (RISCV_LIBTCC)
	#include(FetchContent)
	#FetchContent_Declare(
	#	libtcc
	#	GIT_REPOSITORY    https://github.com/fwsGonzo/libtcc-cmake
	#	GIT_TAG           master
	#)
	#FetchContent_MakeAvailable(libtcc)
	#target_compile_definitions(libtcc PUBLIC LIBTCC1_NAME="libtcc1.a")

	target_link_libraries(riscv PUBLIC libtcc.a)
endif()

if (${CMAKE_PROJECT_NAME} STREQUAL "libriscv")
	install(TARGETS riscv)
	install(FILES
		${CMAKE_CURRENT_BINARY_DIR}/libriscv_settings.h
		DESTINATION include/${PROJECT_NAME}
	)
	install(FILES
		libriscv/cached_address.hpp
		libriscv/common.hpp
		libriscv/cpu.hpp
		libriscv/cpu_inline.hpp
		libriscv/debug.hpp
		libriscv/decoded_exec_segment.hpp
		libriscv/decoder_cache.hpp
		libriscv/elf.hpp
		libriscv/instr_helpers.hpp
		libriscv/instruction_counter.hpp
		libriscv/instruction_list.hpp
		libriscv/machine.hpp
		libriscv/machine_inline.hpp
		libriscv/machine_vmcall.hpp
		libriscv/memory.hpp
		libriscv/memory_helpers_paging.hpp
		libriscv/memory_inline.hpp
		libriscv/memory_inline_pages.hpp
		libriscv/mmap_cache.hpp
		libriscv/native_heap.hpp
		libriscv/page.hpp
		libriscv/prepared_call.hpp
		libriscv/registers.hpp
		libriscv/riscvbase.hpp
		libriscv/rv32i_instr.hpp
		libriscv/rva.hpp
		libriscv/rvc.hpp
		libriscv/rvfd.hpp
		libriscv/rsp_server.hpp
		libriscv/threads.hpp
		libriscv/types.hpp

		DESTINATION include/${PROJECT_NAME}
	)
	install(FILES
		libriscv/linux/rsp_server.hpp

		DESTINATION include/${PROJECT_NAME}/linux
	)
	install(FILES
		libriscv/posix/filedesc.hpp
		libriscv/posix/signals.hpp

		DESTINATION include/${PROJECT_NAME}/posix
	)
	install(FILES
		libriscv/util/buffer.hpp
		libriscv/util/elf.h
		libriscv/util/function.hpp

		DESTINATION include/${PROJECT_NAME}/util
	)
	install(
		FILES ${CMAKE_CURRENT_BINARY_DIR}/libriscv.pc
		DESTINATION lib/pkgconfig
	)
endif()
