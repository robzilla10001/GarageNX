#pragma once
// source/screens/file_editor.hpp
// Plain-text editor for txt/ini-style files. Deliberately basic: line-oriented
// (the system keyboard edits one line at a time), no selection, no undo.
//
// MEMORY: like the viewer, the editor never loads a whole file. It holds one
// WINDOW of up to kWindowBytes, aligned to line boundaries (see
// services/text_window.hpp), and moves the window as the cursor crosses its edge.
//
// SAVING: a window edit changes the file's length, so a save streams
//   [0, window start) + edited window + [window end, EOF)
// into a temp file next to the original and swaps it in. The original is only
// replaced once the temp file is complete and the right size, so a failed save
// (SD full, I/O error) leaves the file exactly as it was. Unsaved edits are held
// in the window; moving to another part of a large file saves first (with a
// prompt), so at most one window is ever dirty.

#include "screens/screen.hpp"
#include "services/text_window.hpp"
#include <cstdint>
#include <string>

class FileEditorScreen : public Screen {
public:
    explicit FileEditorScreen(std::string path);

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

    // Resident-window size. 256 KB matches the viewer's budget with headroom for
    // the line-vector overhead of many short lines.
    static constexpr size_t   kWindowBytes  = 256 * 1024;
    // The edited window may grow past kWindowBytes (inserts, longer lines) up to
    // this before further growth is refused.
    static constexpr size_t   kDocCapBytes  = kWindowBytes + 64 * 1024;
    // A save rewrites the whole file, so very large files are refused rather than
    // freezing the console for a long write. The viewer still opens them.
    static constexpr uint64_t kMaxFileBytes = 32ull * 1024 * 1024;
    // The system keyboard returns text through a fixed buffer; longer lines cannot
    // round-trip through it, so they are shown but not editable.
    static constexpr size_t   kMaxEditLine  = 500;

private:
    enum class Pending { None, Discard, SaveThenShift, DeleteLine };

    std::string m_path;
    uint64_t    m_file_size = 0;

    // The window: [m_win_off, m_win_off + m_win_orig_len) on disk as last loaded
    // or saved, held in memory as m_doc.
    uint64_t                  m_win_off      = 0;
    uint64_t                  m_win_orig_len = 0;
    Services::TextWindow::Doc m_doc;
    bool                      m_dirty = false;

    int m_cursor       = 0;    // line index within the window
    int m_top          = 0;    // first visible line
    int m_visible_rows = 10;   // updated by draw()

    std::string m_error;       // non-empty: the file cannot be edited (shown instead)
    std::string m_toast;
    uint32_t    m_toast_at = 0;

    Pending m_pending     = Pending::None;
    int     m_pending_dir = 0;       // +1 / -1 for SaveThenShift
    bool    m_exit_requested = false;

    uint64_t win_end() const { return m_win_off + m_win_orig_len; }
    bool     window_reaches_eof() const { return win_end() >= m_file_size; }

    bool load_forward(uint64_t off);
    bool load_backward(uint64_t end);
    bool read_range(uint64_t off, size_t len, std::string& out) const;
    void adopt_window(uint64_t off, std::string&& bytes);

    void move_cursor(int delta);
    void request_shift(int dir);
    void do_shift(int dir);
    void clamp_view();

    void edit_line();
    void insert_line_below();
    void confirm_delete_line();   // asks first; delete_line() does the removal
    void delete_line();

    bool save();
    void toast(const std::string& s);
};
