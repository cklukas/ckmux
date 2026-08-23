# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
#
# libFuzzer targets are opt-in: an ordinary build and a release build must not
# need a compiler runtime that supplies libFuzzer. The corpus lane enables this
# directory explicitly with CKMUX_BUILD_FUZZERS=ON, exactly as ckVision's own
# fuzz directory is enabled (the testing plan §6 — "LLVM clang lane, as upstream").
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "CKMUX_BUILD_FUZZERS requires an LLVM Clang toolchain with the libFuzzer runtime; "
        "AppleClang is insufficient - use the Ubuntu Clang CI lane or an installed LLVM "
        "toolchain (e.g. -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++)")
endif()

# The fuzzers are built with libFuzzer AND whatever CKMUX_SANITIZE asked for,
# because a fuzzer without a sanitizer only finds the crashes that would have
# crashed anyway: ASan and UBSan are what turn a silent out-of-bounds read into
# a finding. The testing plan §6 runs the corpora under ASan+UBSan for that reason.
set(CKMUX_FUZZ_SANITIZERS fuzzer)
if(CKMUX_SANITIZE)
    string(REPLACE "," ";" CKMUX_FUZZ_EXTRA_SANITIZERS "${CKMUX_SANITIZE}")
    list(APPEND CKMUX_FUZZ_SANITIZERS ${CKMUX_FUZZ_EXTRA_SANITIZERS})
endif()
list(JOIN CKMUX_FUZZ_SANITIZERS "," CKMUX_FUZZ_SANITIZER_FLAGS)

# Every driver links ckmux_common and nothing else: the protocol codec, the
# grid algebra and the config parser all live there, and a fuzz lane that
# dragged in the client and the server would spend its build time on code no
# driver calls. The ckVision half is not coverage-instrumented unless ckVision's
# own CKVISION_BUILD_FUZZERS is on as well; that costs feedback inside the
# library, never correctness, and a finding that points into ckVision is an
# upstream fix (the engineering standard) rather than something to work around here.
function(ckmux_add_fuzzer name source corpus)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ckmux_common)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    ckmux_strict(${name})
    target_compile_options(${name} PRIVATE -fsanitize=${CKMUX_FUZZ_SANITIZER_FLAGS})
    target_link_options(${name} PRIVATE -fsanitize=${CKMUX_FUZZ_SANITIZER_FLAGS})

    if(CKMUX_BUILD_TESTING)
        # The checked-in corpus, run once per input. This is a regression gate,
        # not a fuzzing session: it proves the drivers still build and that
        # every seed — including the malformed ones — is still handled rather
        # than crashed on. Actual fuzzing is a campaign someone runs on
        # purpose, with a corpus directory and a time budget.
        file(GLOB CKMUX_FUZZ_${name}_CORPUS CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${corpus}/*")
        add_test(NAME ${name}_corpus
            COMMAND ${name} ${CKMUX_FUZZ_${name}_CORPUS}
                    -runs=1 -max_len=4096 -timeout=2 -rss_limit_mb=512)
        set_tests_properties(${name}_corpus PROPERTIES
            LABELS "fuzz-corpus;portable"
            TIMEOUT 45)
    endif()
endfunction()

# The testing plan §6's three: the protocol decoder as each end meets it, and the
# config parser.
ckmux_add_fuzzer(fuzz_proto_server fuzz_proto_server.cpp proto_server)
ckmux_add_fuzzer(fuzz_proto_client fuzz_proto_client.cpp proto_client)
ckmux_add_fuzzer(fuzz_config fuzz_config.cpp config)
