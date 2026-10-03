cmake_minimum_required(VERSION 3.13)
project(tinykv C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# -DTINYKV_SANITIZE=address,undefined   or   -DTINYKV_SANITIZE=thread
set(TINYKV_SANITIZE "" CACHE STRING "Sanitizers to build with (e.g. address,undefined or thread)")
if(TINYKV_SANITIZE)
    add_compile_options(-fsanitize=${TINYKV_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${TINYKV_SANITIZE})
endif()

option(TINYKV_WERROR "Treat warnings as errors" OFF)
add_compile_options(-Wall -Wextra -Wpedantic)
if(TINYKV_WERROR)
    add_compile_options(-Werror)
endif()

# Linux/glibc APIs used: epoll, eventfd, accept4, SO_REUSEPORT, getline,
# pthread_rwlockattr_setkind_np. Defined here once rather than per file.
add_compile_definitions(_GNU_SOURCE)

find_package(Threads REQUIRED)

# Everything except main(), so unit tests can link against it.
add_library(kvcore STATIC
    linebuf.c
    command.c
    hashmap.c
    crc32.c
    wal.c
    snapshot.c
    store.c
)
target_include_directories(kvcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kvcore PUBLIC Threads::Threads)

add_executable(kvserver server.c)
target_link_libraries(kvserver PRIVATE kvcore)

add_executable(kvbench bench/kvbench.c)
target_link_libraries(kvbench PRIVATE Threads::Threads)

# ---- tests ----
enable_testing()

foreach(t command linebuf hashmap wal snapshot store)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE kvcore)
    add_test(NAME unit_${t} COMMAND test_${t})
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME regression
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/regression_test.py $<TARGET_FILE:kvserver>)
endif()
