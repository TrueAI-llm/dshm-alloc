cmake_minimum_required(VERSION 3.16)
project(dshm_malloc C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings
add_compile_options(-Wall -Wextra -Werror -Wpedantic)
# extent_hooks_t functions must match jemalloc's exact signatures,
# which include parameters we don't use in all hooks.
add_compile_options(-Wno-unused-parameter)

# Find jemalloc
find_package(PkgConfig REQUIRED)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)

# Library target
add_library(dshm_malloc STATIC
    src/dshm_l1.c
    src/dshm_l2.c
)
target_include_directories(dshm_malloc PUBLIC include src)
target_include_directories(dshm_malloc PRIVATE ${JEMALLOC_INCLUDE_DIRS})
target_link_libraries(dshm_malloc PUBLIC ${JEMALLOC_LIBRARIES} pthread)

# Tests
enable_testing()
add_subdirectory(tests)
