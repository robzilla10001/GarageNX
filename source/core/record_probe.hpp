#pragma once
// source/core/record_probe.hpp
// Diagnostic battery for the NS application-record write IPC (Push/Delete
// ApplicationRecord), which returns LibnxError_BadInput (0xF601) in our current
// form. Runs many permutations in a single build; the UI shows each variant's
// result so the working combination can be identified in one hardware pass.
// Variants vary service handle, command IDs, and input layout. All variants
// are READ-ONLY-SAFE except the ones that actually push; those are clearly
// labeled and only run when explicitly selected.

#include "core/ncm.hpp"
#include <string>
#include <vector>

namespace Core::RecordProbe {

struct Variant {
    std::string name;     // shown in the menu
    std::string detail;   // filled after running: result codes
    bool        ran = false;
    bool        success = false;
};

// Returns the list of variant names (for building the menu).
std::vector<std::string> variant_names();

// Run one variant by index against the given title's base application id +
// removed meta key. Fills `out_detail` with the per-call result codes. Returns
// true if the variant's write calls succeeded (result 0).
bool run_variant(int index, uint64_t base_app_id,
                 const Core::Ncm::Title& removed_title,
                 std::string& out_detail);

int variant_count();

} // namespace Core::RecordProbe
