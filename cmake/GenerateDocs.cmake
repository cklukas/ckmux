# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# Materialize the two user-facing documents whose key appendix comes from the
# compiled registry. This runs the just-built ckmux binary: the generated
# table is evidence about the binary being packaged, not about a parser's
# approximation of its C++ source.

foreach(required CKMUX_BINARY CKMUX_MARKDOWN_TEMPLATE CKMUX_MAN_TEMPLATE CKMUX_OUTPUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "GenerateDocs.cmake requires -D${required}=...")
    endif()
endforeach()

file(MAKE_DIRECTORY "${CKMUX_OUTPUT_DIR}")

function(render_document format template output)
    execute_process(
        COMMAND "${CKMUX_BINARY}" "--internal-key-appendix=${format}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE appendix
        ERROR_VARIABLE problem)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "ckmux could not generate the ${format} key appendix (exit ${result}): ${problem}")
    endif()

    file(READ "${template}" content)
    string(REGEX MATCHALL "@CKMUX_KEY_APPENDIX@" markers "${content}")
    list(LENGTH markers marker_count)
    if(NOT marker_count EQUAL 1)
        message(FATAL_ERROR
            "${template} must contain @CKMUX_KEY_APPENDIX@ exactly once; found ${marker_count}")
    endif()
    string(REPLACE "@CKMUX_KEY_APPENDIX@" "${appendix}" content "${content}")
    file(WRITE "${output}" "${content}")
endfunction()

render_document(markdown "${CKMUX_MARKDOWN_TEMPLATE}" "${CKMUX_OUTPUT_DIR}/keys.md")
render_document(roff "${CKMUX_MAN_TEMPLATE}" "${CKMUX_OUTPUT_DIR}/ckmux.1")
