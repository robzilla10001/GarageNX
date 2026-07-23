// source/services/write_guard.cpp

#include "services/write_guard.hpp"
#include "services/confirmation_broker.hpp"

namespace Services {

WritePolicy classify_write(const std::string& vfs_path, const Config::MTP& cfg) {
    // An empty or unrecognised path belongs to no storage surface. Default-deny:
    // never let a mutating op run against a location we cannot name.
    if (vfs_path.empty()) return WritePolicy::Deny;

    const StorageSurface* s = StorageCatalog::surface_for_vfs(vfs_path);
    if (!s) return WritePolicy::Deny;

    // A surface the user has switched off is unreachable for writes as well as
    // for browsing — otherwise disabling NAND system in settings would still
    // leave it mutable by a client that guesses the path.
    if (!StorageCatalog::enabled(s->id, cfg)) return WritePolicy::Deny;

    if (s->access == Access::ReadWrite) return WritePolicy::Allow;

    // ReadOnly from here on.
    if (s->confirm == Confirm::OnDevice) return WritePolicy::NeedsConfirm;
    return WritePolicy::Deny;
}

WriteDecision guard_write(const std::string& transport,
                          const std::string& operation,
                          const std::string& vfs_path,
                          const Config::MTP& cfg) {
    switch (classify_write(vfs_path, cfg)) {
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
                         const Config::MTP& cfg) {
    const WritePolicy a = classify_write(from_vfs, cfg);
    const WritePolicy b = classify_write(to_vfs,   cfg);

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
