#pragma once
// source/core/dump.hpp
// Dump an installed title to an NSP on the SD card (Milestone 4 Phase C).
//
// Produces TICKET-LESS NSPs: for NCAs that use titlekey (ticket) crypto, the
// rights ID is stripped, the decrypted titlekey is written into the NCA key
// area, and the key area is re-encrypted with standard key_area_key_application
// crypto. The result installs without needing a tik/cert pair.
//
// Each meta (base / update / DLC) is packaged as its own NSP, matching the
// design decision. Output goes to sdmc:/switch/GarageNX/dumps/.
//
// This streams NCA content from NCM through a modification stage into a PFS0
// container, so multi-GB titles never need to fit in memory.

#include "core/ncm.hpp"
#include "core/keys.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace Core::Dump {

// Live progress for the UI. The worker updates these; the UI polls each frame.
struct Progress {
    std::atomic<bool>     running{false};
    std::atomic<bool>     done{false};
    std::atomic<bool>     success{false};
    std::atomic<bool>     cancel{false};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<int>      ncas_total{0};
    std::atomic<int>      ncas_done{0};
    std::string           current_file;   // best-effort label
    std::string           message;        // final status / error

    float fraction() const {
        uint64_t t = bytes_total.load();
        return t ? (float)bytes_done.load() / (float)t : 0.f;
    }
    void reset() {
        running=false; done=false; success=false; cancel=false;
        bytes_total=0; bytes_done=0; ncas_total=0; ncas_done=0;
        current_file.clear(); message.clear();
    }
};

// Dump a single meta (one Title = base OR update OR DLC) to a ticket-less NSP.
// `keys` supplies header_key + key_area_key_application (+ ES titlekeys for the
// rights-id strip). Runs synchronously on the calling thread — callers run it on
// a worker and poll `progress`. Returns true on success; details in
// progress.message. The output path is returned in out_path.
bool dump_title_to_nsp(const Core::Ncm::Title& title,
                       const Core::Keys::Keyset& keys,
                       Progress& progress,
                       std::string& out_path);

} // namespace Core::Dump
