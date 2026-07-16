#pragma once
// source/ui/title_bar.hpp
// Persistent top bar: app name, version, firmware version, SDK version.
// Data is static placeholder in Milestone 1; libnx queries added in Milestone 3.

#include <string>

namespace TitleBar {

struct Info {
    std::string app_version = APP_VERSION;   // from compile-time define
    std::string fw_version  = "?.?.?";       // populated in Milestone 3
    std::string sdk_version = "?.?.?";       // populated in Milestone 3
};

/// Update the displayed info (call once on init, again when data is available).
void set(const Info& info);

/// Draw the title bar. Call every frame inside begin_frame/end_frame.
void draw();

} // namespace TitleBar
