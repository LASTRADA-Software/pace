# SPDX-License-Identifier: Apache-2.0
# pace-kv — the cache daemon. Links the whole Pace library.

add_executable(pace-kv main.cpp)
target_link_libraries(pace-kv PRIVATE Pace)

install(TARGETS pace-kv
    RUNTIME DESTINATION "${PACE_INSTALL_BINDIR}"
    COMPONENT "${PACE_APP_COMPONENT}"
)
