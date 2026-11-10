#include "tui.hpp"

#include "../core/hotspot.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/pixel.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace apctl::tui {
namespace {
using namespace ftxui;
using apctl::core::HotspotService;
using apctl::core::OperationResult;
using apctl::core::Severity;
using apctl::core::Status;

// ---------------------------------------------------------------- palette
// Every text colour is picked to keep at least ~4.5:1 contrast against kPanel
// (or kField for inputs). kDesktop is only ever the backdrop.
const Color kDesktop = Color::RGB(137, 174, 210);
const Color kPanel = Color::RGB(240, 228, 237);
const Color kInk = Color::RGB(28, 26, 38);        // body text
const Color kMuted = Color::RGB(104, 92, 112);    // placeholders, disabled items
const Color kLine = Color::RGB(58, 50, 72);       // box outlines, separators
const Color kAccent = Color::RGB(96, 76, 222);    // selected row / focused outline
const Color kTitle = Color::RGB(70, 48, 186);     // headings
const Color kGood = Color::RGB(12, 100, 48);
const Color kWarn = Color::RGB(132, 72, 0);
const Color kBad = Color::RGB(168, 22, 32);
const Color kField = Color::RGB(250, 244, 249);   // input background
const Color kFieldFocus = Color::RGB(255, 255, 255);
const Color kDisabledFocus = Color::RGB(214, 200, 212);

constexpr int kPanelWidth = 50;  // minimum outer width, outline included
constexpr int kMinCols = 54;
constexpr int kMinRowsMenu = 21;
constexpr int kMinRowsForm = 24;  // inputs are 5 rows tall each

// A one-cell outline drawn with thin block glyphs placed on the INNER edge of
// the ring cells. The ring keeps whatever background is underneath (backdrop or
// dimmed screen), so there is no second, lighter box outside the line, which is
// what borderStyled() + bgcolor() produced: the line sits in the middle of its
// cell and the panel colour filled the outer half.
class Outline : public Node {
public:
    Outline(Element child, Color line) : Node(Elements{std::move(child)}), line_(line) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
        requirement_.min_x += 2;
        requirement_.min_y += 2;
        requirement_.focused.box.x_min++;
        requirement_.focused.box.x_max++;
        requirement_.focused.box.y_min++;
        requirement_.focused.box.y_max++;
    }

    void SetBox(Box box) override {
        Node::SetBox(box);
        box.x_min++;
        box.x_max--;
        box.y_min++;
        box.y_max--;
        children_[0]->SetBox(box);
    }

    void Render(Screen& screen) override {
        children_[0]->Render(screen);
        if (box_.x_min >= box_.x_max || box_.y_min >= box_.y_max) return;

        auto put = [&](int x, int y, const char* glyph) {
            Pixel& pixel = screen.PixelAt(x, y);  // background is left untouched
            pixel.character = glyph;
            pixel.foreground_color = line_;
            pixel.bold = false;
            pixel.dim = false;
            pixel.inverted = false;
            pixel.automerge = false;
        };
        // Corners stay empty: the lines meet diagonally, a ~1px notch you can't see.
        put(box_.x_min, box_.y_min, " ");
        put(box_.x_max, box_.y_min, " ");
        put(box_.x_min, box_.y_max, " ");
        put(box_.x_max, box_.y_max, " ");
        for (int x = box_.x_min + 1; x < box_.x_max; ++x) {
            put(x, box_.y_min, "▁");  // lower eighth: hugs the content below
            put(x, box_.y_max, "▔");  // upper eighth: hugs the content above
        }
        for (int y = box_.y_min + 1; y < box_.y_max; ++y) {
            put(box_.x_min, y, "▕");  // right eighth: hugs the content to the right
            put(box_.x_max, y, "▏");  // left eighth: hugs the content to the left
        }
    }

private:
    Color line_;
};

Element outline(Element child, Color line) {
    return std::make_shared<Outline>(std::move(child), line);
}

// One blank cell of breathing room on every side, so text never touches the line.
Element padded(Element child) {
    return vbox({text(" "),
                 hbox({text(" "), std::move(child) | flex, text(" ")}),
                 text(" ")});
}

// Indexes into the Container::Tab below. Only the active page receives events.
constexpr int kPageActions = 0;
constexpr int kPagePassword = 1;
constexpr int kPageSsid = 2;
constexpr int kPageResult = 3;

enum class Dialog { none, result, password, ssid };

struct Completion {
    OperationResult operation;
    Status status;
    bool show_result = true;
};

struct Model {
    Status status;
    bool started = false;
    bool busy = false;
    bool quit_after_work = false;
    Dialog dialog = Dialog::none;
    OperationResult result;
    std::string ssid_input;
    std::string password_input;
    std::string confirm_input;
    std::string form_error;
    int working_frame = 0;
    int page = kPageActions;
};

// ---------------------------------------------------------------- helpers
Color severity_color(Severity severity) {
    switch (severity) {
        case Severity::success: return kGood;
        case Severity::warning: return kWarn;
        case Severity::error: return kBad;
        case Severity::info: return kTitle;
    }
    return kTitle;
}

std::string dialog_title(Severity severity) {
    switch (severity) {
        case Severity::success: return "Done";
        case Severity::warning: return "Check this";
        case Severity::error: return "Couldn't complete action";
        case Severity::info: return "Hotspot details";
    }
    return "apctl";
}

std::vector<std::string> split_lines(const std::string& value) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : value) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty() || lines.empty()) lines.push_back(current);
    return lines;
}

// paragraph() collapses newlines, so wrap each line separately.
Elements text_block(const std::string& value) {
    Elements out;
    for (const auto& line : split_lines(value)) {
        if (line.empty()) out.push_back(text(" "));
        else out.push_back(paragraph(line) | color(kInk));
    }
    return out;
}

Element status_line(const Status& status) {
    if (!status.network_manager_available)
        return hbox({text("NetworkManager ") | color(kInk), text("unavailable") | color(kBad)});
    const std::string wifi = status.wifi_device.empty()
        ? "disconnected"
        : (status.wifi_connection.empty() ? status.wifi_device : status.wifi_connection);
    std::string hotspot = "stopped";
    if (status.hotspot_active) {
        hotspot = status.hotspot_ssid.empty() ? "active" : status.hotspot_ssid;
        if (!status.hotspot_channel.empty()) hotspot += "  ·  ch " + status.hotspot_channel;
    }
    return vbox({
        hbox({text("Wi-Fi     ") | color(kInk),
              text((status.wifi_device.empty() ? "" : "● ") + wifi)
                  | color(status.wifi_device.empty() ? kWarn : kGood)}),
        hbox({text("Hotspot  ") | color(kInk),
              text((status.hotspot_active ? "● " : "") + hotspot)
                  | color(status.hotspot_active ? kGood : kInk)})
    });
}

Element too_small(const Dimensions& term, int min_rows) {
    return vbox({
               filler(),
               text("Terminal too small") | bold | hcenter,
               text("Need at least " + std::to_string(kMinCols) + " x " + std::to_string(min_rows) +
                    ", have " + std::to_string(term.dimx) + " x " + std::to_string(term.dimy)) | hcenter,
               text("Resize the window to continue") | hcenter,
               filler(),
           }) | color(kInk) | bgcolor(kDesktop) | flex;
}

Element result_popup(const OperationResult& result, Element ok_button) {
    const Dimensions term = Terminal::Size();
    const Color accent = severity_color(result.severity);

    const bool has_qr = result.credentials && !result.credentials->qr_text.empty();
    Elements body;
    if (!has_qr || result.message != "Hotspot credentials")
        body = text_block(result.message);
    int qr_width = 0;
    std::vector<std::string> qr;
    if (result.credentials) {
        const auto& creds = *result.credentials;
        body.push_back(text("SSID:      " + creds.ssid) | color(kInk));
        body.push_back(text("Password:  " + creds.password) | color(kInk));
        if (!creds.qr_text.empty()) {
            qr = split_lines(creds.qr_text);
            for (const auto& line : qr) qr_width = std::max(qr_width, string_width(line));
        }
    }
    const int available_width = term.dimx - 6;
    const int width = std::max(30, std::min(available_width, std::max(64, qr_width + 2)));
    if (result.credentials && !result.credentials->qr_text.empty()) {
            const int rows_needed = static_cast<int>(qr.size()) + static_cast<int>(body.size()) + 9;
            body.push_back(separator() | color(kLine));
            if (qr_width <= width - 2 && rows_needed <= term.dimy) {
                // One text() per line: paragraph() would re-wrap and destroy the QR.
                Elements qr_rows;
                for (const auto& line : qr) qr_rows.push_back(text(line));
                body.push_back(vbox(std::move(qr_rows)) | color(Color::Black) | bgcolor(Color::White) | hcenter);
            } else {
                body.push_back(paragraph("The QR code needs a larger terminal (about " +
                                         std::to_string(qr_width + 6) + " columns by " +
                                         std::to_string(rows_needed) + " rows). Enlarge the window and try again.")
                               | color(kWarn));
            }
    }

    Elements rows;
    rows.push_back(text(dialog_title(result.severity)) | bold | color(accent));
    rows.push_back(separator() | color(kLine));
    for (auto& line : body) rows.push_back(std::move(line));
    rows.push_back(separator() | color(kLine));
    rows.push_back(std::move(ok_button));

    // clear_under sits INSIDE the outline: it wipes the dimmed screen under the
    // content (otherwise its text bleeds through) but leaves the outline cells
    // alone, so they keep the colour of whatever is behind them.
    return outline(padded(vbox(std::move(rows))) | size(WIDTH, EQUAL, width) | bgcolor(kPanel)
                       | clear_under,
                   accent);
}

}  // namespace

int run() {
    Model model;
    HotspotService service;

    auto screen = ScreenInteractive::FullscreenAlternateScreen();
    screen.TrackMouse(true);
    // Let FTXUI turn Ctrl+C into Event::CtrlC so the loop can unwind and
    // restore the terminal before the process exits.
    screen.ForceHandleCtrlC(true);

    std::thread worker;
    std::thread working_ticker;
    std::atomic<bool> ticker_running{false};

    // Components are declared up front so the Input callbacks can move focus
    // between them before every component has been built.
    Component ssid_input, password_input, confirm_input;
    Component save_password, cancel_password, save_ssid, cancel_ssid, ok_button;

    // ------------------------------------------------------ state helpers
    auto clear_secrets = [&] {
        model.password_input.clear();
        model.confirm_input.clear();
    };
    auto close_dialog = [&] {
        clear_secrets();
        model.ssid_input.clear();
        model.form_error.clear();
        model.dialog = Dialog::none;
        model.page = kPageActions;
    };
    auto open_dialog = [&](Dialog dialog) {
        model.dialog = dialog;
        switch (dialog) {
            case Dialog::password: model.page = kPagePassword; break;
            case Dialog::ssid: model.page = kPageSsid; break;
            case Dialog::result: model.page = kPageResult; break;
            case Dialog::none: model.page = kPageActions; break;
        }
    };
    auto local_result = [&](Severity severity, std::string message, int code = 0) {
        model.result = {severity, code, std::move(message), std::nullopt};
        open_dialog(Dialog::result);
    };

    // Run a closure on the UI thread AND force a redraw. A bare Post(closure)
    // runs the code but does not mark the frame dirty, so the screen would stay
    // stale until the next key press. An Event always invalidates the frame.
    auto post_ui = [&](std::function<void()> fn) {
        screen.Post(std::move(fn));
        screen.PostEvent(Event::Custom);
    };

    // Runs on the UI thread (posted by the worker), so it may touch the model.
    auto finish = [&](Completion& done) {
        ticker_running.store(false);
        if (working_ticker.joinable()) working_ticker.join();
        if (worker.joinable()) worker.join();
        model.status = std::move(done.status);
        model.result = std::move(done.operation);
        model.busy = false;
        if (done.show_result) open_dialog(Dialog::result);
        else close_dialog();
        if (model.quit_after_work) screen.Exit();
    };

    auto launch = [&](std::function<OperationResult()> operation, bool show_result = true) {
        if (model.busy) return;
        if (worker.joinable()) worker.join();
        if (working_ticker.joinable()) working_ticker.join();
        close_dialog();
        model.busy = true;
        model.working_frame = 0;
        try {
            ticker_running.store(true);
            working_ticker = std::thread([&] {
                while (ticker_running.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (!ticker_running.load()) break;
                    post_ui([&] { ++model.working_frame; });
                }
            });
            worker = std::thread([&, operation = std::move(operation), show_result]() mutable {
                auto done = std::make_shared<Completion>();
                done->show_result = show_result;
                try {
                    done->operation = operation();
                } catch (...) {
                    done->operation = {Severity::error, 1, "The action stopped unexpectedly. Try again.", std::nullopt};
                }
                try {
                    done->status = service.status();
                } catch (...) {
                    done->status = {};
                }
                post_ui([&, done] { finish(*done); });
            });
        } catch (...) {
            ticker_running.store(false);
            if (working_ticker.joinable()) working_ticker.join();
            model.busy = false;
            local_result(Severity::error, "Couldn't start the background operation.", 1);
        }
    };

    // ------------------------------------------------------ buttons
    auto action_button = [&](std::string label, std::function<void()> action,
                             std::function<bool()> enabled = nullptr, bool centered = false) {
        auto is_enabled = [enabled] { return !enabled || enabled(); };
        ButtonOption option;
        option.label = std::move(label);
        option.on_click = [&, action = std::move(action), is_enabled] {
            if (model.busy || !is_enabled()) return;
            action();
        };
        option.transform = [is_enabled, centered](const EntryState& state) {
            Color foreground = kInk;
            Color background = kPanel;
            const bool highlighted = state.focused || state.active;
            if (!is_enabled()) {
                foreground = kMuted;
                if (highlighted) background = kDisabledFocus;
            } else if (highlighted) {
                foreground = Color::White;
                background = kAccent;
            }
            Element label = centered ? text(state.label) | hcenter
                                     : hbox({text("  " + state.label), filler()});
            return label | color(foreground) | bgcolor(background);
        };
        return Button(std::move(option));
    };

    save_password = action_button("Save password", [&] {
        if (model.password_input.size() < 8 || model.password_input.size() > 63) {
            model.form_error = "Password must be 8-63 characters.";
            return;
        }
        if (model.password_input != model.confirm_input) {
            model.form_error = "Passwords don't match.";
            return;
        }
        std::string value = std::move(model.password_input);
        clear_secrets();
        launch([&, value = std::move(value)]() mutable { return service.change_password(std::move(value)); });
    });
    cancel_password = action_button("Cancel", close_dialog);

    save_ssid = action_button("Save SSID", [&] {
        if (model.ssid_input.empty() || model.ssid_input.size() > 32) {
            model.form_error = "SSID must be 1-32 characters.";
            return;
        }
        std::string value = std::move(model.ssid_input);
        model.ssid_input.clear();
        launch([&, value = std::move(value)]() mutable { return service.change_ssid(std::move(value)); });
    });
    cancel_ssid = action_button("Cancel", close_dialog);

    ok_button = action_button("OK", close_dialog, nullptr, true);

    auto start_button = action_button("Start hotspot", [&] {
        launch([&] { return service.start(); });
    }, [&] { return !model.status.hotspot_active; });
    auto stop_button = action_button("Stop hotspot", [&] {
        launch([&] { return service.stop(); });
    }, [&] { return model.status.hotspot_active; });
    auto check_button = action_button("Check Wi-Fi support", [&] {
        launch([&] { return service.check(); });
    });
    auto credentials_button = action_button("Show password and QR code", [&] {
        launch([&] { return service.show_credentials(); });
    });
    auto password_button = action_button("Change password", [&] {
        close_dialog();
        open_dialog(Dialog::password);
        password_input->TakeFocus();
    });
    auto ssid_button = action_button("Change hotspot SSID", [&] {
        close_dialog();
        open_dialog(Dialog::ssid);
        ssid_input->TakeFocus();
    });
    auto refresh_button = action_button("Refresh status", [&] {
        launch([&] { return OperationResult{Severity::success, 0, "Status refreshed.", std::nullopt}; });
    });
    auto quit_button = action_button("Quit", [&] { screen.Exit(); });

    // ------------------------------------------------------ inputs
    // The default Input style inverts its colours when focused, which turns the
    // text into a washed-out grey bar. We pick explicit colours instead; the
    // focused state is shown by the field background and outline (see field_box).
    const auto input_style = [](InputState state) {
        state.element |= color(state.is_placeholder ? kMuted : kInk);
        return state.element;
    };

    InputOption ssid_options;
    ssid_options.placeholder = "e.g. apctl-hotspot";
    ssid_options.transform = input_style;
    ssid_options.on_enter = [&] { save_ssid->TakeFocus(); };
    ssid_input = Input(&model.ssid_input, ssid_options);

    InputOption password_options;
    password_options.placeholder = "8-63 characters";
    password_options.password = true;
    password_options.multiline = false;
    password_options.transform = input_style;
    password_options.on_enter = [&] { confirm_input->TakeFocus(); };
    password_input = Input(&model.password_input, password_options);

    InputOption confirm_options;
    confirm_options.placeholder = "Repeat password";
    confirm_options.password = true;
    confirm_options.multiline = false;
    confirm_options.transform = input_style;
    confirm_options.on_enter = [&] { save_password->TakeFocus(); };
    confirm_input = Input(&model.confirm_input, confirm_options);

    // ------------------------------------------------------ pages
    auto action_list = Container::Vertical({start_button, stop_button, check_button,
        credentials_button, password_button, ssid_button, refresh_button, quit_button});
    auto password_form = Container::Vertical({password_input, confirm_input, save_password, cancel_password});
    auto ssid_form = Container::Vertical({ssid_input, save_ssid, cancel_ssid});
    auto result_page = Container::Vertical({ok_button});
    // Order must match kPageActions / kPagePassword / kPageSsid / kPageResult.
    auto pages = Container::Tab({action_list, password_form, ssid_form, result_page}, &model.page);

    // ------------------------------------------------------ rendering
    // Inner width is kPanelWidth minus the two outline cells.
    auto panel = [](Element content, int outer_width, int outer_height = 0) {
        if (outer_height > 0) {
            content = vbox({std::move(content), filler()})
                      | size(HEIGHT, EQUAL, outer_height - 4);
        }
        return outline(padded(std::move(content)) | size(WIDTH, EQUAL, outer_width - 2)
                           | bgcolor(kPanel),
                       kLine);
    };
    // A 5-row input: one blank row above and below the text, filling its outline
    // edge to edge. The outline turns accent-coloured while the field has focus.
    auto field_box = [](const Component& input) -> Element {
        const bool focused = input->Focused();
        Element row = hbox({text(" "), input->Render() | flex, text(" ")});
        Element body = vbox({text(" "), std::move(row), text(" ")})
                       | bgcolor(focused ? kFieldFocus : kField);
        return outline(std::move(body), focused ? kAccent : kLine);
    };
    auto labelled_input = [&](const std::string& label, const Component& input) -> Element {
        return vbox({text(label) | bold | color(kInk), field_box(input)});
    };
    auto form_error_line = [&]() -> Element {
        if (model.form_error.empty()) return text(" ");
        return text(model.form_error) | color(kBad) | bold;
    };

    auto root = CatchEvent(Renderer(pages, [&]() -> Element {
        // First frame: the loop is running now, so the worker's Post() is
        // guaranteed to be delivered (posts made before Loop() are dropped).
        if (!model.started) {
            model.started = true;
            launch([&] { return OperationResult{Severity::info, 0, "Status refreshed.", std::nullopt}; }, false);
        }

        const bool in_form = model.dialog == Dialog::password || model.dialog == Dialog::ssid;
        const Dimensions term = Terminal::Size();
        const int min_rows = in_form ? kMinRowsForm : kMinRowsMenu;
        if (term.dimx < kMinCols || term.dimy < min_rows) return too_small(term, min_rows);
        const int menu_width = std::min(term.dimx - 4, std::max(kPanelWidth, term.dimx * 3 / 4));
        const int menu_height = term.dimy >= kMinRowsMenu + 8
            ? std::min(term.dimy - 3, term.dimy * 2 / 3)
            : 0;

        Element content;
        if (model.dialog == Dialog::password) {
            content = vbox({
                text("Change hotspot password") | bold | color(kTitle),
                separator() | color(kLine),
                labelled_input("Password", password_input),
                labelled_input("Confirm password", confirm_input),
                form_error_line(),
                save_password->Render(),
                cancel_password->Render()
            });
        } else if (model.dialog == Dialog::ssid) {
            content = vbox({
                text("Change hotspot SSID") | bold | color(kTitle),
                separator() | color(kLine),
                labelled_input("New SSID", ssid_input),
                form_error_line(),
                save_ssid->Render(),
                cancel_ssid->Render()
            });
        } else {
            const std::string clients = model.status.hotspot_active
                ? std::to_string(model.status.connected_devices) : "—";
            content = vbox({
                text("apctl  /  Wi-Fi hotspot") | bold | color(kTitle),
                separator() | color(kLine),
                status_line(model.status),
                hbox({text("Connected devices  ") | color(kInk), text(clients) | color(kInk)}),
                separator() | color(kLine),
                action_list->Render() | size(WIDTH, EQUAL, menu_width - 4),
                filler()
            });
        }

        std::string hint;
        if (model.busy) hint = " Working...   Ctrl+C or q quits when it finishes ";
        else if (model.dialog == Dialog::result) hint = " Enter / Esc Close   Click OK ";
        else if (in_form) hint = " Tab / ↑↓ Move   Enter Select   Esc Cancel ";
        else hint = " ↑/↓ Navigate   Enter Select   Esc/q Quit   Mouse: click, scroll ";
        Element footer = hbox({text(hint) | color(kPanel), filler()}) | bgcolor(kInk);

        const int panel_width = in_form ? kPanelWidth : menu_width;
        const int panel_height = in_form ? 0 : menu_height;
        Element base = vbox({filler(), panel(std::move(content), panel_width, panel_height) | hcenter,
                             filler(), footer})
                       | bgcolor(kDesktop);

        if (model.busy) {
            std::string dots(static_cast<size_t>((model.working_frame / 3) % 4), '.');
            Elements lines{text(" "),
                           hbox({text("  Working") | bold | color(kInk),
                                 text(dots) | size(WIDTH, EQUAL, 3) | bold | color(kInk),
                                 text("  ")})};
            if (model.quit_after_work)
                lines.push_back(text("  Quitting when finished ") | bold | color(kWarn));
            lines.push_back(text(" "));
            // clear_under inside the outline: see result_popup().
            Element busy_box = outline(vbox(std::move(lines)) | size(WIDTH, GREATER_THAN, 22)
                                           | bgcolor(kPanel) | clear_under,
                                       kAccent);
            return dbox({base | dim, busy_box | center}) | flex;
        }
        if (model.dialog == Dialog::result) {
            return dbox({base | dim, result_popup(model.result, ok_button->Render()) | center}) | flex;
        }
        return base | flex;
    }), [&](Event event) {
        const bool is_quit_key = event == Event::Escape || event == Event::Character('q')
                                 || event == Event::Character('Q');

        if (event == Event::CtrlC) {
            if (model.busy) model.quit_after_work = true;
            else screen.Exit();
            return true;
        }
        if (model.busy) {
            if (event == Event::Character('q') || event == Event::Character('Q'))
                model.quit_after_work = true;
            return true;  // swallow input while a command runs
        }
        if (model.dialog == Dialog::result) {
            if (is_quit_key) { close_dialog(); return true; }
            return false;  // Enter and mouse clicks reach the OK button
        }
        if (model.dialog == Dialog::password || model.dialog == Dialog::ssid) {
            if (event == Event::Escape) { close_dialog(); return true; }
            return false;  // typing goes to the inputs
        }
        if (is_quit_key) {
            screen.Exit();
            return true;
        }
        return false;
    });

    screen.Loop(root);

    ticker_running.store(false);
    if (working_ticker.joinable()) working_ticker.join();
    if (worker.joinable()) worker.join();
    return 0;
}

}  // namespace apctl::tui
