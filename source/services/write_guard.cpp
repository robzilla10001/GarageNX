// source/services/write_guard.cpp

#include "services/write_guard.hpp"
#include "services/confirmation_broker.hpp"

#include <cstdio>

namespace Services {

namespace {

// Every guarded decision, one line, with the REASON.
//
// There are four ways to reach Deny and, from the outside, all four look
// identical: the operation fails and no modal appears. Working out which one
// fired has already cost more than one hardware round of guessing, and the
// project's own rule is to reach for a log rather than a second theory. This
// makes the whole class self-diagnosing: the log names the path, the surface it
// matched (or that it matched none), whether that surface is enabled for THIS
// transport, and the decision.
//
// Volume is not a concern: guards run on mutations only, which are user-initiated
// and rare. Listing and reading never come through here.
void guardlog(const char* transport, const char* operation,
              const std::string& path, const StorageSurface* s,
              bool enabled, const char* decision, const char* reason) {
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/write_guard.log", "a");
    if (!f) return;
    ::fprintf(f, "%-8s %-14s %-10s %-16s surface=%-16s enabled=%d  %s\n",
              transport, operation, decision, reason,
              s ? s->key : "(none)", enabled ? 1 : 0, path.c_str());
    ::fclose(f);
}

} // namespace

void guard_log_note(const char* transport, const char* operation,
                    const char* decision, const char* reason,
                    const std::string& path) {
    guardlog(transport ? transport : "?", operation ? operation : "?",
             path, nullptr, false, decision, reason);
}

WritePolicy classify_write(const std::string& vfs_path, const Config::Surfaces& cfg,
                           const char* transport, const char* operation) {
    const char* tr = transport ? transport : "?";
    const char* op = operation ? operation : "?";

    // An empty or unrecognised path belongs to no storage surface. Default-deny:
    // never let a mutating op run against a location we cannot name.
    if (vfs_path.empty()) {
        guardlog(tr, op, vfs_path, nullptr, false, "DENY", "empty-path");
        return WritePolicy::Deny;
    }

    const StorageSurface* s = StorageCatalog::surface_for_vfs(vfs_path);
    if (!s) {
        guardlog(tr, op, vfs_path, nullptr, false, "DENY", "no-surface");
        return WritePolicy::Deny;
    }

    // A surface the user has switched off is unreachable for writes as well as
    // for browsing — otherwise disabling NAND system in settings would still
    // leave it mutable by a client that guesses the path.
    //
    // NOTE this is now PER TRANSPORT. A surface enabled for MTP and disabled for
    // FTP denies here for FTP only, silently and with no modal — which looks
    // exactly like every other Deny from the outside. Hence the log line.
    if (!StorageCatalog::enabled(s->id, cfg)) {
        guardlog(tr, op, vfs_path, s, false, "DENY", "surface-disabled");
        return WritePolicy::Deny;
    }

    if (s->access == Access::ReadWrite) {
        guardlog(tr, op, vfs_path, s, true, "ALLOW", "read-write");
        return WritePolicy::Allow;
    }

    // ReadOnly from here on.
    if (s->confirm == Confirm::OnDevice) {
        guardlog(tr, op, vfs_path, s, true, "CONFIRM", "readonly+ondevice");
        return WritePolicy::NeedsConfirm;
    }
    guardlog(tr, op, vfs_path, s, true, "DENY", "readonly-no-confirm");
    return WritePolicy::Deny;
}

WriteDecision guard_write(const std::string& transport,
                          const std::string& operation,
                          const std::string& vfs_path,
                          const Config::Surfaces& cfg) {
    switch (classify_write(vfs_path, cfg, transport.c_str(), operation.c_str())) {
        case WritePolicy::Allow:
            return WriteDecision::Allow;

        case WritePolicy::Deny:
            return WriteDecision::Deny;

        case WritePolicy::NeedsConfirm: {
            // Block this worker until the user answers on the console. The
            // PC -> console -> PC round trip IS the safety property: an operation
            // issued from a PC cannot mutate NAND without someone physically
            // approving it on the device.
            const ConfirmResult r = ConfirmationBroker::instance().ask(
                transport, operation, vfs_path, /*timeout_ms*/ 60000);
            return (r == ConfirmResult::Allowed) ? WriteDecision::Allow
                                                 : WriteDecision::Deny;
        }
    }
    return WriteDecision::Deny;   // unreachable; default-deny anyway
}

WriteDecision guard_move(const std::string& transport,
                         const std::string& operation,
                         const std::string& from_vfs,
                         const std::string& to_vfs,
                         const Config::Surfaces& cfg) {
    const WritePolicy a = classify_write(from_vfs, cfg, transport.c_str(), operation.c_str());
    const WritePolicy b = classify_write(to_vfs,   cfg, transport.c_str(), operation.c_str());

    // Either side forbidden -> the whole move is forbidden.
    if (a == WritePolicy::Deny || b == WritePolicy::Deny) return WriteDecision::Deny;

    // Either side protected -> one confirmation covering both paths.
    if (a == WritePolicy::NeedsConfirm || b == WritePolicy::NeedsConfirm) {
        const ConfirmResult r = ConfirmationBroker::instance().ask(
            transport, operation, from_vfs + "  ->  " + to_vfs, /*timeout_ms*/ 60000);
        return (r == ConfirmResult::Allowed) ? WriteDecision::Allow
                                             : WriteDecision::Deny;
    }
    return WriteDecision::Allow;
}

} // namespace Services
