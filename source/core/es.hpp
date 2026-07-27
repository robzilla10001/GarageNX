#pragma once
// source/core/es.hpp
// Retrieves title keys for titlekey-crypto NCAs from the console's eTicket (ES)
// system. Many installed titles (eShop games, updates) don't use standard
// key-area crypto; their content key comes from a ticket keyed by the NCA's
// rights ID. This module queries ES for the common ticket, extracts the
// encrypted titlekey, and decrypts it with the titlekek for the title's key
// generation.
//
// Personalized tickets (console-specific, RSA-wrapped) are a separate, heavier
// path (eTicket device RSA key via SPL) and are handled as a follow-on; this
// module covers common tickets, which are the majority.
//
// libnx doesn't expose the ES ticket-listing IPC, so those calls are issued
// directly via serviceDispatch (command IDs per switchbrew ETicket_services).

#include "core/keys.hpp"
#include <array>
#include <vector>
#include <string>
#include <cstdint>

namespace Core::Es {

// Diagnostic string describing the last get_ticket_and_cert() attempt.
extern std::string g_es_diag;

// Initialize / shut down the ES service session. init() is safe to call once at
// startup; exit() at shutdown.
bool init();
void exit();

// Given a 0x10-byte rights ID and the title's key generation, produce the
// DECRYPTED titlekey (0x10 bytes) in `out`. Returns true on success. Looks up
// the common ticket for the rights ID via ES, extracts the encrypted titlekey,
// and decrypts it with keys.titlekek[generation].
bool get_titlekey(const uint8_t rights_id[0x10], int key_generation,
                  const Core::Keys::Keyset& keys,
                  std::array<uint8_t, 16>& out);

// Retrieve the raw common ticket AND its certificate chain for a rights ID,
// straight from the console's ES system (cmd 22/23 GetCommonTicketAndCertificate
// Size/Data). This is the user's own console data — nothing is embedded. On
// success, `ticket` and `cert` receive the raw bytes to write into an NSP as
// "<rights_id>.tik" and "<rights_id>.cert". Returns false if no common ticket
// exists for the rights ID (e.g. personalized-only, or absent).
bool get_ticket_and_cert(const uint8_t rights_id[0x10],
                         std::vector<uint8_t>& ticket,
                         std::vector<uint8_t>& cert);

// ── Enumerating the console's common tickets ─────────────────────────────────

struct TicketRef {
    std::array<uint8_t, 0x10> rights_id{};
    // Bytes 0-7 of a rights ID are the title id, big-endian. That layout is
    // standard, but this codebase derives key generation from the NCA header
    // rather than the rights ID, so only the title id is claimed here — guessing
    // at the rest would be inventing a fact the code cannot check.
    //
    // The derivation is SELF-VALIDATING in the UI: a correct title id resolves to
    // an installed title's name, and one that does not resolve is displayed as a
    // raw rights ID rather than as a confidently wrong name.
    uint64_t title_id = 0;
};

/// Every common ticket installed on this console.
///
/// Uses the same ES calls the NSP dump path already relies on
/// (CountCommonTicket / ListCommonTicket), so this adds no new IPC — it exposes
/// enumeration that was previously private to the titlekey lookup.
///
/// Returns empty if ES is unavailable or the console has no common tickets;
/// neither is an error worth surfacing.
std::vector<TicketRef> list_common_tickets();

} // namespace Core::Es
