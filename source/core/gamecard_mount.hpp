#pragma once
// source/core/gamecard_mount.hpp
//
// Mounts an inserted game card's SECURE partition as the stdio device
// "gamecard:", so the file-manager transports and the on-device browser can list
// and pull from it.
//
// The Gamecard surface has been in StorageCatalog since the catalog was written —
// id, display name, vfs_root "gamecard:", ReadOnly — and nothing has ever mounted
// it. Every consumer therefore hid it via the shared mount probe, correctly, for
// the whole project. Providing the mount lights it up on FTP, MTP, HTTP and the
// file browser at once, with no change to any of them. That is the catalog paying
// for itself.
//
// READ-ONLY, and not merely by policy: a game card is physically read-only. The
// catalog marks it ReadOnly/Confirm::None, so the write guard denies mutations
// without even needing an on-device prompt.
//
// A card can be inserted or removed at ANY time, which makes this different from
// NAND: the mount is not a one-shot at startup. refresh() is called periodically
// and mounts or unmounts to follow the physical state.

namespace Core {

/// Mount the inserted card's secure partition as "gamecard:", or unmount if no
/// card is present. Safe to call repeatedly — it only acts on a state CHANGE, so
/// calling it every frame costs one cheap "is a card inserted" query.
///
/// Gated on the catalog: if no transport exposes the Gamecard surface, nothing is
/// mounted, exactly like NAND.
void gamecard_refresh();

/// True if a card is currently mounted at "gamecard:".
bool gamecard_mounted();

/// Unmount if mounted. For shutdown.
void gamecard_unmount();

} // namespace Core
