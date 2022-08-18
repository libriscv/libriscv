cmake_minimum_required(VERSION 3.1.0)
project(hello_world C)

option(MINIMAL     "Minimal build" OFF)
option(DEBUGGING   "Debugging build" OFF)
option(GCSECTIONS  "Garbage collect empty sections" OFF)
set(VERSION_FILE   "symbols.map" CACHE STRING "Retained symbols file")

set(RISCV_ABI "-march=rv64gv_zba -mabi=lp64d")
set(WARNINGS  "-Wall -Wextra")
set(COMMON    "-O2 -ffast-math -fno-math-errno -fno-stack-protector")
set(CMAKE_C_FLAGS "${WARNINGS} ${RISCV_ABI} -std=gnu99 ${COMMON}")

if (DEBUGGING)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g")
elseif (MINIMAL)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Os")
endif()

if (GCSECTIONS)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-gc-sections")
	file(STRINGS "${CMAKE_SOURCE_DIR}/${VERSION_FILE}" SYMBOLS)
	foreach(SYMBOL ${SYMBOLS})
		if (NOT ${SYMBOL} STREQUAL "")
			set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--undefined=${SYMBOL}")
		endif()
	endforeach()
endif()


set(SOURCES
	src/stream.c
)

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "") # remove -rdynamic

add_executable(stream ${SOURCES})
target_link_libraries(stream -static)
# place ELF into the top binary folder
set_target_properties(stream
	PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if (NOT DEBUGGING)
	# remove all symbols except the public API
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--retain-symbols-file=${CMAKE_SOURCE_DIR}/${VERSION_FILE}")
	set_target_properties(stream PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/${VERSION_FILE})
endif()
