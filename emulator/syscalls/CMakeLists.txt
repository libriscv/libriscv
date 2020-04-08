
set(SYSCALLS_SOURCES
	src/native_libc.cpp
	src/native_threads.cpp
	src/posix_threads.cpp
	src/syscalls.cpp
)

add_library(syscalls STATIC ${SYSCALLS_SOURCES})
set_target_properties(syscalls PROPERTIES CXX_STANDARD 17)
target_include_directories(syscalls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(syscalls riscv)
