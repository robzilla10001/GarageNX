#pragma once
// source/screens/file_viewer.hpp
// Paged file viewer — text and hex modes. Loads the file in chunks so large
// files never blow the memory budget or hard-lock the device.
//
// Text mode: reads UTF-8, wraps to the view width, pages by screenful.
// Hex mode:  classic offset | bytes | ASCII dump.
// R3 toggles between modes. L/R page. Files open in the mode chosen by caller.

#include "screens/screen.hpp"
#include <string>
#include <vector>
#include <cstdint>

class FileViewerScreen : public Screen {
public:
    enum class Mode { Text, Hex };

    // path: full path to the file. start_mode: initial view mode.
    FileViewerScreen(std::string path, Mode start_mode);
    ~FileViewerScreen() override;

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    std::string m_path;
    Mode        m_mode;
    uint64_t    m_file_size = 0;
    FILE*       m_fp = nullptr;
    bool        m_error = false;

    // ── Paging ────────────────────────────────────────────────────────────────
    // We keep at most a couple of chunks resident. A "page" is a screenful.
    uint64_t m_view_offset = 0;   // byte offset of the top of the current view
    int      m_visible_rows = 0;  // computed from content height / row height

    // Cached bytes for the current view window
    std::vector<uint8_t> m_buffer;
    uint64_t             m_buffer_offset = 0;   // file offset of m_buffer[0]

    // Text mode: does the file look like UTF-8 text?
    bool m_looks_binary = false;

    // Ensure m_buffer covers [offset, offset+len). Reads from disk if needed.
    void ensure_loaded(uint64_t offset, size_t len);

    void draw_text_mode(int x, int y, int w, int h);
    void draw_hex_mode(int x, int y, int w, int h);

    void page_down();
    void page_up();

    // Bytes shown per hex row and text handling
    static constexpr int HEX_BYTES_PER_ROW = 16;
    static constexpr size_t CHUNK = 64 * 1024;  // 64 KB resident window
};
