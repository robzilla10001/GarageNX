// source/services/mtp_data.cpp

#include "services/mtp_data.hpp"
#include <cstring>

namespace Services::Mtp {
namespace {

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace

// ─── Text conversion ─────────────────────────────────────────────────────────
// MTP strings are UTF-16LE. Filenames on the SD card are UTF-8 and routinely
// non-ASCII, so this is a real conversion, not a byte-widening cast. Invalid
// input is replaced with U+FFFD rather than rejected — a badly-named file must
// not take down the listing.
std::vector<uint16_t> utf8_to_utf16(const std::string& s) {
    std::vector<uint16_t> out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int extra;
        if (c < 0x80)        { cp = c;          extra = 0; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E){ cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); i++; continue; }

        if (i + extra >= n) { out.push_back(0xFFFD); break; }
        bool bad = false;
        for (int k = 1; k <= extra; k++) {
            const unsigned char cc = (unsigned char)s[i + k];
            if ((cc >> 6) != 0x2) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += extra + 1;
        if (bad || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out.push_back(0xFFFD); continue; }

        if (cp >= 0x10000) {   // surrogate pair
            cp -= 0x10000;
            out.push_back((uint16_t)(0xD800 + (cp >> 10)));
            out.push_back((uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((uint16_t)cp);
        }
    }
    return out;
}

std::string utf16_to_utf8(const std::vector<uint16_t>& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;   // lone surrogate
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// ─── Header / command parsing ────────────────────────────────────────────────
bool parse_header(const uint8_t* buf, size_t len, Header& out) {
    if (!buf || len < kHeaderSize) return false;
    out.length         = rd32(buf);
    out.type           = rd16(buf + 4);
    out.code           = rd16(buf + 6);
    out.transaction_id = rd32(buf + 8);
    // A container that claims to be shorter than its own header is malformed.
    if (out.length < kHeaderSize) return false;
    return true;
}

bool parse_command(const uint8_t* buf, size_t len, Command& out) {
    if (!parse_header(buf, len, out.hdr)) return false;
    if (out.hdr.type != Type::Command) return false;

    // Trust the smaller of "what the host claims" and "what actually arrived",
    // so a truncated packet cannot walk us off the end of the buffer.
    const size_t avail = (out.hdr.length < len ? out.hdr.length : len);
    const size_t pbytes = (avail > kHeaderSize) ? (avail - kHeaderSize) : 0;
    int n = (int)(pbytes / 4);
    if (n > 5) n = 5;
    for (int i = 0; i < n; i++) out.param[i] = rd32(buf + kHeaderSize + i * 4);
    out.nparams = n;
    return true;
}

// ─── Writer ──────────────────────────────────────────────────────────────────
void Writer::u8(uint8_t v)  { m_buf.push_back(v); }
void Writer::u16(uint16_t v) {
    m_buf.push_back((uint8_t)(v & 0xFF));
    m_buf.push_back((uint8_t)(v >> 8));
}
void Writer::u32(uint32_t v) {
    for (int i = 0; i < 4; i++) m_buf.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void Writer::u64(uint64_t v) {
    for (int i = 0; i < 8; i++) m_buf.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
void Writer::raw(const uint8_t* p, size_t n) { m_buf.insert(m_buf.end(), p, p + n); }

void Writer::str(const std::string& utf8) {
    if (utf8.empty()) { m_buf.push_back(0); return; }   // empty == single 0 byte
    std::vector<uint16_t> u = utf8_to_utf16(utf8);
    // The count includes the terminating NUL and is a single byte, so 255 chars
    // is the hard ceiling; truncate rather than overflow the length field.
    if (u.size() > 254) u.resize(254);
    m_buf.push_back((uint8_t)(u.size() + 1));
    for (uint16_t c : u) u16(c);
    u16(0);
}

void Writer::au16(const std::vector<uint16_t>& v) {
    u32((uint32_t)v.size());
    for (uint16_t x : v) u16(x);
}
void Writer::au32(const std::vector<uint32_t>& v) {
    u32((uint32_t)v.size());
    for (uint32_t x : v) u32(x);
}

// ─── Reader ──────────────────────────────────────────────────────────────────
bool Reader::u8(uint8_t& v) {
    if (m_i + 1 > m_n) { m_bad = true; return false; }
    v = m_p[m_i++]; return true;
}
bool Reader::u16(uint16_t& v) {
    if (m_i + 2 > m_n) { m_bad = true; return false; }
    v = rd16(m_p + m_i); m_i += 2; return true;
}
bool Reader::u32(uint32_t& v) {
    if (m_i + 4 > m_n) { m_bad = true; return false; }
    v = rd32(m_p + m_i); m_i += 4; return true;
}
bool Reader::u64(uint64_t& v) {
    if (m_i + 8 > m_n) { m_bad = true; return false; }
    v = (uint64_t)rd32(m_p + m_i) | ((uint64_t)rd32(m_p + m_i + 4) << 32);
    m_i += 8; return true;
}
bool Reader::str(std::string& out) {
    uint8_t count;
    if (!u8(count)) return false;
    if (count == 0) { out.clear(); return true; }
    if (m_i + (size_t)count * 2 > m_n) { m_bad = true; return false; }
    std::vector<uint16_t> u;
    u.reserve(count);
    for (int k = 0; k < count; k++) { u.push_back(rd16(m_p + m_i)); m_i += 2; }
    if (!u.empty() && u.back() == 0) u.pop_back();   // drop the terminator
    out = utf16_to_utf8(u);
    return true;
}

// ─── ObjectPropList dataset ──────────────────────────────────────────────────

bool parse_object_prop_list(const uint8_t* p, size_t n,
                            std::vector<ObjectProp>& out, uint32_t* out_bad_index) {
    out.clear();
    if (out_bad_index) *out_bad_index = 0;
    Reader r(p, n);

    uint32_t count = 0;
    if (!r.u32(count)) return false;
    // A host sending an object declares a handful of properties. This bound is
    // not politeness: count is host-supplied and reserve()ing it directly would
    // let a corrupt dataset ask for 4 billion elements.
    if (count > 256) return false;
    out.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        ObjectProp e;
        if (!r.u32(e.handle) || !r.u16(e.code) || !r.u16(e.type)) return false;

        switch (e.type) {
            case DataType::Int8:
            case DataType::UInt8:   { uint8_t  v; if (!r.u8(v))  return false; e.num = v; break; }
            case DataType::Int16:
            case DataType::UInt16:  { uint16_t v; if (!r.u16(v)) return false; e.num = v; break; }
            case DataType::Int32:
            case DataType::UInt32:  { uint32_t v; if (!r.u32(v)) return false; e.num = v; break; }
            case DataType::Int64:
            case DataType::UInt64:  { uint64_t v; if (!r.u64(v)) return false; e.num = v; break; }
            case DataType::Int128:
            case DataType::UInt128: {
                // Consumed but not represented: nothing we read is 128-bit, and
                // silently truncating to 64 would be worse than ignoring it.
                uint64_t lo, hi;
                if (!r.u64(lo) || !r.u64(hi)) return false;
                break;
            }
            case DataType::Str:     { if (!r.str(e.text)) return false; break; }
            default:
                // Unknown datatype: its value length is unknowable, so every
                // element after this one is unparseable too. Report which one.
                if (out_bad_index) *out_bad_index = i;
                return false;
        }
        out.push_back(std::move(e));
    }
    return r.ok();
}

bool build_object_prop_desc(uint16_t code, Writer& w) {
    uint16_t type;
    uint8_t  get_set;   // 1 = the host may set this on an object it is sending

    switch (code) {
        case ObjProp::StorageId:        type = DataType::UInt32; get_set = 1; break;
        case ObjProp::ObjectFormat:     type = DataType::UInt16; get_set = 1; break;
        case ObjProp::ProtectionStatus: type = DataType::UInt16; get_set = 1; break;
        // Read-only: the device computes an object's size, it is not something a
        // host assigns. The size of an INCOMING object travels in
        // SendObjectPropList's parameters instead, which is the whole reason
        // that operation exists.
        case ObjProp::ObjectSize:       type = DataType::UInt64; get_set = 0; break;
        case ObjProp::ObjectFileName:   type = DataType::Str;    get_set = 1; break;
        case ObjProp::ParentObject:     type = DataType::UInt32; get_set = 1; break;
        default:
            return false;
    }

    w.u16(code);
    w.u16(type);
    w.u8(get_set);

    // Default value, encoded in `type`. A host reads this by datatype, exactly
    // as it reads an ObjectPropList element — get the width wrong here and the
    // host desynchronises on the rest of the dataset.
    switch (type) {
        case DataType::UInt16: w.u16(0); break;
        case DataType::UInt32: w.u32(0); break;
        case DataType::UInt64: w.u64(0); break;
        case DataType::Str:    w.str(""); break;   // one 0 byte: zero characters
        default:               w.u32(0); break;
    }

    w.u32(0);   // group code: ungrouped
    w.u8(0);    // form flag: none — and so no form field follows
    return true;
}

const ObjectProp* find_prop(const std::vector<ObjectProp>& props, uint16_t code) {
    for (const auto& p : props)
        if (p.code == code) return &p;
    return nullptr;
}

// ─── Container builders ──────────────────────────────────────────────────────
std::vector<uint8_t> make_response(uint16_t code, uint32_t tid,
                                   const uint32_t* params, int nparams) {
    if (nparams < 0) nparams = 0;
    if (nparams > 5) nparams = 5;
    Writer w;
    w.u32((uint32_t)(kHeaderSize + (size_t)nparams * 4));
    w.u16(Type::Response);
    w.u16(code);
    w.u32(tid);
    for (int i = 0; i < nparams; i++) w.u32(params[i]);
    return w.data();
}

std::vector<uint8_t> make_data(uint16_t code, uint32_t tid,
                               const std::vector<uint8_t>& payload) {
    Writer w;
    w.u32((uint32_t)(kHeaderSize + payload.size()));
    w.u16(Type::Data);
    w.u16(code);
    w.u32(tid);
    if (!payload.empty()) w.raw(payload.data(), payload.size());
    return w.data();
}

} // namespace Services::Mtp
