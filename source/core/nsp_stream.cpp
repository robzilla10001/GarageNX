// source/core/nsp_stream.cpp
//
// The NSP construction logic, lifted from the hardware-verified
// Core::Dump::dump_title_to_nsp so that the SD dump and every transport share one
// implementation. Keeping two would guarantee they drift, and a divergence here
// produces NSPs that install from one path and not the other.

#include "core/nsp_stream.hpp"
#include "services/pfs0_layout.hpp"

#ifdef PLATFORM_SWITCH
#include "core/es.hpp"
#include <switch.h>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#endif

namespace Core {
namespace NspStream {

#ifdef PLATFORM_SWITCH

namespace {

constexpr size_t NCA_HEADER_SIZE = 0xC00;

std::string content_id_hex(const NcmContentId& id) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 0x10; ++i) {
        s.push_back(k[(id.c[i] >> 4) & 0xF]);
        s.push_back(k[id.c[i] & 0xF]);
    }
    return s;
}

// True (and fills rights_id) if this NCA uses titlekey crypto. Requires decrypting
// the NCA header, so it is done once per content at open() time — never per read.
// Parse a rights id out of an ALREADY-READ encrypted NCA header. Split from the
// read so both storage paths share it: gamecard content cannot be read through
// NCM (2002-2964), so its header arrives from the mounted secure partition
// instead, but the decryption and the offset of the rights id are identical.
bool nca_rights_id_from_header(const uint8_t* enc, const Keys::Keyset& keys,
                               uint8_t rights_id[0x10]) {
    if (!keys.has_header_key) return false;
    uint8_t dec[NCA_HEADER_SIZE];
    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, keys.header_key.data(), keys.header_key.data() + 0x10, false);
    for (size_t s = 0; s < NCA_HEADER_SIZE / 0x200; ++s) {
        aes128XtsContextResetSector(&xts, s, true);
        aes128XtsDecrypt(&xts, dec + s * 0x200, enc + s * 0x200, 0x200);
    }
    bool nonzero = false;
    for (int i = 0; i < 0x10; ++i) if (dec[0x230 + i] != 0) { nonzero = true; break; }
    if (nonzero) std::memcpy(rights_id, dec + 0x230, 0x10);
    return nonzero;
}

struct Content {
    NcmContentId         id{};
    uint64_t             size = 0;
    std::vector<uint8_t> inline_data;   // tik/cert live here; `id` unused
    bool                 is_inline = false;
};

class SourceImpl : public Source {
public:
    ~SourceImpl() override {
        if (m_gc_file) ::fclose(m_gc_file);
        if (m_cs_open) ncmContentStorageClose(&m_cs);
        if (m_db_open) ncmContentMetaDatabaseClose(&m_db);
    }

    // Read `n` bytes of one content at `off`.
    //
    // GAME CARD CONTENT CANNOT BE READ THROUGH NCM. ncmContentStorageReadContentIdFile
    // fails on NcmStorageId_GameCard with 2002-2964 (FS, unsupported operation) —
    // and it fails while ncmContentStorageGetSizeFromContentId on the SAME storage
    // and the SAME content id SUCCEEDS. That asymmetry is the tell: the storage
    // handle is fine and the content id is real; that one accessor is simply not
    // implemented for cartridges.
    //
    // The content is readable as an ordinary FILE on the card's mounted secure
    // partition, which GarageNX already mounts as "gamecard:". So for gamecard
    // titles we open <content_id>.nca there and read it directly. Every other
    // storage keeps the NCM path unchanged.
    //
    // The handle is cached because a dump reads one content in many chunks;
    // reopening per chunk would be pointless syscalls.
    bool read_content(void* out, size_t n, const NcmContentId& id, uint64_t off) {
        if (m_storage_id != NcmStorageId_GameCard)
            return R_SUCCEEDED(ncmContentStorageReadContentIdFile(
                       &m_cs, out, n, &id, (s64)off));

        char idhex[33];
        for (int i = 0; i < 16; ++i)
            std::snprintf(idhex + i * 2, 3, "%02x", id.c[i]);

        if (m_gc_file_id != std::string(idhex)) {
            if (m_gc_file) { ::fclose(m_gc_file); m_gc_file = nullptr; }
            // Meta contents carry a .cnmt.nca suffix; everything else is .nca.
            // Try the plain name first because it is the common case.
            const std::string base = std::string("gamecard:/") + idhex;
            m_gc_file = ::fopen((base + ".nca").c_str(), "rb");
            if (!m_gc_file) m_gc_file = ::fopen((base + ".cnmt.nca").c_str(), "rb");
            if (!m_gc_file) {
                if (!m_read_fail_logged) {
                    m_read_fail_logged = true;
                    SDL_Log("nsp_stream: gamecard NCA not found on secure partition: %s",
                            idhex);
                }
                return false;
            }
            m_gc_file_id = idhex;
        }
        if (::fseek(m_gc_file, (long)off, SEEK_SET) != 0) return false;
        return ::fread(out, 1, n, m_gc_file) == n;
    }

    bool rights_id_for(const NcmContentId& id, const Keys::Keyset& keys,
                       uint8_t rights_id[0x10]) {
        if (!keys.has_header_key) return false;
        uint8_t enc[NCA_HEADER_SIZE];
        if (!read_content(enc, NCA_HEADER_SIZE, id, 0)) return false;
        return nca_rights_id_from_header(enc, keys, rights_id);
    }

    bool init(const Ncm::Title& title, const Keys::Keyset& keys, std::string* error) {
        // Mirrors dump.cpp exactly: a title lives in either SD or built-in storage.
        const NcmStorageId storage_id = Ncm::to_ncm_storage_id(title.storage);
        m_storage_id = storage_id;   // kept for diagnostics on a failed read

        if (R_FAILED(ncmOpenContentMetaDatabase(&m_db, storage_id))) {
            if (error) *error = "cannot open content meta database";
            return false;
        }
        m_db_open = true;
        if (R_FAILED(ncmOpenContentStorage(&m_cs, storage_id))) {
            if (error) *error = "cannot open content storage";
            return false;
        }
        m_cs_open = true;

        // One line per stream open, so a failed dump has context even if the
        // read never happens. Cheap: opens are per-dump, not per-chunk.
        {
            FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/nsp_stream.log", "a");
            if (f) {
                std::fprintf(f, "open storage=%d meta_id=%016llX ver=%u type=%d\n",
                             (int)storage_id, (unsigned long long)title.meta_id,
                             (unsigned)title.version, (int)title.type);
                ::fclose(f);
            }
        }

        NcmContentMetaKey key;
        std::memset(&key, 0, sizeof(key));
        key.id      = title.meta_id;
        key.version = title.version;
        key.type    = (title.type == Ncm::TitleType::Application)  ? NcmContentMetaType_Application
                    : (title.type == Ncm::TitleType::Patch)        ? NcmContentMetaType_Patch
                    : (title.type == Ncm::TitleType::AddOnContent) ? NcmContentMetaType_AddOnContent
                                                                   : (NcmContentMetaType)0;
        key.install_type = NcmContentInstallType_Full;

        // Enumerate this title's NCAs.
        std::vector<Services::Pfs0File> files;
        uint8_t rights_id[0x10] = {0};
        bool    have_rights     = false;

        s32 offset = 0;
        for (;;) {
            NcmContentInfo infos[16];
            s32 written = 0;
            Result rc = ncmContentMetaDatabaseListContentInfo(
                &m_db, &written, infos, (s32)(sizeof(infos) / sizeof(infos[0])),
                &key, offset);
            if (R_FAILED(rc) || written <= 0) break;

            for (s32 i = 0; i < written; ++i) {
                const NcmContentInfo& ci = infos[i];
                s64 sz = 0;
                // Log the FIRST size failure too. If sizes fail, the read was
                // never going to work and the cause is the storage/handle, not
                // the read call — that distinction is the whole point of logging
                // both.
                if (R_FAILED(ncmContentStorageGetSizeFromContentId(&m_cs, &sz, &ci.content_id)))
                    continue;

                Content c;
                c.id   = ci.content_id;
                c.size = (uint64_t)sz;
                m_contents.push_back(c);

                Services::Pfs0File f;
                f.name = content_id_hex(ci.content_id) +
                         (ci.content_type == NcmContentType_Meta ? ".cnmt.nca" : ".nca");
                f.size = c.size;
                m_names.push_back(f.name);
                files.push_back(std::move(f));

                if (!have_rights)
                    have_rights = rights_id_for(ci.content_id, keys, rights_id);
            }
            offset += written;
        }

        if (m_contents.empty()) {
            if (error) *error = "title has no content";
            return false;
        }

        // Titlekey titles need their ticket + certificate inside the NSP or the
        // result will not install. Same rule as the SD dump.
        if (have_rights) {
            char rid_hex[33];
            for (int i = 0; i < 0x10; ++i)
                std::snprintf(rid_hex + i * 2, 3, "%02x", rights_id[i]);
            rid_hex[32] = '\0';

            std::vector<uint8_t> tik, cert;
            if (Es::get_ticket_and_cert(rights_id, tik, cert) && !tik.empty()) {
                Content c; c.is_inline = true; c.size = tik.size();
                c.inline_data = std::move(tik);
                m_contents.push_back(std::move(c));
                Services::Pfs0File f;
                f.name = std::string(rid_hex) + ".tik";
                f.size = m_contents.back().size;
                m_names.push_back(f.name);
                files.push_back(std::move(f));

                if (!cert.empty()) {
                    Content cc; cc.is_inline = true; cc.size = cert.size();
                    cc.inline_data = std::move(cert);
                    m_contents.push_back(std::move(cc));
                    Services::Pfs0File cf;
                    cf.name = std::string(rid_hex) + ".cert";
                    cf.size = m_contents.back().size;
                    m_names.push_back(cf.name);
                    files.push_back(std::move(cf));
                }
                m_note = "ticket + cert included";
            } else {
                m_note = "TITLEKEY TITLE WITH NO TICKET — may not install";
                SDL_Log("NspStream: titlekey title but no ticket found");
            }
        }

        // One shared layout module, so the advertised size and the streamed bytes
        // can never disagree.
        m_layout = Services::pfs0_build(files);
        return true;
    }

    uint64_t total_size() const override { return m_layout.total_size; }
    uint64_t position()   const override { return m_pos; }
    std::string note() const override { return m_note; }
    size_t   file_count() const override { return m_contents.size(); }
    size_t   current_index() const override {
        return std::min(m_cursor, m_contents.size());
    }
    std::string current_name() const override {
        return m_cursor < m_names.size() ? m_names[m_cursor] : std::string();
    }

    int64_t read(void* buf, size_t len) override {
        if (m_pos >= m_layout.total_size) return 0;
        uint8_t* out = (uint8_t*)buf;

        // Header region.
        if (m_pos < m_layout.header_size) {
            const size_t n = (size_t)std::min<uint64_t>(
                len, m_layout.header_size - m_pos);
            std::memcpy(out, m_layout.header.data() + m_pos, n);
            m_pos += n;
            return (int64_t)n;
        }

        // Data region: find which content covers m_pos. Linear from a remembered
        // cursor, since reads are sequential — no rescanning from the start.
        while (m_cursor < m_contents.size()) {
            const uint64_t start = m_layout.data_offsets[m_cursor];
            const uint64_t end   = start + m_contents[m_cursor].size;
            if (m_pos < end) {
                const uint64_t rel = m_pos - start;
                const size_t   n   = (size_t)std::min<uint64_t>(len, end - m_pos);
                const Content& c   = m_contents[m_cursor];

                if (c.is_inline) {
                    std::memcpy(out, c.inline_data.data() + rel, n);
                } else {
                    const ::Result rc = read_content(out, n, c.id, rel) ? 0 : 1;
                    if (R_FAILED(rc)) {
                        // The caller only ever saw "NCA read failed" with no
                        // Result, which is unusable for diagnosis — a gamecard
                        // dump failing on its FIRST read could be the storage,
                        // the content id, the offset, or the length, and the rc
                        // distinguishes them immediately. Logged ONCE per stream
                        // so a long read that starts failing does not fill the SD
                        // card with the same line.
                        if (!m_read_fail_logged) {
                            m_read_fail_logged = true;
                            char idhex[33];
                            for (int i = 0; i < 16; ++i)
                                std::snprintf(idhex + i * 2, 3, "%02X", c.id.c[i]);
                            FILE* f = ::fopen(
                                "sdmc:/switch/GarageNX/logs/nsp_stream.log", "a");
                            if (f) {
                                std::fprintf(f,
                                    "ReadContentIdFile rc=0x%08X storage=%d "
                                    "content=%s off=%llu len=%zu size=%llu\n",
                                    rc, (int)m_storage_id, idhex,
                                    (unsigned long long)rel, n,
                                    (unsigned long long)c.size);
                                ::fclose(f);
                            }
                            SDL_Log("nsp_stream: ReadContentIdFile rc=0x%08X "
                                    "storage=%d off=%llu len=%zu",
                                    rc, (int)m_storage_id,
                                    (unsigned long long)rel, n);
                        }
                        return -1;
                    }
                }
                m_pos += n;
                return (int64_t)n;
            }
            ++m_cursor;
        }
        return 0;   // past the last content
    }

private:
    NcmContentMetaDatabase m_db{};
    NcmContentStorage      m_cs{};
    bool                   m_db_open = false;
    bool                   m_cs_open = false;
    NcmStorageId           m_storage_id{};        // also selects the read path
    FILE*                  m_gc_file = nullptr;   // cached gamecard NCA handle
    std::string            m_gc_file_id;          // content id m_gc_file is open on
    bool                   m_read_fail_logged = false;

    std::vector<Content>     m_contents;
    std::vector<std::string> m_names;     // parallel to m_contents, for progress
    std::string              m_note = "n/a";
    Services::Pfs0Layout   m_layout;
    uint64_t               m_pos    = 0;
    size_t                 m_cursor = 0;
};

} // namespace

std::unique_ptr<Source> open(const Ncm::Title& title, const Keys::Keyset& keys,
                             std::string* error) {
    auto s = std::make_unique<SourceImpl>();
    if (!s->init(title, keys, error)) return nullptr;
    return s;
}

#else   // host build

std::unique_ptr<Source> open(const Ncm::Title&, const Keys::Keyset&,
                             std::string* error) {
    if (error) *error = "not supported on host";
    return nullptr;
}

#endif

} // namespace NspStream
} // namespace Core
