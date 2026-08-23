// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// WHAT THIS SUITE COVERS, AND — MORE IMPORTANTLY — WHAT IT DOES NOT.
//
// It covers the TIMER: whether the first terminal may be opened yet, given
// whether the host has answered the capability probe and how long it has been
// given to answer. Four cases, no clock, no PTY, no flake.
//
// **It does not cover the PLUMBING.** It says nothing about whether the
// host's answer actually reaches `session.host_sixel()`, and nothing about
// whether that value lands in the advertisement the child reads out of DA1.
// A build in which the capability never propagates at all passes every
// assertion below.
//
// That distinction is the whole point of writing it down here. A green run of
// this file is NOT evidence that the owner's Sixel defect is fixed — it is
// evidence that the wait is shaped correctly. The propagation half lives in
// `test_capability_wire.cpp` beside it, and neither half is meaningful alone:
// this suite is the mirror image of WP-21 §5's "no device was opened", which
// passes trivially against a build where printing does nothing.
#include "client/capability_grace.hpp"

#include "cvision/testing/cktest.hpp"

using ckm::client::first_terminal_may_open;
using ckm::client::kCapabilityGraceNanos;

CK_TEST(an_answered_probe_opens_the_first_terminal_at_once) {
    // The ordinary case on a host that supports the probe: it replies in
    // microseconds, because it is a round trip to a program already running.
    // Nothing should be waited for once the answer is in hand.
    CK_CHECK(first_terminal_may_open(true, 0));
    CK_CHECK(first_terminal_may_open(true, 1));
}

CK_TEST(an_unanswered_probe_holds_the_first_terminal_back) {
    // The defect this exists to prevent: opening before the answer commits the
    // guess for the lifetime of the terminal, because `host_sixel` is decided
    // once at creation and a child that has asked never asks again.
    CK_CHECK(!first_terminal_may_open(false, 0));
    CK_CHECK(!first_terminal_may_open(false, 1));
    CK_CHECK(!first_terminal_may_open(false, kCapabilityGraceNanos - 1));
}

CK_TEST(an_answer_arriving_late_but_inside_the_grace_still_counts) {
    // THE OWNER'S CASE. The probe answered — just not before the client was
    // otherwise ready to open. The whole purpose of the wait is that this
    // reader gets the true capability rather than the fallback, so a late
    // answer inside the window must be honoured rather than merely tolerated.
    CK_CHECK(first_terminal_may_open(true, kCapabilityGraceNanos - 1));
    CK_CHECK(first_terminal_may_open(true, kCapabilityGraceNanos / 2));
}

CK_TEST(a_host_that_never_answers_costs_a_reader_nothing_but_the_grace) {
    // The bound. A host that does not support the probe never replies at all,
    // so the wait must end: after the deadline the fallback stands and
    // behaviour is exactly what it was before this wait existed.
    //
    // `>=` at the boundary, asserted explicitly: if it were `>`, the moment of
    // expiry would depend on the caller's polling interval rather than on the
    // deadline, and the grace would silently be "250 ms plus one tick".
    CK_CHECK(first_terminal_may_open(false, kCapabilityGraceNanos));
    CK_CHECK(first_terminal_may_open(false, kCapabilityGraceNanos + 1));
    CK_CHECK(first_terminal_may_open(false, kCapabilityGraceNanos * 10));
}
