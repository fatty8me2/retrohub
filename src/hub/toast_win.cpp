/* Real toast notifier: C++/WinRT ToastNotificationManager for the AUMID "RetroHub.Launcher". Windows 10+; no extra
 * runtime dependency. Toasts never take focus; Focus Assist suppresses them automatically. */
#include "toast.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <windows.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>

#include "popup_guard.h"
#include "shell_integration.h"

namespace hubtoast {
namespace {
void log_err(const char* what, const std::string& msg) {
    std::string line = std::string("[toast] ") + what + ": " + msg + '\n';
    OutputDebugStringA(line.c_str());
    std::fputs(line.c_str(), stderr);
}

/* C++/WinRT needs an initialised apartment on the calling thread. The UI thread already has SDL's/OLE's apartment
 * (STA), so toasts are shown from a short-lived worker thread that owns its own MTA: no apartment-type conflict. */
template <class F>
bool run_on_mta_thread(F&& f) {
    bool ok = false;
    std::thread th([&] {
        std::string err;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            ok = f(&err);
            if (!ok && !err.empty()) log_err("failed", err);
        } catch (const winrt::hresult_error& e) {
            log_err("winrt error", winrt::to_string(e.message()) + " (0x" + std::to_string((unsigned)e.code().value) + ")");
        } catch (const std::exception& e) { log_err("exception", e.what()); }
        catch (...) { log_err("exception", "unknown"); }
        try { winrt::uninit_apartment(); } catch (...) {}
    });
    th.join();
    return ok;
}

bool load_xml(const std::string& xml, winrt::Windows::Data::Xml::Dom::XmlDocument& doc, std::string* err) {
    doc.LoadXml(winrt::to_hstring(xml));
    if (!doc.DocumentElement()) { *err = "empty toast XML"; return false; }
    return true;
}

bool show_impl(const std::string& xml, const std::string& tag, std::string* err) {
    using namespace winrt::Windows::UI::Notifications;
    winrt::Windows::Data::Xml::Dom::XmlDocument doc;
    if (!load_xml(xml, doc, err)) return false;
    ToastNotification t(doc);
    t.Tag(winrt::to_hstring(tag));
    t.ExpirationTime(winrt::clock::now() + std::chrono::minutes(30));
    ToastNotificationManager::CreateToastNotifier(RETROHUB_APP_ID).Show(t);
    return true;
}

bool cancel_impl(const std::string& tag, const std::string& group) {
    using namespace winrt::Windows::UI::Notifications;
    auto notifier = ToastNotificationManager::CreateToastNotifier(RETROHUB_APP_ID);
    const winrt::hstring t = winrt::to_hstring(tag), g = winrt::to_hstring(group);
    for (auto const& s : notifier.GetScheduledToastNotifications())
        if (s.Tag() == t && s.Group() == g) notifier.RemoveFromSchedule(s);
    return true;
}

bool schedule_impl(const std::string& xml, const std::string& tag, const std::string& group, int64_t fire_unix_ms, std::string* err) {
    using namespace winrt::Windows::UI::Notifications;
    winrt::Windows::Data::Xml::Dom::XmlDocument doc;
    if (!load_xml(xml, doc, err)) return false;
    cancel_impl(tag, group);   // one pending toast per tag
    ScheduledToastNotification st(doc, winrt::clock::from_time_t((time_t)(fire_unix_ms / 1000)));
    st.Tag(winrt::to_hstring(tag));
    st.Group(winrt::to_hstring(group));
    ToastNotificationManager::CreateToastNotifier(RETROHUB_APP_ID).AddToSchedule(st);
    return true;
}

bool (*g_game_probe)() = nullptr;

struct SystemNotifier : INotifier {
    bool available() const override {
        static const bool ok = shell_registration_allowed();   // only installed copies have the Start-menu shortcut/AUMID
        return ok;
    }
    bool show(const std::string& xml, const std::string& tag) override {
        if (!available()) return false;
        /* Second lock on the one rule (should_toast is the first): whatever a call site passes, a Windows toast (banner and notification
         * sound) is never raised while a game runs. The caller's bell entry / the runner OSD carries the message. */
        if (g_game_probe && !hubguard::may_windows_toast(g_game_probe())) return false;
        return run_on_mta_thread([&](std::string* err) { return show_impl(xml, tag, err); });
    }
};
/* Scheduling shows nothing now: Windows delivers it later, so the caller only keeps it pending while no game or session is up. */
struct SystemScheduler : IScheduler {
    bool available() const override {
        static const bool ok = shell_registration_allowed();
        return ok;
    }
    bool schedule(const std::string& xml, const std::string& tag, const std::string& group, int64_t fire_unix_ms) override {
        if (!available()) return false;
        /* Same gate as show(): a toast Windows delivers later is still a toast, so never register one while a game runs. */
        if (g_game_probe && !hubguard::may_windows_toast(g_game_probe())) return false;
        return run_on_mta_thread([&](std::string* err) { return schedule_impl(xml, tag, group, fire_unix_ms, err); });
    }
    bool cancel(const std::string& tag, const std::string& group) override {
        if (!available()) return true;   // nothing could have been scheduled
        return run_on_mta_thread([&](std::string*) { return cancel_impl(tag, group); });
    }
};
}  // namespace

IScheduler& system_scheduler() { static SystemScheduler s; return s; }

void set_game_probe(bool (*fn)()) { g_game_probe = fn; }

INotifier& system_notifier() { static SystemNotifier n; return n; }

/* Test hook: exercises apartment init + XML load on the same worker-thread path as show(), without showing anything. */
bool toast_winrt_selftest(const std::string& xml, std::string* err) {
    std::string e;
    bool ok = run_on_mta_thread([&](std::string* er) {
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        return load_xml(xml, doc, er);
    });
    if (!ok && err) *err = "winrt init/xml load failed (see [toast] log)";
    return ok;
}

}  // namespace hubtoast
