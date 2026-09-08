#pragma once
// source/core/dump.hpp
// Dump an installed title to an NSP on the SD card (Milestone 4 Phase C).
//
// Produces TICKET-LESS NSPs: for titlekey-crypto NCAs, the rights ID is
// stripped, the decrypted titlekey is written into the NCA key area, and the
// key area is re-encrypted with standard key_area_key_application crypto, so
// the result installs without a tik/cert pair. Each meta (base / update / DLC)
// is packaged as its own NSP; output goes to sdmc:/switch/GarageNX/dumps/.
// NCA content streams from NCM through a modification stage into a PFS0
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
// rights-id strip). Runs synchronously on the calling thread - callers run it on
// a worker and poll `progress`. Returns true on success; details in
// progress.message. The output path is returned in out_path.
bool dump_title_to_nsp(const Core::Ncm::Title& title,
                       const Core::Keys::Keyset& keys,
                       Progress& progress,
                       std::string& out_path);

// ONE definition of firmware content enumeration, shared by the Tools-menu
// dry-run (scan) and the dump itself. This exists because the scan and the dump
// once carried two hand-copied versions of the same enumeration loop and
// diverged - the scan reported one number, the dump wrote a different count of
// files, and three investigation sessions burned down trying to explain the
// gap. Do not re-copy this logic at a call site; call it.
struct FirmwareScan {
    int      meta_count  = 0;   // firmware meta records seen (SystemProgram .. BootImagePackageSafe)
    int      unique_ncas = 0;   // unique NCAs referenced by those metas (deduplicated by content id)
    uint64_t total_bytes = 0;   // sum of the NCAs' recorded sizes
};

// Enumerate the firmware content on a storage (typically BuiltInSystem).
// Counts what the firmware META DATABASE references - filtered to the five
// firmware meta types and NcmContentInstallType_Full. Content that is present
// in the storage but registered differently is NOT counted here; the dump logs
// a storage-side cross-check so that gap is visible on hardware.
// Zeros on failure (e.g. storage not present) - not an error.
FirmwareScan enumerate_firmware_content(Core::Ncm::Storage storage);

// Append one diagnostic line to sdmc:/switch/GarageNX/logs/firmware_dump.log
// and mirror it to the console. WHY A FILE: SDL_Log alone reaches stdout, which
// hardware exposes only over nxlink - the first firmware-dump debugging session
// ended with no log file anywhere and the failure unexplained. Exposed so code
// that triggers the dump (the tools screen) writes the run's start/failure into
// the same file as the dump's own diagnostics. No-op on PC (PC never dumps).
void fw_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Dump all NCAs from a storage (typically BuiltInSystem for firmware) to
// individual .nca files on the SD card.
//
// Output directory: sdmc:/switch/GarageNX/dumps/firmware/<version>/
// Filename format: 32-char lowercase hex content ID + .cnmt.nca suffix for meta
// Contents: SystemProgram, SystemData, SystemUpdate, BootImagePackage titles
//
// Returns true if at least one NCA was dumped. Details in progress.message.
bool dump_ncas_from_storage(Core::Ncm::Storage storage,
                            const Core::Keys::Keyset& keys,
                            Progress& progress,
                            std::string& out_dir);

} // namespace Core::Dump
