#pragma once
// source/screens/tickets.hpp
//
// The common tickets installed on this console.
//
// A ticket carries the content key for a titlekey-crypto title — most eShop
// games and updates. Listing them answers "what does this console actually hold
// keys for", which is the question the menu entry implies.
//
// Read-only. Deleting a ticket can make an installed title unlaunchable, and this
// project's rule is that a destructive action arrives with its own confirmation
// and rollback story rather than riding in on a list screen. If ticket deletion is
// ever wanted it gets the Save Manager treatment, not a button here.

#include "screens/screen.hpp"
#include "core/es.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

class TicketsScreen : public Screen {
public:
    TicketsScreen() = default;

    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    int  m_phase = 0;   // 0 = let a frame land, 1 = gather, 2 = ready
    std::vector<Core::Es::TicketRef> m_tickets;
    Widgets::List m_list;

    void build_rows();
};
