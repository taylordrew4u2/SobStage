# ctest -P script: the Windows side of portable test sources, read by the
# compiler on a machine that is not Windows.
#
# The Windows CI runner is the only place those #if branches are compiled, so
# a name declared inside `#if ! defined(_WIN32)` and used after its `#endif`
# broke the Windows build and the Windows release without a single Linux or
# Mac check noticing. This compiles each file for syntax only with _WIN32
# defined, and fails on any error reported in the project's own files. Errors
# inside the system's headers -- which do not expect _WIN32 on Linux -- are not
# the project's and are ignored.
#
#   -DCOMPILER=<c++ compiler> -DSOURCE_DIR=<repo root> -DFILES=<a;b;...>
if (NOT DEFINED COMPILER OR NOT DEFINED SOURCE_DIR OR NOT DEFINED FILES)
    message(FATAL_ERROR "Pass -DCOMPILER=... -DSOURCE_DIR=... -DFILES=...")
endif()

set(failures "")

foreach (file IN LISTS FILES)
    execute_process(
        COMMAND "${COMPILER}" -std=c++17 -fsyntax-only -D_WIN32
                -I "${SOURCE_DIR}/Source" -I "${SOURCE_DIR}/Tests"
                "${SOURCE_DIR}/${file}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    # A ";" in a message would split it into list items.
    string(REPLACE ";" "," err "${err}")
    string(REGEX MATCHALL "${SOURCE_DIR}/[^:\n]+:[0-9]+:[0-9]+: (fatal )?error: [^\n]*" ours "${err}")

    foreach (line IN LISTS ours)
        string(APPEND failures "\n  ${line}")
    endforeach()
endforeach()

if (NOT failures STREQUAL "")
    message(FATAL_ERROR "The Windows branch of a portable source does not compile:${failures}")
endif()

message(STATUS "The Windows branches of ${FILES} compile")
