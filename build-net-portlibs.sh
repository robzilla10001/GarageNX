#!/usr/bin/env bash
#
# build-net-portlibs.sh - cross-compile libnfs + libsmb2 for the Nintendo Switch
# and install them into the devkitPro portlibs prefix, so GarageNX can be built
# with the network client (GARAGENX_NET_CLIENT=ON, the default).
#
# WHERE TO RUN IT: anywhere. It uses absolute paths and its own work directory —
# it does NOT care what folder you launch it from. Run it as root in your
# container (no sudo needed). Override the work dir with:  WORK=/path ./build-net-portlibs.sh
#
# It is safe to re-run: the work directory is wiped and rebuilt from clean sources
# each time, so patches never double-apply.
#
# Verified pieces: the libnfs tarball in nxmp-portlibs is bzip2 despite its ".gz"
# name (extracted with `tar xf`), and both patches apply cleanly to their pinned
# sources (libnfs @20b39fd, libsmb2 4.0.0) — the versions GarageNX's devoptab was
# written against.

set -euo pipefail

WORK="${WORK:-$HOME/gnx-netbuild}"
HOST_TRIPLE="aarch64-none-elf"
JOBS="$(nproc 2>/dev/null || echo 4)"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# ── Preflight ─────────────────────────────────────────────────────────────────
say "Preflight checks"

[ -n "${DEVKITPRO:-}" ] || die "\$DEVKITPRO is not set. Install devkitPro / source its profile first."
[ -d "$DEVKITPRO" ]     || die "\$DEVKITPRO ($DEVKITPRO) is not a directory."
[ -f "$DEVKITPRO/switchvars.sh" ] || die "$DEVKITPRO/switchvars.sh missing — is the Switch dev environment installed?"

MISSING=""
# These are the binaries ./bootstrap actually invokes. Note: the 'libtool' apt
# package provides 'libtoolize' (there may be no 'libtool' command), and
# 'autoconf' provides 'autoreconf' — so check the tools, not the package names.
for t in git patch make autoreconf autoconf automake aclocal libtoolize curl tar; do
    have "$t" || MISSING="$MISSING $t"
done
[ -z "$MISSING" ] || die "Missing host tools:$MISSING
  Install them (Debian/Ubuntu):  apt install -y git patch make autoconf automake libtool curl
  (the 'libtool' package provides libtoolize; 'autoconf' provides autoreconf)"

# devkitA64 cross compiler must be on PATH (it is, if you can already build GarageNX).
have "${HOST_TRIPLE}-gcc" || die "${HOST_TRIPLE}-gcc not on PATH. Add \$DEVKITPRO/devkitA64/bin (source \$DEVKITPRO/switchvars.sh or your dkp profile)."

# Pull in the cross-compile environment (PORTLIBS_PREFIX, CC, CFLAGS, …).
# switchvars.sh may reference unset vars, so relax -u just for the source.
set +u
# shellcheck disable=SC1091
source "$DEVKITPRO/switchvars.sh"
set -u
PREFIX="${PORTLIBS_PREFIX:-$DEVKITPRO/portlibs/switch}"
say "Install prefix: $PREFIX"
mkdir -p "$PREFIX" || die "cannot create $PREFIX (permissions?)"
[ -w "$PREFIX" ] || die "$PREFIX is not writable by this user. Run as root, or fix ownership."

# ── Fresh work dir + sources ──────────────────────────────────────────────────
say "Preparing work directory: $WORK"
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

say "Cloning nxmp-portlibs (for the Switch patches + libnfs source)"
git clone --depth 1 https://github.com/proconsule/nxmp-portlibs
PATCHES="$WORK/nxmp-portlibs/switch"

# ── libnfs ────────────────────────────────────────────────────────────────────
say "Building libnfs"
cd "$WORK"
# `tar xf` auto-detects compression — this tarball is bzip2 despite its .gz name.
tar xf "$PATCHES"/libnfs/libnfs-*.tar.gz
cd libnfs
patch -Np1 -i "$PATCHES/libnfs/libnfs.patch"
./bootstrap
./configure --prefix="$PREFIX" --host="$HOST_TRIPLE" \
    --disable-shared --enable-static --disable-werror --disable-utils --disable-examples
make -j"$JOBS"
make install
say "libnfs installed"

# ── libsmb2 ───────────────────────────────────────────────────────────────────
say "Building libsmb2 (4.0.0)"
cd "$WORK"
curl -fsSL https://github.com/sahlberg/libsmb2/archive/refs/tags/v4.0.0.tar.gz -o libsmb2.tgz
tar xf libsmb2.tgz
cd libsmb2-4.0.0
patch -Np1 -i "$PATCHES/libsmb2/switch.patch"
./bootstrap
# libsmb2's bundled writev (compiled because newlib has no sys/uio.h) calls
# mempcpy, a GNU extension. GCC 14 rejects the implicit declaration as an error;
# _GNU_SOURCE makes newlib declare mempcpy (it's already implemented there).
CFLAGS="${CFLAGS:-} -D_GNU_SOURCE" \
./configure --prefix="$PREFIX" --host="$HOST_TRIPLE" \
    --disable-shared --enable-static --without-libkrb5 --disable-werror
make -j"$JOBS"
make install
say "libsmb2 installed"

# ── Verify ────────────────────────────────────────────────────────────────────
say "Verifying installed files"
FAIL=0
for f in \
    "$PREFIX/include/nfsc/libnfs.h" \
    "$PREFIX/include/smb2/smb2.h" \
    "$PREFIX/include/smb2/libsmb2.h" \
    "$PREFIX/lib/libnfs.a" \
    "$PREFIX/lib/libsmb2.a"; do
    if [ -e "$f" ]; then printf '  ok  %s\n' "$f"; else printf '  MISSING  %s\n' "$f"; FAIL=1; fi
done
[ "$FAIL" -eq 0 ] || die "some outputs are missing — scroll up for the failing build step."

cat <<EOF

$(say "Done.")
Both libraries are installed under: $PREFIX

Now build GarageNX (the network client is on by default):

    cd /workspace/GarageNX/build
    cmake -- -DPLATFORM=Switch && make -j$JOBS

EOF
