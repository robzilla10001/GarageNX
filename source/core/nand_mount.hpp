// source/core/nand_mount.hpp
//
// Mounts the console's internal NAND partitions as browseable stdio devices so the
// file-manager transports can list and PULL from them:
//   bis_user:    — the User partition (save data, some system files)
//   bis_system:  — the System partition (firmware/system files) — DANGEROUS
//
// These are READ-ONLY by policy (the StorageCatalog marks both ReadOnly +
// OnDevice-confirm). Mounting only exposes them for reading; the write-guard
// (Wave 2c, via the ConfirmationBroker) is what actually blocks mutation. A wrong
// write to bis_system can brick the console, which is why:
//   - NAND system is gated OFF by default in config (nand_system=false)
//   - nothing here ever issues a write; we only mount and browse.
//
// Gated on the catalog: a partition is only mounted if its surface is enabled, so
// a user who hasn't turned NAND system on never even has bis_system: mounted.

#pragma once

namespace Core {

// Mount whichever NAND partitions are enabled in config, as bis_user: / bis_system:.
// Safe to call once at startup. A partition that is disabled or fails to open is
// simply left unmounted — the transports' mount-probe then hides it, so a missing
// mount never shows as a broken folder.
void mount_nand();

// Unmount any NAND devices we mounted. Called during teardown.
void unmount_nand();

} // namespace Core
