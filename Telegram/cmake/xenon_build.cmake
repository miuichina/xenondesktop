find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} log -1 --format=%H%n%ct
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE xenon_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
string(REPLACE "\n" ";" xenon_commit "${xenon_commit}")
list(GET xenon_commit 0 XENON_COMMIT_HASH)
list(GET xenon_commit 1 XENON_COMMIT_TIME)
set(XENON_BUILD_NUMBER "0" CACHE STRING "Xenon release build number.")
if (NOT XENON_BUILD_NUMBER MATCHES "^[0-9]+$" OR XENON_BUILD_NUMBER GREATER 4294967295)
    message(FATAL_ERROR "XENON_BUILD_NUMBER must fit in an unsigned 32-bit integer.")
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/xenon_build.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/gen/xenon_build.h
    @ONLY
)
execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE xenon_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
file(READ "${xenon_git_dir}/HEAD" xenon_git_head)
string(STRIP "${xenon_git_head}" xenon_git_head)
if (xenon_git_head MATCHES "^ref: (.+)$")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute --git-path ${CMAKE_MATCH_1}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE xenon_git_ref
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${xenon_git_ref}
    )
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${xenon_git_dir}/HEAD
)
execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --path-format=absolute --git-path packed-refs
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE xenon_packed_refs
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if (EXISTS "${xenon_packed_refs}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${xenon_packed_refs}
    )
endif()
