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
bool nca_rights_id(NcmContentStorage* cs, const NcmContentId* id,
                   const Keys::Keyset& keys, uint8_t rights_id[0x10]) {
    if (!keys.has_header_key) return false;
    uint8_t enc[NCA_HEADER_SIZE];
    if (R_FAILED(ncmContentStorageReadContentIdFile(cs, enc, NCA_HEADER_SIZE, id, 0)))
        return false;
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
        if (m_cs_open) ncmContentStorageClose(&m_cs);
        if (m_db_open) ncmContentMetaDatabaseClose(&m_db);
    }

    bool init(const Ncm::Title& title, const Keys::Keyset& keys, std::string* error) {
        // Mirrors dump.cpp exactly: a title lives in either SD or built-in storage.
        const NcmStorageId storage_id = (title.storage == Ncm::Storage::SdCard)
            ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

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
                    have_rights = nca_rights_id(&m_cs, &ci.content_id, keys, rights_id);
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
                } else if (R_FAILED(ncmContentStorageReadContentIdFile(
                               &m_cs, out, n, &c.id, (s64)rel))) {
                    return -1;
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
