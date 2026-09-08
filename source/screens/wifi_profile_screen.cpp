// source/screens/wifi_profile_screen.cpp

#include "screens/wifi_profile_screen.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/layout.hpp"
#include "ui/input.hpp"
#include "lang/localization.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

#ifdef PLATFORM_SWITCH
// Appends one line to sdmc:/switch/GarageNX/logs/wifi.log (the dir is created
// by boot's ensure_directories; the RemoveNetworkProfile failure log below
// already uses this file). Enumeration results land here too, so "what did
// the console actually store for each profile" is answerable from the SD
// card instead of from what a list row happened to show.
static void wifi_log(const char* fmt, ...) {
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/wifi.log", "a");
    if (!f) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    ::fprintf(f, "%s\n", line);
    ::fclose(f);
}

// Wire layout per Switchbrew's SfNetworkProfileBasicInfo table (Network
// Interface services wiki page, IGeneralService section - fetched directly
// this session, not from memory). Packed, no compiler-inserted padding: this
// is read straight out of an IPC output buffer byte-for-byte. The
// static_assert catches a layout mistake at COMPILE time rather than a
// silent misread at runtime.
#pragma pack(push, 1)
struct RawNetworkProfileBasicInfo {
    uint8_t id[0x10];        // 0x00 Uuid - the handle RemoveNetworkProfile needs
    char    name[0x40];      // 0x10 NUL-terminated network name
    uint8_t profile_type;    // 0x50 NetworkProfileType
    uint8_t iface_type;      // 0x51 NetworkInterfaceType
    uint8_t ssid[0x21];      // 0x52 Ssid
    uint8_t auth;            // 0x73 Authentication
    uint8_t enc;             // 0x74 Encryption
};
#pragma pack(pop)
static_assert(sizeof(RawNetworkProfileBasicInfo) == 0x75,
             "must match Switchbrew's documented SfNetworkProfileBasicInfo exactly");

constexpr uint8_t kNifmProfileType_User = 1; // bit 0 per Switchbrew's NetworkProfileType table

// cmd 7, EnumerateNetworkProfiles. Not wrapped by libnx's nifm.h (confirmed by
// reading the full header - only GetCurrentNetworkProfile [the ACTIVE one]
// and GetNetworkProfile [needs an already-known Uuid] exist there). Command
// number, input shape, and output struct layout are all from Switchbrew's own
// NIFM_services page. Proven correct on real hardware: this is what found the
// profiles in the first place when this op was still a batch-delete.
Result nifmEnumerateNetworkProfilesRaw(uint8_t profile_type,
                                       RawNetworkProfileBasicInfo* out,
                                       s32 max_count, s32* out_total) {
    return serviceDispatchInOut(nifmGetServiceSession_GeneralService(), 7,
        profile_type, *out_total,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, sizeof(RawNetworkProfileBasicInfo) * (size_t)max_count } });
}

// cmd 10, RemoveNetworkProfile. The command NUMBER is confirmed real (it's in
// Switchbrew's own IGeneralService table); the input shape matches its
// neighbor cmd 8 GetNetworkProfile (a raw Uuid).
//
// HARDWARE FINDING (see docs/ARCHITECTURE.md §7): against
// the boot-time nifm:u (User) session, cmd 10 fails SAFE with
// rc=0x0000F06E (Module 110/Nifm, Description 120) - zero deletions, no
// corruption. The sibling write command SetNetworkProfile (cmd 9) is
// documented by Switchbrew as "only available with nifm:a", and cmd 10 is
// architecturally analogous - so the fix is a temporary session toggle:
// tear down nifm, re-init as ADMIN, run cmd 10, restore User. libnx's own
// nifm.c makes this safe: _nifmInitialize re-creates the GeneralService
// automatically on every init. Nothing else runs during this synchronous call.
Result nifmRemoveNetworkProfileRaw(const uint8_t uuid[0x10]) {
    struct { uint8_t id[0x10]; } raw_uuid;
    std::memcpy(raw_uuid.id, uuid, 0x10);

    nifmExit();
    Result rc = nifmInitialize(NifmServiceType_Admin);
    if (R_SUCCEEDED(rc)) {
        rc = serviceDispatchIn(nifmGetServiceSession_GeneralService(), 10, raw_uuid);
    } else {
        wifi_log("RemoveNetworkProfile: nifmInitialize(Admin) failed rc=0x%08X", rc);
    }

    // Restore the User session no matter how the above went - the rest of the
    // app (network status, connections) expects the boot-time nifm:u session.
    nifmExit();
    const Result rc_user = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc_user)) {
        wifi_log("RemoveNetworkProfile: restore nifm:u failed rc=0x%08X", rc_user);
    }

    char hex[33] = {0};
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02X", uuid[i]);
    if (R_FAILED(rc))
        wifi_log("RemoveNetworkProfile (admin) uuid=%s rc=0x%08X", hex, rc);
    else
        wifi_log("RemoveNetworkProfile (admin) uuid=%s SUCCESS", hex);
    return rc;
}

struct RawHit {
    std::array<uint8_t, 0x10> uuid;
    std::string               name;   // profile name (console-assigned)
    std::string               ssid;   // resolved SSID - the actual network id
};

std::vector<RawHit> enumerate_profiles() {
    constexpr s32 WINDOW = 64; // generous - a console realistically has a handful
    std::vector<RawNetworkProfileBasicInfo> buf(WINDOW);
    s32 total = 0;
    std::vector<RawHit> out;
    if (R_FAILED(nifmEnumerateNetworkProfilesRaw(kNifmProfileType_User, buf.data(),
                                                 WINDOW, &total))) {
        wifi_log("enumerate: nifmEnumerateNetworkProfilesRaw FAILED");
        return out;
    }
    const s32 count = (total < WINDOW) ? total : WINDOW; // defensive cap
    if (total > WINDOW)
        wifi_log("enumerate: %d profile(s) exist, buffer held %d - THE REST ARE DROPPED",
                 total, WINDOW);
    wifi_log("enumerate: %d profile(s) read (total %d)", count, total);
    for (s32 i = 0; i < count; ++i) {
        const RawNetworkProfileBasicInfo& p = buf[(size_t)i];
        RawHit h;
        std::memcpy(h.uuid.data(), p.id, 0x10);
        // name is a NUL-terminated string within a 0x40 buffer, but not
        // guaranteed NUL-terminated if the console ever wrote exactly 0x40
        // bytes with none to spare - bound the copy defensively rather than
        // trust it.
        char namebuf[0x41] = {0};
        std::memcpy(namebuf, p.name, 0x40);
        h.name = namebuf;

        // Resolve the SSID. Per Switchbrew (Network Interface services, fetched
        // 2026-09-07): nn::nifm::Ssid is a 0x21-byte struct at SfNetworkProfile-
        // BasicInfo+0x52 - byte 0 is Length, bytes 1..0x20 hold the NUL-
        // terminated SSID string. The screen used to show ONLY the Name field;
        // on consoles where that is empty or generic, deletion was blind - the
        // user could not tell WHICH network a row was. Length is authoritative
        // but clamped, and a premature NUL ends the string early either way.
        const uint8_t ssid_len = (p.ssid[0] < 0x20) ? p.ssid[0] : 0x20;
        char ssidbuf[0x21] = {0};
        std::memcpy(ssidbuf, p.ssid + 1, ssid_len);
        // SSIDs are raw bytes; keep the renderer safe by masking anything
        // outside printable ASCII.
        for (char* q = ssidbuf; *q; ++q) {
            const unsigned char ch = (unsigned char)*q;
            if (ch < 0x20 || ch > 0x7E) *q = '?';
        }
        h.ssid = ssidbuf;

        wifi_log("  profile %d: name='%s' ssid(len=%u)='%s'",
                 i, h.name.c_str(), (unsigned)p.ssid[0], h.ssid.c_str());
        out.push_back(std::move(h));
    }
    return out;
}
#endif // PLATFORM_SWITCH

} // namespace

WifiProfileScreen::WifiProfileScreen() {}

void WifiProfileScreen::on_enter() {
    reload();
}

void WifiProfileScreen::reload() {
    m_profiles.clear();
#ifdef PLATFORM_SWITCH
    for (const auto& h : enumerate_profiles())
        m_profiles.push_back({ h.uuid, h.name, h.ssid });
#endif

    std::vector<Widgets::ListItem> rows;
    for (const auto& c : m_profiles) {
        Widgets::ListItem row;
        // The SSID is the actual network identifier - show it first. The
        // profile name is a fallback (and shown alongside in the confirm text
        // when it differs), never a substitute that leaves the user deleting
        // blind.
        row.label = !c.ssid.empty() ? c.ssid
                  : !c.name.empty() ? c.name
                  : std::string("(unnamed network)");
        rows.push_back(row);
    }
    m_list.set_items(std::move(rows));
}

void WifiProfileScreen::select(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_profiles.size())) return;
    const auto& c = m_profiles[(size_t)idx];

    // Name the SPECIFIC network being deleted: SSID first (the authoritative
    // identifier), with the console's profile name alongside when it differs -
    // a confirm dialog that cannot say which network dies is not a confirm.
    std::string display = !c.ssid.empty() ? c.ssid
                        : !c.name.empty() ? c.name
                        : std::string("(unnamed network)");
    if (!c.ssid.empty() && !c.name.empty() && c.ssid != c.name)
        display += "  (" + c.name + ")";

    std::string body = Lang::t("tools.warn_wifi_profiles") + "\n\n" + display;
    Modal::show({ Lang::t("tools.delete_wifi_profiles"), body,
                  Modal::Kind::Danger,
                  Lang::t("tools.confirm_remove"),
                  Lang::t("modal.cancel") });
    m_pending = idx;
}

void WifiProfileScreen::on_modal_result(int result) {
    if (m_pending < 0) return;
    const int idx = m_pending;
    m_pending = -1;

    if (static_cast<Modal::Result>(result) != Modal::Result::Confirmed) return;
    if (idx < 0 || idx >= static_cast<int>(m_profiles.size())) return;

    bool ok = false;
#ifdef PLATFORM_SWITCH
    ok = R_SUCCEEDED(nifmRemoveNetworkProfileRaw(m_profiles[(size_t)idx].uuid.data()));
#endif
    Modal::show({ Lang::t("tools.delete_wifi_profiles"),
                  ok ? "Wi-Fi profile deleted." : "Failed to delete Wi-Fi profile.",
                  Modal::Kind::Info, Lang::t("modal.ok"), "" });
    reload();
}

std::unique_ptr<Screen> WifiProfileScreen::update(bool& pop) {
    pop = false;
    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }
    if (m_list.handle_input()) select(m_list.cursor());
    return nullptr;
}

void WifiProfileScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0, y = Layout::CONTENT_Y, w = Layout::SCREEN_W, h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    Widgets::ListStyle style;
    style.row_height    = Layout::MENU_ITEM_H;
    style.indent_x       = Layout::MENU_INDENT_X;
    style.show_checkbox  = false;
    style.show_dividers  = true;
    m_list.draw(x, y, w, h - 36, style);

    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("hints.select") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
