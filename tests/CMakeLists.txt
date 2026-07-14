# Test helpers: we use a simple C test harness, not GoogleTest, to avoid
# external dependencies. Each test file has a main() and returns 0 on success.
#
# Test targets are added incrementally as each task creates the corresponding
# test source file. This avoids CMake errors about missing source files.

# Task 2: L1 cache fast path
add_executable(test_l1_cache test_l1_cache.c)
target_link_libraries(test_l1_cache dshm_malloc pthread)
add_test(NAME l1_cache COMMAND test_l1_cache)

# Task 3 will append test_l1_cas here.
add_executable(test_l1_cas test_l1_cas.c)
target_link_libraries(test_l1_cas dshm_malloc pthread)
add_test(NAME l1_cas COMMAND test_l1_cas)
# Task 4 will append test_l1_concurrent here.
add_executable(test_l1_concurrent test_l1_concurrent.c)
target_link_libraries(test_l1_concurrent dshm_malloc pthread)
add_test(NAME l1_concurrent COMMAND test_l1_concurrent)
# Task 6 will append test_l2_init here.
# Task 7 will append test_l2_malloc here.
