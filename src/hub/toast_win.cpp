/* Real toast notifier: C++/WinRT ToastNotificationManager for the AUMID "RetroHub.Launcher". Windows 10+; no extra
 * runtime dependency. Toasts never take focus; Focus Assist suppresses them automatically. */
#include "toast.h"

#include <chrono>

#include <windows.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>

#include "shell_integration.h"

namespace hubtoast {
namespace {
struct SystemNotifier : INotifier {
    bool available() const override {
        static const bool ok = shell_registration_allowed();   // only installed copies have the Start-menu shortcut/AUMID
        return ok;
    }
    bool show(const std::string& xml, const std::string& tag) override {
        if (!available()) return false;
        try {
            using namespace winrt::Windows::UI::Notifications;
            winrt::Windows::Data::Xml::Dom::XmlDocument doc;
            doc.LoadXml(winrt::to_hstring(xml));
            ToastNotification t(doc);
            t.Tag(winrt::to_hstring(tag));
            t.ExpirationTime(winrt::clock::now() + std::chrono::minutes(30));
            ToastNotificationManager::CreateToastNotifier(RETROHUB_APP_ID).Show(t);
            return true;
        } catch (...) { return false; }
    }
};
}  // namespace

INotifier& system_notifier() { static SystemNotifier n; return n; }

}  // namespace hubtoast
