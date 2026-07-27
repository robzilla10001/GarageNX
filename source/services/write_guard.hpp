#pragma once
// source/services/write_guard.hpp
//
// ONE enforcement point for "may this mutating operation proceed?", shared by every
// transport (FTP, MTP, and later HTTP). Sprinkling per-command checks across two or
// three transports is exactly how policies drift apart, so the rule lives here and
// the transports only ask.
//
// The policy comes from the StorageCatalog:
//   Access::ReadWrite                       -> Allow, no prompt (SD card, album)
//   Access::ReadOnly + Confirm::OnDevice    -> require ON-DEVICE confirmation (NAND)
//   Access::ReadOnly + Confirm::None        -> Deny outright (gamecard, etc.)
//   unknown path / disabled surface         -> Deny (default-deny)
//
// Split deliberately in two: classify_write() is PURE (no blocking, no UI, no
// globals) so the decision table is host-testable, and guard_write() is the thin
// blocking wrapper that actually waits on the user.

#include "services/storage_catalog.hpp"
#include "config/config.hpp"

#include <string>

namespace Services {

// What the policy says, before any user interaction.
enum class WritePolicy {
    Allow,          // freely writable — proceed with no prompt
    NeedsConfirm,   // protected — must be confirmed on the console first
    Deny,           // not writable at all, or an unknown/disabled location
};

/// PURE decision: may a mutating operation touch `vfs_path`? Host-testable.
/// Unknown paths and disabled surfaces are DENIED, so a client cannot reach a
/// protected surface by guessing a path that isn't listed.
/// Classify a mutating operation on `vfs_path` under one transport's surface
/// config. `transport`/`operation` are for the decision log only and may be null;
/// the classification never depends on them.
/// Record a refusal that happens BEFORE the guard is consulted, into the same
/// log and the same format. Without this, the branches that reject early are
/// invisible: the operation fails, no modal appears, and the guard log is silent
/// because it was never asked — which is indistinguishable from a guard Deny to
/// anyone reading the log afterwards.
void guard_log_note(const char* transport, const char* operation,
                    const char* decision, const char* reason,
                    const std::string& path);

WritePolicy classify_write(const std::string& vfs_path, const Config::Surfaces& cfg,
                           const char* transport = nullptr,
                           const char* operation = nullptr);

// The final answer after any confirmation has happened.
enum class WriteDecision { Allow, Deny };

/// BLOCKING — call only from a transport WORKER thread, never the main/UI thread
/// (it waits on the modal that the main thread draws, so calling it there would
/// deadlock). Applies classify_write(), and for NeedsConfirm blocks on the
/// ConfirmationBroker until the user answers on the console. Denied by default if
/// the user declines, the request times out, or the app is shutting down.
/// Config is passed in rather than read from the global so this stays testable and
/// has no hidden dependency; callers pass their OWN transport's surfaces
/// (Config::get().ftp.surfaces, .mtp.surfaces, .http.surfaces).
WriteDecision guard_write(const std::string& transport,
                          const std::string& operation,
                          const std::string& vfs_path,
                          const Config::Surfaces& cfg);

/// Rename/move touches TWO locations, and BOTH must pass — moving a file OUT of
/// NAND mutates NAND just as much as moving one in. If either side needs
/// confirmation the user is asked ONCE, with both paths named, rather than being
/// prompted twice for a single logical operation.
WriteDecision guard_move(const std::string& transport,
                         const std::string& operation,
                         const std::string& from_vfs,
                         const std::string& to_vfs,
                         const Config::Surfaces& cfg);

} // namespace Services
