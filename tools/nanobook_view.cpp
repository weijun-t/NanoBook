/*
 * nanobook_view.cpp: a live watchlist of every stock's best bid and ask, read from shared memory.
 *
 * What it does: Opens the L1 table that `nanobook_pipeline --view NAME` writes (/dev/shm/NAME,
 *   NAME "nanobook" if none is given), read-only, and shows it full screen, ten times a second:
 *   the run's health and one line per stock (support/watchlist.hpp, tools/view_render.hpp).
 *   Keys: up/down/page up/page down/home/end move, p pins the selected stock to the top, /
 *   filters by symbol prefix (Enter or Esc to finish, Backspace to edit), s cycles the sort
 *   (activity, symbol, spread), q or Esc quits.
 *     nanobook_view [NAME]
 *
 * Why it exists: Viewer design §6: watching the replay is the quickest way to see what the books
 *   are doing, and the pipeline's health while they do it. It is another process: the engine
 *   never knows it is there, and the publisher never waits for it.
 *
 * How it works: A ticker thread posts an FTXUI custom event every 100 ms; the UI thread handles
 *   it by refreshing the Watchlist from the table and redrawing (every table read is on the UI
 *   thread). With no table yet, or once the run is done or quiet, it tries the name again once a
 *   second, and switches to a newer run's table (another run_id) when there is one: start the
 *   viewer before or after the pipeline, and leave it open across runs.
 *
 * Trade-offs: A tool: it allocates and sleeps freely. Needs a terminal FTXUI can drive (any
 *   ANSI terminal, also over SSH). Built only with -DNANOBOOK_BUILD_VIEWER=ON.
 */

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "l1_mapping.hpp" // support/
#include "view_render.hpp"
#include "watchlist.hpp"

namespace {

using namespace nanobook::view;

double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The table, the numbers worked out from it, and what the user has typed.
class Viewer {
public:
    explicit Viewer(std::string name) {
        ui_.name = std::move(name);
        ui_.waiting = "no table yet: start nanobook_pipeline FILE --view " + ui_.name;
    }

    // Every 100 ms, on the UI thread.
    void tick() {
        const double now = seconds_now();
        const bool look =
            !mapping_ || state(mapping_->table()) == RunState::Done || !list_.health().alive;
        if (look && now - last_look_ >= 1.0) {
            last_look_ = now;
            attach();
        }
        if (mapping_) {
            list_.refresh(mapping_->table(), now);
        }
        update_rows();
    }

    // A key. True if it was ours.
    bool key(const ftxui::Event &e, const ftxui::Closure &quit) {
        if (ui_.typing) {
            if (e == ftxui::Event::Return || e == ftxui::Event::Escape) {
                ui_.typing = false;
            } else if (e == ftxui::Event::Backspace) {
                if (!ui_.filter.empty()) {
                    ui_.filter.pop_back();
                }
            } else if (e.is_character()) {
                for (const char c : e.character()) {
                    ui_.filter.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A')
                                                              : c);
                }
            } else {
                return false;
            }
            ui_.cursor = 0;
            update_rows();
            return true;
        }
        const std::size_t page = std::max<std::size_t>(1, visible_rows());
        if (e == ftxui::Event::Character('q') || e == ftxui::Event::Escape) {
            quit();
        } else if (e == ftxui::Event::Character('/')) {
            ui_.typing = true;
        } else if (e == ftxui::Event::Character('s')) {
            ui_.sort = ui_.sort == SortBy::Activity ? SortBy::Symbol
                       : ui_.sort == SortBy::Symbol ? SortBy::Spread
                                                    : SortBy::Activity;
        } else if (e == ftxui::Event::Character('p')) {
            if (ui_.cursor < rows_.size()) {
                list_.toggle_pin(rows_[ui_.cursor].locate);
            }
        } else if (e == ftxui::Event::ArrowUp) {
            ui_.cursor -= ui_.cursor > 0 ? 1 : 0;
        } else if (e == ftxui::Event::ArrowDown) {
            ++ui_.cursor;
        } else if (e == ftxui::Event::PageUp) {
            ui_.cursor -= std::min(ui_.cursor, page);
        } else if (e == ftxui::Event::PageDown) {
            ui_.cursor += page;
        } else if (e == ftxui::Event::Home) {
            ui_.cursor = 0;
        } else if (e == ftxui::Event::End) {
            ui_.cursor = rows_.empty() ? 0 : rows_.size() - 1;
        } else {
            return false;
        }
        update_rows();
        return true;
    }

    [[nodiscard]] ftxui::Element draw() const {
        return render(list_.health(), rows_, ui_, visible_rows());
    }

private:
    // Opens the name; keeps the table we have unless this is a newer run's.
    void attach() {
        auto opened = L1Mapping::open(ui_.name);
        if (!opened) {
            if (!mapping_) {
                ui_.waiting = opened.error();
            }
            return;
        }
        const auto run = [](const L1Mapping &m) {
            return m.table().header.run_id.load(std::memory_order_relaxed);
        };
        if (!mapping_ || run(*opened) != run(*mapping_)) {
            mapping_ = std::move(*opened);
            ui_.attached = true;
            ui_.cursor = 0;
        }
    }

    void update_rows() {
        rows_ = list_.rows(ui_.filter, ui_.sort);
        if (ui_.cursor >= rows_.size()) {
            ui_.cursor = rows_.empty() ? 0 : rows_.size() - 1;
        }
    }

    static std::size_t visible_rows() {
        return nanobook::view::visible_rows(ftxui::Terminal::Size().dimy);
    }

    std::optional<L1Mapping> mapping_;
    Watchlist list_;
    UiState ui_;
    std::vector<Row> rows_;
    double last_look_{-1e9};
};

} // namespace

int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "usage: nanobook_view [NAME]   (the NAME given to nanobook_pipeline --view; "
                     "default nanobook)\n";
        return 2;
    }
    Viewer viewer{argc == 2 ? argv[1] : "nanobook"};
    viewer.tick();

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    const ftxui::Closure quit = screen.ExitLoopClosure();
    auto component = ftxui::CatchEvent(ftxui::Renderer([&] { return viewer.draw(); }),
                                       [&](const ftxui::Event &e) {
                                           if (e == ftxui::Event::Custom) {
                                               viewer.tick();
                                               return true;
                                           }
                                           return viewer.key(e, quit);
                                       });
    // Declared after the screen, so it stops (and is joined) before the screen goes away.
    std::jthread ticker{[&screen](const std::stop_token &stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // viewer design §9 V4
            screen.PostEvent(ftxui::Event::Custom);
        }
    }};
    screen.Loop(component);
    ticker.request_stop();
    return 0;
}
