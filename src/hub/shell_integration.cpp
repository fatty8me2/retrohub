/* Windows shell integration (see shell_integration.h). */
#include "shell_integration.h"
#include "resource.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <objbase.h>

#include <algorithm>

namespace {

std::wstring exe_path() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

std::wstring dir_of(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

bool ieq(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

struct ComScope {   // COM may already be initialised by the caller (S_FALSE / RPC_E_CHANGED_MODE are fine)
    bool own;
    ComScope() { HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); own = SUCCEEDED(hr); }
    ~ComScope() { if (own) CoUninitialize(); }
};

template <class T> struct Rel {
    T* p = nullptr;
    ~Rel() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

HICON load_icon(int px) {
    return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, px, px, LR_DEFAULTCOLOR | LR_SHARED);
}

}   // namespace

void shell_set_app_id() {
    SetCurrentProcessExplicitAppUserModelID(RETROHUB_APP_ID);
}

void shell_set_window_icon(SDL_Window* win) {
    if (!win) return;
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm)) return;
    HWND hwnd = wm.info.win.window;
    if (!hwnd) return;
    UINT dpi = 96;
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        using Fn = UINT(WINAPI*)(HWND);
        if (auto fn = (Fn)GetProcAddress(u, "GetDpiForWindow")) { UINT d = fn(hwnd); if (d) dpi = d; }
    }
    int small_px = std::max(16, MulDiv(16, (int)dpi, 96));   // 16 @100%, 24 @150%, 32 @200% (all hand-hinted ICO entries)
    int big_px = dpi >= 168 ? 256 : 48;                        // alt-tab / large taskbar
    if (HICON big = load_icon(big_px)) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (HICON sm = load_icon(small_px)) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
}

bool shell_read_shortcut(const std::wstring& lnk_path, ShellLinkInfo* out) {
    ComScope com;
    Rel<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return false;
    Rel<IPersistFile> pf;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(pf.put())))) return false;
    if (FAILED(pf->Load(lnk_path.c_str(), STGM_READ))) return false;
    ShellLinkInfo r;
    wchar_t buf[MAX_PATH * 2];
    const int N = (int)(sizeof(buf) / sizeof(buf[0]));
    if (SUCCEEDED(link->GetPath(buf, N, nullptr, SLGP_RAWPATH))) r.target = buf;
    if (SUCCEEDED(link->GetWorkingDirectory(buf, N))) r.workdir = buf;
    if (SUCCEEDED(link->GetDescription(buf, N))) r.description = buf;
    int idx = 0;
    if (SUCCEEDED(link->GetIconLocation(buf, N, &idx))) { r.icon = buf; r.icon_index = idx; }
    Rel<IPropertyStore> ps;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(ps.put())))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) r.app_id = pv.pwszVal;
        PropVariantClear(&pv);
    }
    if (out) *out = r;
    return true;
}

bool shell_write_shortcut(const std::wstring& lnk_path, const std::wstring& exe, bool* ok) {
    if (ok) *ok = false;
    const std::wstring workdir = dir_of(exe);
    const std::wstring desc = L"RetroHub - your retro game library";
    ShellLinkInfo cur;   // already up to date?
    if (shell_read_shortcut(lnk_path, &cur) && ieq(cur.target, exe) && ieq(cur.workdir, workdir) && ieq(cur.icon, exe) &&
        cur.description == desc && cur.app_id == RETROHUB_APP_ID) {
        if (ok) *ok = true;
        return false;
    }
    ComScope com;
    Rel<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return false;
    link->SetPath(exe.c_str());
    link->SetWorkingDirectory(workdir.c_str());
    link->SetDescription(desc.c_str());
    link->SetIconLocation(exe.c_str(), 0);   // IDI_APPICON is the lowest-numbered icon -> index 0
    Rel<IPropertyStore> ps;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(ps.put())))) {
        PROPVARIANT pv;
        if (SUCCEEDED(InitPropVariantFromString(RETROHUB_APP_ID, &pv))) {
            ps->SetValue(PKEY_AppUserModel_ID, pv);
            ps->Commit();
            PropVariantClear(&pv);
        }
    }
    Rel<IPersistFile> pf;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(pf.put())))) return false;
    std::wstring d = dir_of(lnk_path);
    if (!d.empty()) SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    if (FAILED(pf->Save(lnk_path.c_str(), TRUE))) return false;
    if (ok) *ok = true;
    return true;
}

void shell_ensure_start_menu_shortcut() {
    std::wstring programs = known_folder(FOLDERID_Programs);   // %APPDATA%\Microsoft\Windows\Start Menu\Programs
    if (programs.empty()) return;
    bool ok = false;
    shell_write_shortcut(programs + L"\\RetroHub.lnk", exe_path(), &ok);
}

void shell_ensure_desktop_shortcut(bool create) {
    std::wstring desk = known_folder(FOLDERID_Desktop);
    if (desk.empty()) return;
    std::wstring lnk = desk + L"\\RetroHub.lnk";
    if (create) {
        bool ok = false;
        shell_write_shortcut(lnk, exe_path(), &ok);
    } else {
        ShellLinkInfo cur;   // only delete a shortcut that is ours
        if (shell_read_shortcut(lnk, &cur) && cur.app_id == RETROHUB_APP_ID) DeleteFileW(lnk.c_str());
    }
}
