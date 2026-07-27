// tests/save_surface_test.cpp
//
// Pins the PURE half of the shared Save Data surface: the synthetic path form
// MTP interns object handles under, and the classification that decides whether
// a request can be answered without mounting anything.
//
// The listing and mount halves of save_surface live behind libnx (accounts, fs)
// and can only be exercised on hardware — per this directory's admission rule
// they are NOT stubbed here. What IS testable is exactly the part that has to be
// right for a handle to mean one thing: the prefix, the round trip, and the
// no-mount classification.
//
// Why this file exists at all: a save object's identity cannot be its mounted
// path. "save:/slot1.dat" names a different file depending on which title is
// mounted, and MTP handles must stay valid for a whole session, so two titles'
// files would collide on one handle and a host would silently receive the wrong
// bytes. The synthetic form carries user and title, which is what makes a handle
// unambiguous.

#include "services/save_surface.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

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

static void test_prefix_recognition() {
    CHECK(save_is_synthetic("savedata:/Rob"), "user folder is synthetic");
    CHECK(save_is_synthetic("savedata:/Rob/Zelda [0100000000010000]/slot.dat"),
          "deep path is synthetic");
    CHECK(save_is_synthetic("savedata:/"), "the synthetic root is synthetic");

    CHECK(!save_is_synthetic("save:/slot.dat"), "a MOUNTED save path is not synthetic");
    CHECK(!save_is_synthetic("sdmc:/switch/app.nro"), "SD path is not synthetic");
    CHECK(!save_is_synthetic("titles:/Zelda.nsp"), "titles path is not synthetic");
    CHECK(!save_is_synthetic(""), "empty is not synthetic");
    CHECK(!save_is_synthetic("saved"), "a short lookalike is not synthetic");
    std::printf("  ok: prefix recognition\n");
}

// THE COLLISION HAZARD. StorageCatalog::surface_for_vfs() and MTP's
// mtp_storage_for_path() both identify a surface by a plain prefix compare
// against vfs_root, and the Saves surface's vfs_root is "save:". A synthetic
// prefix that began with "save:" would therefore be mistaken for a mounted save
// path by the WRITE GUARD — which is the difference between default-deny and a
// confirmation modal pointed at whichever title happens to be mounted.
//
// This asserts the property directly rather than trusting the spelling: the
// synthetic prefix must not start with the Saves vfs_root, and vice versa.
static void test_prefix_cannot_collide_with_mount_root() {
    const StorageSurface* saves = StorageCatalog::find(StorageSurface::Id::Saves);
    CHECK(saves != nullptr, "Saves surface exists in the catalog");

    const std::string mount  = saves->vfs_root;        // "save:"
    const std::string synth  = save_synth_prefix();    // "savedata:/"

    CHECK(!mount.empty(), "Saves has a vfs_root");
    CHECK(synth.compare(0, mount.size(), mount) != 0,
          "synthetic prefix must NOT begin with the Saves mount root");
    CHECK(mount.compare(0, synth.size(), synth) != 0,
          "Saves mount root must NOT begin with the synthetic prefix");

    // And the consequence that actually matters: a synthetic path belongs to no
    // catalog surface, so any mutating op on one default-denies until 3e
    // teaches the guard to resolve it first.
    CHECK(StorageCatalog::surface_for_vfs("savedata:/Rob/Game [01]/s.dat") == nullptr,
          "a synthetic save path is claimed by NO surface (default-deny)");
    CHECK(StorageCatalog::surface_for_vfs("save:/s.dat") == saves,
          "a MOUNTED save path is still claimed by the Saves surface");
    std::printf("  ok: synthetic prefix cannot be confused with the mount root\n");
}

static void test_round_trip() {
    const std::string rel = "Rob/Zelda [0100000000010000]/slot1.dat";
    const std::string p   = save_synth_path(rel);
    CHECK(p == "savedata:/Rob/Zelda [0100000000010000]/slot1.dat", "path is built");
    CHECK(save_synth_rel(p) == rel, "rel survives the round trip");

    CHECK(save_synth_path("") == "savedata:/", "empty rel gives the root");
    CHECK(save_synth_rel("savedata:/") == "", "root gives an empty rel");
    CHECK(save_synth_rel("sdmc:/x") == "", "non-synthetic yields no rel");

    // The rel is exactly what sp_split_save() consumes — that shared parser is
    // the reason no second decomposition exists to drift from this one.
    const SavePath sp = sp_split_save(save_synth_rel(p));
    CHECK(sp.level == SavePath::Level::Files, "deep path is file level");
    CHECK(sp.user  == "Rob", "user survives");
    CHECK(sp.title == "Zelda [0100000000010000]", "title survives");
    CHECK(sp.rest  == "slot1.dat", "rest survives");
    std::printf("  ok: synthetic <-> rel round trip\n");
}

// Names with spaces, brackets and dots are the norm here ("Zelda [0100...]"),
// and a user nickname is arbitrary text. None of it may disturb the split.
static void test_awkward_names() {
    const std::string rel = "Bob Jr./Some.Game [0100ABCDEF012000]/sub dir/f.bin";
    const SavePath sp = sp_split_save(save_synth_rel(save_synth_path(rel)));
    CHECK(sp.user  == "Bob Jr.", "dotted user name survives");
    CHECK(sp.title == "Some.Game [0100ABCDEF012000]", "bracketed title survives");
    CHECK(sp.rest  == "sub dir/f.bin", "nested rest survives");
    std::printf("  ok: awkward names\n");
}

// The classification that keeps browsing cheap. An MTP host asks for an
// ObjectInfo for EVERY object it lists; if answering "is this a directory?" for a
// title folder required a mount, browsing one user's folder would mount and
// unmount every save on the console — the bulk-churn pattern that has already
// crashed this project once. User and title folders are directories by
// construction, so they must be answerable without touching the mount.
static void test_no_mount_needed_for_synthesized_levels() {
    CHECK(save_synth_is_synthesized_dir("savedata:/"),
          "storage root needs no mount");
    CHECK(save_synth_is_synthesized_dir("savedata:/Rob"),
          "user folder needs no mount");
    CHECK(save_synth_is_synthesized_dir("savedata:/Rob/Zelda [0100000000010000]"),
          "title folder needs no mount");

    CHECK(!save_synth_is_synthesized_dir("savedata:/Rob/Zelda [0100000000010000]/s.dat"),
          "a file inside a save DOES need the mount");
    CHECK(!save_synth_is_synthesized_dir("savedata:/Rob/Zelda [0100000000010000]/sub/f"),
          "a nested entry DOES need the mount");

    CHECK(!save_synth_is_synthesized_dir("sdmc:/switch"),
          "a non-synthetic path is not a synthesized save dir");
    std::printf("  ok: synthesized levels need no mount\n");
}

// A trailing slash is what a client sends when it treats a folder as a folder;
// sp_split_save already handles it, and the synthetic wrapper must not undo that.
static void test_trailing_slash() {
    CHECK(save_synth_is_synthesized_dir("savedata:/Rob/"),
          "user folder with trailing slash is still a synthesized dir");
    const SavePath sp = sp_split_save(save_synth_rel("savedata:/Rob/"));
    CHECK(sp.level == SavePath::Level::Titles, "trailing slash stays at title level");
    CHECK(sp.user == "Rob", "user still parsed");
    std::printf("  ok: trailing slash\n");
}

// The write path's classification, mirrored from FTP: a mutating op inside a
// title's save must reach the guard as a MOUNTED "save:/..." path (so the broker
// raises the on-device confirmation and an Allow performs it), while the levels
// above a title have no file behind them and are refused with no prompt at all.
//
// The pure, testable half of that is which paths are "inside a title" — the same
// split FTP's to_vfs() makes when it returns "" for the synthesized levels.
static void test_write_levels_match_ftp() {
    // Refused outright, no prompt: nothing here names a file.
    CHECK(sp_split_save(save_synth_rel("savedata:/")).level == SavePath::Level::Users,
          "storage root is the users level — no title, no prompt");
    CHECK(sp_split_save(save_synth_rel("savedata:/Rob")).level == SavePath::Level::Titles,
          "user folder is the titles level — no title, no prompt");

    // Reaches the guard as a real path, so the confirmation names the file.
    const SavePath in_save =
        sp_split_save(save_synth_rel("savedata:/Rob/Zelda [0100000000010000]/slot.dat"));
    CHECK(in_save.level == SavePath::Level::Files, "a file inside a save is guardable");
    CHECK(in_save.title == "Zelda [0100000000010000]",
          "the title is carried to the guard, so the mount is the right one");

    // A NEW name inside a title (the SendObjectInfo case) is equally guardable —
    // it need not already exist for the destination to be resolvable.
    const SavePath fresh =
        sp_split_save(save_synth_rel("savedata:/Rob/Zelda [0100000000010000]/new.dat"));
    CHECK(fresh.level == SavePath::Level::Files, "a not-yet-existing target resolves");
    CHECK(fresh.rest == "new.dat", "its leaf is carried through");

    // And the guard must never see the synthetic form itself: no surface claims
    // it, so guarding it directly would default-deny instead of prompting. This
    // is why every mutating path resolves BEFORE it guards.
    CHECK(StorageCatalog::surface_for_vfs("savedata:/Rob/Z [01]/slot.dat") == nullptr,
          "synthetic form is unguardable — resolve first, then guard");
    std::printf("  ok: write levels match FTP\n");
}

// THE MOUNT ROOT IS NOT AN OBJECT. A title folder in the display hierarchy,
// "/Save Data/<User>/<Title>", resolves to exactly "save:/" — perfectly good to
// LIST, which is why browsing a save works, but not a legal target for delete or
// rename: it is the filesystem, not a file in it.
//
// Reported from hardware: deleting a save produced TWO confirmation dialogs, both
// approved, and the operation failed anyway. That is what an ordinary client does
// when it deletes a folder — clear the contents, then remove the folder — and the
// second prompt was asking permission for an rmdir of a mount point, which cannot
// succeed. A confirmation for something that cannot succeed is worse than none:
// it teaches people that approving these prompts does nothing.
static void test_mount_root_is_not_a_target() {
    CHECK(save_is_mount_root("save:/"), "the mount root with a slash");
    CHECK(save_is_mount_root("save:"),  "and without one");

    CHECK(!save_is_mount_root("save:/slot1.dat"), "a file inside it IS a target");
    CHECK(!save_is_mount_root("save:/sub"),       "so is a directory inside it");
    CHECK(!save_is_mount_root("sdmc:/"),          "another device root is unrelated");
    CHECK(!save_is_mount_root(""),                "empty is not the mount root");
    CHECK(!save_is_mount_root("savedata:/Rob"),   "a synthetic path is not it either");

    // The path a title folder resolves to is exactly the root — which is the
    // whole reason this check has to exist rather than being obvious.
    const SavePath title_folder = sp_split_save("Rob/Zelda [0100000000010000]");
    CHECK(title_folder.level == SavePath::Level::Files, "a title folder is file level");
    CHECK(title_folder.rest.empty(), "with an empty remainder — i.e. the root itself");

    // A file INSIDE the save resolves to a rest, so it is NOT the mount root and
    // deletes normally rather than being turned into a wipe. This is the boundary
    // the delete bug hinged on: file -> normal delete, title folder -> wipe.
    const SavePath a_file = sp_split_save("Rob/Zelda [0100000000010000]/slot1.dat");
    CHECK(a_file.level == SavePath::Level::Files, "a file is file level");
    CHECK(a_file.rest == "slot1.dat", "with a non-empty remainder — a real object");
    std::printf("  ok: the save mount root is not a mutation target\n");
}

// The predicate that decides when the on-device Save Manager STOPS re-labelling.
// It must match exactly the id-only fallback save_title_labels() emits, and NOT a
// real game whose name happens to start with "Title" — a false positive there
// pins the screen in a permanent per-frame relabel.
static void test_unresolved_label_predicate() {
    CHECK(save_label_is_unresolved("Title 0100000000010000"), "the exact id fallback");
    CHECK(save_label_is_unresolved("Title ABCDEF0123456789"), "any 16 hex digits");

    CHECK(!save_label_is_unresolved("Zelda [0100000000010000]"), "a resolved name");
    CHECK(!save_label_is_unresolved("Title Quest"), "a real game called 'Title Quest'");
    CHECK(!save_label_is_unresolved("Titles"), "a real game called 'Titles'");
    CHECK(!save_label_is_unresolved("Title 0100"), "too few hex digits");
    CHECK(!save_label_is_unresolved("Title 0100000000010000X"), "trailing junk");
    CHECK(!save_label_is_unresolved("Title 010000000001000g"), "a non-hex digit");
    CHECK(!save_label_is_unresolved("title 0100000000010000"), "wrong case prefix");
    CHECK(!save_label_is_unresolved(""), "empty");
    std::printf("  ok: unresolved-label predicate is exact\n");
}

int main() {
    std::printf("save_surface_test\n");
    test_prefix_recognition();
    test_prefix_cannot_collide_with_mount_root();
    test_round_trip();
    test_awkward_names();
    test_no_mount_needed_for_synthesized_levels();
    test_trailing_slash();
    test_write_levels_match_ftp();
    test_mount_root_is_not_a_target();
    test_unresolved_label_predicate();
    std::printf("save_surface_test: %d checks passed\n", g_checks);
    return 0;
}
