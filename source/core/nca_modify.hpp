#pragma once
// source/core/nca_modify.hpp
// Ticket-less NCA header conversion (see nca_modify.cpp).

#include "core/keys.hpp"
#include <array>
#include <cstdint>

namespace Core::NcaModify {

// Convert a titlekey-crypto NCA header to standard key-area crypto.
//   decrypted_header:     0xC00 bytes, the PLAINTEXT (already-decrypted) header,
//                         modified in place (key area + rights id).
//   decrypted_titlekey:   the 0x10-byte decrypted titlekey for this NCA.
//   keys:                 keyset (needs header_key + key_area_key_application).
//   out_encrypted_header: receives the 0xC00 re-encrypted header to write out.
// Returns false if the key generation is unsupported or keys are missing.
bool convert_to_ticketless(uint8_t* decrypted_header,
                           const std::array<uint8_t,16>& decrypted_titlekey,
                           const Core::Keys::Keyset& keys,
                           uint8_t* out_encrypted_header);

} // namespace Core::NcaModify
