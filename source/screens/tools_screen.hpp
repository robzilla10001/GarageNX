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
#include "core/dump.hpp"
#include "ui/widgets.hpp"

#include <functional>
#include <string>
#include <vector>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <vector>

// One repair job: re-dump `title` through the standard pipeline, replacing the
// incomplete NSP at `nsp_path` once the re-dump succeeds. Defined here (not in
// the .cpp) because the worker thread, the scan and the op registration all
// handle these.
struct RepairCandidate {
    Core::Ncm::Title title;
    std::string      nsp_path;
};
#endif

class ToolsScreen : public Screen {
public:
    ToolsScreen();
    ~ToolsScreen() override;

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
        bool                                     is_destructive = true;  // for modal kind

        // Alternative to scan+run: when set, selecting this row pushes a new
        // Screen instead of running the batch dry-run/hold-confirm flow. For
        // ops where there's no way to say which SPECIFIC item "should" be
        // acted on - an inherently per-item choice (which Wi-Fi network,
        // which user) rather than a bulk cleanup category. scan/run are
        // ignored when this is set.
        std::function<std::unique_ptr<Screen>()> push;

        // When set, on_modal_result routes the CONFIRMED op to the background-
        // thread progress flow (start_background_dump) instead of calling run()
        // on the UI thread - run() is then never invoked. The firmware dump is
        // the one op long enough to freeze the console if run inline (minutes
        // for a full BuiltInSystem walk + write). Keep LAST in the struct: the
        // five-field aggregates above (with push) must keep their positions.
        bool run_in_background = false;

        // Which background worker run_in_background routes to. 0 = firmware
        // dump (the original user of this machinery), 1 = ticket repair
        // (re-dump NSPs whose ticket fetch failed). Appended AFTER
        // run_in_background so existing positional aggregates stay valid.
        int bg_kind = 0;
    };

    std::vector<Op> m_ops;
    Widgets::List   m_list;

    int        m_pending = -1;   // index of the op awaiting held confirmation
    ScanResult m_pending_scan;

    // ── Background dump work: thread + polled progress ─────────────────────
    // Shared by the firmware dump and the ticket-repair re-dump: both are
    // minutes-long dump pipelines that publish into an atomic Progress the UI
    // polls every frame. Running either synchronously on the UI thread (the
    // way this screen used to run the firmware dump) froze the console - the
    // exact mistake gamecard.cpp documents and fixed with this same pattern.
    enum class BgOp { FirmwareDump, TicketRepair };
    Core::Dump::Progress m_fw_progress;
    std::string          m_fw_out_dir;
    BgOp                 m_bg_op = BgOp::FirmwareDump;
    bool                 m_fw_dumping = false;   // worker running
    bool                 m_fw_failed  = false;   // failure notification up
    std::string          m_fw_fail_msg;          // formatted failure text
#ifdef PLATFORM_SWITCH
    std::vector<RepairCandidate> m_repair_list;   // confirmed by the scan
    size_t m_repair_next = 0;                     // next candidate index
    int    m_repair_ok = 0, m_repair_fail = 0;    // running tally
    Thread m_fw_thread{};
    bool   m_fw_thread_active = false;
#endif

    void start_background_dump(BgOp op);
    void poll_firmware_dump();
    static void fw_dump_thread_fn(void* arg);
#ifdef PLATFORM_SWITCH
    std::vector<RepairCandidate> repair_scan();
    bool repair_one(const RepairCandidate& c, Core::Dump::Progress& progress,
                    std::string& out_path);
#endif

    std::unique_ptr<Screen> select(int idx);
};