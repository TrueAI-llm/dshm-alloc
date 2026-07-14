# Test helpers: we use a simple C test harness, not GoogleTest, to avoid
# external dependencies. Each test file has a main() and returns 0 on success.

# Small-pool test mode: override DSHM_NUM_CHUNKS and DSHM_CHUNK_SIZE so tests
# can mmap a manageable pool (64 chunks × 1MB = 64MB) instead of 128T.
# Tests compile library sources directly (not the pre-built library) so the
# macro overrides apply to both test code and library code.
set(DSHM_TEST_NUM_CHUNKS 512)
set(DSHM_TEST_CHUNK_SIZE  0x1000000ULL)  # 16MB, 8× jemalloc extent size  # 4MB, matches jemalloc extent size

set(DSHM_TEST_DEFS
    DSHM_NUM_CHUNKS=${DSHM_TEST_NUM_CHUNKS}
    DSHM_CHUNK_SIZE=${DSHM_TEST_CHUNK_SIZE})

set(DSHM_TEST_INC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src)

set(DSHM_TEST_SRC
    ${CMAKE_SOURCE_DIR}/src/dshm_l1.c
    ${CMAKE_SOURCE_DIR}/src/dshm_l2.c)

# Task 2: L1 cache fast path
add_executable(test_l1_cache test_l1_cache.c ${DSHM_TEST_SRC})
target_include_directories(test_l1_cache PRIVATE ${DSHM_TEST_INC})
target_link_libraries(test_l1_cache ${JEMALLOC_LIBRARIES} pthread)
target_compile_definitions(test_l1_cache PRIVATE ${DSHM_TEST_DEFS})
add_test(NAME l1_cache COMMAND test_l1_cache)

# Task 3: L1 CAS slow path
add_executable(test_l1_cas test_l1_cas.c ${DSHM_TEST_SRC})
target_include_directories(test_l1_cas PRIVATE ${DSHM_TEST_INC})
target_link_libraries(test_l1_cas ${JEMALLOC_LIBRARIES} pthread)
target_compile_definitions(test_l1_cas PRIVATE ${DSHM_TEST_DEFS})
add_test(NAME l1_cas COMMAND test_l1_cas)

# Task 4: L1 concurrent multi-thread stress test
add_executable(test_l1_concurrent test_l1_concurrent.c ${DSHM_TEST_SRC})
target_include_directories(test_l1_concurrent PRIVATE ${DSHM_TEST_INC})
target_link_libraries(test_l1_concurrent ${JEMALLOC_LIBRARIES} pthread)
target_compile_definitions(test_l1_concurrent PRIVATE ${DSHM_TEST_DEFS})
add_test(NAME l1_concurrent COMMAND test_l1_concurrent)

# Task 6: L2 dshm_init validation
add_executable(test_l2_init test_l2_init.c ${DSHM_TEST_SRC})
target_include_directories(test_l2_init PRIVATE ${DSHM_TEST_INC})
target_link_libraries(test_l2_init ${JEMALLOC_LIBRARIES} pthread)
target_compile_definitions(test_l2_init PRIVATE ${DSHM_TEST_DEFS})
add_test(NAME l2_init COMMAND test_l2_init)

# Task 7: End-to-end integration test
add_executable(test_l2_malloc test_l2_malloc.c ${DSHM_TEST_SRC})
target_include_directories(test_l2_malloc PRIVATE ${DSHM_TEST_INC})
target_link_libraries(test_l2_malloc ${JEMALLOC_LIBRARIES} pthread)
target_compile_definitions(test_l2_malloc PRIVATE ${DSHM_TEST_DEFS})
add_test(NAME l2_malloc COMMAND test_l2_malloc)
