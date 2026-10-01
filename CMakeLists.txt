cmake_minimum_required(VERSION 3.10)
project(tinykv C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(kvserver
    server.c
    linebuf.c
    command.c
    hashmap.c
    wal.c
)
target_compile_options(kvserver PRIVATE -Wall -Wextra)
target_link_libraries(kvserver PRIVATE Threads::Threads)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME regression
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/regression_test.py $<TARGET_FILE:kvserver>)
endif()
