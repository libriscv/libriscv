cmake_minimum_required(VERSION 3.14)
project(guest LANGUAGES C CXX)

set(WARNINGS  "-Wall -Wextra")
set(COMMON    "-fno-math-errno -fno-threadsafe-statics -fno-stack-protector")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WARNINGS} -std=gnu++20 ${COMMON}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -ffat-lto-objects")

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

# Generate host function stubs from the shared JSON definition
set(JSON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../host_functions.json")
set(PYPROGRAM "${CMAKE_CURRENT_SOURCE_DIR}/../generate.py")
set(GEN_DIR   "${CMAKE_CURRENT_BINARY_DIR}/dyncalls")
set(GEN_FILES "${GEN_DIR}/host_functions")

file(MAKE_DIRECTORY ${GEN_DIR})
add_custom_command(
    OUTPUT ${GEN_FILES}.h ${GEN_FILES}.c
    COMMAND python3 ${PYPROGRAM} --verbose -j ${JSON_FILE} -o ${GEN_FILES}
    DEPENDS ${PYPROGRAM} ${JSON_FILE}
    COMMENT "Generating host function stubs from JSON"
)
add_custom_target(generate_dyncalls ALL DEPENDS ${GEN_FILES}.h ${GEN_FILES}.c)
set_source_files_properties(${GEN_FILES}.h ${GEN_FILES}.c PROPERTIES GENERATED TRUE)

function(add_guest_binary NAME)
    add_executable(${NAME} ${ARGN} env.cpp ${GEN_FILES}.c)
    add_dependencies(${NAME} generate_dyncalls)
    target_include_directories(${NAME} PRIVATE ${GEN_DIR})
    target_link_libraries(${NAME} -static)
    target_link_libraries(${NAME} "-Wl,-Ttext-segment=0x400000")
    target_link_libraries(${NAME} "-Wl,--undefined=fast_exit")
    target_link_libraries(${NAME} "-Wl,--undefined=dyncall_table")
    target_link_libraries(${NAME} "-Wl,--wrap=memcpy,--wrap=memset,--wrap=memcmp,--wrap=memmove")
    target_link_libraries(${NAME} "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc")
    target_link_libraries(${NAME} "-Wl,--wrap=strlen,--wrap=strcmp,--wrap=strncmp")
    set_target_properties(${NAME}
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endfunction()

add_guest_binary(guest program.cpp)
