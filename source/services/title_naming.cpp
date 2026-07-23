// source/services/title_naming.cpp

#include "services/title_naming.hpp"

#include <algorithm>
#include <cstdio>

namespace Services {

namespace {

const char* type_tag(Core::Ncm::TitleType t) {
    switch (t) {
        case Core::Ncm::TitleType::Application:  return "BASE";
        case Core::Ncm::TitleType::Patch:        return "UPD";
        case Core::Ncm::TitleType::AddOnContent: return "DLC";
        default:                                 return "OTHER";
    }
}

Core::Ncm::TitleType tag_to_type(const std::string& tag) {
    if (tag == "BASE") return Core::Ncm::TitleType::Application;
    if (tag == "UPD")  return Core::Ncm::TitleType::Patch;
    if (tag == "DLC")  return Core::Ncm::TitleType::AddOnContent;
    return Core::Ncm::TitleType::Other;
}

// Longest display name we keep. The bracketed fields plus ".nsp" add ~40 chars, and
// FAT tops out at 255 — leaving headroom keeps us clear of clients that append
// their own suffixes (".part", " (1)") while downloading.
constexpr size_t kMaxDisplayName = 128;

// Extract the contents of the Nth "[...]" group, or "" if absent.
std::string bracket_field(const std::string& s, int index) {
    int seen = 0;
    size_t i = 0;
    while (i < s.size()) {
        const size_t open = s.find('[', i);
        if (open == std::string::npos) return "";
        const size_t close = s.find(']', open + 1);
        if (close == std::string::npos) return "";
        if (seen == index) return s.substr(open + 1, close - open - 1);
        ++seen;
        i = close + 1;
    }
    return "";
}

bool parse_hex64(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint64_t)(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}

} // namespace

std::string sanitize_for_filename(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        // Reject the FAT-illegal set plus control characters. Brackets are stripped
        // too: they delimit our own metadata fields, so a game whose title contains
        // one must not be able to fake a field and break the parse.
        const bool illegal =
            c < 0x20 || c == 0x7F ||
            c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'  ||
            c == '"' || c == '<'  || c == '>' || c == '|' ||
            c == '[' || c == ']';
        out.push_back(illegal ? '_' : (char)c);
    }
    // Trim trailing dots/spaces — FAT silently drops them, which would make the
    // name we advertise differ from the name the client ends up with.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.size() > kMaxDisplayName) out.resize(kMaxDisplayName);
    if (out.empty()) out = "Unknown";
    return out;
}

std::string title_to_filename(const Core::Ncm::Title& t) {
    // Fall back to the id when the control NCA hasn't been read (or has no name),
    // so a title is always downloadable even if its metadata is unreadable.
    std::string display = t.name.empty() ? std::string("Title") : t.name;
    display = sanitize_for_filename(display);

    char buf[64];
    std::snprintf(buf, sizeof(buf), " [%016llX][%s][v%u].nsp",
                  (unsigned long long)t.meta_id, type_tag(t.type), (unsigned)t.version);
    return display + buf;
}

ParsedTitleName parse_title_filename(const std::string& filename) {
    ParsedTitleName p;

    // Must end in .nsp (case-insensitive: some clients normalise case).
    if (filename.size() < 4) return p;
    std::string ext = filename.substr(filename.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (ext != ".nsp") return p;

    const std::string stem = filename.substr(0, filename.size() - 4);

    const std::string id_s   = bracket_field(stem, 0);
    const std::string type_s = bracket_field(stem, 1);
    const std::string ver_s  = bracket_field(stem, 2);
    if (id_s.empty() || type_s.empty() || ver_s.empty()) return p;

    uint64_t id = 0;
    if (!parse_hex64(id_s, id)) return p;

    // Version field is "vN".
    if (ver_s.size() < 2 || (ver_s[0] != 'v' && ver_s[0] != 'V')) return p;
    uint64_t ver = 0;
    for (size_t i = 1; i < ver_s.size(); ++i) {
        if (ver_s[i] < '0' || ver_s[i] > '9') return p;
        ver = ver * 10 + (uint64_t)(ver_s[i] - '0');
        if (ver > 0xFFFFFFFFull) return p;
    }

    p.ok      = true;
    p.meta_id = id;
    p.version = (uint32_t)ver;
    p.type    = tag_to_type(type_s);
    return p;
}

std::string title_to_save_dirname(const Core::Ncm::Title& t) {
    std::string display = t.name.empty() ? std::string("Title") : t.name;
    display = sanitize_for_filename(display);
    char buf[32];
    // Saves key off the APPLICATION id, not the meta id: a game's save belongs to
    // the application, not to a particular update or DLC meta.
    std::snprintf(buf, sizeof(buf), " [%016llX]",
                  (unsigned long long)Core::Ncm::base_application_id(t.program_id, t.type));
    return display + buf;
}

} // namespace Services
