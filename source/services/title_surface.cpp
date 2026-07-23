// source/services/title_surface.cpp

#include "services/title_surface.hpp"
#include "services/title_naming.hpp"
#include "core/keys.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

#ifdef PLATFORM_SWITCH
#include <cstdio>
#include <cstdarg>
#endif

namespace Services {

namespace {

std::mutex                      g_mutex;
std::condition_variable         g_cv;
bool                            g_shutdown = false;
std::vector<Core::Ncm::Title>   g_titles;
std::vector<VirtualEntry>       g_entries;
std::map<uint64_t, std::string> g_names;
size_t                          g_next_resolve = 0;
bool                            g_enumerated = false;
bool                            g_wanted     = false;

#ifdef PLATFORM_SWITCH
// TEMP DIAGNOSTIC: three attempts at this listing have now failed differently
// (placeholder names, a fatal, a silent exit). Record what actually happens so the
// next round is decided by evidence rather than another theory.
void tlog(const char* fmt, ...) {
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/titles.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    ::vfprintf(f, fmt, ap);
    va_end(ap);
    ::fputc('\n', f);
    ::fclose(f);
}
#else
void tlog(const char*, ...) {}
#endif

// Cheap: no ncm, no decryption. Safe to call from any thread. Caller holds mutex.
void rebuild_entries_locked() {
    g_entries.clear();
    g_entries.reserve(g_titles.size());
    for (const auto& t : g_titles) {
        Core::Ncm::Title named = t;
        auto it = g_names.find(t.meta_id);
        if (it != g_names.end() && !it->second.empty()) named.name = it->second;

        VirtualEntry e;
        e.name   = title_to_filename(named);
        e.size   = t.size_bytes;   // approximate until 3c
        e.is_dir = false;
        g_entries.push_back(std::move(e));
    }
}

} // namespace

// ─── Transport side: PURE CACHE READS. Never calls ncm, on any thread. ────────

std::vector<VirtualEntry> installed_titles_list() {
    std::unique_lock<std::mutex> lk(g_mutex);
    g_wanted = true;          // ask the main loop to do the ncm work
    g_cv.notify_all();

    // Block this WORKER thread until the main loop has enumerated and finished
    // resolving names — it does one unit of work per frame, so this is roughly
    // (1 + title_count) frames. Waiting here is what makes the FIRST listing
    // correct: without it a client saw an empty folder, because the cache is only
    // filled a frame after the request arrives.
    //
    // Blocking a transport worker while the main thread works is the same pattern
    // the ConfirmationBroker uses. The timeout means a stalled or busy main loop
    // (a modal is up, say) degrades to "listing shows what is ready so far"
    // instead of hanging the client forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    g_cv.wait_until(lk, deadline, [] {
        return g_shutdown || (g_enumerated && g_next_resolve >= g_titles.size());
    });

    return g_entries;   // COPY under the lock — see the header for why
}

bool installed_titles_find(const std::string& filename, Core::Ncm::Title& out) {
    std::unique_lock<std::mutex> lk(g_mutex);
    g_wanted = true;
    g_cv.notify_all();
    // Same wait as the listing: enumeration is enough here (we match on ids, not
    // names), so a client can request a title before names have finished resolving.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    g_cv.wait_until(lk, deadline, [] { return g_shutdown || g_enumerated; });

    const ParsedTitleName want = parse_title_filename(filename);
    if (!want.ok) return false;

    for (const auto& t : g_titles) {
        if (t.meta_id == want.meta_id && t.version == want.version &&
            t.type == want.type) {
            out = t;
            auto it = g_names.find(t.meta_id);
            if (it != g_names.end() && !it->second.empty()) out.name = it->second;
            return true;
        }
    }
    return false;
}

// ─── Main-thread side: ALL ncm work happens here, a little at a time ──────────

void installed_titles_tick() {
#ifdef PLATFORM_SWITCH
    std::lock_guard<std::mutex> lk(g_mutex);

    if (!g_wanted) return;   // nobody has asked; do nothing at all

    if (Core::Ncm::titles_dirty()) {
        Core::Ncm::clear_titles_dirty();
        g_enumerated = false;
        g_names.clear();
        tlog("titles dirty -> will re-enumerate");
    }

    // Enumerate ONCE, here on the main thread. This used to happen on whichever
    // thread called the listing (i.e. the FTP worker); ncm work on a transport
    // worker is what took the app down.
    if (!g_enumerated) {
        bool ok = false;
        g_titles = Core::Ncm::list_all(&ok);
        if (!ok) g_titles.clear();
        // Resolve base applications FIRST. Patches and DLC inherit their base's
        // name, so if one were processed before its base it would inherit an empty
        // name and keep it (each meta is only visited once).
        std::stable_partition(g_titles.begin(), g_titles.end(),
                              [](const Core::Ncm::Title& t) {
                                  return t.type == Core::Ncm::TitleType::Application;
                              });
        g_next_resolve = 0;
        g_enumerated   = true;
        rebuild_entries_locked();
        tlog("enumerate: ok=%d count=%zu keys_available=%d",
             ok ? 1 : 0, g_titles.size(), Core::Keys::available() ? 1 : 0);
        g_cv.notify_all();
        return;   // one unit of work per frame
    }

    if (g_next_resolve >= g_titles.size()) { g_cv.notify_all(); return; }   // all done
    if (!Core::Keys::available()) {
        static bool warned = false;
        if (!warned) { warned = true; tlog("no keys: names stay id-based"); }
        // Nothing more will ever happen without keys — release any waiter now
        // rather than making it sit out the full timeout.
        g_next_resolve = g_titles.size();
        g_cv.notify_all();
        return;
    }

    // Pace only the EXPENSIVE path. Reading a Control NCA is the operation that
    // must be spread across frames (doing them all at once is what took the app
    // down); inheriting a name for a patch or DLC is a lookup and costs nothing.
    // So: keep going until we have done ONE decryption, then yield the frame.
    // Base applications are ordered first, so in practice this resolves one app
    // per frame and then absorbs every patch/DLC in a single final frame.
    bool did_expensive_work = false;
    while (g_next_resolve < g_titles.size() && !did_expensive_work) {
        const Core::Ncm::Title t = g_titles[g_next_resolve];

        if (g_names.find(t.meta_id) != g_names.end()) {
            ++g_next_resolve;          // already handled
            continue;
        }

        if (t.type == Core::Ncm::TitleType::Application) {
            // ONLY base applications get their Control NCA read. The on-device
            // title screen resolves a grouped BASE application and never a patch or
            // add-on; DLC commonly has no Control content at all, so asking for one
            // walks a path nothing else in this codebase exercises.
            //
            // Logged BEFORE the call: if a specific title takes the app down, the
            // last line in the log names it.
            tlog("resolving %zu/%zu meta=%016llX",
                 g_next_resolve + 1, g_titles.size(), (unsigned long long)t.meta_id);

            Core::Nca::ControlData cd =
                Core::Ncm::resolve_control(t, Core::Keys::get(), /*want_icon*/ false);
            if (cd.ok && !cd.name.empty()) {
                g_names[t.meta_id] = cd.name;
            } else {
                g_names[t.meta_id] = std::string();   // remember: don't retry forever
                tlog("  -> FAILED: %s",
                     cd.fail_reason.empty() ? "unknown" : cd.fail_reason.c_str());
            }
            did_expensive_work = true;
        } else {
            // Patch / DLC: inherit the base application's name. No ncm work.
            const uint64_t base = Core::Ncm::base_application_id(t.program_id, t.type);
            std::string inherited;
            for (const auto& other : g_titles) {
                if (other.type != Core::Ncm::TitleType::Application) continue;
                if (Core::Ncm::base_application_id(other.program_id, other.type) != base)
                    continue;
                auto it = g_names.find(other.meta_id);
                if (it != g_names.end()) inherited = it->second;
                break;
            }
            g_names[t.meta_id] = inherited;
        }
        ++g_next_resolve;
    }

    rebuild_entries_locked();
    if (g_next_resolve >= g_titles.size()) {
        tlog("resolve pass complete (%zu titles)", g_titles.size());
        g_cv.notify_all();
    }
#endif
}

void installed_titles_shutdown() {
    // Teardown safety: release any transport worker blocked in a wait, so it
    // cannot sit on a main loop that is going away. Same rule as the broker.
    std::lock_guard<std::mutex> lk(g_mutex);
    g_shutdown = true;
    g_cv.notify_all();
}

void installed_titles_invalidate() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_enumerated = false;
}

} // namespace Services
