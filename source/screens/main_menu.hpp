#pragma once
// source/screens/main_menu.hpp

#include "screens/screen.hpp"
#include "ui/widgets.hpp"
#include <memory>

class MainMenuScreen : public Screen {
public:
    MainMenuScreen();

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    Widgets::List m_list;

    void rebuild_items();
};
