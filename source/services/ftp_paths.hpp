// source/services/ftp_paths.hpp
//
// Pure path model for the FTP storage roots — no sockets, no libnx, so it is
// unit-tested on the host. This mirrors MTP's model, where the client picks a
// STORAGE first and its contents live inside:
//
//   /                → a chooser: "SD Card", "SD Install", ["NAND Install"]
//   /SD Card/...     → the real SD filesystem (this is where files live)
//   /SD Install/x    → drop x here to install it to the SD card
//   /NAND Install/x  → drop x here to install it to NAND
//
// The root itself holds NO real files — only the storage folders — so the client
// gets a clean chooser instead of install folders hybridised into the SD listing.
// Names match the MTP storage descriptions so both transports look identical.

#pragma once

#include <string>

namespace Services {

// Storage folder names shown at the FTP root. Identical to the MTP descriptions.
inline constexpr const char* kFtpSdCardDir      = "SD Card";
inline constexpr const char* kFtpSdInstallDir   = "SD Install";
inline constexpr const char* kFtpNandInstallDir = "NAND Install";

enum class FtpTarget {
    Filesystem,   // a path under /SD Card — normal file I/O
    SdInstall,    // under /SD Install — install to SD card
    NandInstall,  // under /NAND Install — install to NAND
    Root,         // the chooser "/" itself — no files, only storage folders
    Invalid,      // a bare path not under any known storage root
};

// Split a POSIX-style absolute path into its first component and the remainder.
// "/SD Install/Game.nsz" → first="SD Install", rest="Game.nsz".
// "/" → first="", rest="".
inline void ftp_split_first(const std::string& posix,
                            std::string& first, std::string& rest) {
    size_t i = 0;
    while (i < posix.size() && posix[i] == '/') ++i;   // skip leading '/'
    size_t start = i;
    while (i < posix.size() && posix[i] != '/') ++i;
    first = posix.substr(start, i - start);
    while (i < posix.size() && posix[i] == '/') ++i;   // skip the separator
    rest = (i < posix.size()) ? posix.substr(i) : std::string();
}

// True when `posix` is exactly the FTP root ("/", "" or all slashes).
inline bool ftp_is_root(const std::string& posix) {
    for (char c : posix) if (c != '/') return false;
    return true;
}

// Classify an absolute POSIX path. For SD Card paths, `rel` receives the path
// RELATIVE to the SD root (what maps under "sdmc:"). For install targets, `rel`
// receives the leaf filename. For Root/Invalid, `rel` is cleared.
inline FtpTarget ftp_classify(const std::string& posix, std::string& rel) {
    rel.clear();
    if (ftp_is_root(posix)) return FtpTarget::Root;

    std::string first, rest;
    ftp_split_first(posix, first, rest);
    if (first == kFtpSdCardDir)      { rel = rest; return FtpTarget::Filesystem; }
    if (first == kFtpSdInstallDir)   { rel = rest; return FtpTarget::SdInstall; }
    if (first == kFtpNandInstallDir) { rel = rest; return FtpTarget::NandInstall; }
    return FtpTarget::Invalid;   // bare path not under a storage root
}

// True when `posix` names a storage folder itself (not a path within it) — the
// install folders and the SD Card folder at depth 1.
inline bool ftp_is_storage_dir(const std::string& posix) {
    std::string first, rest;
    ftp_split_first(posix, first, rest);
    return rest.empty() &&
           (first == kFtpSdCardDir || first == kFtpSdInstallDir ||
            first == kFtpNandInstallDir);
}

// True specifically for the two install folders (write-only drop targets).
inline bool ftp_is_install_dir(const std::string& posix) {
    std::string first, rest;
    ftp_split_first(posix, first, rest);
    return rest.empty() &&
           (first == kFtpSdInstallDir || first == kFtpNandInstallDir);
}

} // namespace Services
