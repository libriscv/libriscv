cmake_minimum_required(VERSION 3.14)
project(guest LANGUAGES C CXX)

set(WARNINGS  "-Wall -Wextra")
set(COMMON    "-fno-math-errno -fno-threadsafe-statics -fno-stack-protector")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WARNINGS} -std=gnu++20 ${COMMON}")

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

add_executable(guest program.cpp attributes.cpp env.cpp)
target_link_libraries(guest -static)
target_link_libraries(guest "-Wl,-Ttext-segment=0x400000")
target_link_libraries(guest "-Wl,--undefined=fast_exit")
# The guest heap must be the host's arena, which is what the wrapped allocator
# and string/memory functions below route every allocation through
target_link_libraries(guest "-Wl,--wrap=memcpy,--wrap=memset,--wrap=memcmp,--wrap=memmove")
target_link_libraries(guest "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc")
target_link_libraries(guest "-Wl,--wrap=strlen,--wrap=strcmp,--wrap=strncmp")
set_target_properties(guest
	PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
