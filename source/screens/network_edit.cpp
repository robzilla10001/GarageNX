// source/screens/network_edit.cpp

#include "screens/network_edit.hpp"
#include "services/net_surface.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "ui/input.hpp"
#include "ui/keyboard.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <SDL2/SDL.h>

#include <string>

NetworkEditScreen::NetworkEditScreen() {
    // Sensible defaults for a new connection: SMB is the common home-NAS case.
    m_draft.protocol = "smb";
    rebuild();
}

NetworkEditScreen::NetworkEditScreen(int edit_index) : m_edit_index(edit_index) {
    const auto& shares = Config::get().network.shares;
    if (edit_index >= 0 && edit_index < static_cast<int>(shares.size()))
        m_draft = shares[edit_index];
    else
        m_edit_index = -1;   // out of range — treat as a new connection
    if (m_draft.protocol.empty()) m_draft.protocol = "smb";
    rebuild();
}

static std::string field_row(const std::string& label, const std::string& value,
                             const std::string& empty_placeholder) {
    return label + ":  " + (value.empty() ? empty_placeholder : value);
}

void NetworkEditScreen::rebuild() {
    const bool is_smb =
        Services::net_protocol_parse(m_draft.protocol) == Services::NetProtocol::Smb;

    m_row_actions.clear();
    std::vector<Widgets::ListItem> rows;

    auto add = [&](Action a, const std::string& text) {
        m_row_actions.push_back(a);
        Widgets::ListItem it; it.label = text; rows.push_back(std::move(it));
    };

    const std::string req = Lang::t("network.value_required");

    add(Action::Name,     field_row(Lang::t("network.field_name"),     m_draft.name, req));
    add(Action::Protocol, Lang::t("network.field_protocol") + ":  " +
                          (is_smb ? "SMB" : "NFS"));
    add(Action::Host,     field_row(Lang::t("network.field_host"),     m_draft.host, req));
    add(Action::Share,    field_row(Lang::t("network.field_share"),    m_draft.share, req));
    add(Action::Path,     field_row(Lang::t("network.field_path"),     m_draft.path,
                                    Lang::t("network.value_optional")));

    // Port: 0 means "use the protocol default", shown so the user knows what will
    // be dialled without having to know the number.
    const uint16_t def = Services::net_default_port(Services::net_protocol_parse(m_draft.protocol));
    const std::string port_val = m_draft.port
        ? std::to_string(m_draft.port)
        : "(" + Lang::t("network.port_default") + " " + std::to_string(def) + ")";
    add(Action::Port, Lang::t("network.field_port") + ":  " + port_val);

    // Username/domain apply to SMB only — hide them for NFS so the form matches the
    // protocol and there are no fields that quietly do nothing.
    if (is_smb) {
        add(Action::Username, field_row(Lang::t("network.field_username"),
                                        m_draft.username, Lang::t("network.value_guest")));
        add(Action::Domain,   field_row(Lang::t("network.field_domain"),
                                        m_draft.domain, Lang::t("network.value_none")));
    }

    add(Action::Save, Lang::t("network.save"));
    if (m_edit_index >= 0) add(Action::Delete, Lang::t("network.delete"));

    // Keep the cursor in range if the row count shrank (e.g. SMB→NFS hid two rows).
    int cur = m_list.cursor();
    m_list.set_items(std::move(rows));
    if (cur >= static_cast<int>(m_row_actions.size()))
        cur = static_cast<int>(m_row_actions.size()) - 1;
    if (cur >= 0) m_list.set_cursor(cur);
}

void NetworkEditScreen::activate(Action a) {
    switch (a) {
        case Action::Name: {
            Keyboard::Options ko;
            ko.header = Lang::t("network.field_name");
            ko.initial_text = m_draft.name;
            std::string v;
            if (Keyboard::get_text(ko, v)) { m_draft.name = v; rebuild(); }
            break;
        }
        case Action::Protocol:
            // Toggle SMB <-> NFS. rebuild() then shows/hides the SMB-only fields.
            m_draft.protocol =
                (Services::net_protocol_parse(m_draft.protocol) == Services::NetProtocol::Smb)
                    ? "nfs" : "smb";
            rebuild();
            break;
        case Action::Host: {
            Keyboard::Options ko;
            ko.header = Lang::t("network.field_host");
            ko.initial_text = m_draft.host;
            std::string v;
            if (Keyboard::get_text(ko, v)) { m_draft.host = v; rebuild(); }
            break;
        }
        case Action::Share: {
            Keyboard::Options ko;
            ko.header = Lang::t("network.field_share");
            ko.initial_text = m_draft.share;
            std::string v;
            if (Keyboard::get_text(ko, v)) { m_draft.share = v; rebuild(); }
            break;
        }
        case Action::Path: {
            Keyboard::Options ko;
            ko.header = Lang::t("network.field_path");
            ko.initial_text = m_draft.path;
            ko.allow_empty = true;   // optional — empty means start at the share root
            std::string v;
            if (Keyboard::get_text(ko, v)) { m_draft.path = v; rebuild(); }
            break;
        }
        case Action::Port: {
            int v = m_draft.port;
            if (Keyboard::get_number(Lang::t("network.field_port"), v, v)) {
                if (v < 0) v = 0;
                if (v > 65535) v = 65535;
                m_draft.port = static_cast<uint16_t>(v);
                rebuild();
            }
            break;
        }
        case Action::Username: {
            Keyboard::Options ko;
            ko.header = Lang::t("network.field_username");
            ko.initial_text = m_draft.username;
            ko.allow_empty = true;   // empty username = guest
            std::string v;
            if (Keyboard::get_text(ko, v)) { m_draft.username = v; rebuild(); }
            break;
        }
        case Action::Domain: {
            Keyboard::Options ko;
            ko.header = Lang::t("network.field_domain");
            ko.initial_text = m_draft.domain;
            ko.allow_empty = true;
            std::string v;
            if (Keyboard::get_text(ko, v)) { m_draft.domain = v; rebuild(); }
            break;
        }
        case Action::Save: {
            const std::string err = validate();
            if (!err.empty()) {
                Modal::Options o;
                o.kind  = Modal::Kind::Info;
                o.title = Lang::t(err == "dup" ? "network.dup_title" : "network.invalid_title");
                o.body  = Lang::t(err == "dup" ? "network.dup_body"  : "network.invalid_body");
                o.confirm_label = Lang::t("common.ok");
                Modal::show(o);
                break;
            }
            commit();
            m_pop_requested = true;   // back to the chooser, which rebuilds on entry
            break;
        }
        case Action::Delete: {
            Modal::Options o;
            o.kind          = Modal::Kind::Danger;
            o.title         = Lang::t("network.delete_confirm_title");
            o.body          = Lang::t("network.delete_confirm_body");
            o.confirm_label = Lang::t("network.delete");
            o.cancel_label  = Lang::t("common.cancel");
            m_confirm_delete = true;
            Modal::show(o);
            break;
        }
    }
}

std::string NetworkEditScreen::validate() const {
    if (m_draft.name.empty() || m_draft.host.empty() || m_draft.share.empty())
        return "incomplete";

    // Name is the chooser identity and the credential-store key; it must be unique.
    // When editing, a name that matches THIS entry is fine — only a clash with a
    // DIFFERENT connection is rejected.
    const auto& shares = Config::get().network.shares;
    for (int i = 0; i < static_cast<int>(shares.size()); ++i) {
        if (i == m_edit_index) continue;
        if (shares[i].name == m_draft.name) return "dup";
    }
    return "";
}

void NetworkEditScreen::commit() {
    auto& shares = Config::get_mutable().network.shares;
    if (m_edit_index >= 0 && m_edit_index < static_cast<int>(shares.size()))
        shares[m_edit_index] = m_draft;
    else
        shares.push_back(m_draft);

    if (!Config::save())
        SDL_Log("network_edit: Config::save() failed");
}

void NetworkEditScreen::do_delete() {
    auto& shares = Config::get_mutable().network.shares;
    if (m_edit_index < 0 || m_edit_index >= static_cast<int>(shares.size())) return;

    // Drop any session password held for this connection before it disappears.
    Services::net_credentials().clear(shares[m_edit_index].name);
    shares.erase(shares.begin() + m_edit_index);

    if (!Config::save())
        SDL_Log("network_edit: Config::save() failed on delete");
    m_pop_requested = true;
}

void NetworkEditScreen::on_modal_result(int result) {
    if (!m_confirm_delete) return;
    m_confirm_delete = false;
    if (static_cast<Modal::Result>(result) == Modal::Result::Confirmed)
        do_delete();
}

std::unique_ptr<Screen> NetworkEditScreen::update(bool& pop) {
    pop = false;

    if (m_pop_requested) { pop = true; return nullptr; }

    // B discards unsaved edits and returns to the chooser.
    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    if (m_list.handle_input()) {
        const int idx = m_list.cursor();
        if (idx >= 0 && idx < static_cast<int>(m_row_actions.size()))
            activate(m_row_actions[idx]);
    }
    return nullptr;
}

void NetworkEditScreen::draw() {
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

    // The A hint reads "Change" because every row edits a value or performs the
    // Save/Delete action — none of them "open" anything.
    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("network.hint_change") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
