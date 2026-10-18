# SPDX-License-Identifier: Apache-2.0
# fastcached — the cache daemon. Links the whole Pace library.

add_executable(fastcached main.cpp)
target_link_libraries(fastcached PRIVATE Pace)

install(TARGETS fastcached
    RUNTIME DESTINATION "${FASTCACHED_INSTALL_BINDIR}"
    COMPONENT "${FASTCACHED_APP_COMPONENT}"
)
