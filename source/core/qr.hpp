#pragma once
// source/core/qr.hpp
// Clean-room QR Code encoder (ISO/IEC 18004), deliberately scoped to what
// GarageNX actually needs: a short ASCII service URL rendered on a status
// screen and scanned by a phone.
//
// Scope and why:
//   * Byte mode only          — URLs are mixed-case ASCII; alphanumeric mode
//                               would only help all-uppercase payloads.
//   * Error correction M      — ~15% recovery, a good trade for a QR being
//                               photographed off a TV/handheld screen.
//   * Versions 1-6 (21..41)   — v6-M holds 106 bytes; our URLs run ~28
//                               ("http://192.168.100.100:8080/"). Stopping at
//                               v6 also avoids the version-information block
//                               entirely, which only exists from v7 up.
//
// encode() returns an invalid Code (size == 0) if the payload does not fit in
// v6-M; callers fall back to showing the URL as text.

#include <cstdint>
#include <string>
#include <vector>

namespace Core::Qr {

struct Code {
    int                  size = 0;   // modules per side; 0 means "not encodable"
    std::vector<uint8_t> modules;    // size*size, row-major, 1 = dark

    bool ok() const { return size > 0; }
    bool at(int x, int y) const { return modules[(size_t)y * size + x] != 0; }
};

// Encode text as a QR Code (byte mode, ECC level M). Picks the smallest
// version from 1..6 that fits.
Code encode(const std::string& text);

} // namespace Core::Qr
