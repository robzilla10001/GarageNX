#pragma once
// source/screens/tools_screen.hpp
//
// The Tools (maintenance) menu. Each entry is a potentially destructive
// operation that follows the same safe pattern:
//   1. select        → run a read-only DRY RUN that reports what WOULD be removed
//   2. nothing found  → Info modal, no confirmation shown
//   3. something found→ Danger modal ("hold to confirm", 2 s) summarising the scan
//   4. confirmed      → execute, then an Info modal with the result
//
// The scan never mutates anything, so a user can inspect every operation before
// committing. Execution only runs after the held confirmation resolves.

#include "screens/screen.hpp"
#include "ui/widgets.hpp"

#include <functional>
#include <string>
#include <vector>

class ToolsScreen : public Screen {
public:
    ToolsScreen();

    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

private:
    // Result of a dry run: whether anything matched, how many, and a short
    // human summary used in the confirmation body.
    struct ScanResult {
        bool        any   = false;
        int         count = 0;
        std::string detail;   // e.g. "3 placeholder files (128 MB)"
    };

    struct Op {
        std::string                              label;
        std::function<ScanResult()>              scan;      // read-only
        std::function<std::string(const ScanResult&)> run;  // returns result text

        // Alternative to scan+run: when set, selecting this row pushes a new
        // Screen instead of running the batch dry-run/hold-confirm flow. For
        // ops where there's no way to say which SPECIFIC item "should" be
        // acted on — an inherently per-item choice (which Wi-Fi network,
        // which user) rather than a bulk cleanup category. scan/run are
        // ignored when this is set.
        std::function<std::unique_ptr<Screen>()> push;
    };

    std::vector<Op> m_ops;
    Widgets::List   m_list;

    int        m_pending = -1;   // index of the op awaiting held confirmation
    ScanResult m_pending_scan;

    std::unique_ptr<Screen> select(int idx);
};
