// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The single cktest entry point for ckmux_tests. Every other test_*.cpp
// registers CK_TEST cases by inline static initialization and must NOT define
// its own CKTEST_MAIN.
#include "cvision/testing/cktest.hpp"

CKTEST_MAIN
