// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
//
// The server's side of the wire (the testing plan §6): bytes arrive from a client in
// whatever pieces a socket read produced, and nothing about them is trusted.
//
// Two surfaces, because the server meets both. `decode` sees one frame from
// the front of a buffer; `FrameReader` sees a stream, which is the part with
// state — a peer that declares a payload and never sends it must not be able
// to make the buffer grow without bound, and a frame boundary landing in the
// middle of a header must not change what the next frame decodes to.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/proto.hpp"
#include "fuzz_common.hpp"

namespace proto = ckm::proto;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    // 1. One frame from the front of an arbitrary buffer.
    proto::Message message;
    const proto::DecodeResult result = proto::decode(input, message);
    if (result.ok()) {
        ckm::fuzz::require(result.consumed >= proto::kHeaderBytes);
        ckm::fuzz::require(result.consumed <= input.size());
        // The client→server half is this lane's; the other half is the client
        // driver's, so the two do not spend their budgets on the same frames.
        if (ckm::fuzz::is_client_to_server(proto::type_of(message)))
            ckm::fuzz::require_survives_its_encoder(message);
    } else {
        // A rejected frame reports where the next one would start, or nothing
        // at all. It must never claim to have consumed more than it was given
        // — that number is what a caller wanting to resynchronise would trust.
        ckm::fuzz::require(result.consumed <= input.size());
    }

    // 2. The same bytes as a stream, split the way a socket splits them. The
    //    chunk sizes come out of the input itself, so the fuzzer can steer a
    //    read boundary into the middle of a header rather than only between
    //    frames.
    proto::FrameReader reader;
    std::size_t offset = 0;
    bool fatal = false;
    while (offset < input.size() && !fatal) {
        const std::size_t chunk = 1U + (static_cast<unsigned char>(input[offset]) % 23U);
        const std::string_view slice = input.substr(offset, chunk);
        // False means the peer has declared more than any frame may be. That
        // ends the connection; it must not end the process.
        if (!reader.append(slice)) break;
        offset += slice.size();
        for (;;) {
            proto::Message streamed;
            const proto::DecodeError error = reader.next(streamed);
            if (error == proto::DecodeError::Incomplete) break;
            if (error != proto::DecodeError::None) {
                fatal = true;  // every other error drops the connection (the protocol spec)
                break;
            }
            if (ckm::fuzz::is_client_to_server(proto::type_of(streamed)))
                ckm::fuzz::require_survives_its_encoder(streamed);
        }
    }

    // Whatever the peer did, the reassembly buffer stays inside the one bound
    // the protocol allows a single frame: that bound is the whole defence
    // against a length nobody follows with bytes.
    ckm::fuzz::require(reader.buffered() <= proto::kHeaderBytes + proto::kMaxSnapshotPayloadBytes);
    reader.clear();
    ckm::fuzz::require(reader.buffered() == 0);
    return 0;
}
