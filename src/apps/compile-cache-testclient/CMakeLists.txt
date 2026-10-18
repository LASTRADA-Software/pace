# SPDX-License-Identifier: Apache-2.0
# Standalone compile-cache test client. Links the Pace library so it
# reuses the exact PathCanon / CompileValue codec the server uses — the
# canonicalization parity contract is exercised, not reimplemented.

# ProcessRunner.cpp is compiled in from the launcher rather than copied, exactly
# as pace-node does with it: spawning a compiler is ambient I/O with
# a wildly different implementation per platform, and a fourth hand-written
# version here is what issue #84 was about.
add_executable(compile-cache-testclient
    main.cpp
    TestClientCli.cpp
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ProcessRunner.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/ReplayGuard.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/pace-cc/DirectManifest.cpp")
target_include_directories(compile-cache-testclient PRIVATE "${CMAKE_SOURCE_DIR}/src/apps/pace-cc")
target_link_libraries(compile-cache-testclient PRIVATE Pace)

# The CLI is a pure function over argv, so it is unit-testable without a daemon
# or a compiler. Its own binary rather than a row in PaceTest: the test
# glob only reaches src/Pace, and this tool is gated off by default.
if(PACE_BUILD_TESTS)
    add_executable(compile-cache-testclient-tests
        TestClientCli.cpp
        TestClientCli_test.cpp
    )
    target_include_directories(compile-cache-testclient-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(compile-cache-testclient-tests PRIVATE Pace Catch2::Catch2WithMain)
    include(Catch)
    # `SKIP_RETURN_CODE 4` tells ctest a Catch2 skip is a skip and not a failure
    # (#499), and under the Catch2 the top-level CMakeLists pins EXACTLY it is exact: a
    # run whose every case skipped exits 4 and any failure exits 42, whatever the
    # count. On 3.6.0 the exit code was the failure count, and a case failing four
    # assertions was scored *skipped* (#1128). `catch-skip-return-code` requires the
    # property, and `catch-skip-exit-contract` that it still means what it says.
    catch_discover_tests(compile-cache-testclient-tests PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION "${PACE_ERROR_DIALOG_ENVIRONMENT}")
endif()
