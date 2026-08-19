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

#include <cstdio>
#include <cstring>

namespace {

#ifdef PLATFORM_SWITCH
// Wire layout per Switchbrew's SfNetworkProfileBasicInfo table (Network
// Interface services wiki page, IGeneralService section — fetched directly
// this session, not from memory). Packed, no compiler-inserted padding: this
// is read straight out of an IPC output buffer byte-for-byte. The
// static_assert catches a layout mistake at COMPILE time rather than a
// silent misread at runtime.
#pragma pack(push, 1)
struct RawNetworkProfileBasicInfo {
    uint8_t id[0x10];        // 0x00 Uuid — the handle RemoveNetworkProfile needs
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
// reading the full header — only GetCurrentNetworkProfile [the ACTIVE one]
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
// Switchbrew's own IGeneralService table), but Switchbrew never wrote a
// parameter-shape description for this one specifically — unlike its
// immediate neighbors, cmd 8 GetNetworkProfile (takes a Uuid) and cmd 9
// SetNetworkProfile (returns one), both explicitly documented.
//
// FIRST ATTEMPT (this exact shape: a raw 0x10-byte Uuid input, matching cmd
// 8's confirmed pattern) tested on real hardware as a SAFE failure — 0
// deletions, no error surfaced, no corruption. That's reassuring on the risk
// question (a wrong guess here fails closed, it does not mistarget a
// profile) but it means this shape is not yet proven right either. The
// session/command number ARE proven right — enumeration via this same
// session works — so the remaining mystery is narrowly scoped to this one
// call's exact wire shape.
//
// Added: logs the raw Result code to disk on failure, so the next hardware
// test produces a concrete error code to reason from instead of another
// blind guess.
Result nifmRemoveNetworkProfileRaw(const uint8_t uuid[0x10]) {
    struct { uint8_t id[0x10]; } raw_uuid;
    std::memcpy(raw_uuid.id, uuid, 0x10);
    const Result rc = serviceDispatchIn(nifmGetServiceSession_GeneralService(), 10, raw_uuid);
    if (R_FAILED(rc)) {
        FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/wifi.log", "a");
        if (f) {
            char hex[33] = {0};
            for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02X", uuid[i]);
            ::fprintf(f, "  RemoveNetworkProfile uuid=%s rc=0x%08X\n", hex, rc);
            ::fclose(f);
        }
    }
    return rc;
}

struct RawHit { std::array<uint8_t, 0x10> uuid; std::string name; };

std::vector<RawHit> enumerate_profiles() {
    constexpr s32 WINDOW = 32; // generous — a console realistically has a handful
    std::vector<RawNetworkProfileBasicInfo> buf(WINDOW);
    s32 total = 0;
    std::vector<RawHit> out;
    if (R_FAILED(nifmEnumerateNetworkProfilesRaw(kNifmProfileType_User, buf.data(),
                                                 WINDOW, &total)))
        return out;
    const s32 count = (total < WINDOW) ? total : WINDOW; // defensive cap
    for (s32 i = 0; i < count; ++i) {
        RawHit h;
        std::memcpy(h.uuid.data(), buf[(size_t)i].id, 0x10);
        // name is a NUL-terminated string within a 0x40 buffer, but not
        // guaranteed NUL-terminated if the console ever wrote exactly 0x40
        // bytes with none to spare — bound the copy defensively rather than
        // trust it.
        char namebuf[0x41] = {0};
        std::memcpy(namebuf, buf[(size_t)i].name, 0x40);
        h.name = namebuf;
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
        m_profiles.push_back({ h.uuid, h.name });
#endif

    std::vector<Widgets::ListItem> rows;
    for (const auto& c : m_profiles) {
        Widgets::ListItem row;
        row.label = c.name.empty() ? std::string("(unnamed network)") : c.name;
        rows.push_back(row);
    }
    m_list.set_items(std::move(rows));
}

void WifiProfileScreen::select(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_profiles.size())) return;
    const auto& c = m_profiles[(size_t)idx];

    std::string body = Lang::t("tools.warn_wifi_profiles") + "\n\n" + c.name;
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
