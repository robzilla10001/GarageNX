// source/core/qr.cpp
//
// Clean-room QR encoder. See qr.hpp for the deliberate scope limits
// (byte mode, ECC M, versions 1-6).
//
// Pipeline, in order:
//   text -> bit stream (mode + length + payload + terminator + pad)
//        -> split into RS blocks, append error-correction codewords
//        -> interleave blocks
//        -> place function patterns, then zigzag the data bits in
//        -> try all 8 masks, keep the one with the lowest penalty
//        -> write format info for the chosen mask

#include "core/qr.hpp"

#include <algorithm>
#include <cstdlib>

namespace Core::Qr {
namespace {

// ─── Per-version tables (ECC level M only) ───────────────────────────────────
// For versions 1-6 at level M every block within a version is the same size,
// so there is no group-1/group-2 split to handle.
struct VersionSpec {
    int ec_per_block;    // error-correction codewords per block
    int blocks;          // number of RS blocks
    int data_per_block;  // data codewords per block
};

// Indexed by version (1-6); index 0 unused.
constexpr VersionSpec kSpec[7] = {
    {  0, 0,  0 },
    { 10, 1, 16 },   // v1-M: 26 total codewords
    { 16, 1, 28 },   // v2-M: 44
    { 26, 1, 44 },   // v3-M: 70
    { 18, 2, 32 },   // v4-M: 100
    { 24, 2, 43 },   // v5-M: 134
    { 16, 4, 27 },   // v6-M: 172
};

constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 6;

int version_size(int v) { return 17 + 4 * v; }
int data_codewords(int v) { return kSpec[v].blocks * kSpec[v].data_per_block; }

// Byte-mode payload capacity: 4 bits mode + 8 bits length (v1-9) = 12 bits of
// header before the data itself.
int byte_capacity(int v) { return (data_codewords(v) * 8 - 12) / 8; }

// ─── GF(256) arithmetic, primitive polynomial 0x11D ──────────────────────────
struct Gf {
    uint8_t exp[512];
    uint8_t log[256];
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = (uint8_t)x;
            log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;   // unused; guarded at the call sites
    }
    uint8_t mul(uint8_t a, uint8_t b) const {
        if (a == 0 || b == 0) return 0;
        return exp[log[a] + log[b]];
    }
};
const Gf& gf() { static const Gf g; return g; }

// Reed-Solomon generator polynomial of the given degree.
// Built lowest-degree-first, then reversed: rs_remainder walks it
// highest-degree-first with the leading (monic) coefficient at index 0.
std::vector<uint8_t> rs_generator(int degree) {
    std::vector<uint8_t> poly{1};
    for (int i = 0; i < degree; i++) {
        // multiply poly by (x - a^i)
        std::vector<uint8_t> next(poly.size() + 1, 0);
        for (size_t j = 0; j < poly.size(); j++) {
            next[j]     ^= gf().mul(poly[j], gf().exp[i]);
            next[j + 1] ^= poly[j];
        }
        poly = next;
    }
    std::reverse(poly.begin(), poly.end());
    return poly;
}

// Remainder of data * x^ec divided by the generator — the EC codewords.
std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, int ec) {
    const std::vector<uint8_t> gen = rs_generator(ec);
    std::vector<uint8_t> rem(ec, 0);
    for (uint8_t d : data) {
        const uint8_t factor = d ^ rem[0];
        rem.erase(rem.begin());
        rem.push_back(0);
        for (int i = 0; i < ec; i++) rem[i] ^= gf().mul(gen[i + 1], factor);
    }
    return rem;
}

// ─── Bit buffer ──────────────────────────────────────────────────────────────
struct BitBuf {
    std::vector<uint8_t> bits;
    void put(uint32_t value, int len) {
        for (int i = len - 1; i >= 0; i--) bits.push_back((value >> i) & 1);
    }
    size_t size() const { return bits.size(); }
};

// ─── Matrix helpers ──────────────────────────────────────────────────────────
struct Matrix {
    int size;
    std::vector<uint8_t> mod;   // 1 = dark
    std::vector<uint8_t> fn;    // 1 = function module (not data, never masked)

    explicit Matrix(int s) : size(s), mod((size_t)s * s, 0), fn((size_t)s * s, 0) {}

    void set_fn(int x, int y, bool dark) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        mod[(size_t)y * size + x] = dark ? 1 : 0;
        fn[(size_t)y * size + x]  = 1;
    }
    bool get(int x, int y) const { return mod[(size_t)y * size + x] != 0; }
    bool is_fn(int x, int y) const { return fn[(size_t)y * size + x] != 0; }
};

// 7x7 finder plus its one-module light separator.
void draw_finder(Matrix& m, int tlx, int tly) {
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 7; dx++) {
            const int dist = std::max(std::abs(dx - 3), std::abs(dy - 3));
            m.set_fn(tlx + dx, tly + dy, dist != 2 && dist <= 3);
        }
    }
}

// 5x5 alignment pattern centred on (cx, cy).
void draw_alignment(Matrix& m, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            m.set_fn(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

// Reserve (with placeholder values) the two format-information areas.
void reserve_format(Matrix& m) {
    for (int i = 0; i < 9; i++) {
        m.set_fn(8, i, false);
        m.set_fn(i, 8, false);
    }
    for (int i = 0; i < 8; i++) {
        m.set_fn(m.size - 1 - i, 8, false);
        m.set_fn(8, m.size - 1 - i, false);
    }
}

void draw_function_patterns(Matrix& m, int version) {
    const int size = m.size;

    draw_finder(m, 0, 0);
    draw_finder(m, size - 7, 0);
    draw_finder(m, 0, size - 7);

    // Timing patterns along row 6 and column 6.
    for (int i = 8; i < size - 8; i++) {
        const bool dark = (i % 2 == 0);
        m.set_fn(i, 6, dark);
        m.set_fn(6, i, dark);
    }

    // Alignment patterns. For v2-6 the centres are {6, 4v+10}; the three that
    // would collide with the finders are skipped.
    if (version >= 2) {
        const int centers[2] = {6, 4 * version + 10};
        for (int a = 0; a < 2; a++) {
            for (int b = 0; b < 2; b++) {
                const int cx = centers[a], cy = centers[b];
                const bool corner = (cx == 6 && cy == 6) ||
                                    (cx == 6 && cy == size - 7) ||
                                    (cx == size - 7 && cy == 6);
                if (!corner) draw_alignment(m, cx, cy);
            }
        }
    }

    // Always-dark module next to the lower-left finder.
    m.set_fn(8, size - 8, true);

    reserve_format(m);
}

// BCH(15,5) with generator 0x537, then the standard 0x5412 mask.
uint32_t format_bits(int mask) {
    const uint32_t data = (uint32_t)mask;   // ECC level M == 0b00, so bits are just the mask
    uint32_t rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return ((data << 10) | rem) ^ 0x5412;
}

void draw_format(Matrix& m, int mask) {
    const uint32_t bits = format_bits(mask);
    const int size = m.size;

    // Copy 1, wrapped around the top-left finder.
    for (int i = 0; i <= 5; i++)  m.set_fn(8, i, (bits >> i) & 1);
    m.set_fn(8, 7, (bits >> 6) & 1);
    m.set_fn(8, 8, (bits >> 7) & 1);
    m.set_fn(7, 8, (bits >> 8) & 1);
    for (int i = 9; i <= 14; i++) m.set_fn(14 - i, 8, (bits >> i) & 1);

    // Copy 2, split between the other two finders.
    for (int i = 0; i <= 7; i++)  m.set_fn(size - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i <= 14; i++) m.set_fn(8, size - 15 + i, (bits >> i) & 1);
    m.set_fn(8, size - 8, true);   // dark module stays dark
}

bool mask_bit(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (y / 2 + x / 3) % 2 == 0;
        case 5: return (x * y) % 2 + (x * y) % 3 == 0;
        case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
        default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    }
}

// Zigzag the codeword bits into every non-function module, bottom-right first.
void place_data(Matrix& m, const std::vector<uint8_t>& codewords) {
    const int size = m.size;
    size_t bit = 0;
    const size_t total = codewords.size() * 8;
    int dir = -1, y = size - 1;

    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;   // column 6 is the vertical timing pattern
        while (true) {
            for (int c = 0; c < 2; c++) {
                const int x = right - c;
                if (!m.is_fn(x, y)) {
                    bool dark = false;
                    if (bit < total)
                        dark = (codewords[bit >> 3] >> (7 - (bit & 7))) & 1;
                    m.mod[(size_t)y * size + x] = dark ? 1 : 0;
                    bit++;
                }
            }
            y += dir;
            if (y < 0 || y >= size) { y -= dir; dir = -dir; break; }
        }
    }
}

void apply_mask(Matrix& m, int mask) {
    for (int y = 0; y < m.size; y++)
        for (int x = 0; x < m.size; x++)
            if (!m.is_fn(x, y) && mask_bit(mask, x, y))
                m.mod[(size_t)y * m.size + x] ^= 1;
}

// ─── Penalty scoring (used only to pick the friendliest mask) ────────────────
int penalty(const Matrix& m) {
    const int size = m.size;
    int score = 0;

    // Rule 1: runs of 5+ same-coloured modules in a row or column.
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < size; a++) {
            int run = 1;
            bool prev = pass ? m.get(a, 0) : m.get(0, a);
            for (int b = 1; b < size; b++) {
                const bool cur = pass ? m.get(a, b) : m.get(b, a);
                if (cur == prev) {
                    if (++run == 5) score += 3;
                    else if (run > 5) score += 1;
                } else {
                    run = 1; prev = cur;
                }
            }
        }
    }

    // Rule 2: 2x2 blocks of one colour.
    for (int y = 0; y < size - 1; y++)
        for (int x = 0; x < size - 1; x++) {
            const bool c = m.get(x, y);
            if (c == m.get(x + 1, y) && c == m.get(x, y + 1) && c == m.get(x + 1, y + 1))
                score += 3;
        }

    // Rule 3: finder-like 1:1:3:1:1 sequences with four light modules beside them.
    static const int pat_a[11] = {1,0,1,1,1,0,1,0,0,0,0};
    static const int pat_b[11] = {0,0,0,0,1,0,1,1,1,0,1};
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < size; a++) {
            for (int b = 0; b + 11 <= size; b++) {
                bool ma = true, mb = true;
                for (int k = 0; k < 11; k++) {
                    const bool v = pass ? m.get(a, b + k) : m.get(b + k, a);
                    if (v != (bool)pat_a[k]) ma = false;
                    if (v != (bool)pat_b[k]) mb = false;
                }
                if (ma) score += 40;
                if (mb) score += 40;
            }
        }
    }

    // Rule 4: deviation from an even dark/light balance.
    int dark = 0;
    for (int i = 0; i < size * size; i++) dark += m.mod[i];
    const int percent = dark * 100 / (size * size);
    score += std::abs(percent - 50) / 5 * 10;

    return score;
}

} // namespace

// ─── Public entry point ──────────────────────────────────────────────────────
Code encode(const std::string& text) {
    Code out;

    // Pick the smallest version that fits.
    int version = 0;
    for (int v = kMinVersion; v <= kMaxVersion; v++) {
        if ((int)text.size() <= byte_capacity(v)) { version = v; break; }
    }
    if (version == 0) return out;   // too long for v6-M; caller falls back to text

    const VersionSpec& spec = kSpec[version];
    const int total_data = data_codewords(version);

    // Bit stream: mode (0100 = byte), 8-bit length, payload, terminator, pad.
    BitBuf bb;
    bb.put(0b0100, 4);
    bb.put((uint32_t)text.size(), 8);
    for (unsigned char ch : text) bb.put(ch, 8);

    const size_t capacity_bits = (size_t)total_data * 8;
    const size_t terminator = std::min<size_t>(4, capacity_bits - bb.size());
    bb.put(0, (int)terminator);
    while (bb.size() % 8 != 0) bb.put(0, 1);

    std::vector<uint8_t> data;
    data.reserve(total_data);
    for (size_t i = 0; i < bb.size(); i += 8) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) byte = (byte << 1) | bb.bits[i + b];
        data.push_back(byte);
    }
    // Alternating pad bytes until the data capacity is full.
    for (uint8_t pad = 0xEC; (int)data.size() < total_data; pad ^= 0xEC ^ 0x11)
        data.push_back(pad);

    // Split into blocks, compute EC per block.
    std::vector<std::vector<uint8_t>> dblocks, eblocks;
    for (int i = 0; i < spec.blocks; i++) {
        std::vector<uint8_t> d(data.begin() + (size_t)i * spec.data_per_block,
                               data.begin() + (size_t)(i + 1) * spec.data_per_block);
        eblocks.push_back(rs_remainder(d, spec.ec_per_block));
        dblocks.push_back(std::move(d));
    }

    // Interleave: data codewords column-wise across blocks, then EC likewise.
    std::vector<uint8_t> final_cw;
    final_cw.reserve((size_t)spec.blocks * (spec.data_per_block + spec.ec_per_block));
    for (int i = 0; i < spec.data_per_block; i++)
        for (int b = 0; b < spec.blocks; b++) final_cw.push_back(dblocks[b][i]);
    for (int i = 0; i < spec.ec_per_block; i++)
        for (int b = 0; b < spec.blocks; b++) final_cw.push_back(eblocks[b][i]);

    // Build the matrix once, then try each mask on a copy.
    Matrix base(version_size(version));
    draw_function_patterns(base, version);
    place_data(base, final_cw);

    int best_mask = 0, best_score = -1;
    Matrix best = base;
    for (int mask = 0; mask < 8; mask++) {
        Matrix m = base;
        apply_mask(m, mask);
        draw_format(m, mask);
        const int s = penalty(m);
        if (best_score < 0 || s < best_score) { best_score = s; best_mask = mask; best = m; }
    }
    (void)best_mask;

    out.size    = best.size;
    out.modules = best.mod;
    return out;
}

} // namespace Core::Qr
