#include "tui.hpp"

#include "../core/hotspot.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <mutex>
#include <optional>
#include <exception>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace apctl::tui {
namespace {
using namespace ftxui;
using apctl::core::Credentials;
using apctl::core::HotspotService;
using apctl::core::OperationResult;
using apctl::core::Severity;
using apctl::core::Status;

const Color kDesktop = Color::RGB(137, 174, 210);
const Color kPanel = Color::RGB(231, 210, 222);
const Color kInk = Color::RGB(52, 52, 52);
const Color kAccent = Color::RGB(127, 110, 255);

enum class Dialog { none, result, password, ssid };

struct Completion {
    OperationResult operation;
    Status status;
    bool show_result = true;
};

Element status_line(const Status& status) {
    if (!status.network_manager_available)
        return hbox({text("NetworkManager ") | color(kInk), text("unavailable") | color(Color::Red)});
    Elements items;
    items.push_back(text("Wi-Fi  ") | color(kInk));
    if (status.wifi_device.empty()) items.push_back(text("disconnected") | color(Color::Yellow));
    else {
        std::string connected = status.wifi_connection.empty() ? status.wifi_device : status.wifi_connection;
        items.push_back(text("● " + connected) | color(Color::Green));
    }
    items.push_back(text("     Hotspot  ") | color(kInk));
    if (!status.hotspot_active) items.push_back(text("stopped") | color(kInk));
    else {
        std::string active = "● " + (status.hotspot_ssid.empty() ? std::string("active") : status.hotspot_ssid);
        if (!status.hotspot_channel.empty()) active += "  ·  ch " + status.hotspot_channel;
        items.push_back(text(active) | color(Color::Green));
    }
    return hbox(std::move(items));
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

}  // namespace

int run() {
    struct Model {
        Status status;
        bool busy = false;
        bool quit_after_work = false;
        Dialog dialog = Dialog::none;
        OperationResult result;
        std::string ssid_input;
        std::string password_input;
        std::string confirm_input;
        int working_frame = 0;
        int page = 0;
    } model;

    HotspotService service;
    auto screen = ScreenInteractive::FullscreenAlternateScreen();
    screen.TrackMouse(true);
    // Let FTXUI turn Ctrl+C into Event::CtrlC so the event loop can unwind
    // and restore the alternate screen before the process exits.
    screen.ForceHandleCtrlC(true);

    std::mutex completion_mutex;
    std::optional<Completion> completed;
    std::thread worker;
    std::thread working_ticker;
    std::atomic<bool> ticker_running{false};

    auto local_result = [&](Severity severity, std::string message, int code = 0) {
        model.result = {severity, code, std::move(message), std::nullopt};
        model.dialog = Dialog::result;
        model.page = 0;
    };

    auto launch = [&](std::function<OperationResult()> operation, bool show_result = true) {
        if (model.busy) return;
        if (worker.joinable()) worker.join();
        model.dialog = Dialog::none;
        model.page = 0;
        model.busy = true;
        try {
            ticker_running.store(true);
            working_ticker = std::thread([&] {
                while (ticker_running.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(280));
                    if (ticker_running.load()) screen.PostEvent(Event::Custom);
                }
            });
            worker = std::thread([&, operation = std::move(operation)]() mutable {
                Completion done;
                done.show_result = show_result;
                try {
                    done.operation = operation();
                } catch (const std::exception&) {
                    done.operation = {Severity::error, 1, "The action stopped unexpectedly. Try again.", std::nullopt};
                } catch (...) {
                    done.operation = {Severity::error, 1, "The action stopped unexpectedly. Try again.", std::nullopt};
                }
                try {
                    done.status = service.status();
                } catch (...) {
                    done.status = {};
                }
                {
                    std::lock_guard<std::mutex> lock(completion_mutex);
                    completed = std::move(done);
                }
                ticker_running.store(false);
                screen.PostEvent(Event::Custom);
            });
        } catch (...) {
            ticker_running.store(false);
            if (working_ticker.joinable()) working_ticker.join();
            model.busy = false;
            local_result(Severity::error, "Couldn't start the background operation.", 1);
        }
    };

    InputOption ssid_options;
    ssid_options.placeholder = "e.g. apctl-hotspot";
    auto ssid_input = Input(&model.ssid_input, ssid_options);
    InputOption password_options;
    password_options.placeholder = "8–63 characters";
    password_options.password = true;
    password_options.multiline = false;
    auto password_input = Input(&model.password_input, password_options);
    InputOption confirm_options;
    confirm_options.placeholder = "Repeat password";
    confirm_options.password = true;
    confirm_options.multiline = false;
    auto confirm_input = Input(&model.confirm_input, confirm_options);

    auto action_button = [&](std::string label, std::function<void()> action,
                             std::function<bool()> enabled = [] { return true; }) {
        ButtonOption option;
        option.label = std::move(label);
        option.on_click = [&, action = std::move(action), enabled] {
            if (model.busy || !enabled()) return;
            action();
        };
        option.transform = [enabled](const EntryState& state) {
            Color foreground = kInk;
            Color background = kPanel;
            if (!enabled()) foreground = Color::GrayDark;
            if (state.focused || state.active) {
                foreground = Color::White;
                background = kAccent;
            }
            return hbox({text("  " + state.label), filler()})
                | size(WIDTH, EQUAL, 48) | color(foreground) | bgcolor(background);
        };
        return Button(std::move(option));
    };

    auto cancel_form = [&] {
        model.dialog = Dialog::none;
        model.page = 0;
    };
    auto save_password = action_button("Save password", [&] {
        if (model.password_input != model.confirm_input) {
            local_result(Severity::error, "Passwords didn't match. No changes made.", 2);
            return;
        }
        std::string value = std::move(model.password_input);
        model.password_input.clear();
        model.confirm_input.clear();
        launch([&, value = std::move(value)]() mutable { return service.change_password(std::move(value)); });
    });
    auto save_ssid = action_button("Save name", [&] {
        std::string value = std::move(model.ssid_input);
        model.ssid_input.clear();
        launch([&, value = std::move(value)]() mutable { return service.change_ssid(std::move(value)); });
    });
    auto cancel_password = action_button("Cancel", cancel_form);
    auto cancel_ssid = action_button("Cancel", cancel_form);

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
        model.password_input.clear();
        model.confirm_input.clear();
        model.dialog = Dialog::password;
        model.page = 1;
    });
    auto ssid_button = action_button("Change hotspot name", [&] {
        model.ssid_input.clear();
        model.dialog = Dialog::ssid;
        model.page = 2;
    });
    auto refresh_button = action_button("Refresh status", [&] {
        launch([&] { return OperationResult{Severity::success, 0, "Status refreshed.", std::nullopt}; });
    });
    auto quit_button = action_button("Quit", [&] { screen.Exit(); });
    auto action_list = Container::Vertical({start_button, stop_button, check_button,
        credentials_button, password_button, ssid_button, refresh_button, quit_button});

    auto password_form = Container::Vertical({
        password_input, confirm_input, Container::Vertical({save_password, cancel_password})
    });
    auto ssid_form = Container::Vertical({
        ssid_input, Container::Vertical({save_ssid, cancel_ssid})
    });
    auto pages = Container::Tab({action_list, password_form, ssid_form}, &model.page);

    auto root = CatchEvent(Renderer(pages, [&] {
        const bool editing_password = model.dialog == Dialog::password;
        const bool editing_ssid = model.dialog == Dialog::ssid;
        Element action_area;
        if (editing_password) {
            action_area = vbox({
                text("Change hotspot password") | bold | color(kAccent),
                text("Password") | color(kInk), password_input->Render(),
                text("Confirm password") | color(kInk), confirm_input->Render(),
                save_password->Render(), cancel_password->Render()
            }) | borderStyled(LIGHT, kInk) | bgcolor(kPanel) | size(WIDTH, EQUAL, 52);
        } else if (editing_ssid) {
            action_area = vbox({
                text("Change hotspot name") | bold | color(kAccent),
                text("New name") | color(kInk), ssid_input->Render(),
                save_ssid->Render(), cancel_ssid->Render()
            }) | borderStyled(LIGHT, kInk) | bgcolor(kPanel) | size(WIDTH, EQUAL, 52);
        } else {
            const std::string clients = model.status.hotspot_active
                ? std::to_string(model.status.connected_devices) : "—";
            action_area = vbox({
                text(" apctl  /  Wi-Fi hotspot ") | bold | color(kAccent),
                separator() | color(kInk),
                status_line(model.status),
                hbox({text("Connected devices  ") | color(kInk), text(clients) | color(kInk)}),
                separator() | color(kInk),
                text("Connections and hotspot actions") | color(kInk),
                action_list->Render() | vscroll_indicator | frame | size(WIDTH, EQUAL, 48) | size(HEIGHT, LESS_THAN, 18)
            }) | borderStyled(LIGHT, kInk) | bgcolor(kPanel) | size(WIDTH, EQUAL, 52);
        }

        Element footer = hbox({
            text(" ↑/↓ Navigate   Enter Select   Esc/q Quit   Mouse ") | color(kPanel)
        }) | bgcolor(kInk) | size(WIDTH, GREATER_THAN, 0);
        Element base = vbox({filler(), action_area | center, filler(), footer}) | bgcolor(kDesktop);

        if (model.dialog == Dialog::result) {
            Elements modal_lines{
                text(" " + dialog_title(model.result.severity) + " ") | bold | color(kAccent),
                separator() | color(kInk),
                paragraph(model.result.message) | color(kInk)
            };
            if (model.result.credentials) {
                modal_lines.push_back(text("SSID: " + model.result.credentials->ssid) | color(kInk));
                modal_lines.push_back(text("Password: " + model.result.credentials->password) | color(kInk));
                if (!model.result.credentials->qr_text.empty()) {
                    modal_lines.push_back(separator());
                    modal_lines.push_back(paragraph(model.result.credentials->qr_text) | color(kInk));
                }
            }
            modal_lines.push_back(separator());
            modal_lines.push_back(text("Enter or click to close") | color(Color::GrayDark));
            Element popup = vbox(std::move(modal_lines)) | borderStyled(LIGHT, kInk) | bgcolor(kPanel) |
                            size(WIDTH, GREATER_THAN, 48) | size(HEIGHT, LESS_THAN, 24);
            return dbox({base | dim, popup | center}) | flex;
        }
        if (model.busy) {
            std::string dots(static_cast<size_t>(model.working_frame % 4), '.');
            Element busy_box = hbox({
                text(" Working") | bold | color(kInk),
                text(dots) | size(WIDTH, EQUAL, 3) | color(kInk),
                text(" ") | color(kInk)
            }) | borderStyled(LIGHT, kInk) | bgcolor(kPanel);
            return dbox({base | dim, busy_box | center}) | flex;
        }
        return base | flex;
    }), [&](Event event) {
        if (event == Event::Custom) {
            std::optional<Completion> done;
            {
                std::lock_guard<std::mutex> lock(completion_mutex);
                done = std::move(completed);
                completed.reset();
            }
            if (done) {
                ticker_running.store(false);
                if (working_ticker.joinable()) working_ticker.join();
                if (worker.joinable()) worker.join();
                model.status = std::move(done->status);
                model.result = std::move(done->operation);
                model.busy = false;
                model.dialog = done->show_result ? Dialog::result : Dialog::none;
                model.page = 0;
                if (model.quit_after_work) screen.Exit();
            } else if (model.busy) {
                model.working_frame = (model.working_frame + 1) % 4;
            }
            return true;
        }

        if (event == Event::CtrlC) {
            model.quit_after_work = model.busy;
            if (!model.busy) screen.Exit();
            return true;
        }
        if (model.busy) {
            if (event == Event::Escape || event == Event::Character('q') || event == Event::Character('Q')) {
                model.quit_after_work = true;
            }
            return true;
        }
        if (model.dialog == Dialog::result) {
            const bool clicked = event.is_mouse() && event.mouse().button == Mouse::Left &&
                                 event.mouse().motion == Mouse::Pressed;
            if (event == Event::Return || clicked) {
                model.dialog = Dialog::none;
                return true;
            }
            if (event == Event::Escape || event == Event::Character('q') || event == Event::Character('Q')) {
                screen.Exit();
                return true;
            }
            return true;
        }
        if (model.dialog == Dialog::password || model.dialog == Dialog::ssid) {
            if (event == Event::Escape) { cancel_form(); return true; }
            return false;
        }
        if (event == Event::Escape || event == Event::Character('q') || event == Event::Character('Q')) {
            screen.Exit();
            return true;
        }
        return false;
    });

    launch([&] { return OperationResult{Severity::info, 0, "Status refreshed.", std::nullopt}; }, false);
    screen.Loop(root);
    if (worker.joinable()) worker.join();
    return 0;
}

}  // namespace apctl::tui
