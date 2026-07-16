#pragma once
// source/core/title_ops.hpp
// Destructive title-management operations (Milestone 4 Phase C).
//
// Delete is implemented first, via the ns application-management API, which
// removes content AND records atomically the way the OS does — far safer than
// deleting individual NCAs through ncm (which risks orphaned records/content).
//
// Dump-to-SD and move SD<->NAND are added in subsequent passes.

#include "core/ncm.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace Core::TitleOps {

struct Result {
    bool        ok = false;
    std::string message;   // human-readable outcome (success detail or error)
};

// Completely delete an application: base program, all updates, and all DLC,
// plus its record. Uses nsDeleteApplicationCompletely. This matches the design
// decision that deleting a base app removes everything belonging to it.
//
// `application_id` is the base application's program id.
Result delete_application_completely(uint64_t application_id);

// ─── Move (SD <-> NAND) ─────────────────────────────────────────────────────────

// Progress for a move operation (worker updates, UI polls). Mirrors the dump
// progress shape so the TitleDetail move UI can reuse the same rendering.
struct MoveProgress {
    std::atomic<bool>     running{false};
    std::atomic<bool>     done{false};
    std::atomic<bool>     success{false};
    std::atomic<bool>     cancel{false};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<int>      ncas_total{0};
    std::atomic<int>      ncas_done{0};
    std::string           current_file;
    std::string           message;
    std::string           stage;   // "copying" / "registering" / "deleting src"

    float fraction() const {
        uint64_t t = bytes_total.load();
        return t ? (float)bytes_done.load() / (float)t : 0.f;
    }
    void reset() {
        running=false; done=false; success=false; cancel=false;
        bytes_total=0; bytes_done=0; ncas_total=0; ncas_done=0;
        current_file.clear(); message.clear(); stage.clear();
    }
};

// Move a single meta (base / update / DLC Title) from its current storage to the
// other one (SD <-> NAND). This is a TRUE move: the title is fully installed and
// verified on the destination FIRST, and only then is the source removed. If any
// step fails before the source-delete phase, the source is left completely
// intact and any partial destination content is cleaned up. Runs synchronously
// on the calling thread; callers run it on a worker and poll `progress`.
Result move_title(const Core::Ncm::Title& title, MoveProgress& progress);

} // namespace Core::TitleOps
