#pragma once
// source/ui/stats_format.hpp
//
// Shared formatting for the transport status screens (MTP / FTP / HTTP).
//
// format_eta() existed twice — once in mtp_screen.cpp and once in
// ftp_screen.cpp — as byte-identical file-local copies. Adding HTTP would have
// made three. Two identical copies are not a bug yet; they are a bug waiting for
// the first person to fix one of them, and this codebase has already paid for
// that lesson more than once (the "<Name> [APPID]" save label, the mount probe
// that FTP had and MTP did not).
//
// Pure — no SDL, no libnx — so it is host-tested rather than eyeballed on a
// console, which for a function whose whole job is boundary behaviour (negative,
// absurd, zero, rollover at 60s and 3600s) is the difference between knowing and
// hoping.

#include <cstdio>
#include <string>

namespace UI {

/// Human ETA from a seconds estimate: "45s", "3m 07s", "2h 05m".
///
/// Returns an em dash for anything not worth showing: a negative estimate
/// (arithmetic on a stalled transfer) or one beyond ~100 hours, which in practice
/// means the rate is so near zero that the number is noise rather than
/// information. Showing "2847h" would be technically true and useless.
inline std::string format_eta(double seconds) {
    if (!(seconds >= 0.0) || seconds > 359999.0) return "\u2014";   // NaN-safe
    const int total = (int)(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    char buf[32];
    if (h > 0)      std::snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    else            std::snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

} // namespace UI
