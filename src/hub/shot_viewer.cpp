#include "app.h"

#include <shellapi.h>

#include "prefs.h"
#include "savestore.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

/* ---------- Full-size screenshot viewer (modal) ----------
 * Opened by the screenshots strip (quickmenu.cpp shots_gallery, which also serves the details modal's Screenshots tab).
 * Pad: left/right (or LB/RB) = previous/next picture, B = close (or cancel the delete question), up/down + A = actions.
 * Actions: Copy to clipboard (CF_DIB), Set as cover (prefs covers[key]; shown by the L3 "make it yours" work later), Delete (asks),
 * Open in viewer (the default Windows image app), Send to a friend (STUB, see shot_send_to_friend). */

namespace {

struct Viewer {
    bool want_open = false;
    std::vector<std::string> files;   // full paths, newest first
    int idx = 0;
    std::string game_key, game_name;
    ShotTexFn tex;
    bool confirm_delete = false;
};
Viewer V;

/* Decode a PNG/BMP into 32-bit RGBA (top-down). */
bool load_rgba(const std::string& path, std::vector<unsigned char>* px, int* w, int* h) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) return false;
    bool bmp = path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".bmp") == 0;
    if (bmp) {
        SDL_RWops* rw = SDL_RWFromConstMem(data.data(), (int)data.size());
        SDL_Surface* s = rw ? SDL_LoadBMP_RW(rw, 1) : nullptr;
        if (!s) return false;
        SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(s);
        if (!c) return false;
        *w = c->w; *h = c->h;
        px->resize((size_t)c->w * c->h * 4);
        for (int y = 0; y < c->h; ++y) memcpy(px->data() + (size_t)y * c->w * 4, (unsigned char*)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
        SDL_FreeSurface(c);
        return true;
    }
    int n = 0;
    unsigned char* p = stbi_load_from_memory((const unsigned char*)data.data(), (int)data.size(), w, h, &n, 4);
    if (!p) return false;
    px->assign(p, p + (size_t)*w * *h * 4);
    stbi_image_free(p);
    return true;
}

}  // namespace

/* Windows clipboard, CF_DIB: BITMAPINFOHEADER + bottom-up BGR(A) rows. */
bool shot_copy_to_clipboard(const std::string& path) {
    std::vector<unsigned char> rgba; int w = 0, h = 0;
    if (!load_rgba(path, &rgba, &w, &h) || w <= 0 || h <= 0) return false;
    const size_t bytes = (size_t)w * h * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (!mem) return false;
    unsigned char* d = (unsigned char*)GlobalLock(mem);
    if (!d) { GlobalFree(mem); return false; }
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    bi.biSizeImage = (DWORD)bytes;
    memcpy(d, &bi, sizeof bi);
    unsigned char* o = d + sizeof bi;
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = rgba.data() + (size_t)(h - 1 - y) * w * 4;
        for (int x = 0; x < w; ++x, src += 4, o += 4) { o[0] = src[2]; o[1] = src[1]; o[2] = src[0]; o[3] = 255; }   // opaque: a screenshot has no alpha
    }
    GlobalUnlock(mem);
    bool ok = false;
    for (int i = 0; i < 5 && !ok; ++i) {   // another app may hold the clipboard for a moment
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_DIB, mem) != nullptr;
            CloseClipboard();
        } else Sleep(20);
    }
    if (!ok) GlobalFree(mem);   // on success the clipboard owns it
    return ok;
}

/* prefs covers[key] = path (field-wise merge keeps this PC's other prefs; L3 reads it later). */
bool shot_set_cover(const std::string& game_key, const std::string& path) {
    return custom_set_cover(game_key, path);   // customize_ui.cpp: copies it into the vault (synced) and stores that path
}

/* STUB: sending a screenshot to a friend as a DM attachment.
 * Checked: there is no DM client API yet (src/net has no direct-message send) and screenshots are not uploaded to the cloud
 * (cloud_engine: files under shots/ stay local unless set_backup_screenshots is on), so there is no blob to reuse.
 * Needs server work: a DM message type carrying a blob reference in the 50 MB large-item pool, plus a client upload of the PNG.
 * Not built here. */
bool shot_send_to_friend(const std::string& /*path*/) { return false; }

void shot_viewer_open(const hub::GameEntry& e, const std::vector<std::string>& paths, int idx, ShotTexFn tex) {
    if (paths.empty()) return;
    V.files = paths;
    V.idx = std::max(0, std::min(idx, (int)paths.size() - 1));
    V.game_key = e.key;
    V.game_name = e.title;
    V.tex = tex;
    V.confirm_delete = false;
    V.want_open = true;
}

/* Returns true if a picture was deleted (the strip should refresh). */
bool shot_viewer_draw() {
    static const char* kName = "Screenshot##viewer";
    bool deleted = false;
    if (V.want_open) { ImGui::OpenPopup(kName); V.want_open = false; }
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vs.x * 0.94f, vs.y * 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.5f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal(kName, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar(2);
    if (!open) return false;

    const int n = (int)V.files.size();
    if (n == 0) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return false; }
    if (V.idx >= n) V.idx = n - 1;
    const std::string path = V.files[V.idx];
    const std::string nm = std::filesystem::u8path(path).filename().u8string();

    ImGuiIO& io = ImGui::GetIO();
    const bool typing = io.WantTextInput;
    if (!typing && !V.confirm_delete) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadL1, true))
            V.idx = (V.idx + n - 1) % n;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1, true))
            V.idx = (V.idx + 1) % n;
    }
    if (ui::modal_back()) {
        if (V.confirm_delete) V.confirm_delete = false;
        else { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return false; }
    }

    const float side = sp(30.f);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##shotimg", ImVec2(avail.x - side - sp(2.f), avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(0, 0, 0, 255), sp(1.5f));
        int tw = 0, th = 0;
        SDL_Texture* t = V.tex ? V.tex(path, &tw, &th) : nullptr;
        if (t && tw > 0 && th > 0) {
            float sc = std::min(sz.x / tw, sz.y / th);   // letterbox: the whole picture, never cropped
            float w = tw * sc, h = th * sc;
            ImVec2 a(p0.x + (sz.x - w) * 0.5f, p0.y + (sz.y - h) * 0.5f);
            dl->AddImage((ImTextureID)t, a, ImVec2(a.x + w, a.y + h));
        } else {
            ImVec2 ts = ImGui::CalcTextSize("Can't read this picture");
            dl->AddText(ImVec2(p0.x + (sz.x - ts.x) * 0.5f, p0.y + sz.y * 0.5f), ui::TEXT2, "Can't read this picture");
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##shotside", ImVec2(side, avail.y), ImGuiChildFlags_NavFlattened);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::TEXT), "%s", V.game_name.c_str());
    ImGui::TextDisabled("%d of %d", V.idx + 1, n);
    ImGui::TextDisabled("%s", nm.c_str());
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const ImVec2 bsz(-FLT_MIN, sp(5.f));
    if (!V.confirm_delete) {
        if (ui::button("Copy to clipboard", bsz, true, true, ui::Icon::Check)) {
            if (shot_copy_to_clipboard(path)) push_toast("Copied to the clipboard", COL_GOOD, 3.0);
            else push_toast("Couldn't copy that picture", COL_BAD, 4.0);
        }
        ImGui::SetItemDefaultFocus();
        if (ui::button("Set as cover", bsz)) {
            if (shot_set_cover(V.game_key, path)) push_toast("Saved as this game's cover", COL_GOOD, 3.0);
            else push_toast("Couldn't save the cover", COL_BAD, 4.0);
        }
        if (ui::button("Send to a friend", bsz)) {
            if (!shot_send_to_friend(path)) push_toast("Sending pictures to friends isn't available yet", COL_BAD, 4.0);
        }
        if (ui::button("Open in viewer", bsz))
            ShellExecuteW(nullptr, L"open", std::filesystem::u8path(path).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ui::button("Delete", bsz)) V.confirm_delete = true;
        if (ui::button("Close", bsz)) { ImGui::EndChild(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return false; }
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        ImGui::TextDisabled("Left / Right: next picture\nB: close");
    } else {
        ImGui::TextWrapped("Delete this picture for good?");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (ui::button("Yes, delete", bsz, true)) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path), ec);
            V.confirm_delete = false;
            if (!ec) {
                deleted = true;
                V.files.erase(V.files.begin() + V.idx);
                if (V.files.empty()) { ImGui::EndChild(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return true; }
                if (V.idx >= (int)V.files.size()) V.idx = (int)V.files.size() - 1;
            } else push_toast("Couldn't delete that picture", COL_BAD, 4.0);
        }
        if (ui::button("Cancel", bsz)) V.confirm_delete = false;
    }
    ImGui::EndChild();
    ImGui::EndPopup();
    return deleted;
}
