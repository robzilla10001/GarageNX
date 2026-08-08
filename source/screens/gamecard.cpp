// source/screens/gamecard.cpp

#include "screens/gamecard.hpp"

#include "core/keys.hpp"
#include "core/nsp_stream.hpp"
#include "install/stream_driver.hpp"
#include "core/fs.hpp"
#include "lang/localization.hpp"
#include "ui/font.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/status_bar.hpp"
#include "ui/theme.hpp"
#include "ui/title_bar.hpp"
#include "ui/widgets.hpp"

#include <SDL2/SDL.h>

#include <cstdio>

GamecardScreen::~GamecardScreen() {
#ifdef PLATFORM_SWITCH
    if (m_inst_thread_active) {
        m_inst_progress.cancel.store(true);
        threadWaitForExit(&m_inst_thread);
        threadClose(&m_inst_thread);
        m_inst_thread_active = false;
    }
    if (m_dump_thread_active) {
        // Ask the dump to stop, then WAIT. Cancelling without waiting would return
        // from this destructor with the worker still running against members that
        // are about to be destroyed — the cancel makes the wait short, it does not
        // replace it.
        m_progress.cancel.store(true);
        threadWaitForExit(&m_dump_thread);
        threadClose(&m_dump_thread);
        m_dump_thread_active = false;
    }
#endif
}

void GamecardScreen::build_rows() {
    bool ok = false;
    m_groups = Core::Ncm::group_by_application(Core::Ncm::list_gamecard(&ok));

    std::vector<Widgets::ListItem> items;
    items.reserve(m_groups.size());
    for (const auto& g : m_groups) {
        Widgets::ListItem it;
        // Title::name is resolved lazily from the Control NCA and is empty until
        // then; the id is the honest fallback rather than a blank row.
        char idhex[24];
        std::snprintf(idhex, sizeof(idhex), "%016llX",
                      (unsigned long long)g.app.program_id);
        it.label = g.app.name.empty() ? std::string("Title ") + idhex : g.app.name;

        // Size of the base application. Updates and DLC on the card are separate
        // Titles in the group and are not dumped by this row.
        it.meta = Fs::format_size(g.app.size_bytes);
        items.push_back(std::move(it));
    }
    m_list.set_items(std::move(items));
}

void GamecardScreen::start_dump(int idx) {
    if (idx < 0 || idx >= (int)m_groups.size()) return;

    if (!Core::Keys::available()) Core::Keys::load();
    if (!Core::Keys::available()) {
        Modal::Options o;
        o.kind          = Modal::Kind::Info;
        o.title         = Lang::t("gamecard.dump_failed_title");
        o.body          = Core::Keys::requirement_message();
        o.confirm_label = Lang::t("common.ok");
        Modal::show(o);
        return;
    }

    // Run on a BACKGROUND THREAD and poll, copying TitleDetailScreen::start_dump()
    // exactly. The first version of this screen called dump_title_to_nsp()
    // synchronously on the main thread with a comment claiming it "draws its own
    // frames" — it does not, and cannot: unlike the save sweep, the dump API takes
    // no progress callback. It only publishes into an atomic Progress struct,
    // which is the shape of something meant to be polled from another thread.
    // The result was a console frozen for the length of a whole dump.
    //
    // I asserted a contract the API does not have. The existing caller was three
    // files away and already did this correctly.
    m_progress.reset();
    m_dump_idx = idx;
    m_dumping  = true;

#ifdef PLATFORM_SWITCH
    // Priority just below the main thread; 128 KB stack (the dump's buffers are
    // heap-allocated), same numbers as the proven caller.
    if (R_SUCCEEDED(threadCreate(&m_dump_thread, dump_thread_fn, this, nullptr,
                                 0x20000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&m_dump_thread))) {
            m_dump_thread_active = true;
            return;
        }
        threadClose(&m_dump_thread);
    }
    // Thread refused to start. Falling back to a synchronous dump freezes the UI,
    // which is bad — but silently doing nothing is worse, and this path only
    // happens when the system is already out of thread resources.
    dump_thread_fn(this);
    poll_dump();
#else
    dump_thread_fn(this);
    poll_dump();
#endif
}

void GamecardScreen::dump_thread_fn(void* arg) {
    auto* self = static_cast<GamecardScreen*>(arg);
    if (self->m_dump_idx < 0 ||
        self->m_dump_idx >= (int)self->m_groups.size()) return;
    Core::Dump::dump_title_to_nsp(self->m_groups[(size_t)self->m_dump_idx].app,
                                  Core::Keys::get(), self->m_progress,
                                  self->m_dump_out_path);
}

void GamecardScreen::install_thread_fn(void* arg) {
    auto* self = static_cast<GamecardScreen*>(arg);
    if (self->m_inst_idx < 0 || self->m_inst_idx >= (int)self->m_groups.size()) return;

    const Core::Ncm::Title& t = self->m_groups[(size_t)self->m_inst_idx].app;

    std::string err;
    auto src = Core::NspStream::open(t, Core::Keys::get(), &err);
    if (!src) {
        self->m_inst_progress.message = err.empty() ? "Cannot read title" : err;
        self->m_inst_progress.push_log("ERROR: " + self->m_inst_progress.message);
        self->m_inst_progress.done = true;
        return;
    }

    Install::StreamInstaller inst(self->m_inst_dest, Core::Keys::get(),
                                  self->m_inst_progress);
    if (!inst.begin(t.name.empty() ? std::string("gamecard.nsp") : t.name + ".nsp",
                    src->total_size())) {
        self->m_inst_progress.done = true;
        return;
    }

    std::vector<uint8_t> scratch(1024 * 1024);
    Install::StreamSource ssrc;
    ssrc.buffer      = scratch.data();
    ssrc.buffer_size = scratch.size();
    ssrc.read = [&](uint8_t* buf, size_t n) -> ssize_t {
        return (ssize_t)src->read(buf, n);
    };
    // Cancel is the progress flag, same as every other install surface.
    ssrc.stop  = [self] { return self->m_inst_progress.cancel.load(); };
    ssrc.drain = [](uint64_t) {};   // nothing to drain: the source is local

    Install::WireSink wire;   // no wire for a local source; totals come from below

    // Size is EXACT — NspStream knows the full container length up front, unlike
    // FTP where it has to be recovered from the container table.
    Install::drive(inst, ssrc, Install::FirstChunk{nullptr, 0},
                   src->total_size(), /*size_exact=*/true, wire);
    self->m_inst_progress.done = true;
}

void GamecardScreen::start_install(int idx, Core::Ncm::Storage dest) {
    if (!Core::Keys::available()) Core::Keys::load();
    if (!Core::Keys::available()) {
        Modal::Options o;
        o.kind          = Modal::Kind::Info;
        o.title         = Lang::t("gamecard.install_failed_title");
        o.body          = Core::Keys::requirement_message();
        o.confirm_label = Lang::t("common.ok");
        Modal::show(o);
        return;
    }

    m_inst_progress.reset();
    m_inst_idx  = idx;
    m_inst_dest = dest;
    m_installing = true;

#ifdef PLATFORM_SWITCH
    if (R_SUCCEEDED(threadCreate(&m_inst_thread, install_thread_fn, this, nullptr,
                                 0x20000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&m_inst_thread))) {
            m_inst_thread_active = true;
            return;
        }
        threadClose(&m_inst_thread);
    }
#endif
    install_thread_fn(this);
    poll_install();
}

void GamecardScreen::poll_install() {
    if (!m_installing) return;
    if (!m_inst_progress.done.load()) return;

#ifdef PLATFORM_SWITCH
    if (m_inst_thread_active) {
        threadWaitForExit(&m_inst_thread);
        threadClose(&m_inst_thread);
        m_inst_thread_active = false;
    }
#endif
    m_installing = false;

    Modal::Options o;
    o.kind          = Modal::Kind::Info;
    o.confirm_label = Lang::t("common.ok");
    if (m_inst_progress.success.load()) {
        o.title  = Lang::t("gamecard.install_done_title");
        o.body   = Lang::t("gamecard.install_done_body");
        m_status = o.title;
    } else {
        o.title  = Lang::t("gamecard.install_failed_title");
        o.body   = m_inst_progress.message.empty()
                       ? Lang::t("gamecard.install_failed_body")
                       : m_inst_progress.message;
        m_status = o.title;
    }
    Modal::show(o);
}

void GamecardScreen::poll_dump() {
    if (!m_dumping) return;
    if (!m_progress.done.load()) return;

#ifdef PLATFORM_SWITCH
    if (m_inst_thread_active) {
        m_inst_progress.cancel.store(true);
        threadWaitForExit(&m_inst_thread);
        threadClose(&m_inst_thread);
        m_inst_thread_active = false;
    }
    if (m_dump_thread_active) {
        threadWaitForExit(&m_dump_thread);
        threadClose(&m_dump_thread);
        m_dump_thread_active = false;
    }
#endif
    m_dumping = false;

    Modal::Options o;
    o.kind          = Modal::Kind::Info;
    o.confirm_label = Lang::t("common.ok");
    if (m_progress.success.load()) {
        o.title  = Lang::t("gamecard.dump_done_title");
        o.body   = Lang::t("gamecard.dump_done_body") + "\n\n" + m_dump_out_path;
        m_status = m_dump_out_path;
    } else {
        o.title  = Lang::t("gamecard.dump_failed_title");
        o.body   = m_progress.message.empty() ? Lang::t("gamecard.dump_failed_body")
                                              : m_progress.message;
        m_status = o.title;
    }
    Modal::show(o);
}

std::unique_ptr<Screen> GamecardScreen::update(bool& pop) {
    pop = false;

    // Poll BOTH workers every frame, before anything else — this is what turns a
    // background operation into a finished one.
    poll_dump();
    poll_install();

    if (Modal::is_active()) return nullptr;

    if (m_installing) {
        if (Input::pressed(Input::Button::B)) m_inst_progress.cancel.store(true);
        return nullptr;
    }
    if (m_dumping) {
        // B cancels. The dump checks progress.cancel between chunks, so this is
        // responsive without the UI ever blocking on it.
        if (Input::pressed(Input::Button::B)) m_progress.cancel.store(true);
        return nullptr;
    }

    if (m_phase == 0) { m_phase = 1; return nullptr; }
    if (m_phase == 1) { m_phase = 2; build_rows(); return nullptr; }

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    const int idx = m_list.cursor();
    const bool have = idx >= 0 && idx < (int)m_groups.size();

    // X installs to SD, Y to NAND — directly from the card, no intermediate dump.
    // Confirmed first because it writes to console storage, unlike Dump.
    if (have && (Input::pressed(Input::Button::X) || Input::pressed(Input::Button::Y))) {
        m_pending_install = idx;
        m_pending_dest = Input::pressed(Input::Button::Y)
                             ? Core::Ncm::Storage::BuiltIn : Core::Ncm::Storage::SdCard;
        Modal::Options o;
        o.kind          = Modal::Kind::Confirm;
        o.title         = Lang::t("gamecard.install_confirm_title");
        o.body          = Lang::t("gamecard.install_confirm_body") + "\n\n" +
                          m_list.item(idx).label + "  ->  " +
                          Lang::t(m_pending_dest == Core::Ncm::Storage::BuiltIn
                                      ? "gamecard.dest_nand" : "gamecard.dest_sd");
        o.confirm_label = Lang::t("gamecard.install");
        o.cancel_label  = Lang::t("common.cancel");
        Modal::show(o);
        return nullptr;
    }

    if (m_list.handle_input() && have) {
        // Dumping only READS the card and writes to SD, so it needs no danger
        // confirmation — but it is long, so it confirms that you meant to start it.
        m_pending_dump = idx;
        Modal::Options o;
        o.kind          = Modal::Kind::Confirm;
        o.title         = Lang::t("gamecard.dump_confirm_title");
        o.body          = Lang::t("gamecard.dump_confirm_body") + "\n\n" +
                          m_list.item(idx).label;
        o.confirm_label = Lang::t("gamecard.dump");
        o.cancel_label  = Lang::t("common.cancel");
        Modal::show(o);
    }
    return nullptr;
}

void GamecardScreen::on_modal_result(int result) {
    if (m_pending_install >= 0) {
        const int i = m_pending_install;
        m_pending_install = -1;
        if (static_cast<Modal::Result>(result) == Modal::Result::Confirmed)
            start_install(i, m_pending_dest);
        return;
    }

    const int idx = m_pending_dump;
    m_pending_dump = -1;
    if (idx < 0) return;
    if (static_cast<Modal::Result>(result) != Modal::Result::Confirmed) return;
    start_dump(idx);
}

void GamecardScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    SDL_Color fg  = Theme::get(Theme::Token::FgPrimary);
    SDL_Color fg2 = Theme::get(Theme::Token::FgSecondary);

    if (m_installing) {
        Renderer::draw_text(Lang::t("gamecard.installing"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            x + Layout::MENU_INDENT_X, y + 50, nullptr, nullptr, w);
        const uint64_t total = m_inst_progress.bytes_total.load();
        const uint64_t done  = m_inst_progress.bytes_done.load();
        char line[96];
        std::snprintf(line, sizeof(line), "%s / %s",
                      Fs::format_size(done).c_str(), Fs::format_size(total).c_str());
        Renderer::draw_text(line, (int)Font::Size::Body, (int)Font::Weight::Regular,
                            (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 96, nullptr, nullptr, w);
        Widgets::draw_progress(x + Layout::MENU_INDENT_X, y + 132,
                               w - Layout::MENU_INDENT_X * 2, 14,
                               total ? (float)((double)done / (double)total) : 0.f);
        std::vector<Widgets::ButtonHint> ih = { { "B", Lang::t("common.cancel") } };
        Widgets::draw_button_legend(x, y + h - 32, w, ih);
        return;
    }

    if (m_dumping) {
        // A live progress page. Without this the screen looked identical whether
        // the dump was running or hung — which is exactly how a slow operation
        // gets force-closed.
        Renderer::draw_text(Lang::t("gamecard.dumping"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            x + Layout::MENU_INDENT_X, y + 50, nullptr, nullptr, w);

        const uint64_t total = m_progress.bytes_total.load();
        const uint64_t done  = m_progress.bytes_done.load();
        char line[96];
        std::snprintf(line, sizeof(line), "%s / %s",
                      Fs::format_size(done).c_str(), Fs::format_size(total).c_str());
        Renderer::draw_text(line, (int)Font::Size::Body, (int)Font::Weight::Regular,
                            (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 96, nullptr, nullptr, w);

        Widgets::draw_progress(x + Layout::MENU_INDENT_X, y + 132,
                               w - Layout::MENU_INDENT_X * 2, 14,
                               total ? (float)((double)done / (double)total) : 0.f);

        std::vector<Widgets::ButtonHint> dh = { { "B", Lang::t("common.cancel") } };
        Widgets::draw_button_legend(x, y + h - 32, w, dh);
        return;
    }

    if (m_phase < 2) {
        Renderer::draw_text(Lang::t("gamecard.loading"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            x + Layout::MENU_INDENT_X, y + 60, nullptr, nullptr, w);
        return;
    }

    if (m_groups.empty()) {
        Renderer::draw_text(Lang::t("gamecard.empty"), (int)Font::Size::Body,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 60,
                            nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
    } else {
        Widgets::ListStyle style;
        style.row_height    = Layout::MENU_ITEM_H;
        style.indent_x      = Layout::MENU_INDENT_X;
        style.show_checkbox = false;
        style.show_dividers = true;
        m_list.draw(x, y, w, h - 36 - (m_status.empty() ? 0 : 24), style);

        if (!m_status.empty())
            Renderer::draw_text(m_status, (int)Font::Size::Small,
                                (int)Font::Weight::Regular, (int)Font::Family::Sans,
                                fg2, x + Layout::MENU_INDENT_X, y + h - 56,
                                nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
    }

    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("gamecard.dump") },
        { "X", Lang::t("gamecard.install_sd") },
        { "Y", Lang::t("gamecard.install_nand") },
        { "B", Lang::t("hints.back") } };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
