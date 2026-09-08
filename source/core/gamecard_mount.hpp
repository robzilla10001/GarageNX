#pragma once
// source/core/gamecard_mount.hpp
//
// Mounts an inserted game card's SECURE partition as the stdio device
// "gamecard:", so the file-manager transports and the on-device browser can
// list and pull from it. The surface has been in StorageCatalog from the start;
// providing the mount lights it up on FTP, MTP, HTTP and the file browser at
// once, with no change to any of them.
//
// READ-ONLY, and not merely by policy: a game card is physically read-only, so
// the catalog's ReadOnly/Confirm::None marks make the write guard deny
// mutations without an on-device prompt.
//
// A card can be inserted or removed at ANY time, so unlike NAND the mount is
// not a one-shot at startup: refresh() follows the physical state.

namespace Core {

/// Mount the inserted card's secure partition as "gamecard:", or unmount if no
/// card is present. Safe to call repeatedly - it only acts on a state CHANGE, so
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
