
set(LIBC_SOURCES
	assert.cpp
	heap.cpp
	libc.cpp
	libcxx.cpp
	microthread.cpp
	print.cpp
	start.cpp
	write.cpp
  )

add_library(libc STATIC ${LIBC_SOURCES})
target_include_directories(libc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(libc PUBLIC tinyprintf)
target_compile_definitions(libc PRIVATE NATIVE_MEM_SYSCALLS=1)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	set_source_files_properties(
			kernel/panic.cpp
		PROPERTIES COMPILE_FLAGS -Wno-frame-address)
endif()
