// source/core/nca_modify.cpp
// Ticket-less NCA header conversion. Given a decrypted NCA header that uses
// titlekey crypto, produce a modified header that uses standard key-area crypto:
//   1. Write the decrypted titlekey into key-area slot 2 (the body key slot).
//   2. Re-encrypt the key area with key_area_key_application[crypto_type] (ECB).
//   3. Clear the rights ID (16 zero bytes).
//   4. Leave crypto_type/crypto_type2 as-is (they already select the generation;
//      standard crypto is implied once the rights ID is zero and the key area
//      holds a valid key).
//   5. Re-encrypt the 0xC00 header region with header_key (AES-XTS, Nintendo
//      tweak) so it can be written back into the NSP.
//
// Reference: nxdumptool keys.c generateEncryptedNcaKeyAreaWithTitlekey +
// hactool nca header handling.

#include "core/nca_modify.hpp"
#include <SDL2/SDL.h>
#include <cstring>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::NcaModify {

#ifdef PLATFORM_SWITCH

static constexpr size_t MEDIA_UNIT = 0x200;
static constexpr size_t HEADER_SIZE = 0xC00;

// Offsets within the 0x400 NCA header (mirrors core/nca.cpp).
static constexpr size_t OFF_CRYPTO_TYPE      = 0x206; // key_generation_old
static constexpr size_t OFF_KAEK_INDEX       = 0x207;
static constexpr size_t OFF_KEY_GENERATION   = 0x220;
static constexpr size_t OFF_RIGHTS_ID        = 0x230;
static constexpr size_t OFF_ENCRYPTED_KEYS   = 0x300;

// Encrypt the NCA header region (AES-XTS, Nintendo tweak, sector 0x200).
static void xts_encrypt_header(const uint8_t header_key[0x20],
                               void* dst, const void* src, size_t size) {
    Aes128XtsContext ctx;
    aes128XtsContextCreate(&ctx, header_key, header_key + 0x10, true /*encrypt*/);
    auto* s = (const uint8_t*)src;
    auto* d = (uint8_t*)dst;
    size_t sectors = size / MEDIA_UNIT;
    for (size_t i = 0; i < sectors; ++i) {
        aes128XtsContextResetSector(&ctx, i, true /*is_nintendo*/);
        aes128XtsEncrypt(&ctx, d + i * MEDIA_UNIT, s + i * MEDIA_UNIT, MEDIA_UNIT);
    }
}

bool convert_to_ticketless(uint8_t* decrypted_header /*0xC00, in-place plaintext*/,
                           const std::array<uint8_t,16>& decrypted_titlekey,
                           const Core::Keys::Keyset& keys,
                           uint8_t* out_encrypted_header /*0xC00*/) {
    // Determine key generation (same logic as the reader).
    int keygen = decrypted_header[OFF_KEY_GENERATION];
    int keygen_old = decrypted_header[OFF_CRYPTO_TYPE];
    if (keygen_old > keygen) keygen = keygen_old;
    if (keygen > 0) keygen -= 1;
    if (keygen < 0 || keygen >= Core::Keys::MAX_KEY_GENERATION) return false;

    if (!keys.has_kaek_application[keygen]) {
        SDL_Log("NcaModify — missing key_area_key_application_%02x", keygen);
        return false;
    }

    // Build the new plaintext key area: slot 2 (offset 0x20 in the 0x40 area)
    // holds the titlekey; other slots zeroed. Then ECB-encrypt with the app KAEK.
    uint8_t plain_keys[0x40];
    std::memset(plain_keys, 0, sizeof(plain_keys));
    std::memcpy(plain_keys + 0x20, decrypted_titlekey.data(), 0x10);

    uint8_t enc_keys[0x40];
    Aes128Context kctx;
    aes128ContextCreate(&kctx, keys.key_area_key_application[keygen].data(), true /*encrypt*/);
    for (int i = 0; i < 4; ++i)
        aes128EncryptBlock(&kctx, enc_keys + i * 0x10, plain_keys + i * 0x10);

    // Write the encrypted key area back into the header.
    std::memcpy(decrypted_header + OFF_ENCRYPTED_KEYS, enc_keys, 0x40);

    // Clear the rights ID → signals standard (non-titlekey) crypto.
    std::memset(decrypted_header + OFF_RIGHTS_ID, 0, 0x10);

    // Re-encrypt the whole 0xC00 header with the header key.
    xts_encrypt_header(keys.header_key.data(), out_encrypted_header,
                       decrypted_header, HEADER_SIZE);
    return true;
}

#else  // PC stub

bool convert_to_ticketless(uint8_t*, const std::array<uint8_t,16>&,
                           const Core::Keys::Keyset&, uint8_t*) {
    return false;
}

#endif

} // namespace Core::NcaModify
