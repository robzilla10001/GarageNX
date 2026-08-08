// source/screens/network_browser.cpp

#include "screens/network_browser.hpp"
#include "screens/network_edit.hpp"
#include "screens/file_browser.hpp"
#include "services/net_surface.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/keyboard.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <SDL2/SDL.h>

NetworkBrowserScreen::NetworkBrowserScreen() {
    rebuild();
}

void NetworkBrowserScreen::on_enter() {
    // Rebuild on entry so a connection added by hand-editing config.json (or, later,
    // an in-app editor) shows up without restarting.
    rebuild();
}

void NetworkBrowserScreen::rebuild() {
    m_rows.clear();

    std::vector<Widgets::ListItem> items;
    for (const auto& s : Config::get().network.shares) {
        const std::string label = Services::net_display_label(s);
        m_rows.push_back({ s.name, label });
        Widgets::ListItem it;
        it.label = label;
        items.push_back(std::move(it));
    }

    // No connections: show a single, non-actionable info row instead of a blank
    // pane, so the screen explains what to do rather than looking broken.
    if (m_rows.empty()) {
        Widgets::ListItem it;
        it.label = Lang::t("network.no_connections");
        items.push_back(std::move(it));
    }

    m_list.set_items(std::move(items));
}

std::unique_ptr<Screen> NetworkBrowserScreen::open_selected() {
    if (m_rows.empty()) return nullptr;          // the info row is not actionable

    const int idx = m_list.cursor();
    if (idx < 0 || idx >= static_cast<int>(m_rows.size())) return nullptr;

    const Config::NetShare* s =
        Services::net_find(Config::get().network.shares, m_rows[idx].name);
    if (!s) return nullptr;

    // Prompt for a password only when the connection needs one and none is held
    // for this session yet. Masked entry; the answer goes to the in-memory store,
    // never to disk.
    if (Services::net_needs_password(*s)) {
        std::string held;
        if (!Services::net_credentials().get(s->name, held)) {
            Keyboard::Options ko;
            // Say plainly what is being asked for: the header names the connection,
            // and the guide text labels the field itself so the swkbd overlay never
            // looks like a blank, unexplained box.
            ko.header      = Lang::t("network.password_for") + " \"" + s->name + "\"";
            ko.guide       = Lang::t("network.password_guide");
            ko.password    = true;
            ko.allow_empty = false;
            std::string pw;
            if (!Keyboard::get_text(ko, pw)) return nullptr;   // cancelled
            Services::net_credentials().set(s->name, pw);
        }
    }

    // Connect + mount through the shared choke point. A file-level path for this
    // connection (rest empty = the share root) drives the single-slot mount.
    const std::string root = Services::net_resolve(
        Services::NetPath{ Services::NetPath::Level::Files, s->name, std::string() });

    if (root.empty()) {
        // The specific reason (set by net_resolve): log it so there is a durable
        // record, and show it in the modal instead of a generic line that told the
        // user nothing. The modal now stays up until dismissed (see ui/modal.cpp).
        const std::string reason = Services::net_last_error();
        SDL_Log("network: could not open '%s' (%s://%s): %s",
                s->name.c_str(), s->protocol.c_str(), s->host.c_str(),
                reason.empty() ? "unknown error" : reason.c_str());

        Modal::Options o;
        o.kind          = Modal::Kind::Info;
        o.title         = Lang::t("network.connect_failed_title");
        o.body          = reason.empty() ? Lang::t("network.connect_failed_body") : reason;
        o.confirm_label = Lang::t("common.ok");
        Modal::show(o);
        // Do not let a bad password stick: clear it so the next attempt re-prompts
        // rather than failing the same way with no chance to correct it.
        Services::net_credentials().clear(s->name);
        return nullptr;
    }

    // Start at the configured subfolder within the share, if any. net_resolve
    // returns "net:/" (ending in '/'); append the trimmed path so a connection can
    // open straight into e.g. "net:/movies/4k".
    std::string start = root;
    if (!s->path.empty()) {
        std::string p = s->path;
        while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
        while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
        if (!p.empty()) start = root + p;
    }
    return std::unique_ptr<Screen>(new FileBrowserScreen(start, m_rows[idx].label));
}

std::unique_ptr<Screen> NetworkBrowserScreen::update(bool& pop) {
    pop = false;

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    // X adds a new connection; Y edits the highlighted one. Both push the editor;
    // on return, on_enter() rebuilds this list so the change shows immediately.
    if (Input::pressed(Input::Button::X))
        return std::unique_ptr<Screen>(new NetworkEditScreen());
    if (Input::pressed(Input::Button::Y) && !m_rows.empty()) {
        const int idx = m_list.cursor();
        if (idx >= 0 && idx < static_cast<int>(m_rows.size()))
            return std::unique_ptr<Screen>(new NetworkEditScreen(idx));
    }

    if (m_list.handle_input()) {
        return open_selected();
    }
    return nullptr;
}

void NetworkBrowserScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    Widgets::ListStyle style;
    style.row_height    = Layout::MENU_ITEM_H;
    style.indent_x      = Layout::MENU_INDENT_X;
    style.show_checkbox = false;
    style.show_dividers = true;

    m_list.draw(x, y, w, h - 36, style);

    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("hints.open") },
        { "X", Lang::t("network.hint_add") },
        { "Y", Lang::t("network.hint_edit") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
