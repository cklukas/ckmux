// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// When the first terminal of a session may be opened, given that the host has
// not necessarily said yet what it can draw.
//
// `Attach.host_sixel` is decided ONCE, when a terminal is created: the server
// folds it into the advertisement the child reads out of DA1, and a program
// that has already asked never asks again. So opening before the capability
// probe has answered does not merely guess — it commits the guess for the
// lifetime of the terminal the reader spends all their time in.
//
// That is not hypothetical. The owner reported Sixel as absent on a
// Sixel-capable host: the probe answers asynchronously, the FIRST terminal was
// opened before the reply landed, and every LATER terminal was correct — which
// is exactly what hid it, because the second window a reader opens works.
//
// Lifted out of `run_client.cpp` so the decision can be asserted without
// standing up a client, a server and a host: the same reason
// `printer_button_state()` is a function rather than a branch inside a dialog.
#pragma once

#include <cstdint>

namespace ckm::client {

// How long the first terminal waits for the host to answer the capability
// probe before giving up and opening anyway.
//
// Bounded, because a host that never answers must not cost a reader their
// terminal. A host that supports the probe replies in microseconds — it is a
// round trip to a program already running — and a host that does not never
// replies at all, so after this deadline the fallback stands and behaviour is
// exactly what it was before the wait existed.
inline constexpr std::int64_t kCapabilityGraceNanos = 250'000'000;  // 250 ms

// Whether the first terminal may be opened now.
//
// Both inputs are named in the signature on purpose: the answer depends on
// whether the host has spoken AND on how long it has been given, and a
// predicate that hid either would be a timer whose expiry nobody could see.
//
// `waited_nanos` is measured from the first moment the terminal *could* have
// been opened — not from process start, which would spend the grace on
// connecting to the server.
bool first_terminal_may_open(bool host_capability_answered,
                             std::int64_t waited_nanos) noexcept;

}  // namespace ckm::client
