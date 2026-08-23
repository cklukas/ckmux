// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Shared scaffolding for ckmux's libFuzzer drivers (the testing plan §6).
#pragma once

#include <cstdlib>

#include "common/proto.hpp"

namespace ckm::fuzz {

// A driver states invariants, and a violated one must stop the process where
// it happened so libFuzzer writes the input out. Deliberately not `assert`:
// this lane is built with sanitizers and optimisations, and an invariant that
// evaporates with NDEBUG is an invariant nobody checks where it matters.
[[noreturn]] inline void invariant_failure() { std::abort(); }

inline void require(bool value) {
    if (!value) invariant_failure();
}

// Which end may legitimately send a message.
//
// The wire is one catalogue and one codec by design — the server encodes what
// the client decodes out of the same struct (proto.hpp) — so direction is not
// a property of the decoder and there is no second decoder to fuzz. It is what
// each driver holds itself to, and that is what makes the two lanes explore
// different halves of the catalogue instead of the same half twice.
//
// Messages nothing produces yet (the protocol spec's dormant half) are listed on the
// side they will arrive from: the decoder accepts them today, so they are
// attack surface today.
constexpr bool is_client_to_server(proto::MessageType type) {
    switch (type) {
        case proto::MessageType::Hello:
        case proto::MessageType::Ping:
        case proto::MessageType::Pong:
        case proto::MessageType::ListSessions:
        case proto::MessageType::NewSession:
        case proto::MessageType::Attach:
        case proto::MessageType::Detach:
        case proto::MessageType::ClientResize:
        case proto::MessageType::RenameSession:
        case proto::MessageType::KillSession:
        case proto::MessageType::KillServer:
        case proto::MessageType::NewTerminal:
        case proto::MessageType::CloseTerminal:
        case proto::MessageType::KillTerminal:
        case proto::MessageType::RespawnTerminal:
        case proto::MessageType::MoveTerminal:
        case proto::MessageType::MoveResize:
        case proto::MessageType::Raise:
        case proto::MessageType::FocusTerm:
        case proto::MessageType::ZoomTerm:
        case proto::MessageType::SetLayout:
        case proto::MessageType::RenameTerminal:
        case proto::MessageType::Input:
        case proto::MessageType::PasteChunk:
        case proto::MessageType::SetPrinterPolicy:
        case proto::MessageType::PrintJobFetch:
        case proto::MessageType::PrintJobDiscard: return true;
        default: return false;
    }
}

constexpr bool is_server_to_client(proto::MessageType type) {
    switch (type) {
        case proto::MessageType::HelloAck:
        case proto::MessageType::Refuse:
        case proto::MessageType::Ping:
        case proto::MessageType::Pong:
        case proto::MessageType::SessionList:
        case proto::MessageType::SessionsChanged:
        case proto::MessageType::Attached:
        case proto::MessageType::Detached:
        case proto::MessageType::PasteAck:
        case proto::MessageType::LayoutDelta:
        case proto::MessageType::TermOpened:
        case proto::MessageType::TermClosed:
        case proto::MessageType::TermMeta:
        case proto::MessageType::GridDelta:
        case proto::MessageType::ImageAddBegin:
        case proto::MessageType::ImageChunk:
        case proto::MessageType::ImageEnd:
        case proto::MessageType::ImagePlace:
        case proto::MessageType::ImageRemove:
        case proto::MessageType::ClipboardSet:
        case proto::MessageType::TermDiagnostic:
        case proto::MessageType::Error:
        case proto::MessageType::PrintState:
        case proto::MessageType::PrintJobAdded:
        case proto::MessageType::PrintJobData: return true;
        default: return false;
    }
}

// What every decoded frame must survive, whichever end decoded it: its own
// encoder. Re-encoding a decoded message and decoding that again has to give
// the same value back, and has to be accepted — a codec that decodes something
// it cannot then produce holds two definitions of the wire, which is exactly
// what proto.hpp's one-struct rule exists to make impossible.
inline void require_survives_its_encoder(const proto::Message& message) {
    const std::string bytes = proto::encode(message);
    proto::Message again;
    const proto::DecodeResult result = proto::decode(bytes, again);
    require(result.ok());
    require(result.consumed == bytes.size());
    require(proto::type_of(again) == proto::type_of(message));
    require(again == message);
}

}  // namespace ckm::fuzz
