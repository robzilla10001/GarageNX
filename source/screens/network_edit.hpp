#pragma once
// source/screens/network_edit.hpp
//
// On-device editor for a single Browse Network connection — so connections are
// added and changed on the console, never by hand-editing config.json. Fields are
// entered through the system keyboard; the result is written to
// Config::network.shares and persisted with the non-destructive Config::save().
//
// The PASSWORD is deliberately NOT a field here: the model is prompt-per-session,
// so a secret never lands in config.json. It is entered at connect time (masked)
// by the chooser. This editor captures only the durable connection definition.

#include "screens/screen.hpp"
#include "config/config.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

class NetworkEditScreen : public Screen {
public:
    NetworkEditScreen();                       // add a new connection
    explicit NetworkEditScreen(int edit_index); // edit shares[edit_index]

    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

private:
    // What a selected row does. A parallel vector keeps rows and actions in step
    // even though the row set changes (username/domain show for SMB only, Delete
    // shows only when editing) — the same pattern file_browser uses for its
    // context menu.
    enum class Action {
        Name, Protocol, Host, Share, Path, Port, Username, Domain, Save, Delete
    };

    int               m_edit_index = -1;   // -1 = adding a new connection
    Config::NetShare  m_draft;
    Widgets::List     m_list;
    std::vector<Action> m_row_actions;

    bool m_confirm_delete = false;  // a delete confirmation modal is outstanding
    bool m_pop_requested  = false;  // set from on_modal_result; acted on in update()

    void rebuild();
    void activate(Action a);
    std::string validate() const;   // "" if ok, else a reason
    void commit();                  // write draft to config + save
    void do_delete();               // remove + save
};
