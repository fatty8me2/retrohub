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
#include <winrt/Windows.UI.Notifications.h>

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

struct SystemNotifier : INotifier {
    bool available() const override {
        static const bool ok = shell_registration_allowed();   // only installed copies have the Start-menu shortcut/AUMID
        return ok;
    }
    bool show(const std::string& xml, const std::string& tag) override {
        if (!available()) return false;
        return run_on_mta_thread([&](std::string* err) { return show_impl(xml, tag, err); });
    }
};
}  // namespace

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
