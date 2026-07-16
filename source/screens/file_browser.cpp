// source/screens/file_browser.cpp

#include "screens/file_browser.hpp"
#include "screens/file_viewer.hpp"
#include "core/fs.hpp"
#include "core/keys.hpp"
#include "core/datetime.hpp"
#include "install/installer.hpp"
#include "install/nsp_reader.hpp"
#include "install/xci_reader.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include "ui/modal.hpp"
#include "ui/keyboard.hpp"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

// Number of log lines shown at once in the install overlay's log box.
static constexpr int kInstallLogVisibleRows = 6;


// ─── Context menu action IDs ──────────────────────────────────────────────────
enum {
    ACT_COPY = 1,
    ACT_CUT,
    ACT_PASTE,
    ACT_RENAME,
    ACT_DELETE,
    ACT_NEW_DIR,
    ACT_NEW_FILE,
    ACT_OPEN_TEXT,
    ACT_OPEN_HEX,
    ACT_PEEK,
    ACT_INSTALL_SD,
    ACT_INSTALL_NAND,
    ACT_NRO_INSTALL,
};

// ─── File-type classification ─────────────────────────────────────────────────

static bool is_installable(const std::string& ext) {
    return ext == "nsp" || ext == "xci" || ext == "nsz" || ext == "xcz";
}
static bool is_nro(const std::string& ext) { return ext == "nro"; }

// Default-to-text extensions; everything else defaults to hex when opened raw.
static bool prefers_text(const std::string& ext) {
    static const char* kTextExts[] = {
        "txt","ini","json","xml","yaml","yml","log","md","cfg","conf",
        "cpp","hpp","c","h","cc","cxx","py","sh","lua","js","ts","html","css",
        "toml","csv","gitignore","","license"
    };
    for (auto* e : kTextExts) if (ext == e) return true;
    return false;
}

// ─── Pane ─────────────────────────────────────────────────────────────────────

void FileBrowserScreen::Pane::reload(bool reset_cursor) {
    entries = Fs::list(path, &ok);

    std::vector<Widgets::ListItem> items;
    items.reserve(entries.size());
    for (auto& e : entries) {
        Widgets::ListItem item;
        item.label = e.name;
        item.meta  = e.is_dir() ? Lang::t("file_browser.dir")
                                : Fs::format_size(e.size);
        items.push_back(std::move(item));
    }
    int prev = list.cursor();
    list.set_items(std::move(items));   // set_items resets the cursor to 0
    // Preserve the cursor for same-directory refreshes (after a file op); reset
    // to the top when the directory changed (opening a folder).
    if (!reset_cursor && prev >= 0) list.set_cursor(prev);
}

std::string FileBrowserScreen::Pane::selected_path() const {
    int idx = list.cursor();
    if (idx < 0 || idx >= (int)entries.size()) return "";
    return Fs::join(path, entries[idx].name);
}

const Fs::Entry* FileBrowserScreen::Pane::selected_entry() const {
    int idx = list.cursor();
    if (idx < 0 || idx >= (int)entries.size()) return nullptr;
    return &entries[idx];
}

// ─── Construction ─────────────────────────────────────────────────────────────

FileBrowserScreen::FileBrowserScreen(std::string root, std::string title)
    : m_root(std::move(root)), m_title(std::move(title)) {
    m_left.path  = m_root;
    m_right.path = m_root;
}

void FileBrowserScreen::on_enter() {
    m_left.reload();
    m_right.reload();
}

void FileBrowserScreen::on_modal_result(int result) {
    // Only meaningful when we have a delete pending confirmation.
    if (m_pending_delete.empty()) return;

    auto res = static_cast<Modal::Result>(result);
    if (res == Modal::Result::Confirmed) {
        auto targets = m_pending_delete;
        m_pending_delete.clear();
        m_progress.reset();
        m_op_active = true;
        m_op_label  = Lang::t("file_browser.op_deleting");
        Fs::remove_many(targets, m_progress);
        m_progress.done = true;
    } else {
        m_pending_delete.clear();
    }
}

// ─── Selection helpers ────────────────────────────────────────────────────────

std::vector<std::string> FileBrowserScreen::selected_or_cursor(Pane& pane) {
    std::vector<std::string> result;
    auto sel = pane.list.selected_indices();
    if (!sel.empty()) {
        for (int i : sel) {
            if (i >= 0 && i < (int)pane.entries.size())
                result.push_back(Fs::join(pane.path, pane.entries[i].name));
        }
    } else {
        std::string s = pane.selected_path();
        if (!s.empty()) result.push_back(s);
    }
    return result;
}

// ─── Update ───────────────────────────────────────────────────────────────────

std::unique_ptr<Screen> FileBrowserScreen::update(bool& pop) {
    pop = false;

    // ── Pending: open file in viewer (from context menu) ──────────────────────
    if (!m_pending_view_path.empty()) {
        std::string path = m_pending_view_path;
        bool hex = m_pending_view_hex;
        m_pending_view_path.clear();
        auto mode = hex ? FileViewerScreen::Mode::Hex
                        : FileViewerScreen::Mode::Text;
        return std::make_unique<FileViewerScreen>(path, mode);
    }

    // Poll active install — blocks other input while running.
    if (m_install_mode != InstallMode::None) {
        poll_install();
        // While installing, all input is swallowed (the install runs to
        // completion). In Result mode the box STAYS OPEN until the user
        // dismisses it, and the completed log is scrollable.
        if (m_install_mode == InstallMode::Result) {
            const int log_total = (int)m_install_progress.log_count();
            const int visible   = kInstallLogVisibleRows;
            const int max_scroll = std::max(0, log_total - visible);
            if (Input::repeat(Input::Button::DDown))
                m_install_log_scroll = std::min(max_scroll, m_install_log_scroll + 1);
            if (Input::repeat(Input::Button::DUp))
                m_install_log_scroll = std::max(0, m_install_log_scroll - 1);
            if (Input::pressed(Input::Button::A) || Input::pressed(Input::Button::B)) {
                m_install_mode = InstallMode::None;
                m_install_progress.reset();
            }
        }
        return nullptr;
    }

    // If an op is running, poll it; block other input until done.
    if (m_op_active) {
        if (m_progress.done.load()) {
            m_op_active = false;
            active_pane().reload();
            if (m_split) { m_left.reload(); m_right.reload(); }
        }
        return nullptr;
    }

    // If the context menu is open, it captures input.
    if (m_menu_open) {
        if (Input::pressed(Input::Button::B)) { m_menu_open = false; return nullptr; }
        if (m_menu_list.handle_input()) {
            int row = m_menu_list.cursor();
            if (row >= 0 && row < (int)m_menu_action_ids.size()) {
                int action = m_menu_action_ids[row];
                m_menu_open = false;
                handle_context_action(action);
            }
        }
        return nullptr;
    }

    Pane& pane = active_pane();

    // Split-view toggle
    if (Input::pressed(Input::Button::Minus)) {
        m_split = !m_split;
        if (m_split) {
            m_right.path = m_left.path;
            m_right.cursor_stack.clear();
            m_right.reload(true);
        }
        m_right_active = false;
        return nullptr;
    }

    // In split mode, L/R switches active pane
    if (m_split) {
        if (Input::pressed(Input::Button::L)) m_right_active = false;
        if (Input::pressed(Input::Button::R)) m_right_active = true;
    }

    // Context menu
    if (Input::pressed(Input::Button::Plus)) {
        open_context_menu();
        return nullptr;
    }

    // Back / up
    if (Input::pressed(Input::Button::B)) {
        if (Fs::is_root(pane.path)) {
            pop = true;   // exit browser at root
        } else {
            pane.path = Fs::parent(pane.path);
            pane.reload(true);   // new directory → cursor to top
            // Restore the selector to the folder we descended from.
            if (!pane.cursor_stack.empty()) {
                pane.list.set_cursor(pane.cursor_stack.back());
                pane.cursor_stack.pop_back();
            }
        }
        return nullptr;
    }

    // List navigation + A to open
    if (pane.list.handle_input()) {
        const Fs::Entry* e = pane.selected_entry();
        if (e) {
            if (e->is_dir()) {
                pane.cursor_stack.push_back(pane.list.cursor());  // remember where we were
                pane.path = Fs::join(pane.path, e->name);
                pane.reload(true);   // opening a folder → cursor at the top
            } else {
                std::string ext = e->extension();
                // Installable files default to a small Install submenu rather
                // than the text/hex viewer.
                if (is_installable(ext)) {
                    open_install_menu();
                    return nullptr;
                }
                auto mode = prefers_text(ext)
                    ? FileViewerScreen::Mode::Text
                    : FileViewerScreen::Mode::Hex;
                return std::make_unique<FileViewerScreen>(
                    Fs::join(pane.path, e->name), mode);
            }
        }
    }

    return nullptr;
}

// ─── Context menu construction ────────────────────────────────────────────────

void FileBrowserScreen::open_context_menu() {
    Pane& pane = active_pane();
    auto selected = pane.list.selected_indices();
    const Fs::Entry* cur = pane.selected_entry();

    std::vector<Widgets::ListItem> items;
    m_menu_action_ids.clear();

    auto add = [&](const std::string& key, int action) {
        Widgets::ListItem it; it.label = Lang::t(key);
        items.push_back(std::move(it));
        m_menu_action_ids.push_back(action);
    };

    bool multi = selected.size() > 1;

    if (multi) {
        // Multi-select: limited set
        add("file_browser.context_copy",   ACT_COPY);
        add("file_browser.context_cut",    ACT_CUT);
        add("file_browser.context_delete", ACT_DELETE);
    } else {
        add("file_browser.context_copy",   ACT_COPY);
        add("file_browser.context_cut",    ACT_CUT);
        if (m_clip_mode != ClipMode::None && !m_clipboard.empty())
            add("file_browser.context_paste", ACT_PASTE);
        add("file_browser.context_rename", ACT_RENAME);
        add("file_browser.context_delete", ACT_DELETE);
        add("file_browser.context_new_dir",  ACT_NEW_DIR);
        add("file_browser.context_new_file", ACT_NEW_FILE);

        if (cur && !cur->is_dir()) {
            std::string ext = cur->extension();
            add("file_browser.context_open_text", ACT_OPEN_TEXT);
            add("file_browser.context_open_hex",  ACT_OPEN_HEX);

            if (is_installable(ext)) {
                add("file_browser.context_peek",         ACT_PEEK);
                add("file_browser.context_install_sd",   ACT_INSTALL_SD);
                add("file_browser.context_install_nand", ACT_INSTALL_NAND);
            }
            if (is_nro(ext)) {
                add("file_browser.context_nro_install", ACT_NRO_INSTALL);
            }
        }
    }

    m_menu_list.set_items(std::move(items));
    m_menu_open = true;
}

// A small menu shown when A is pressed on an installable file. Reuses the
// context-menu machinery (m_menu_list / m_menu_action_ids), so the existing
// menu input handler drives selection and B closes it.
void FileBrowserScreen::open_install_menu() {
    std::vector<Widgets::ListItem> items;
    m_menu_action_ids.clear();

    auto add = [&](const std::string& key, int action) {
        Widgets::ListItem it; it.label = Lang::t(key);
        items.push_back(std::move(it));
        m_menu_action_ids.push_back(action);
    };

    add("file_browser.context_install_sd",   ACT_INSTALL_SD);
    add("file_browser.context_install_nand", ACT_INSTALL_NAND);

    m_menu_list.set_items(std::move(items));
    m_menu_open = true;
}

// ─── Context menu actions ─────────────────────────────────────────────────────

void FileBrowserScreen::handle_context_action(int action_id) {
    Pane& pane = active_pane();

    switch (action_id) {
        case ACT_COPY: {
            m_clipboard = selected_or_cursor(pane);
            m_clip_mode = ClipMode::Copy;
            break;
        }
        case ACT_CUT: {
            m_clipboard = selected_or_cursor(pane);
            m_clip_mode = ClipMode::Cut;
            break;
        }
        case ACT_PASTE:
            do_paste(m_clip_mode == ClipMode::Cut);
            break;
        case ACT_DELETE:
            do_delete_selected();
            break;
        case ACT_RENAME:
            do_rename();
            break;
        case ACT_NEW_DIR:
            do_new_dir();
            break;
        case ACT_NEW_FILE:
            do_new_file();
            break;
        case ACT_OPEN_TEXT:
        case ACT_OPEN_HEX: {
            // Handled by pushing a viewer — but update() returns screens, not
            // this function. Set a pending action the update path can pick up.
            // Simplest: open directly here is not possible (no return path), so
            // we stash and let update handle it next frame.
            // For Milestone 2 we open via the A button; these menu entries are
            // a convenience. We push through a small pending mechanism:
            m_pending_view_path = pane.selected_path();
            m_pending_view_hex  = (action_id == ACT_OPEN_HEX);
            break;
        }
        case ACT_PEEK:
            // TODO: Milestone 7 (NSP peek view)
            Modal::show({ Lang::t("modal.warning_title"),
                          Lang::t("modal.not_implemented"),
                          Modal::Kind::Info, Lang::t("modal.ok"), "" });
            break;
        case ACT_INSTALL_SD:
        case ACT_INSTALL_NAND: {
            std::string path = active_pane().selected_path();
            if (!path.empty()) {
                Core::Ncm::Storage dst = (action_id == ACT_INSTALL_SD)
                    ? Core::Ncm::Storage::SdCard
                    : Core::Ncm::Storage::BuiltIn;
                start_install(path, dst);
            }
            break;
        }
        case ACT_NRO_INSTALL:
            // TODO: Milestone 7 (NRO forwarder)
            Modal::show({ Lang::t("modal.warning_title"),
                          Lang::t("modal.not_implemented"),
                          Modal::Kind::Info, Lang::t("modal.ok"), "" });
            break;
    }
}

// ─── Operations ───────────────────────────────────────────────────────────────

void FileBrowserScreen::do_paste(bool cut) {
    if (m_clipboard.empty()) return;

    // Paste always targets the currently-active pane's directory. In split mode
    // the user selects the destination pane (L/R) before pasting; in single-pane
    // mode it's simply the current directory. This matches the intuitive model:
    // "wherever I'm looking is where it lands."
    std::string dest_dir = active_pane().path;

    // Conflict resolver: for Milestone 2 we default to a synchronous three-way
    // decision via a simple heuristic (auto-rename). A modal-driven resolver is
    // wired in when the threading harness lands; auto-rename keeps data safe now.
    auto resolver = [](const std::string& dest) -> Fs::Conflict {
        (void)dest;
        return Fs::Conflict::Rename;
    };

    // Run synchronously (SD copies are fast enough for M2; threaded in M6).
    m_progress.reset();
    m_op_active = true;
    m_op_label  = cut ? Lang::t("file_browser.op_moving")
                      : Lang::t("file_browser.op_copying");

    for (auto& src : m_clipboard) {
        if (cut) Fs::move(src, dest_dir, m_progress, resolver);
        else     Fs::copy(src, dest_dir, m_progress, resolver);
    }
    m_progress.done = true;

    if (cut) { m_clipboard.clear(); m_clip_mode = ClipMode::None; }
}

void FileBrowserScreen::do_delete_selected() {
    Pane& pane = active_pane();
    auto targets = selected_or_cursor(pane);
    if (targets.empty()) return;

    std::string body;
    if (targets.size() == 1) {
        body = Lang::t("file_browser.confirm_delete_one");
        // naive {name} substitution
        auto pos = body.find("{name}");
        if (pos != std::string::npos) body.replace(pos, 6, Fs::basename(targets[0]));
    } else {
        body = Lang::t("file_browser.confirm_delete_many");
        auto pos = body.find("{count}");
        if (pos != std::string::npos) body.replace(pos, 7, std::to_string(targets.size()));
    }
    body += "\n" + Lang::t("file_browser.confirm_delete_body");

    Modal::show({ Lang::t("file_browser.context_delete"), body,
                  Modal::Kind::Danger,
                  Lang::t("file_browser.context_delete"),
                  Lang::t("modal.cancel") });

    // Stash targets; the modal result is checked in update()/draw() cycle.
    m_pending_delete = targets;
}

void FileBrowserScreen::do_rename() {
    Pane& pane = active_pane();
    const Fs::Entry* e = pane.selected_entry();
    if (!e) return;

    Keyboard::Options opts;
    opts.header       = Lang::t("file_browser.rename_prompt");
    opts.initial_text = e->name;

    std::string new_name;
    if (Keyboard::get_text(opts, new_name) && !new_name.empty()) {
        std::string from = Fs::join(pane.path, e->name);
        std::string to   = Fs::join(pane.path, new_name);
        if (!Fs::rename(from, to)) {
            Modal::show({ Lang::t("modal.error_title"),
                          Lang::t("errors.write_error"),
                          Modal::Kind::Info, Lang::t("modal.ok"), "" });
        }
        pane.reload();
    }
}

void FileBrowserScreen::do_new_dir() {
    Pane& pane = active_pane();
    Keyboard::Options opts;
    opts.header = Lang::t("file_browser.new_dir_prompt");

    std::string name;
    if (Keyboard::get_text(opts, name) && !name.empty()) {
        Fs::make_directory(Fs::join(pane.path, name));
        pane.reload();
    }
}

void FileBrowserScreen::do_new_file() {
    Pane& pane = active_pane();
    Keyboard::Options opts;
    opts.header = Lang::t("file_browser.new_file_prompt");

    std::string name;
    if (Keyboard::get_text(opts, name) && !name.empty()) {
        Fs::create_empty_file(Fs::join(pane.path, name));
        pane.reload();
    }
}

// ─── Draw ───────────────────────────────────────────────────────────────────

void FileBrowserScreen::draw() {
    const int y = Layout::CONTENT_Y;
    const int h = Layout::CONTENT_H;

    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(0, y, Layout::SCREEN_W, h);

    // Nav column (always present)
    draw_nav_column(Layout::FB_COL1_X, y, Layout::FB_COL1_W, h);

    if (!m_split) {
        // Default: listing | details
        draw_pane(m_left, Layout::FB_COL2_X, y, Layout::FB_COL2_W, h, true, false);
        draw_details(m_left, Layout::FB_COL3_X, y, Layout::FB_COL3_W, h);
    } else {
        // Split: source | destination
        draw_pane(m_left,  Layout::FB_SPLIT_SRC_X, y, Layout::FB_SPLIT_HALF_W, h,
                  !m_right_active, false);
        draw_pane(m_right, Layout::FB_SPLIT_DST_X, y, Layout::FB_SPLIT_HALF_W, h,
                  m_right_active, false);
    }

    // Button legend
    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("hints.open") },
        { "X", Lang::t("hints.select") },
        { "+", Lang::t("hints.context_menu") },
        { "-", Lang::t("hints.split_view") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(0, y + h - 30, Layout::SCREEN_W, hints);

    // Context menu overlay
    if (m_menu_open) draw_context_menu();

    // Install overlay (takes priority over context menu)
    if (m_install_mode != InstallMode::None) draw_install_overlay();

    // Operation progress overlay
    if (m_op_active) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 140);
        Renderer::fill_rect(0, 0, Layout::SCREEN_W, Layout::SCREEN_H);
        int bx = (Layout::SCREEN_W - 480) / 2;
        int by = (Layout::SCREEN_H - 120) / 2;
        Theme::apply(r, Theme::Token::BgSurface);
        Renderer::fill_rect(bx, by, 480, 120);
        Theme::apply(r, Theme::Token::Border);
        Renderer::draw_rect(bx, by, 480, 120);
        Widgets::draw_text(bx + 20, by + 20, m_op_label,
                           Font::Size::Medium, Font::Weight::Bold,
                           Theme::Token::FgPrimary);
        Widgets::draw_progress(bx + 20, by + 70, 440, 16, m_progress.fraction());
    }
}

void FileBrowserScreen::draw_nav_column(int x, int y, int w, int h) {
    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(x + w - 1, y, h);   // right edge

    // Title
    Widgets::draw_text(x + Layout::PAD_MD, y + Layout::PAD_MD, m_title,
                       Font::Size::Medium, Font::Weight::Bold,
                       Theme::Token::FgPrimary, w - Layout::PAD_MD * 2);

    // Current path (wrapped/truncated)
    const std::string& path = active_pane().path;
    Widgets::draw_text(x + Layout::PAD_MD, y + 48, path,
                       Font::Size::Tiny, Font::Weight::Regular,
                       Theme::Token::FgSecondary, w - Layout::PAD_MD * 2);

    // Clipboard indicator
    if (m_clip_mode != ClipMode::None && !m_clipboard.empty()) {
        std::string label = (m_clip_mode == ClipMode::Copy ? "Copy: " : "Cut: ")
                          + std::to_string(m_clipboard.size());
        Widgets::draw_text(x + Layout::PAD_MD, y + h - 40, label,
                           Font::Size::Tiny, Font::Weight::Regular,
                           Theme::Token::Accent, w - Layout::PAD_MD * 2);
    }
}

void FileBrowserScreen::draw_pane(Pane& pane, int x, int y, int w, int h,
                                   bool active, bool /*details*/) {
    SDL_Renderer* r = Renderer::get();

    // Active pane gets a subtle top accent to show focus in split mode
    if (m_split && active) {
        Theme::apply(r, Theme::Token::Accent);
        Renderer::fill_rect(x, y, w, 2);
    }

    Widgets::ListStyle style;
    style.row_height    = Layout::LIST_ROW_H;
    style.indent_x      = Layout::PAD_MD;
    style.show_checkbox = true;   // file browser supports multi-select
    style.show_dividers = false;

    pane.list.draw(x, y + (m_split ? 4 : 0), w, h - 34, style);

    // Right edge divider
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(x + w - 1, y, h);
}

void FileBrowserScreen::draw_details(const Pane& pane, int x, int y, int w, int h) {
    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    const Fs::Entry* e = pane.selected_entry();
    if (!e) {
        Widgets::draw_text(x + Layout::PAD_MD, y + Layout::PAD_MD,
                           Lang::t("file_browser.empty"),
                           Font::Size::Body, Font::Weight::Regular,
                           Theme::Token::FgDisabled);
        return;
    }

    int cy = y + Layout::PAD_MD;

    // Name
    Widgets::draw_text(x + Layout::PAD_MD, cy, e->name,
                       Font::Size::Medium, Font::Weight::Bold,
                       Theme::Token::FgPrimary, w - Layout::PAD_MD * 2);
    cy += 36;

    // Type
    std::string type_label = e->is_dir()
        ? Lang::t("file_browser.dir")
        : (e->extension().empty() ? "file" : e->extension());
    Widgets::draw_text(x + Layout::PAD_MD, cy, "Type: " + type_label,
                       Font::Size::Small, Font::Weight::Regular,
                       Theme::Token::FgSecondary);
    cy += 26;

    // Size
    if (!e->is_dir()) {
        Widgets::draw_text(x + Layout::PAD_MD, cy,
                           "Size: " + Fs::format_size(e->size),
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgSecondary);
        cy += 26;
    }

    // Full path
    Widgets::draw_text(x + Layout::PAD_MD, cy,
                       Fs::join(pane.path, e->name),
                       Font::Size::Tiny, Font::Weight::Regular,
                       Theme::Token::FgDisabled, w - Layout::PAD_MD * 2);
}

void FileBrowserScreen::draw_context_menu() {
    SDL_Renderer* r = Renderer::get();

    // Dim
    SDL_SetRenderDrawColor(r, 0, 0, 0, 120);
    Renderer::fill_rect(0, 0, Layout::SCREEN_W, Layout::SCREEN_H);

    int mw = 340;
    int rows = m_menu_list.count();
    int mh = std::min(rows * Layout::LIST_ROW_H + Layout::PAD_MD * 2, 560);
    int mx = (Layout::SCREEN_W - mw) / 2;
    int my = (Layout::SCREEN_H - mh) / 2;

    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(mx, my, mw, mh);
    Theme::apply(r, Theme::Token::Accent);
    Renderer::fill_rect(mx, my, mw, 3);
    Theme::apply(r, Theme::Token::Border);
    Renderer::draw_rect(mx, my, mw, mh);

    Widgets::ListStyle style;
    style.row_height   = Layout::LIST_ROW_H;
    style.indent_x     = Layout::PAD_LG;
    style.show_dividers = false;
    m_menu_list.draw(mx, my + Layout::PAD_MD, mw, mh - Layout::PAD_MD * 2, style);
}

// ─── Install pipeline (Milestone 5) ──────────────────────────────────────────

struct InstallThreadArg {
    std::string                path;
    Core::Ncm::Storage         storage;
    const Core::Keys::Keyset*  keys;
    Install::Progress*         progress;
};

void FileBrowserScreen::install_thread_fn(void* arg_raw) {
    auto* arg = static_cast<InstallThreadArg*>(arg_raw);

#ifdef PLATFORM_SWITCH
    // Determine file type from extension.
    const std::string& path = arg->path;
    bool is_xci = false;
    {
        size_t dot = path.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = path.substr(dot + 1);
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            is_xci = (ext == "xci" || ext == "xcz");
        }
    }

    if (is_xci) {
        Install::XciReader reader(path);
        if (!reader.valid()) {
            arg->progress->message = "XCI open failed: " + reader.error();
            arg->progress->success = false;
            arg->progress->done    = true;
            delete arg;
            return;
        }
        auto entries = Install::entries_from_reader(reader);
        Install::install(std::move(entries), arg->storage, *arg->keys, *arg->progress);
    } else {
        Install::NspReader reader(path);
        if (!reader.valid()) {
            arg->progress->message = "NSP open failed: " + reader.error();
            arg->progress->success = false;
            arg->progress->done    = true;
            delete arg;
            return;
        }
        auto entries = Install::entries_from_reader(reader);
        Install::install(std::move(entries), arg->storage, *arg->keys, *arg->progress);
    }
#else
    // PC stub
    arg->progress->message = "Install (stub)";
    arg->progress->success = true;
    arg->progress->done    = true;
#endif

    delete arg;
}

void FileBrowserScreen::start_install(const std::string& path,
                                       Core::Ncm::Storage storage) {
    if (m_install_thread_active) return;

    m_install_progress.reset();
    m_install_progress.running = true;
    m_install_path   = path;
    m_install_storage = storage;
    m_install_mode   = InstallMode::Installing;
    m_install_log_saved  = false;
    m_install_log_scroll = 0;

    static const Core::Keys::Keyset* s_keys = nullptr;
    static bool s_keys_loaded = false;
    if (!s_keys_loaded) {
        Core::Keys::load();   // loads into the internal singleton
        s_keys = &Core::Keys::get();
        s_keys_loaded = true;
    }

    auto* arg = new InstallThreadArg{path, storage, s_keys, &m_install_progress};

#ifdef PLATFORM_SWITCH
    threadCreate(&m_install_thread, install_thread_fn, arg, nullptr, 512 * 1024, 0x2C, -2);
    threadStart(&m_install_thread);
    m_install_thread_active = true;
#else
    // PC: run synchronously for stub.
    install_thread_fn(arg);
#endif
}

void FileBrowserScreen::poll_install() {
    if (m_install_mode == InstallMode::Installing) {
        if (m_install_progress.done.load()) {
            m_install_result_ok = m_install_progress.success.load();
#ifdef PLATFORM_SWITCH
            if (m_install_thread_active) {
                threadWaitForExit(&m_install_thread);
                threadClose(&m_install_thread);
                m_install_thread_active = false;
            }
#endif
            // Persist the verbose log (every install, success or failure) and
            // start the scrollable view at the bottom (most recent lines).
            save_install_log();
            const int total = (int)m_install_progress.log_count();
            m_install_log_scroll = std::max(0, total - kInstallLogVisibleRows);
            m_install_mode = InstallMode::Result;
        }
    }
}

// Writes the full install log to <log_folder>/<timestamp>.log. The filename is
// the current date/time (date order per the user's preference, time always 24h),
// which is both easy to locate and unique per install.
void FileBrowserScreen::save_install_log() {
    if (m_install_log_saved) return;
    m_install_log_saved = true;

    const std::string dir = Config::get().paths.log_folder;
    if (!dir.empty() && !Fs::exists(dir)) Fs::make_directory(dir);

    const std::string stamp = Core::DateTime::log_stamp_now();
    const std::string path  = (dir.empty() ? std::string("sdmc:/switch/GarageNX/logs")
                                            : dir) + "/" + stamp + ".log";

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        SDL_Log("save_install_log: could not open %s", path.c_str());
        return;
    }
    std::fprintf(f, "GarageNX install log — %s\n", Core::DateTime::clock_string_now().c_str());
    std::fprintf(f, "Source: %s\n", m_install_path.c_str());
    std::fprintf(f, "Target: %s\n",
                 m_install_storage == Core::Ncm::Storage::SdCard ? "SD card" : "internal (NAND)");
    std::fprintf(f, "Result: %s\n\n", m_install_result_ok ? "SUCCESS" : "FAILED");

    for (const auto& line : m_install_progress.log_snapshot())
        std::fprintf(f, "%s\n", line.c_str());

    // Ensure the final one-line status is always present at the end of the file.
    const std::string& msg = m_install_progress.message;
    if (!msg.empty())
        std::fprintf(f, "\n> %s\n", msg.c_str());

    std::fclose(f);
    SDL_Log("save_install_log: wrote %s", path.c_str());
}

void FileBrowserScreen::draw_install_overlay() {
    SDL_Renderer* r = Renderer::get();
    // Dim background.
    SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
    Renderer::fill_rect(0, 0, Layout::SCREEN_W, Layout::SCREEN_H);

    const int row_h  = 22;                                   // log line height
    const int log_h  = kInstallLogVisibleRows * row_h;
    const int bw = 720;
    const int bh = 250 + log_h;
    const int bx = (Layout::SCREEN_W - bw) / 2;
    const int by = (Layout::SCREEN_H - bh) / 2;

    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(bx, by, bw, bh);
    Theme::apply(r, Theme::Token::Border);
    Renderer::draw_rect(bx, by, bw, bh);

    const bool installing = (m_install_mode == InstallMode::Installing);

    // ── Title ─────────────────────────────────────────────────────────────────
    Theme::Token title_color = Theme::Token::FgPrimary;
    std::string  title       = Lang::t("install.installing");
    if (!installing) {
        title_color = m_install_result_ok ? Theme::Token::AccentOk : Theme::Token::AccentDanger;
        title       = m_install_result_ok ? Lang::t("install.success") : Lang::t("install.failed");
    }
    Widgets::draw_text(bx + 20, by + 16, title,
                       Font::Size::Medium, Font::Weight::Bold, title_color);

    // ── Current file + stage (installing only) ────────────────────────────────
    if (installing) {
        std::string cur = m_install_progress.current_file;
        if (!cur.empty()) {
            if (cur.size() > 60) cur = cur.substr(0, 57) + "...";
            Widgets::draw_text(bx + 20, by + 48, cur,
                               Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
        }
        std::string stage = m_install_progress.stage;
        if (!stage.empty())
            Widgets::draw_text(bx + bw - 200, by + 48, stage,
                               Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
    }

    // ── Progress bar + NCA count ──────────────────────────────────────────────
    Widgets::draw_progress(bx + 20, by + 78, bw - 40, 16, m_install_progress.fraction());
    {
        int done = m_install_progress.ncas_done.load();
        int total = m_install_progress.ncas_total.load();
        if (total > 0) {
            char nbuf[32];
            snprintf(nbuf, sizeof(nbuf), "%d / %d NCAs", done, total);
            Widgets::draw_text(bx + 20, by + 100, nbuf,
                               Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
        }
    }

    // ── Log box ───────────────────────────────────────────────────────────────
    const int lx = bx + 20, ly = by + 128, lw = bw - 40;
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(lx, ly, lw, log_h);
    Theme::apply(r, Theme::Token::Border);
    Renderer::draw_rect(lx, ly, lw, log_h);

    std::vector<std::string> lines = m_install_progress.log_snapshot();
    const int total_lines = (int)lines.size();

    // While installing, auto-follow the tail; in Result mode, use the user's
    // scroll offset (clamped).
    int start;
    if (installing) {
        start = std::max(0, total_lines - kInstallLogVisibleRows);
    } else {
        int max_scroll = std::max(0, total_lines - kInstallLogVisibleRows);
        start = std::min(std::max(0, m_install_log_scroll), max_scroll);
    }

    for (int i = 0; i < kInstallLogVisibleRows; ++i) {
        int idx = start + i;
        if (idx >= total_lines) break;
        const std::string& raw = lines[idx];
        // Error lines are highlighted; on a failed result the final line is too.
        bool is_error = (raw.rfind("ERROR", 0) == 0) ||
                        (!installing && !m_install_result_ok && idx == total_lines - 1);
        std::string ln = raw;
        if (ln.size() > 92) ln = ln.substr(0, 89) + "...";
        Widgets::draw_text(lx + 8, ly + 4 + i * row_h, ln,
                           Font::Size::Small, Font::Weight::Regular,
                           is_error ? Theme::Token::AccentDanger : Theme::Token::FgPrimary);
    }

    // Scroll indicator (Result mode, when scrollable).
    if (!installing && total_lines > kInstallLogVisibleRows) {
        char sc[24];
        snprintf(sc, sizeof(sc), "%d-%d/%d",
                 start + 1, std::min(start + kInstallLogVisibleRows, total_lines), total_lines);
        Widgets::draw_text(lx + lw - 90, ly - 20, sc,
                           Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    const int fy = ly + log_h + 12;
    if (installing) {
        Widgets::draw_text(bx + 20, fy, Lang::t("install.please_wait"),
                           Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
    } else {
        // Final one-line status, highlighted on failure.
        std::string msg = m_install_progress.message;
        if (msg.size() > 78) msg = msg.substr(0, 75) + "...";
        Widgets::draw_text(bx + 20, fy, msg,
                           Font::Size::Small, Font::Weight::Regular,
                           m_install_result_ok ? Theme::Token::FgPrimary : Theme::Token::AccentDanger);
        Widgets::draw_text(bx + 20, fy + 22, Lang::t("install.log_hint"),
                           Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
    }
}
