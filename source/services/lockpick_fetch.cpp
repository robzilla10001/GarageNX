// source/services/lockpick_fetch.cpp

#include "services/lockpick_fetch.hpp"
#include "core/fs.hpp"

#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <minizip/unzip.h>

#include <cstdio>
#include <string>
#include <vector>

namespace Services::LockpickFetch {

namespace {

constexpr size_t kMaxApiBytes = 1024 * 1024;
constexpr size_t kMaxZipBytes = 16 * 1024 * 1024;
constexpr size_t kMaxBinBytes = 4 * 1024 * 1024;   // Lockpick_RCM is well under 200 KB
const char* const kTmpZip = "sdmc:/switch/GarageNX/lockpick.zip.tmp";

struct Sink {
    std::string* mem = nullptr;   // either memory ...
    FILE*        fp  = nullptr;   // ... or a file
    size_t       cap = 0;
    size_t       got = 0;
    bool         overflow = false;
};

size_t write_cb(char* p, size_t sz, size_t n, void* user) {
    auto* s = static_cast<Sink*>(user);
    const size_t len = sz * n;
    s->got += len;
    if (s->got > s->cap) { s->overflow = true; return 0; }
    if (s->mem) { s->mem->append(p, len); return len; }
    return fwrite(p, 1, len, s->fp) == len ? len : 0;
}

struct Prog {
    const std::function<void(const Status&)>* cb = nullptr;
    Status::Phase phase = Status::Phase::Release;
};

int xfer_cb(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* p = static_cast<Prog*>(user);
    Status st;
    st.phase    = p->phase;
    st.fraction = dltotal > 0 ? (double)dlnow / (double)dltotal : 0.0;
    if (p->cb && *p->cb) (*p->cb)(st);
    return 0;
}

// One GET. Certificates are not verified (see header).
bool http_get(const std::string& url, Sink& sink, Prog& prog, std::string& err) {
    static bool inited = false;
    if (!inited) { curl_global_init(CURL_GLOBAL_DEFAULT); inited = true; }

    CURL* c = curl_easy_init();
    if (!c) { err = "curl init failed"; return false; }
    curl_slist* hdrs = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "GarageNX/" APP_VERSION);   // GitHub rejects requests without one
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &prog);

    const CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
        err = sink.overflow ? "download too large"
                            : std::string("network error: ") + curl_easy_strerror(rc);
        if (http) err += " (HTTP " + std::to_string(http) + ")";
        return false;
    }
    return true;
}

// Extract the payload entry to kPayloadPath via a temp file + swap, so a failure
// never leaves a truncated payload where a reboot might run it.
bool extract_payload(const std::string& zip_path, std::string& err) {
    unzFile uz = unzOpen(zip_path.c_str());
    if (!uz) { err = "downloaded file is not a valid zip"; return false; }

    std::vector<std::string> names;
    if (unzGoToFirstFile(uz) == UNZ_OK) {
        do {
            char name[512];
            unz_file_info info;
            if (unzGetCurrentFileInfo(uz, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
                name[0] = '\0';
            names.emplace_back(name);
        } while (unzGoToNextFile(uz) == UNZ_OK);
    }
    const int idx = find_payload_entry(names);
    if (idx < 0) { unzClose(uz); err = "Lockpick_RCM_pro.bin not found in the zip"; return false; }

    unzGoToFirstFile(uz);
    for (int i = 0; i < idx; ++i) unzGoToNextFile(uz);

    unz_file_info info;
    unzGetCurrentFileInfo(uz, &info, nullptr, 0, nullptr, 0, nullptr, 0);
    if (info.uncompressed_size == 0 || info.uncompressed_size > kMaxBinBytes) {
        unzClose(uz); err = "payload has an unexpected size"; return false;
    }
    if (unzOpenCurrentFile(uz) != UNZ_OK) { unzClose(uz); err = "cannot read zip entry"; return false; }

    const std::string tmp = std::string(kPayloadPath) + ".gnxtmp";
    Fs::make_directory_recursive(kPayloadDir);   // NOT-A-SAVE: SD card
    FILE* out = fopen(tmp.c_str(), "wb");
    bool ok = out != nullptr;
    size_t total = 0;
    std::vector<char> buf(32 * 1024);
    while (ok) {
        const int n = unzReadCurrentFile(uz, buf.data(), (unsigned)buf.size());
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (fwrite(buf.data(), 1, (size_t)n, out) != (size_t)n) { ok = false; break; }
        total += (size_t)n;
    }
    if (out && fclose(out) != 0) ok = false;
    const bool crc_ok = (unzCloseCurrentFile(uz) == UNZ_OK);   // UNZ_CRCERROR if corrupt
    unzClose(uz);

    if (!ok || !crc_ok || total != info.uncompressed_size) {
        Fs::remove_file(tmp);   // NOT-A-SAVE: SD card
        err = "could not extract the payload";
        return false;
    }
    if (Fs::exists(kPayloadPath)) Fs::remove_file(kPayloadPath);   // NOT-A-SAVE: SD card; FAT cannot rename over a file
    if (!Fs::rename(tmp, kPayloadPath)) {                           // NOT-A-SAVE: SD card
        Fs::remove_file(tmp);                                       // NOT-A-SAVE: SD card
        err = "could not write " + std::string(kPayloadPath);
        return false;
    }
    return true;
}

} // namespace

Result fetch_latest(const std::function<void(const Status&)>& on_progress) {
    Result r;
    Prog prog;
    prog.cb = &on_progress;

    auto fail = [&](const std::string& why) {
        SDL_Log("LockpickFetch - %s", why.c_str());
        r.error = why;
        return r;
    };

    // 1. Release metadata.
    std::string json, err;
    {
        Sink s; s.mem = &json; s.cap = kMaxApiBytes;
        prog.phase = Status::Phase::Release;
        if (!http_get(kRepoApiUrl, s, prog, err)) return fail(err);
    }
    std::string tag;
    const std::string zip_url = pick_zip_url(json, &tag);
    if (zip_url.empty()) return fail("latest release has no zip download");

    // 2. The zip, to a temp file next to the app's data.
    Fs::make_directory_recursive("sdmc:/switch/GarageNX");   // NOT-A-SAVE: SD card
    {
        Sink s; s.fp = fopen(kTmpZip, "wb"); s.cap = kMaxZipBytes;
        if (!s.fp) return fail("cannot create temp file on the SD card");
        prog.phase = Status::Phase::Download;
        const bool ok = http_get(zip_url, s, prog, err);
        const bool closed = fclose(s.fp) == 0;
        if (!ok || !closed) {
            Fs::remove_file(kTmpZip);   // NOT-A-SAVE: SD card
            return fail(ok ? "could not write the download" : err);
        }
    }

    // 3. Pull the payload out.
    if (on_progress) { Status st; st.phase = Status::Phase::Extract; on_progress(st); }
    const bool ok = extract_payload(kTmpZip, err);
    Fs::remove_file(kTmpZip);   // NOT-A-SAVE: SD card
    if (!ok) return fail(err);

    r.ok  = true;
    r.tag = tag;
    SDL_Log("LockpickFetch - installed %s from %s", kPayloadPath, tag.c_str());
    return r;
}

} // namespace Services::LockpickFetch
