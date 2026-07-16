// source/core/es.cpp

#include "core/es.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <vector>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::Es {

// Diagnostic string for the last ticket/cert attempt (surfaced by the dump UI).
std::string g_es_diag;

#ifdef PLATFORM_SWITCH

// ES service session. libnx has no es.h, so we open the "es" service directly
// and issue commands via serviceDispatch (IDs from switchbrew ETicket_services /
// SwIPC IETicketService).
static Service s_es;
static bool    s_init = false;

// ── IPC wrappers ────────────────────────────────────────────────────────────────

// [9] CountCommonTicket() -> u32
static Result esCountCommonTicket(s32* out_count) {
    u32 count = 0;
    Result rc = serviceDispatchOut(&s_es, 9, count);
    if (R_SUCCEEDED(rc)) *out_count = (s32)count;
    return rc;
}

// [11] ListCommonTicket() -> (u32 written, buffer<rights_ids, 6, 0>)
static Result esListCommonTicket(s32* out_written, void* rights_ids, size_t size) {
    u32 written = 0;
    Result rc = serviceDispatchOut(&s_es, 11, written,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { rights_ids, size } });
    if (R_SUCCEEDED(rc)) *out_written = (s32)written;
    return rc;
}

// [16] GetCommonTicketData(RightsId rights_id) -> (u32, buffer<ticket, 6>)
// The rights_id is a 0x10-byte value passed as RAW INPUT DATA (an in-struct
// argument), NOT a buffer. The only buffer is the output ticket (type-6). Using
// a buffer descriptor for rights_id is what produced LibnxError_BadInput (0xF601)
// — libnx rejected the malformed request before it reached ES.
struct EsRightsId { uint8_t c[0x10]; };

static Result esGetCommonTicketData(const uint8_t rights_id[0x10],
                                    void* out_ticket, size_t ticket_size,
                                    u32* out_unk) {
    EsRightsId rid;
    std::memcpy(rid.c, rights_id, 0x10);
    u32 unk = 0;
    Result rc = serviceDispatchInOut(&s_es, 16, rid, unk,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out_ticket, ticket_size } });
    if (R_SUCCEEDED(rc) && out_unk) *out_unk = unk;
    return rc;
}

// [22] GetCommonTicketAndCertificateSize(rights_id) -> (u64 tik_size, u64 cert_size)
struct TikCertSizes { u64 ticket_size; u64 cert_size; };

static Result esGetCommonTicketAndCertificateSize(const uint8_t rights_id[0x10],
                                                  TikCertSizes* out) {
    EsRightsId rid;
    std::memcpy(rid.c, rights_id, 0x10);
    return serviceDispatchInOut(&s_es, 22, rid, *out);
}

// [23] GetCommonTicketAndCertificateData(RightsId) ->
//        (u64 tik_size, u64 cert_size, buffer<ticket||cert, 6>)
// rights_id is raw input data; the only buffer is the output.
static Result esGetCommonTicketAndCertificateData(const uint8_t rights_id[0x10],
                                                  void* out_buf, size_t buf_size,
                                                  TikCertSizes* out_sizes) {
    EsRightsId rid;
    std::memcpy(rid.c, rights_id, 0x10);
    return serviceDispatchInOut(&s_es, 23, rid, *out_sizes,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out_buf, buf_size } });
}

// ── Titlekek decryption ─────────────────────────────────────────────────────────

// Decrypt a 0x10-byte titlekey with the titlekek (AES-128-ECB).
static void titlekek_decrypt(const uint8_t titlekek[0x10],
                             uint8_t out[0x10], const uint8_t in[0x10]) {
    Aes128Context ctx;
    aes128ContextCreate(&ctx, titlekek, false /*decrypt*/);
    aes128DecryptBlock(&ctx, out, in);
}

#endif // PLATFORM_SWITCH

// ── Public API ──────────────────────────────────────────────────────────────────

bool init() {
#ifdef PLATFORM_SWITCH
    if (s_init) return true;
    Result rc = smGetService(&s_es, "es");
    if (R_FAILED(rc)) {
        SDL_Log("Es::init — smGetService(es) failed: 0x%x", rc);
        return false;
    }
    s_init = true;
    return true;
#else
    return false;
#endif
}

void exit() {
#ifdef PLATFORM_SWITCH
    if (s_init) { serviceClose(&s_es); s_init = false; }
#endif
}

bool get_titlekey(const uint8_t rights_id[0x10], int key_generation,
                  const Core::Keys::Keyset& keys,
                  std::array<uint8_t, 16>& out) {
#ifndef PLATFORM_SWITCH
    (void)rights_id; (void)key_generation; (void)keys; (void)out;
    return false;
#else
    if (!s_init && !init()) return false;

    if (key_generation < 0 || key_generation >= Core::Keys::MAX_KEY_GENERATION)
        return false;
    if (!keys.has_titlekek[key_generation]) {
        SDL_Log("Es — missing titlekek_%02x", key_generation);
        return false;
    }

    // 1. Enumerate common tickets and confirm our rights id is present. (Not
    //    strictly required before GetCommonTicketData, but it lets us fail fast
    //    and avoids a service error for tickets that don't exist.)
    s32 count = 0;
    if (R_FAILED(esCountCommonTicket(&count)) || count <= 0) {
        return false;
    }

    // 2. Fetch the ticket blob for this rights id. A ticket is 0x2C0 bytes for
    //    a common ticket (signature 0x140 + ticket data). We read a generous
    //    buffer.
    std::vector<uint8_t> ticket(0x400, 0);
    u32 unk = 0;
    Result rc = esGetCommonTicketData(rights_id, ticket.data(), ticket.size(), &unk);
    if (R_FAILED(rc)) {
        // No common ticket for this rights id (may be personalized, or absent).
        return false;
    }

    // 3. The titlekey block sits at offset 0x180 within the ticket (right after
    //    the 0x140-byte RSA-2048 signature + 0x40 into the ticket data). For a
    //    common ticket the first 0x10 bytes there are the titlekek-encrypted
    //    titlekey.
    //    Ticket layout: [0x000] sig type+sig+padding (0x140) then ticket data;
    //    titlekey block is at data+0x00 = absolute 0x180.
    static constexpr size_t TITLEKEY_BLOCK_OFFSET = 0x180;
    if (ticket.size() < TITLEKEY_BLOCK_OFFSET + 0x10) return false;

    const uint8_t* enc_titlekey = ticket.data() + TITLEKEY_BLOCK_OFFSET;

    // 4. Decrypt with the titlekek for this generation.
    titlekek_decrypt(keys.titlekek[key_generation].data(),
                     out.data(), enc_titlekey);
    return true;
#endif
}

bool get_ticket_and_cert(const uint8_t rights_id[0x10],
                         std::vector<uint8_t>& ticket,
                         std::vector<uint8_t>& cert) {
#ifndef PLATFORM_SWITCH
    (void)rights_id; (void)ticket; (void)cert;
    return false;
#else
    // Granular diagnostics so we can see exactly which ES stage fails. These go
    // to the log AND into g_es_diag which the dump surfaces on-screen.
    if (!s_init && !init()) { g_es_diag = "init fail"; return false; }

    s32 count = 0;
    Result crc = esCountCommonTicket(&count);
    if (R_FAILED(crc)) {
        char b[48]; snprintf(b, sizeof(b), "cnt rc=%08X", crc);
        g_es_diag = b; return false;
    }

    // ── Ticket via GetCommonTicketData (cmd 16). ──
    std::vector<uint8_t> tik(0x400, 0);
    u32 unk = 0;
    Result trc = esGetCommonTicketData(rights_id, tik.data(), tik.size(), &unk);
    if (R_FAILED(trc)) {
        // Show the count first (is it 0? = no common tickets = personalized) and
        // the error code, kept short so nothing is truncated on screen.
        char b[48]; snprintf(b, sizeof(b), "cnt=%d rc=%08X", count, trc);
        g_es_diag = b; return false;
    }
    tik.resize(0x400);
    ticket = std::move(tik);

    // ── Cert: best-effort via cmd 22/23. May fail; that's fine. ──
    cert.clear();
    TikCertSizes sizes{};
    if (R_SUCCEEDED(esGetCommonTicketAndCertificateSize(rights_id, &sizes)) &&
        sizes.cert_size > 0 && sizes.ticket_size > 0) {
        size_t total = (size_t)(sizes.ticket_size + sizes.cert_size);
        std::vector<uint8_t> buf(total, 0);
        TikCertSizes got{};
        if (R_SUCCEEDED(esGetCommonTicketAndCertificateData(
                rights_id, buf.data(), buf.size(), &got)) &&
            got.cert_size > 0 &&
            got.ticket_size + got.cert_size <= buf.size()) {
            cert.assign(buf.begin() + got.ticket_size,
                        buf.begin() + got.ticket_size + got.cert_size);
        }
    }

    char b[48]; snprintf(b, sizeof(b), "ok cnt=%d cert=%zu", count, cert.size());
    g_es_diag = b;
    return !ticket.empty();
#endif
}

} // namespace Core::Es
