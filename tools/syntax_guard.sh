#!/usr/bin/env bash
# tools/syntax_guard.sh
#
# Structural check for the libnx-only sources that the host test build does NOT
# compile (mtp_server.cpp, ftp_server.cpp, save_surface.cpp, *_mount.cpp,
# nsp_stream.cpp, main.cpp, ...). A structural break in one of those passes
# "all tests" and fails only on the devkitPro build — that has already happened
# twice (a str_replace ate a `case` header; another dropped a wire field).
#
# WHY THIS SCRIPT EXISTS RATHER THAN A ONE-LINER
#
# The documented guard was
#
#     g++ -fsyntax-only -std=c++17 -I source -DPLATFORM_SWITCH <file>
#     # header-not-found errors are EXPECTED and should be ignored
#
# and it never checked anything. A missing header is a FATAL error, not an
# ignorable one: g++ prints "fatal error: switch.h: No such file or directory /
# compilation terminated." and stops. The parser never reaches the function
# bodies, so the exact breakage the guard was added to catch sails through. On
# mtp_server.cpp the real guard produced six lines of output and zero coverage.
#
# The fix is to satisfy the PREPROCESSOR so the PARSER runs. tools/stubs/ holds
# EMPTY stand-ins for the platform headers. Types then go undeclared, which
# produces a large number of ordinary non-fatal errors — those are expected and
# filtered out. What survives the filter is structure: unbalanced braces, a
# missing `case`, a stray declaration, a jump crossing an initialisation.
#
# LIMITS (all of these have bitten this project — do not treat a pass as safety):
#   * It stops before type-checking, so WRONG TYPES AND WRONG FIELD NAMES PASS.
#     A manual audit of each API call against its real header is still required.
#   * It cannot see use-before-definition of file-local statics; scan for that
#     separately when adding helpers (this script does a crude check, below).
#   * A green run means "this file will parse", never "this file is correct".
#
# An unterminated #ifdef/#ifndef/#if used to be one of these blind spots: g++
# reports it (it's in STRUCTURAL below), but only once the parser reaches EOF,
# and a missing project header aborts the parse first — the BLIND path fires
# and the actual break underneath never gets checked. A str_replace that
# nested a new #ifdef PLATFORM_SWITCH inside an existing one instead of
# sequencing after it reached the devkitPro build this way: guard said "ok"
# because a missing header made it BLIND before EOF. Fixed by a lexical
# #if-family balance pass (below) that runs before the g++ invocation and
# doesn't depend on it succeeding.
#
# Usage:
#   tools/syntax_guard.sh                      # every libnx-only file
#   tools/syntax_guard.sh source/services/mtp_server.cpp ...

set -uo pipefail

cd "$(dirname "$0")/.."
STUBS="tools/stubs"

DEFAULT_FILES=(
    source/main.cpp
    source/services/mtp_server.cpp
    source/services/ftp_server.cpp
    source/services/http_server.cpp
    source/services/save_surface.cpp
    source/screens/settings_screen.cpp
    source/screens/save_manager.cpp
    source/screens/save_backup_screen.cpp
    source/screens/activity_log.cpp
    source/screens/gamecard.cpp
    source/core/es.cpp
    source/core/activity.cpp
    source/ui/backup_overlay.cpp
    source/screens/file_browser.cpp
    source/screens/network_browser.cpp
    source/screens/network_edit.cpp
    source/ui/modal.cpp
    source/ui/font.cpp
    source/services/title_surface.cpp
    source/core/album_mount.cpp
    source/core/nand_mount.cpp
    source/core/gamecard_mount.cpp
    source/core/usb_mount.cpp
    source/core/save_mount.cpp
    source/core/save_backup.cpp
    source/core/nsp_stream.cpp
    source/core/dump.cpp

    # ── Added after an audit found 52 compilable files with NO mechanical
    # check at all. The host suite does not compile any of these, so before
    # this the only thing standing between a typo in them and the devkitPro
    # build was review. Everything here PARSES today; the two that do not
    # (install/installer.cpp, install/ncz.cpp) are listed in the comment at
    # the end of this array with the reason.
    source/config/config.cpp
    source/core/atmosphere.cpp
    source/core/battery.cpp
    source/core/datetime.cpp
    source/core/fs.cpp
    source/core/keys.cpp
    source/core/nca.cpp
    source/core/nca_modify.cpp
    source/core/ncm.cpp
    source/core/net.cpp
    source/core/ntp.cpp
    source/core/qr.cpp
    source/core/record_probe.cpp
    source/core/sleep_inhibit.cpp
    source/core/storage.cpp
    source/core/system.cpp
    source/core/title_ops.cpp
    source/install/ncz_window.cpp
    source/install/nsp_reader.cpp
    source/install/stream_driver.cpp
    source/install/stream_installer.cpp
    source/install/xci_reader.cpp
    source/install/installer.cpp
    source/install/ncz.cpp
    source/lang/localization.cpp
    source/screens/file_viewer.cpp
    source/screens/ftp_screen.cpp
    source/screens/http_screen.cpp
    source/screens/main_menu.cpp
    source/screens/menu_dispatch.cpp
    source/screens/mtp_screen.cpp
    source/screens/submenu_screen.cpp
    source/screens/system_info.cpp
    source/screens/title_detail.cpp
    source/screens/title_list.cpp
    source/screens/title_test.cpp
    source/services/confirmation_broker.cpp
    source/services/mtp_data.cpp
    source/services/overlap_buffer.cpp
    source/services/pfs0_layout.cpp
    source/services/rate_meter.cpp
    source/services/service_manager.cpp
    source/services/storage_catalog.cpp
    source/services/net_surface.cpp
    source/services/title_naming.cpp
    source/services/write_guard.cpp
    source/ui/input.cpp
    source/ui/keyboard.cpp
    source/ui/layout.cpp
    source/ui/renderer.cpp
    source/ui/splash.cpp
    source/ui/status_bar.cpp
    source/ui/theme.cpp
    source/ui/title_bar.cpp
    source/ui/widgets.cpp
)

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then FILES=("${DEFAULT_FILES[@]}"); fi

# Errors that mean the STRUCTURE is broken, as opposed to the expected flood of
# "undeclared identifier" from the empty stubs.
# Deliberately NARROW. An earlier draft also matched "expected ';' before X",
# which fires on every `u32 n = 0;` and `s32 i = 0;` in the tree — the stubs
# leave libnx's typedefs undeclared, so a plain variable declaration looks like a
# syntax error. That is a TYPE symptom, not a structural one, and including it
# made the guard cry wolf on files that were perfectly well-formed. Only patterns
# that indicate genuinely broken structure belong here.
# Errors that are STRUCTURAL — i.e. real mistakes in this file, not artefacts of
# compiling without libnx. Anything caused by a missing type or header must stay
# OUT of this list, or the guard drowns in noise and gets ignored.
#
# 'duplicate case value' and 'redefinition' were added after a duplicate case
# label in menu_dispatch.cpp reached the Switch build: g++ reported it here, the
# filter discarded it as noise, and the guard printed a green tick. Neither can be
# produced by a missing header, so both are safe to trust.
STRUCTURAL='not within a switch|expected declaration|expected .\}.|expected unqualified-id|at end of input|crosses initialization|jump to case label|extra .\}.|unterminated|duplicate case value|redefinition of|multiple definition of'

fail=0
for f in "${FILES[@]}"; do
    [ -f "$f" ] || { printf '  SKIP  %s (not found)\n' "$f"; continue; }

    # ── Lexical #if/#ifdef/#ifndef vs #endif balance ─────────────────────────
    # Runs BEFORE the g++ parse and does not depend on it succeeding. This is
    # the one check that still fires when a file is BLIND: g++'s own
    # "unterminated #ifdef" is already caught by the STRUCTURAL regex below,
    # but only if the parser reaches EOF — which needs every header along the
    # way to resolve. A missing project header (this tarball's own gap, more
    # than once) aborts the parse before EOF and swallows the finding along
    # with it. This check is pure text, so a missing header can't hide it.
    # Crude like the brace-balance check below: does not strip comments/string
    # literals, so it's a smell test, not a proof — but #if-family directives
    # inside a comment or string are rare enough that this earns its keep.
    # NOTE: deliberately avoids \b — mawk (present on at least one dev machine
    # for this project) does not support it, and a regex that silently matches
    # nothing is worse than no check: it prints a clean "ok" while checking
    # zero lines. ([[:space:]]|$) after the keyword does the same job and is
    # plain POSIX ERE.
    ifdepth=$(awk '
        /^[[:space:]]*#[[:space:]]*(ifdef|ifndef|if)([[:space:]]|$)/ { depth++; if (depth==1) startline=NR }
        /^[[:space:]]*#[[:space:]]*endif([[:space:]]|$)/ {
            depth--
            if (depth < 0) { print "unmatched #endif at line " NR; exit }
        }
        END {
            if (depth > 0) print "unterminated #if/#ifdef/#ifndef, still open at EOF (opened line " startline ")"
        }
    ' "$f")
    if [ -n "$ifdepth" ]; then
        printf '  FAIL  %s — preprocessor imbalance: %s\n' "$f" "$ifdepth"
        fail=1
        continue
    fi

    out=$(g++ -fsyntax-only -std=c++17 -I source -I "$STUBS" \
              -DPLATFORM_SWITCH -DGNX_NET_CLIENT -DAPP_VERSION='"guard"' -DAPP_SOURCE_URL='"guard"' \
              "$f" 2>&1)

    # A fatal error means the parse never happened — the guard is blind, which is
    # the exact failure mode this script was written to remove. Report it loudly
    # rather than printing a green tick over nothing.
    if grep -q 'fatal error' <<<"$out"; then
        printf '  BLIND %s — parse aborted, add a stub for:\n' "$f"
        grep 'fatal error' <<<"$out" | head -3 | sed 's/^/          /'
        fail=1
        continue
    fi

    hits=$(grep -E "$STRUCTURAL" <<<"$out" | head -20)

    # Wrong FIELD NAMES on OUR OWN types. g++ reports these as "has no member
    # named", which is normally filtered as noise — a libnx struct reduced to an
    # empty stub genuinely has no members, so the message is meaningless there.
    # But when the type belongs to one of OUR namespaces the stubs are irrelevant
    # and the error is real.
    #
    # Added after `e.application_id` on Services::VirtualEntry (which has no such
    # field) reached the devkitPro build twice-removed from the host suite, which
    # does not compile this file at all. This is the one class of "wrong field
    # name" the guard CAN see, so it should.
    own=$(grep 'has no member named' <<<"$out" \
          | grep -E "(Core|Services|Config|Install|UI|Widgets|Theme|Layout)::" \
          | head -10)
    if [ -n "$own" ]; then
        hits="${hits}${hits:+$'\n'}${own}"
    fi

    # Crude brace balance, ignoring the obvious noise (string/char literals are
    # not stripped, so this is a smell test, not a proof).
    ob=$(tr -cd '{' < "$f" | wc -c)
    cb=$(tr -cd '}' < "$f" | wc -c)

    if [ -n "$hits" ]; then
        printf '  FAIL  %s\n' "$f"
        sed 's/^/          /' <<<"$hits"
        fail=1
    elif [ "$ob" != "$cb" ]; then
        printf '  FAIL  %s — brace imbalance: %s { vs %s }\n' "$f" "$ob" "$cb"
        fail=1
    else
        printf '  ok    %s (parsed, braces %s/%s)\n' "$f" "$ob" "$cb"
    fi
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "STRUCTURAL BREAK — this would fail the devkitPro build, not the host tests."
    exit 1
fi
echo
echo "All files parsed. REMINDER: this proves structure only — wrong types and"
echo "wrong field names still pass. Audit every libnx call against its header."
