// source/screens/file_editor.cpp

#include "screens/file_editor.hpp"
#include "core/fs.hpp"
#include "services/save_write.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include "ui/keyboard.hpp"
#include "ui/modal.hpp"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace TW = Services::TextWindow;

namespace {

// Copy bytes [from, to) of `in` to `out`. Used to carry the untouched head and
// tail of the file across a save without ever holding them in memory.
bool copy_range(FILE* in, FILE* out, uint64_t from, uint64_t to) {
    if (to <= from) return true;
    if (fseek(in, (long)from, SEEK_SET) != 0) return false;
    std::vector<char> buf(64 * 1024);
    uint64_t left = to - from;
    while (left > 0) {
        const size_t want = (size_t)std::min<uint64_t>(left, buf.size());
        const size_t got  = fread(buf.data(), 1, want, in);
        if (got == 0) return false;
        if (fwrite(buf.data(), 1, got, out) != got) return false;
        left -= got;
    }
    return true;
}

// What gets drawn for a line: CR dropped, tabs expanded, control bytes shown as a
// dot, and a cap so one enormous line cannot make every frame expensive. The real
// text is never altered - this is display only.
std::string display_text(const std::string& raw) {
    constexpr size_t kMaxShown = 400;
    std::string out;
    out.reserve(std::min(raw.size(), kMaxShown) + 8);
    size_t n = raw.size();
    if (n > 0 && raw[n - 1] == '\r') --n;
    for (size_t i = 0; i < n && out.size() < kMaxShown; ++i) {
        const unsigned char c = (unsigned char)raw[i];
        if (c == '\t')      out += "    ";
        else if (c < 0x20)  out += '.';
        else                out += (char)c;
    }
    // Don't leave half a UTF-8 sequence at the cut.
    if (n > 0 && out.size() >= kMaxShown) {
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && ((unsigned char)out.back() & 0x80)) out.pop_back();
        out += "…";
    }
    return out;
}

} // namespace

FileEditorScreen::FileEditorScreen(std::string path) : m_path(std::move(path)) {}

// ─── Loading ──────────────────────────────────────────────────────────────────

bool FileEditorScreen::read_range(uint64_t off, size_t len, std::string& out) const {
    out.clear();
    FILE* f = fopen(m_path.c_str(), "rb");
    if (!f) return false;
    bool ok = (fseek(f, (long)off, SEEK_SET) == 0);
    if (ok) {
        out.resize(len);
        const size_t got = fread(&out[0], 1, len, f);
        out.resize(got);
        ok = (got == len);
    }
    fclose(f);
    return ok;
}

void FileEditorScreen::adopt_window(uint64_t off, std::string&& bytes) {
    m_win_off      = off;
    m_win_orig_len = bytes.size();
    m_doc          = TW::parse(bytes, off + bytes.size() >= m_file_size);
    m_dirty        = false;
}

bool FileEditorScreen::load_forward(uint64_t off) {
    const uint64_t remaining = m_file_size - off;
    const size_t   want      = (size_t)std::min<uint64_t>(remaining, kWindowBytes);
    std::string bytes;
    if (!read_range(off, want, bytes)) { m_error = Lang::t("errors.read_error"); return false; }

    if (off + want < m_file_size) {          // not the end: cut back to whole lines
        const size_t keep = TW::complete_prefix_len(bytes.data(), bytes.size());
        if (keep == 0) { m_error = Lang::t("editor.line_too_long_file"); return false; }
        bytes.resize(keep);
    }
    if (TW::has_nul(bytes.data(), bytes.size())) { m_error = Lang::t("editor.binary"); return false; }
    adopt_window(off, std::move(bytes));
    return true;
}

bool FileEditorScreen::load_backward(uint64_t end) {
    const uint64_t start = end > kWindowBytes ? end - kWindowBytes : 0;
    std::string bytes;
    if (!read_range(start, (size_t)(end - start), bytes)) {
        m_error = Lang::t("errors.read_error");
        return false;
    }
    uint64_t off = start;
    if (start > 0) {
        // We began mid-line; drop the partial first line so the window starts on a
        // line boundary.
        const size_t skip = TW::first_line_start(bytes.data(), bytes.size());
        if (skip >= bytes.size()) { m_error = Lang::t("editor.line_too_long_file"); return false; }
        bytes.erase(0, skip);
        off += skip;
    }
    if (TW::has_nul(bytes.data(), bytes.size())) { m_error = Lang::t("editor.binary"); return false; }
    adopt_window(off, std::move(bytes));
    return true;
}

void FileEditorScreen::on_enter() {
    m_file_size = Fs::file_size(m_path);
    if (m_file_size > kMaxFileBytes) {
        std::string msg = Lang::t("editor.too_large");
        const auto p = msg.find("{limit}");
        if (p != std::string::npos) msg.replace(p, 7, Fs::format_size(kMaxFileBytes));
        m_error = msg;
        return;
    }
    load_forward(0);
}

// ─── Cursor / window movement ─────────────────────────────────────────────────

void FileEditorScreen::clamp_view() {
    const int n = (int)m_doc.lines.size();
    m_cursor = std::max(0, std::min(m_cursor, n - 1));
    if (m_cursor < m_top) m_top = m_cursor;
    if (m_cursor >= m_top + m_visible_rows) m_top = m_cursor - m_visible_rows + 1;
    m_top = std::max(0, m_top);
}

void FileEditorScreen::move_cursor(int delta) {
    const int n      = (int)m_doc.lines.size();
    const int target = m_cursor + delta;
    if (target >= 0 && target < n) { m_cursor = target; clamp_view(); return; }
    if (target < 0) {
        if (m_win_off == 0) m_cursor = 0;
        else { request_shift(-1); return; }
    } else {
        if (window_reaches_eof()) m_cursor = n - 1;
        else { request_shift(+1); return; }
    }
    clamp_view();
}

void FileEditorScreen::request_shift(int dir) {
    if (!m_dirty) { do_shift(dir); return; }
    // Unsaved edits live in this window; leaving it means writing them out first.
    m_pending     = Pending::SaveThenShift;
    m_pending_dir = dir;
    Modal::show({ Lang::t("editor.shift_title"), Lang::t("editor.shift_body"),
                  Modal::Kind::Warning, Lang::t("editor.save"), Lang::t("modal.cancel") });
}

void FileEditorScreen::do_shift(int dir) {
    const bool ok = (dir > 0) ? load_forward(win_end()) : load_backward(m_win_off);
    if (!ok) return;
    m_cursor = (dir > 0) ? 0 : (int)m_doc.lines.size() - 1;
    m_top    = (dir > 0) ? 0 : std::max(0, m_cursor - m_visible_rows + 1);
    clamp_view();
}

// ─── Editing ──────────────────────────────────────────────────────────────────

void FileEditorScreen::toast(const std::string& s) {
    m_toast    = s;
    m_toast_at = SDL_GetTicks();
}

void FileEditorScreen::edit_line() {
    std::string& line = m_doc.lines[m_cursor];
    const bool   cr   = !line.empty() && line.back() == '\r';
    const std::string cur = cr ? line.substr(0, line.size() - 1) : line;

    if (cur.size() > kMaxEditLine) {
        std::string msg = Lang::t("editor.line_too_long");
        const auto p = msg.find("{n}");
        if (p != std::string::npos) msg.replace(p, 3, std::to_string(kMaxEditLine));
        toast(msg);
        return;
    }

    Keyboard::Options o;
    o.header        = Lang::t("editor.line_header");
    o.initial_text  = cur;
    o.ok_text       = Lang::t("editor.apply");
    o.max_length    = (int)kMaxEditLine;
    o.allow_empty   = true;
    std::string out;
    if (!Keyboard::get_text(o, out)) return;
    if (cr) out += '\r';
    if (out == line) return;

    if (TW::byte_size(m_doc) - line.size() + out.size() > kDocCapBytes) {
        toast(Lang::t("editor.buffer_full"));
        return;
    }
    line    = std::move(out);
    m_dirty = true;
}

void FileEditorScreen::insert_line_below() {
    if (TW::byte_size(m_doc) + 2 > kDocCapBytes) { toast(Lang::t("editor.buffer_full")); return; }

    // Match the neighbour's line ending so a CRLF file stays CRLF throughout.
    const bool cr = !m_doc.lines[m_cursor].empty() && m_doc.lines[m_cursor].back() == '\r';
    const int  at = m_cursor + 1;
    m_doc.lines.insert(m_doc.lines.begin() + at, cr ? std::string("\r") : std::string());
    m_cursor = at;
    clamp_view();

    // Open the keyboard straight away; cancelling takes the empty line back out.
    std::string& line = m_doc.lines[m_cursor];
    Keyboard::Options o;
    o.header      = Lang::t("editor.new_line_header");
    o.ok_text     = Lang::t("editor.apply");
    o.max_length  = (int)kMaxEditLine;
    o.allow_empty = true;
    std::string out;
    if (!Keyboard::get_text(o, out)) {
        m_doc.lines.erase(m_doc.lines.begin() + at);
        m_cursor = at - 1;
        clamp_view();
        return;
    }
    if (cr) out += '\r';
    if (TW::byte_size(m_doc) - line.size() + out.size() > kDocCapBytes) {
        m_doc.lines.erase(m_doc.lines.begin() + at);
        m_cursor = at - 1;
        clamp_view();
        toast(Lang::t("editor.buffer_full"));
        return;
    }
    line    = std::move(out);
    m_dirty = true;
}

// A plain confirm (not the hold-to-confirm Danger kind): a deleted line is only
// lost if the user then saves, and Discard on exit still guards the whole edit.
void FileEditorScreen::confirm_delete_line() {
    std::string shown = display_text(m_doc.lines[m_cursor]);
    if (shown.size() > 80) {
        shown.resize(80);
        while (!shown.empty() && ((unsigned char)shown.back() & 0xC0) == 0x80) shown.pop_back();
        if (!shown.empty() && ((unsigned char)shown.back() & 0x80)) shown.pop_back();
        shown += "…";
    }
    m_pending = Pending::DeleteLine;
    Modal::show({ Lang::t("editor.delete_line_title"),
                  Lang::t("editor.delete_line_body") + "\n\n" + shown,
                  Modal::Kind::Warning, Lang::t("editor.hint_delete"), Lang::t("modal.cancel") });
}

void FileEditorScreen::delete_line() {
    if (m_doc.lines.size() == 1) {
        // Last line standing: leave one empty line. At EOF that serialises to an
        // empty file; mid-file it stays an empty line so the window keeps its shape.
        m_doc.lines[0].clear();
        m_doc.eof_no_nl = window_reaches_eof();
    } else {
        const bool was_last = (m_cursor == (int)m_doc.lines.size() - 1);
        m_doc.lines.erase(m_doc.lines.begin() + m_cursor);
        // Removing the unterminated final line: the line before it keeps its
        // newline rather than becoming the new unterminated one.
        if (was_last) m_doc.eof_no_nl = false;
        if (m_cursor >= (int)m_doc.lines.size()) m_cursor = (int)m_doc.lines.size() - 1;
    }
    m_dirty = true;
    clamp_view();
}

// ─── Save ─────────────────────────────────────────────────────────────────────

bool FileEditorScreen::save() {
    const std::string win = TW::serialize(m_doc);
    const std::string tmp = m_path + ".gnxtmp";
    const std::string bak = m_path + ".gnxbak";
    const uint64_t tail_from = win_end();
    const uint64_t expected  = m_file_size - m_win_orig_len + win.size();

    auto fail = [&](const char* why) {
        SDL_Log("FileEditor - save failed (%s): %s", why, m_path.c_str());
        if (Fs::exists(tmp)) Services::SaveWrite::remove_file(tmp);
        Modal::show({ Lang::t("editor.save_failed_title"), Lang::t("editor.save_failed"),
                      Modal::Kind::Info, Lang::t("modal.ok"), "" });
        return false;
    };

    FILE* in  = fopen(m_path.c_str(), "rb");
    FILE* out = fopen(tmp.c_str(), "wb");
    bool ok = in && out && copy_range(in, out, 0, m_win_off);
    if (ok && !win.empty()) ok = (fwrite(win.data(), 1, win.size(), out) == win.size());
    if (ok) ok = copy_range(in, out, tail_from, m_file_size);
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    if (!ok)                               return fail("write");
    if (Fs::file_size(tmp) != expected)    return fail("size mismatch");

    // Swap in. The original is moved aside rather than deleted so a failure at any
    // step can put it back; FAT will not rename over an existing file.
    if (Fs::exists(bak)) Services::SaveWrite::remove_file(bak);
    if (!Services::SaveWrite::rename(m_path, bak))       return fail("move original aside");
    if (!Services::SaveWrite::rename(tmp, m_path)) {
        Services::SaveWrite::rename(bak, m_path);        // put the original back
        return fail("move new file into place");
    }
    Services::SaveWrite::remove_file(bak);
    Services::SaveWrite::after_write(m_path);

    m_file_size    = expected;
    m_win_orig_len = win.size();
    m_dirty        = false;
    toast(Lang::t("editor.saved"));
    return true;
}

// ─── Update / modal ───────────────────────────────────────────────────────────

void FileEditorScreen::on_modal_result(int result) {
    const bool yes = (static_cast<Modal::Result>(result) == Modal::Result::Confirmed);
    const Pending p = m_pending;
    m_pending = Pending::None;
    if (!yes) return;

    if (p == Pending::Discard) {
        m_exit_requested = true;
    } else if (p == Pending::DeleteLine) {
        delete_line();
    } else if (p == Pending::SaveThenShift) {
        if (save()) do_shift(m_pending_dir);
    }
}

std::unique_ptr<Screen> FileEditorScreen::update(bool& pop) {
    pop = false;

    if (m_exit_requested) { pop = true; return nullptr; }

    if (Input::pressed(Input::Button::B)) {
        if (!m_error.empty() || !m_dirty) { pop = true; return nullptr; }
        m_pending = Pending::Discard;
        Modal::show({ Lang::t("editor.discard_title"), Lang::t("editor.discard_body"),
                      Modal::Kind::Danger, Lang::t("editor.discard"), Lang::t("editor.keep") });
        return nullptr;
    }
    if (!m_error.empty()) return nullptr;

    if (Input::repeat(Input::Button::DDown)) move_cursor(+1);
    if (Input::repeat(Input::Button::DUp))   move_cursor(-1);
    if (Input::repeat(Input::Button::R))     move_cursor(+std::max(1, m_visible_rows - 1));
    if (Input::repeat(Input::Button::L))     move_cursor(-std::max(1, m_visible_rows - 1));

    if (Input::pressed(Input::Button::A))    edit_line();
    if (Input::pressed(Input::Button::Y))    insert_line_below();
    if (Input::pressed(Input::Button::X))    confirm_delete_line();
    if (Input::pressed(Input::Button::Plus) && m_dirty) save();
    return nullptr;
}

// ─── Draw ─────────────────────────────────────────────────────────────────────

void FileEditorScreen::draw() {
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    // Header: file name (with a marker while there are unsaved edits) + position.
    Widgets::draw_text(x + Layout::PAD_MD, y + Layout::PAD_SM,
                       std::string(m_dirty ? "* " : "") + Fs::basename(m_path),
                       Font::Size::Medium, Font::Weight::Bold,
                       m_dirty ? Theme::Token::AccentWarn : Theme::Token::FgPrimary,
                       w - 280);
    {
        char pos[64];
        const int pct = m_file_size > 0 ? (int)((m_win_off * 100) / m_file_size) : 0;
        std::snprintf(pos, sizeof(pos), "%d%%  ·  %s", pct,
                      Fs::format_size(m_file_size).c_str());
        SDL_Color c = Theme::get(Theme::Token::FgSecondary);
        SDL_Surface* s = Font::render(pos, Font::Size::Small, Font::Weight::Regular, c);
        if (s) { Renderer::blit_right(s, x, y + Layout::PAD_SM, w - Layout::PAD_MD, 24); SDL_FreeSurface(s); }
    }
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(x, y + 34, w);

    const int content_y = y + 42;
    const int content_h = h - 42 - 34;

    if (!m_error.empty()) {
        Widgets::draw_text(x + Layout::PAD_MD, content_y, m_error,
                           Font::Size::Body, Font::Weight::Regular,
                           Theme::Token::AccentDanger, w - Layout::PAD_MD * 2);
        Widgets::draw_button_legend(x, y + h - 30, w,
                                    { { "B", Lang::t("hints.back") } });
        return;
    }

    const int row_h = static_cast<int>(Font::Size::Small) + 6;
    m_visible_rows  = std::max(1, content_h / row_h);
    clamp_view();

    const int n = (int)m_doc.lines.size();
    for (int i = 0; i < m_visible_rows && m_top + i < n; ++i) {
        const int idx = m_top + i;
        const int ry  = content_y + i * row_h;
        if (idx == m_cursor) {
            Theme::apply(r, Theme::Token::BgElevated);
            Renderer::fill_rect(x, ry, w, row_h);
            Theme::apply(r, Theme::Token::Accent);
            Renderer::fill_rect(x, ry, 3, row_h);
        }
        const std::string text = display_text(m_doc.lines[idx]);
        if (!text.empty())
            Widgets::draw_text(x + Layout::PAD_MD, ry + 2, text,
                               Font::Size::Small, Font::Weight::Regular,
                               Theme::Token::FgPrimary, w - Layout::PAD_MD * 2);
    }

    // Transient status (saved / too long / buffer full).
    if (!m_toast.empty() && SDL_GetTicks() - m_toast_at < 3000) {
        Widgets::draw_text(x + Layout::PAD_MD, y + h - 54, m_toast,
                           Font::Size::Small, Font::Weight::Bold, Theme::Token::Accent,
                           w - Layout::PAD_MD * 2);
    }

    std::vector<Widgets::ButtonHint> hints = {
        { "D-Pad", Lang::t("editor.hint_move") },
        { "A", Lang::t("editor.hint_edit") },
        { "Y", Lang::t("editor.hint_insert") },
        { "X", Lang::t("editor.hint_delete") },
        { "+", Lang::t("editor.save") },
        { "L/R", Lang::t("editor.hint_page") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 30, w, hints);
}
