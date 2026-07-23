// source/services/storage_catalog.cpp

#include "services/storage_catalog.hpp"

namespace Services {

using Id = StorageSurface::Id;

// The complete surface table, in display order. This is the ONE place the storage
// set is defined. Notes on the choices:
//   - vfs_root uses the libnx mount names the wave work will mount under. SD is
//     "sdmc:" (already mounted by libnx). NAND user/system, saves, album, and
//     gamecard get mounted in their respective waves; the names here are the
//     intended mount points so transports have a stable prefix to key on.
//   - NAND user + system are ReadOnly + OnDevice: visible and browsable by
//     default, but any mutation requires an on-device confirmation (the
//     cross-transport safety model). system is additionally gated off by default
//     in config (nand_system defaults false).
//   - Gamecard + InstalledTitles are ReadOnly (you don't write to a gamecard;
//     Installed Titles is a synthesized view, not a writable tree).
//   - SdInstall / NandInstall are Install kind: writing a file installs it. They
//     carry no vfs_root because they are not browsable filesystems.
static const std::vector<StorageSurface> kSurfaces = {
    { Id::SdCard,          "sd_card",         "SD Card",
      "sdmc:",       StorageKind::Filesystem, Access::ReadWrite, Confirm::None },

    { Id::SdInstall,       "sd_install",      "SD Install",
      "",            StorageKind::Install,    Access::ReadWrite, Confirm::None },

    { Id::NandInstall,     "nand_install",    "NAND Install",
      "",            StorageKind::Install,    Access::ReadWrite, Confirm::None },

    { Id::NandUser,        "nand_user",       "NAND (User)",
      "bis_user:",   StorageKind::Filesystem, Access::ReadOnly,  Confirm::OnDevice },

    { Id::NandSystem,      "nand_system",     "NAND (System)",
      "bis_system:", StorageKind::Filesystem, Access::ReadOnly,  Confirm::OnDevice },

    { Id::Saves,           "saves",           "Save Data",
      "save:",       StorageKind::Filesystem, Access::ReadOnly,  Confirm::OnDevice },

    { Id::Album,           "album",           "Album",
      "album:",      StorageKind::Filesystem, Access::ReadWrite, Confirm::None },

    { Id::Gamecard,        "gamecard",        "Game Card",
      "gamecard:",   StorageKind::Filesystem, Access::ReadOnly,  Confirm::None },

    { Id::InstalledTitles, "installed_games", "Installed Titles",
      "",            StorageKind::TitleQuery, Access::ReadOnly,  Confirm::None },
};

const std::vector<StorageSurface>& StorageCatalog::all() {
    return kSurfaces;
}

bool StorageCatalog::enabled(Id id, const Config::MTP& c) {
    switch (id) {
        case Id::SdCard:          return c.sd_card;
        case Id::SdInstall:       return c.sd_install;
        case Id::NandInstall:     return c.nand_install;
        case Id::NandUser:        return c.nand_user;
        case Id::NandSystem:      return c.nand_system;
        case Id::Saves:           return c.saves;
        case Id::Album:           return c.album;
        case Id::Gamecard:        return c.gamecard;
        case Id::InstalledTitles: return c.installed_games;
    }
    return false;
}

std::vector<StorageSurface> StorageCatalog::enabled_surfaces(const Config::MTP& c) {
    std::vector<StorageSurface> out;
    for (const auto& s : kSurfaces)
        if (enabled(s.id, c)) out.push_back(s);
    return out;
}

const StorageSurface* StorageCatalog::find(Id id) {
    for (const auto& s : kSurfaces)
        if (s.id == id) return &s;
    return nullptr;
}

const StorageSurface* StorageCatalog::surface_for_vfs(const std::string& vfs_path) {
    // Match on the mount prefix. Only Filesystem surfaces have a real vfs_root;
    // Install/TitleQuery surfaces are virtual and own no concrete path.
    for (const auto& s : kSurfaces) {
        if (s.kind != StorageKind::Filesystem) continue;
        const std::string root = s.vfs_root;          // e.g. "sdmc:", "bis_user:"
        if (root.empty()) continue;
        if (vfs_path.compare(0, root.size(), root) == 0) return &s;
    }
    return nullptr;
}

} // namespace Services
