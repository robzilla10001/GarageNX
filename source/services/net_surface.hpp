#pragma once
// source/services/net_surface.hpp
//
// THE shared Network (SMB / NFS) surface. Everything a consumer needs to present
//
//     /Network/<Connection>/...
//
// lives here so that the on-device browser and — later — FTP, MTP and HTTP
// cannot drift into different connection lists, different labels, or (the
// dangerous one) different mount policies. This is the same anti-drift move as
// StorageCatalog for storages, NspStream for NSP building, and save_surface for
// Save Data: ONE implementation, several thin adapters.
//
// It is modelled directly on save_surface, because the two surfaces have the
// same awkward shape: a synthesized top level that lists things (there, users
// and titles; here, the configured connections) and a real mount that only
// becomes valid once a specific leaf is entered. A network share, like "save:",
// has no single always-valid vfs_root — nothing is mounted until a connection is
// opened, and only one connection is mounted at a time (single-slot).
//
// ── What is PURE here, and what is a hardware seam ───────────────────────────
// The header is pure and host-testable: the protocol/port model, the display
// path decomposition, the synthetic-prefix helpers, and the session-credential
// store. The actual SMB/NFS connect + mount lives in net_surface.cpp behind
// #ifdef PLATFORM_SWITCH and is deliberately NOT written against a guessed API —
// see the note at the top of that file. Keeping the description pure is what
// makes the level logic testable without a console (as with sp_split_save).
//
// ── Session credentials are NEVER persisted ──────────────────────────────────
// The password is prompt-per-session by decision: it never lands in config.json.
// NFS is host/UID-based and needs none; SMB prompts at connect time and the
// answer lives only in memory, in net_credentials(), for the life of the
// session. config only stores the connection DEFINITION (see Config::NetShare).
//
// ── Synthetic paths (for the future MTP handle wave) ─────────────────────────
// MTP interns handles by path and a handle must stay valid for a whole session.
// A mounted "net:/foo" is not a safe handle key — "net:" means a different
// server depending on what is connected, exactly like "save:". So a network
// object is named by a synthetic path that carries the connection identity:
//
//     network:/<Connection>/<rest...>
//
// The part after the prefix is exactly the `rel` net_split() parses, so there is
// only one parser and it cannot drift. The concrete "net:/..." path is derived
// at the moment of use, via net_resolve(). The prefix deliberately does NOT
// begin with the mount root "net:" — StorageCatalog::surface_for_vfs() and the
// write guard match a mount prefix with a plain string compare, and a synthetic
// path that matched "net:" would be mistaken for a real mounted one. ("network:/"
// and "net:" share no prefix: compare("network:/"[0..4], "net:") is "netw" != "net:".)

#include "config/config.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Services {

// ── Protocol ──────────────────────────────────────────────────────────────────

enum class NetProtocol { Unknown, Smb, Nfs };

inline NetProtocol net_protocol_parse(const std::string& s) {
    if (s == "smb") return NetProtocol::Smb;
    if (s == "nfs") return NetProtocol::Nfs;
    return NetProtocol::Unknown;
}

inline const char* net_protocol_str(NetProtocol p) {
    switch (p) {
        case NetProtocol::Smb: return "smb";
        case NetProtocol::Nfs: return "nfs";
        default:               return "";
    }
}

// Default TCP port for a protocol, used when a connection leaves `port` at 0.
// SMB-over-TCP is 445; NFS is 2049. Unknown returns 0 rather than a plausible
// guess, so a connection with a bad protocol string cannot silently dial a port
// that happens to be listening.
inline uint16_t net_default_port(NetProtocol p) {
    switch (p) {
        case NetProtocol::Smb: return 445;
        case NetProtocol::Nfs: return 2049;
        default:               return 0;
    }
}

// The port a connection will actually use: its explicit port, or the protocol
// default when it left the field at 0.
inline uint16_t net_effective_port(const Config::NetShare& s) {
    return s.port ? s.port : net_default_port(net_protocol_parse(s.protocol));
}

// Whether this connection needs a password prompt at connect time. SMB may
// (unless it is a guest/anonymous connection with an empty username); NFS never
// does. Consumers use this to decide whether to raise the credential prompt
// before handing the choke point a Files-level path.
inline bool net_needs_password(const Config::NetShare& s) {
    return net_protocol_parse(s.protocol) == NetProtocol::Smb && !s.username.empty();
}

// ── Synthetic prefix (MTP handle keys — see the header comment) ───────────────

inline const char* net_synth_prefix()     { return "network:/"; }
inline std::size_t  net_synth_prefix_len() { return 9; }

/// True if `p` is a synthetic network path (a handle key), NOT a mounted
/// "net:/..." path.
inline bool net_is_synthetic(const std::string& p) {
    return p.compare(0, net_synth_prefix_len(), net_synth_prefix()) == 0;
}

/// Build the synthetic path for a display-relative network path.
/// net_synth_path("NAS/Movies/a.mkv") -> "network:/NAS/Movies/a.mkv"
inline std::string net_synth_path(const std::string& rel) {
    return std::string(net_synth_prefix()) + rel;
}

/// Inverse: strip the prefix, yielding the `rel` net_split() consumes. Returns
/// "" for the storage root ("network:/") and for a non-synthetic path.
inline std::string net_synth_rel(const std::string& p) {
    if (!net_is_synthetic(p)) return std::string();
    return p.substr(net_synth_prefix_len());
}

// ── Display-path decomposition ────────────────────────────────────────────────
//
// `rel` is the part AFTER "/Network/" — i.e. what a catalog resolver hands back
// as the Network surface's relative path — decomposed into its two real levels:
//
//   ""                      -> Connections : list the configured connections
//   "<Conn>"                -> Files, rest="" : that connection's share ROOT
//   "<Conn>/<rest...>"      -> Files          : inside the mounted share
//
// Only one synthesized level (the chooser). A connection folder maps to the
// share root, which is a directory by construction, so it can be listed like any
// other folder once mounted. This mirrors sp_split_save() minus the middle level.

struct NetPath {
    enum class Level { Connections, Files };
    Level       level = Level::Connections;
    std::string connection;   // display name of the chosen connection
    std::string rest;         // remainder inside the share, "" at the share root
};

inline NetPath net_split(const std::string& rel) {
    NetPath out;
    if (rel.empty()) { out.level = NetPath::Level::Connections; return out; }

    out.level = NetPath::Level::Files;
    const std::size_t slash = rel.find('/');
    if (slash == std::string::npos) {
        out.connection = rel;               // "<Conn>" — the share root
        return out;
    }
    out.connection = rel.substr(0, slash);
    out.rest       = rel.substr(slash + 1); // may be "" for a trailing slash
    return out;
}

/// True if the synthetic path names something a consumer can classify as a
/// directory WITHOUT connecting: the chooser root, or a connection's share root.
/// Only a Files path with a non-empty `rest` needs a live mount to answer.
///
/// This matters for the same reason it does for saves: an MTP host asks for an
/// ObjectInfo for every object it lists, so answering the connection level by
/// mounting would connect and disconnect a server just to browse the chooser —
/// exactly the bulk-churn the single-slot design exists to avoid.
inline bool net_synth_is_synthesized_dir(const std::string& p) {
    if (!net_is_synthetic(p)) return false;
    const std::string rel = net_synth_rel(p);
    if (rel.empty()) return true;                 // the chooser root
    return net_split(rel).rest.empty();           // a connection's share root
}

// ── Connection lookup / labelling (PURE — takes the list explicitly) ──────────
//
// The pure forms take the shares vector so they are host-testable with no global
// state; net_connection_names() in the .cpp is the thin wrapper that reads the
// live config. (Same split as save_surface: pure predicates inline here, the
// global-reading listings non-inline in the .cpp.)

/// Find a configured connection by its display name. nullptr if none matches.
inline const Config::NetShare* net_find(const std::vector<Config::NetShare>& shares,
                                        const std::string& name) {
    for (const auto& s : shares)
        if (s.name == name) return &s;
    return nullptr;
}

/// The label shown for a connection in the chooser: its name, with the protocol
/// and host in brackets so two connections to different servers are
/// distinguishable at a glance. Pure; the SINGLE definition of that label so a
/// label built for display always matches one matched on selection.
inline std::string net_display_label(const Config::NetShare& s) {
    std::string proto = net_protocol_str(net_protocol_parse(s.protocol));
    if (proto.empty()) proto = "?";
    return s.name + "  [" + proto + "://" + s.host + "]";
}

// ── Session credentials (in memory ONLY — never persisted) ────────────────────
//
// A tiny per-connection store for the prompt-per-session password. The choke
// point reads it when it needs to connect an SMB share; the credential-prompt UI
// (a later wave) writes it. Cleared connection-by-connection, or wholesale at
// app teardown — never written to disk.
//
// Linear scan over a vector: the number of configured connections is small
// (single digits), so a map buys nothing and a vector keeps the state trivially
// inspectable.
class NetCredentials {
public:
    void set(const std::string& connection, const std::string& password) {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& kv : creds_) {
            if (kv.first == connection) { kv.second = password; return; }
        }
        creds_.emplace_back(connection, password);
    }

    /// Copies the held password into `out` and returns true if one is held.
    /// Returns false (and leaves `out` untouched) if none is held for this
    /// connection — which the choke point treats as "prompt first", NOT as an
    /// empty password.
    bool get(const std::string& connection, std::string& out) const {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& kv : creds_)
            if (kv.first == connection) { out = kv.second; return true; }
        return false;
    }

    void clear(const std::string& connection) {
        std::lock_guard<std::mutex> lk(m_);
        for (std::size_t i = 0; i < creds_.size(); ++i) {
            if (creds_[i].first == connection) {
                creds_.erase(creds_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    void clear_all() {
        std::lock_guard<std::mutex> lk(m_);
        creds_.clear();
    }

private:
    mutable std::mutex m_;
    std::vector<std::pair<std::string, std::string>> creds_;
};

/// The process-wide session credential store. Function-local static so there is
/// exactly one across every translation unit that includes this header.
inline NetCredentials& net_credentials() {
    static NetCredentials store;
    return store;
}

// ── The choke point (declared here, defined in net_surface.cpp) ───────────────

/// Every configured connection's display name, in config order. Reads the live
/// config; the pure host tests exercise net_find()/net_display_label() instead.
std::vector<std::string> net_connection_names();

/// Apply the single-slot mount policy for one request and return the concrete
/// path to use.
///
/// Returns "net:/..." for a Files-level request whose connection mounted
/// successfully; returns "" for the Connections level and for any failure — and
/// in both of those cases the mount is RELEASED. SMB connections read their
/// session password from net_credentials(); a Files request for a connection
/// that needs a password but has none held fails (the caller is expected to
/// prompt and set it first).
///
/// NOTE: the connect/mount body is a hardware seam — see net_surface.cpp.
std::string net_resolve(const NetPath& np);

/// Convenience for the synthetic-path callers: split and resolve in one step.
/// `synth` is a full "network:/..." path.
std::string net_resolve_synth(const std::string& synth);

/// Release the mounted connection, if any. Does NOT clear session credentials —
/// switching between connections must not discard a password the user just
/// entered. For app teardown, call net_credentials().clear_all() as well.
void net_surface_release();

/// A human-readable reason for the most recent net_resolve() that returned "".
/// Set at every failure (and at the "connected but browsing not built yet" case)
/// so callers can both log it and show it, instead of a generic message. Empty
/// if the last resolve succeeded or none has run.
std::string net_last_error();

} // namespace Services
