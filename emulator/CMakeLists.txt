cmake_minimum_required(VERSION 3.9.4)
project(riscv CXX)

option(LTO         "Enable interprocedural optimizations" ON)
option(NATIVE      "Enable native instructions" ON)
option(SANITIZE    "Enable sanitizers" OFF)
option(GPROF       "Enable profiling with gprof" OFF)
option(LINKER_GC   "Enable linker section garbage collection" OFF)
option(STATIC_BUILD "Built the emulator statically" ON)

set(SOURCES
	src/main.cpp
)

if (SANITIZE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined")
endif()
if (NATIVE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()
if (GPROF)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg -g -fno-inline")
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
# GC-sections
if (LINKER_GC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-gc-sections")
endif()

if (LTO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT supported OUTPUT error)
	if (supported)
		message(STATUS "IPO / LTO enabled")
		set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
	else()
		message(STATUS "IPO / LTO not supported: <${error}>")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
	endif()
endif()

add_subdirectory(../lib lib)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
function (add_emulator NAME)
	add_executable(${NAME} ${SOURCES})
	target_link_libraries(${NAME} PUBLIC riscv PRIVATE Threads::Threads)
	message(STATUS "ARGN is ${ARGN}")
	target_compile_definitions(${NAME} PRIVATE ${ARGN})
	if (STATIC_BUILD)
		target_link_libraries(${NAME} PUBLIC -static)
	endif()

	if (SANITIZE)
		target_link_libraries(${NAME} PUBLIC "-fsanitize=address,undefined")
	endif()
	if (GPROF)
		target_link_libraries(${NAME} PUBLIC "-pg")
	endif()

endfunction()

add_emulator(rvlinux  EMULATOR_MODE_LINUX=1)
add_emulator(rvnewlib EMULATOR_MODE_NEWLIB=1)
add_emulator(rvmicro  EMULATOR_MODE_MICRO=1)
