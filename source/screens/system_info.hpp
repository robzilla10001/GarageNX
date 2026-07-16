#pragma once
// source/screens/system_info.hpp
// Scrollable multi-section hardware/firmware/battery/activity readout.
// Sensitive identifiers are masked until the user reveals them (Y button).

#include "screens/screen.hpp"
#include <string>
#include <vector>

class SystemInfoScreen : public Screen {
public:
    SystemInfoScreen();

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    // A single displayable row: either a section header or a label/value pair.
    struct Row {
        bool        is_header = false;
        std::string label;      // resolved text (already localized)
        std::string value;      // resolved text; empty for headers
        bool        sensitive = false;
    };

    std::vector<Row> m_rows;
    int  m_scroll = 0;          // index of the first visible row
    bool m_reveal = false;      // reveal sensitive fields

    void rebuild();
    void add_header(const std::string& key);
    void add_row(const std::string& label_key, const std::string& value,
                 bool sensitive = false);
};
