#pragma once
// source/screens/activity_log.hpp
//
// Per-title play statistics: what has been played, for how long, how often.
//
// NOT the aggregate summary shown on the System Information screen - those
// totals are documented in core/activity.cpp as unreliable and report N/A.
// Per-title figures come from a different query, addressed directly by
// application id.
//
// Gathering blocks briefly (title-name resolution + one pdm query per installed
// title), so it runs on the SECOND update with a progress frame, the same shape
// as the save backup sweep.

#include "screens/screen.hpp"
#include "core/activity.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

class ActivityLogScreen : public Screen {
public:
    ActivityLogScreen() = default;

    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    int  m_phase = 0;          // 0 = let a frame land, 1 = gather, 2 = ready
    std::vector<Core::Activity::TitlePlay> m_rows;
    Widgets::List m_list;

    void build_rows();
};
