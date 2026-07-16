// source/screens/file_viewer.cpp

#include "screens/file_viewer.hpp"
#include "core/fs.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cctype>
#include <algorithm>

// Monospace metrics: we render hex/text with Inter, but for alignment in hex
// mode we advance by a fixed glyph width measured from the font at Body size.

FileViewerScreen::FileViewerScreen(std::string path, Mode start_mode)
    : m_path(std::move(path)), m_mode(start_mode) {}

FileViewerScreen::~FileViewerScreen() {
    if (m_fp) fclose(m_fp);
}

void FileViewerScreen::on_enter() {
    m_file_size = Fs::file_size(m_path);
    m_fp = fopen(m_path.c_str(), "rb");
    if (!m_fp) {
        m_error = true;
        SDL_Log("FileViewer — cannot open %s", m_path.c_str());
        return;
    }

    // Sniff the first chunk to decide if the file looks binary (for text mode
    // we still display it, but this lets us warn / default sensibly).
    ensure_loaded(0, std::min<size_t>(CHUNK, (size_t)m_file_size));
    size_t sniff = std::min<size_t>(m_buffer.size(), 1024);
    int control_chars = 0;
    for (size_t i = 0; i < sniff; ++i) {
        uint8_t b = m_buffer[i];
        // Count non-text control bytes (excluding tab, newline, carriage return)
        if (b < 0x09 || (b > 0x0D && b < 0x20)) control_chars++;
        if (b == 0x00) { control_chars += 4; }  // nulls strongly imply binary
    }
    m_looks_binary = (sniff > 0) && (control_chars * 100 / (int)sniff > 5);

    m_view_offset = 0;
}

void FileViewerScreen::ensure_loaded(uint64_t offset, size_t len) {
    if (m_error || !m_fp) return;

    // Already covered?
    if (offset >= m_buffer_offset &&
        offset + len <= m_buffer_offset + m_buffer.size()) {
        return;
    }

    // Load a CHUNK-aligned window starting at `offset`.
    uint64_t load_start = offset;
    size_t   load_len   = std::max(len, CHUNK);
    if (load_start + load_len > m_file_size)
        load_len = (size_t)(m_file_size - load_start);

    m_buffer.resize(load_len);
    if (fseek(m_fp, (long)load_start, SEEK_SET) != 0) {
        m_error = true;
        return;
    }
    size_t got = fread(m_buffer.data(), 1, load_len, m_fp);
    m_buffer.resize(got);
    m_buffer_offset = load_start;
}

std::unique_ptr<Screen> FileViewerScreen::update(bool& pop) {
    pop = false;

    if (Input::pressed(Input::Button::B)) {
        pop = true;
        return nullptr;
    }

    // Toggle text/hex with R3 (right stick click)
    if (Input::pressed(Input::Button::RStickClick)) {
        m_mode = (m_mode == Mode::Text) ? Mode::Hex : Mode::Text;
    }

    // Paging
    if (Input::repeat(Input::Button::R) || Input::repeat(Input::Button::DDown)) {
        page_down();
    }
    if (Input::repeat(Input::Button::L) || Input::repeat(Input::Button::DUp)) {
        page_up();
    }

    return nullptr;
}

void FileViewerScreen::page_down() {
    // Advance by one screenful. Row size differs per mode; compute conservatively.
    uint64_t step;
    if (m_mode == Mode::Hex) {
        step = (uint64_t)m_visible_rows * HEX_BYTES_PER_ROW;
    } else {
        // Text: approximate bytes per screen. draw_text_mode refines the exact
        // wrapping; here we just move by an estimate and clamp.
        step = (uint64_t)m_visible_rows * 80;
    }
    if (step == 0) step = HEX_BYTES_PER_ROW;

    uint64_t max_off = (m_file_size > step) ? (m_file_size - 1) : 0;
    m_view_offset = std::min(m_view_offset + step, max_off);
}

void FileViewerScreen::page_up() {
    uint64_t step;
    if (m_mode == Mode::Hex) {
        step = (uint64_t)m_visible_rows * HEX_BYTES_PER_ROW;
    } else {
        step = (uint64_t)m_visible_rows * 80;
    }
    if (step == 0) step = HEX_BYTES_PER_ROW;

    m_view_offset = (m_view_offset > step) ? (m_view_offset - step) : 0;
}

void FileViewerScreen::draw() {
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    // Header: filename + mode + position
    std::string mode_label = (m_mode == Mode::Text)
        ? Lang::t("file_browser.viewer_text_mode")
        : Lang::t("file_browser.viewer_hex_mode");

    Widgets::draw_text(x + Layout::PAD_MD, y + Layout::PAD_SM,
                       Fs::basename(m_path),
                       Font::Size::Medium, Font::Weight::Bold,
                       Theme::Token::FgPrimary,
                       w - 260);

    // Mode + offset on the right
    char pos[64];
    int pct = (m_file_size > 0)
        ? (int)((m_view_offset * 100) / m_file_size) : 0;
    snprintf(pos, sizeof(pos), "%s  ·  %d%%  ·  %s",
             mode_label.c_str(), pct, Fs::format_size(m_file_size).c_str());
    {
        SDL_Color c = Theme::get(Theme::Token::FgSecondary);
        SDL_Surface* s = Font::render(pos, Font::Size::Small, Font::Weight::Regular, c);
        if (s) { Renderer::blit_right(s, x, y + Layout::PAD_SM, w - Layout::PAD_MD, 24); SDL_FreeSurface(s); }
    }

    // Divider under header
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(x, y + 34, w);

    int content_y = y + 42;
    int content_h = h - 42 - 34;   // leave room for button legend

    if (m_error) {
        Widgets::draw_text(x + Layout::PAD_MD, content_y,
                           Lang::t("errors.read_error"),
                           Font::Size::Body, Font::Weight::Regular,
                           Theme::Token::AccentDanger);
    } else if (m_mode == Mode::Hex) {
        draw_hex_mode(x + Layout::PAD_MD, content_y, w - Layout::PAD_MD * 2, content_h);
    } else {
        draw_text_mode(x + Layout::PAD_MD, content_y, w - Layout::PAD_MD * 2, content_h);
    }

    // Button legend
    std::vector<Widgets::ButtonHint> hints = {
        { "R3", m_mode == Mode::Text ? Lang::t("file_browser.viewer_hex_mode")
                                     : Lang::t("file_browser.viewer_text_mode") },
        { "L/R", Lang::t("hints.page_next") },
        { "B",   Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 30, w, hints);
}

// ─── Text mode ────────────────────────────────────────────────────────────────

void FileViewerScreen::draw_text_mode(int x, int y, int w, int h) {
    const int row_h = static_cast<int>(Font::Size::Small) + 6;
    m_visible_rows = h / row_h;

    // Load the window we need. Text can wrap, so we load a generous chunk from
    // the current offset and render line by line until we run out of vertical space.
    ensure_loaded(m_view_offset, CHUNK);

    if (m_buffer.empty()) return;

    // Determine the slice of the buffer that corresponds to the view offset.
    size_t buf_start = (size_t)(m_view_offset - m_buffer_offset);
    if (buf_start >= m_buffer.size()) return;

    // Rough character budget per line based on average glyph width.
    // Inter isn't monospace, so we wrap on width by measuring incrementally.
    int cur_y = y;
    size_t i = buf_start;

    std::string line;
    auto flush_line = [&](void) {
        if (cur_y + row_h > y + h) return;  // out of vertical space
        SDL_Color c = Theme::get(Theme::Token::FgPrimary);
        SDL_Surface* s = Font::render(line.empty() ? " " : line,
                                      Font::Size::Small, Font::Weight::Regular, c);
        if (s) { Renderer::blit(s, x, cur_y); SDL_FreeSurface(s); }
        cur_y += row_h;
        line.clear();
    };

    while (i < m_buffer.size() && cur_y + row_h <= y + h) {
        uint8_t b = m_buffer[i];

        if (b == '\n') { flush_line(); i++; continue; }
        if (b == '\r') { i++; continue; }
        if (b == '\t') { line += "    "; i++; continue; }

        // Non-printable in a "text" file: show a middle dot rather than garbage.
        if (b < 0x20) { line += '.'; i++; continue; }

        line += (char)b;
        i++;

        // Wrap when the line gets close to the view width. Measure occasionally
        // (every few chars) to avoid a TTF_SizeText call per character.
        if ((line.size() % 8) == 0) {
            int tw = 0, th = 0;
            TTF_Font* f = Font::get(Font::Size::Small, Font::Weight::Regular);
            if (f) TTF_SizeUTF8(f, line.c_str(), &tw, &th);
            if (tw >= w) flush_line();
        }
    }
    // Flush any trailing partial line
    if (!line.empty()) flush_line();
}

// ─── Hex mode ─────────────────────────────────────────────────────────────────

void FileViewerScreen::draw_hex_mode(int x, int y, int w, int h) {
    const int row_h = static_cast<int>(Font::Size::Small) + 4;
    m_visible_rows = h / row_h;

    uint64_t need = (uint64_t)m_visible_rows * HEX_BYTES_PER_ROW;
    ensure_loaded(m_view_offset, (size_t)std::min<uint64_t>(need, CHUNK));

    if (m_buffer.empty()) return;

    // Build each row as a single fixed-width string so the monospace font keeps
    // every column aligned into a uniform block:
    //   "OFFSET   HH HH HH HH HH HH HH HH  HH HH HH HH HH HH HH HH  |ASCII|"
    // Rendering the whole row in one mono surface guarantees perfect alignment
    // regardless of byte values.
    int cur_y = y;
    uint64_t off = m_view_offset;

    for (int row = 0; row < m_visible_rows; ++row) {
        if (off >= m_file_size) break;
        if (off < m_buffer_offset ||
            off >= m_buffer_offset + m_buffer.size()) {
            ensure_loaded(off, (size_t)need);
            if (m_buffer.empty()) break;
        }

        size_t base = (size_t)(off - m_buffer_offset);

        // Offset column
        char line[128];
        int pos = snprintf(line, sizeof(line), "%08llX  ", (unsigned long long)off);

        // Hex bytes with a mid-row gap after 8
        char ascii[HEX_BYTES_PER_ROW + 1];
        for (int c = 0; c < HEX_BYTES_PER_ROW; ++c) {
            size_t idx = base + c;
            if (idx < m_buffer.size()) {
                uint8_t bv = m_buffer[idx];
                pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", bv);
                ascii[c] = (bv >= 0x20 && bv < 0x7F) ? (char)bv : '.';
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "   ");
                ascii[c] = ' ';
            }
            if (c == 7) pos += snprintf(line + pos, sizeof(line) - pos, " ");
        }
        ascii[HEX_BYTES_PER_ROW] = '\0';

        // ASCII sidebar
        snprintf(line + pos, sizeof(line) - pos, " |%s|", ascii);

        // Render the full row in the mono font, primary color.
        SDL_Color c = Theme::get(Theme::Token::FgPrimary);
        SDL_Surface* s = Font::render(line, Font::Size::Small,
                                      Font::Weight::Regular, c,
                                      Font::Family::Mono);
        if (s) { Renderer::blit(s, x, cur_y); SDL_FreeSurface(s); }

        cur_y += row_h;
        off += HEX_BYTES_PER_ROW;
    }
}
