cmake_minimum_required(VERSION 3.16)
project(dshm_malloc C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings
add_compile_options(-Wall -Wextra -Werror -Wpedantic)

# Library target — start with L1 only; L2 (dshm_l2.c) is added in Task 5
add_library(dshm_malloc STATIC
    src/dshm_l1.c
)
target_include_directories(dshm_malloc PUBLIC include src)

# Tests
enable_testing()
add_subdirectory(tests)
