#include "recycle_bin.h"

#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace hubrb {

std::wstring shfile_path_buffer(const std::wstring& path) {
    std::wstring w = path;
    w.push_back(L'\0');
    w.push_back(L'\0');
    return w;
}

bool drive_type_has_bin(unsigned drive_type) { return drive_type == 3; }   // DRIVE_FIXED

bool recycle_expected(const std::string& utf8_path) {
#ifdef _WIN32
    std::error_code ec;
    fs::path abs = fs::absolute(fs::u8path(utf8_path), ec);
    if (ec) return false;
    std::wstring root(MAX_PATH + 1, L'\0');
    if (!GetVolumePathNameW(abs.make_preferred().wstring().c_str(), root.data(), (DWORD)root.size())) return false;
    root.resize(wcslen(root.c_str()));
    if (!drive_type_has_bin(GetDriveTypeW(root.c_str()))) return false;
    SHQUERYRBINFO info{};
    info.cbSize = sizeof info;
    return SHQueryRecycleBinW(root.c_str(), &info) == S_OK;   // fails on a drive without a bin
#else
    (void)utf8_path;
    return false;
#endif
}

bool move_to_recycle_bin(const std::string& utf8_path) {
#ifdef _WIN32
    std::error_code ec;
    fs::path abs = fs::absolute(fs::u8path(utf8_path), ec);   // undo (the bin) needs a full path
    if (ec) return false;
    std::wstring from = shfile_path_buffer(abs.make_preferred().wstring());
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    int rc = SHFileOperationW(&op);
    return rc == 0 && !op.fAnyOperationsAborted && !fs::exists(abs, ec);
#else
    (void)utf8_path;
    return false;
#endif
}

bool delete_user_file(const std::string& utf8_path, const DeleteFn& recycle) {
    fs::path p = fs::u8path(utf8_path);
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    if ((recycle ? recycle : DeleteFn(move_to_recycle_bin))(utf8_path)) return true;
    fs::remove(p, ec);   // the bin refused: the old permanent delete
    return !ec && !fs::exists(p, ec);
}

}  // namespace hubrb
