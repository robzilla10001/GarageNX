// tests/net_surface_test.cpp
//
// Pins the PURE half of the shared Network (SMB/NFS) surface: the protocol/port
// model, the "/Network/<Connection>/<rest>" decomposition, the synthetic-prefix
// helpers MTP will intern handles under, the no-mount directory classification,
// and the in-memory session-credential store.
//
// The connect/mount half lives behind libsmb2/libnfs and can only be exercised
// on hardware — per this directory's admission rule it is NOT stubbed here. What
// IS testable is exactly the part that has to be right before a byte moves: a
// handle's identity, the level a path names, and the promise that a password
// never outlives the session in a way this store can observe.

#include "services/net_surface.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Services;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static Config::NetShare make(const std::string& name, const std::string& proto,
                             const std::string& host, uint16_t port = 0,
                             const std::string& user = "") {
    Config::NetShare s;
    s.name = name; s.protocol = proto; s.host = host; s.port = port; s.username = user;
    return s;
}

static void test_protocol_and_ports() {
    CHECK(net_protocol_parse("smb") == NetProtocol::Smb, "smb parses");
    CHECK(net_protocol_parse("nfs") == NetProtocol::Nfs, "nfs parses");
    CHECK(net_protocol_parse("")    == NetProtocol::Unknown, "empty is unknown");
    CHECK(net_protocol_parse("SMB") == NetProtocol::Unknown, "parse is case-sensitive");

    CHECK(std::string(net_protocol_str(NetProtocol::Smb)) == "smb", "smb formats");
    CHECK(std::string(net_protocol_str(NetProtocol::Nfs)) == "nfs", "nfs formats");
    CHECK(std::string(net_protocol_str(NetProtocol::Unknown)).empty(), "unknown formats empty");

    CHECK(net_default_port(NetProtocol::Smb) == 445,  "SMB default port");
    CHECK(net_default_port(NetProtocol::Nfs) == 2049, "NFS default port");
    CHECK(net_default_port(NetProtocol::Unknown) == 0,
          "unknown gets 0, never a plausible guess");

    CHECK(net_effective_port(make("a", "smb", "h"))       == 445,  "SMB falls to default");
    CHECK(net_effective_port(make("a", "nfs", "h"))       == 2049, "NFS falls to default");
    CHECK(net_effective_port(make("a", "smb", "h", 1445)) == 1445, "explicit port wins");
    CHECK(net_effective_port(make("a", "bad", "h"))       == 0,    "bad protocol has no port");
    std::printf("  ok: protocol + port model\n");
}

static void test_password_requirement() {
    CHECK(net_needs_password(make("a", "smb", "h", 0, "rob")), "named SMB needs a password");
    CHECK(!net_needs_password(make("a", "smb", "h", 0, "")),   "guest SMB does not");
    CHECK(!net_needs_password(make("a", "nfs", "h", 0, "rob")),
          "NFS never needs a password (host/UID based) even if a user is set");
    std::printf("  ok: password-required predicate\n");
}

static void test_synthetic_prefix() {
    CHECK(net_is_synthetic("network:/NAS"), "connection folder is synthetic");
    CHECK(net_is_synthetic("network:/NAS/Movies/a.mkv"), "deep path is synthetic");
    CHECK(net_is_synthetic("network:/"), "the synthetic root is synthetic");

    CHECK(!net_is_synthetic("net:/a.mkv"), "a MOUNTED network path is not synthetic");
    CHECK(!net_is_synthetic("sdmc:/switch/app.nro"), "an SD path is not synthetic");
    CHECK(!net_is_synthetic(""), "empty is not synthetic");
    std::printf("  ok: synthetic prefix recognition\n");
}

// The synthetic prefix and the mount root MUST NOT be confusable, or the write
// guard (which matches the "net:" mount root by a plain string compare) would
// treat a synthetic handle path as a live mounted one — the "savedata:/" vs
// "save:" hazard, in this surface.
static void test_prefix_cannot_collide_with_mount_root() {
    const std::string synth = "network:/NAS/x";
    CHECK(synth.compare(0, 4, "net:") != 0,
          "a synthetic path does NOT start with the mount root \"net:\"");
    CHECK(net_is_synthetic(synth), "...and IS recognised as synthetic");
    std::printf("  ok: synthetic prefix cannot be mistaken for the mount root\n");
}

static void test_synth_round_trip() {
    const std::string rel = "NAS/Movies/a.mkv";
    CHECK(net_synth_rel(net_synth_path(rel)) == rel, "rel -> synth -> rel round trips");
    CHECK(net_synth_path("") == "network:/", "empty rel is the storage root");
    CHECK(net_synth_rel("network:/").empty(), "storage root yields empty rel");
    CHECK(net_synth_rel("sdmc:/x").empty(), "a non-synthetic path yields empty rel");
    std::printf("  ok: synthetic path round trip\n");
}

static void test_split_levels() {
    NetPath a = net_split("");
    CHECK(a.level == NetPath::Level::Connections, "empty rel = the chooser");

    NetPath b = net_split("NAS");
    CHECK(b.level == NetPath::Level::Files, "one component = a connection (Files)");
    CHECK(b.connection == "NAS" && b.rest.empty(), "connection captured, rest empty");

    NetPath c = net_split("NAS/");
    CHECK(c.level == NetPath::Level::Files && c.connection == "NAS" && c.rest.empty(),
          "trailing slash is still the share root");

    NetPath d = net_split("NAS/Movies/a.mkv");
    CHECK(d.level == NetPath::Level::Files, "deep path = Files");
    CHECK(d.connection == "NAS", "connection captured on a deep path");
    CHECK(d.rest == "Movies/a.mkv", "rest is everything below the connection");
    std::printf("  ok: display-path decomposition\n");
}

// A consumer must be able to answer "is this a directory?" for the chooser root
// and a connection's share root WITHOUT connecting — otherwise browsing the
// chooser would connect and disconnect servers just to draw the list.
static void test_no_mount_needed_for_synthesized_levels() {
    CHECK(net_synth_is_synthesized_dir("network:/"), "the chooser root is a dir, no mount");
    CHECK(net_synth_is_synthesized_dir("network:/NAS"),
          "a connection's share root is a dir, no mount");
    CHECK(net_synth_is_synthesized_dir("network:/NAS/"),
          "trailing slash on the share root too");
    CHECK(!net_synth_is_synthesized_dir("network:/NAS/Movies/a.mkv"),
          "a file inside the share needs a live mount to classify");
    CHECK(!net_synth_is_synthesized_dir("net:/NAS"),
          "a non-synthetic path is not answered here at all");
    std::printf("  ok: no-mount classification of synthesized levels\n");
}

static void test_find_and_label() {
    std::vector<Config::NetShare> shares = {
        make("NAS",   "smb", "192.168.1.10"),
        make("Media", "nfs", "nas.local"),
    };
    CHECK(net_find(shares, "NAS")   != nullptr, "existing connection found");
    CHECK(net_find(shares, "Media") != nullptr, "second connection found");
    CHECK(net_find(shares, "nope")  == nullptr, "missing connection is null");
    CHECK(net_find({}, "NAS")       == nullptr, "empty list finds nothing");

    CHECK(net_display_label(shares[0]) == "NAS  [smb://192.168.1.10]", "SMB label");
    CHECK(net_display_label(shares[1]) == "Media  [nfs://nas.local]",  "NFS label");
    CHECK(net_display_label(make("x", "bad", "h")) == "x  [?://h]",
          "an unknown protocol still yields a distinguishable label");
    std::printf("  ok: connection lookup + labelling\n");
}

static void test_credential_store() {
    NetCredentials c;
    std::string out = "SENTINEL";

    CHECK(!c.get("NAS", out), "nothing held initially");
    CHECK(out == "SENTINEL", "a miss leaves the out-param untouched (not an empty pw)");

    c.set("NAS", "hunter2");
    CHECK(c.get("NAS", out) && out == "hunter2", "set then get returns the password");

    // Per-connection isolation: one connection's password is not another's.
    out = "SENTINEL";
    CHECK(!c.get("Media", out), "a different connection has no password");
    CHECK(out == "SENTINEL", "and its miss also leaves out untouched");

    c.set("NAS", "changed");
    CHECK(c.get("NAS", out) && out == "changed", "set overwrites in place");

    c.set("Media", "nfspw");
    c.clear("NAS");
    CHECK(!c.get("NAS", out), "clear removes one connection");
    out = "SENTINEL";
    CHECK(c.get("Media", out) && out == "nfspw", "...and leaves the others");

    c.clear_all();
    out = "SENTINEL";
    CHECK(!c.get("Media", out), "clear_all wipes everything (session teardown)");
    std::printf("  ok: session credential store\n");
}

int main() {
    std::printf("net_surface_test\n");
    test_protocol_and_ports();
    test_password_requirement();
    test_synthetic_prefix();
    test_prefix_cannot_collide_with_mount_root();
    test_synth_round_trip();
    test_split_levels();
    test_no_mount_needed_for_synthesized_levels();
    test_find_and_label();
    test_credential_store();
    std::printf("net_surface_test: %d checks passed\n", g_checks);
    return 0;
}
