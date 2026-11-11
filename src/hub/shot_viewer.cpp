#include "app.h"

#include <shellapi.h>

#include "prefs.h"
#include "savestore.h"
#include "attach_xfer.h"
#include "friend_pick_ui.h"
#include "recycle_bin.h"

#include <filesystem>
#include <map>
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

/* ---------- Full-size screenshot viewer (modal) ----------
 * Opened by the screenshots strip (quickmenu.cpp shots_gallery, which also serves the details modal's Screenshots tab).
 * Pad: left/right (or LB/RB) = previous/next picture, B = close (or cancel the delete question), up/down + A = actions.
 * Actions: Copy to clipboard (CF_DIB), Set as cover (custom_set_cover: copied into the synced vault and stored as the game's cover),
 * Delete (asks, then moves the file to the Recycle Bin where the drive has one; queued and run at the start of the next frame), Open in viewer (the default Windows image app),
 * Send to a friend (friend picker, then upload + DM, see shot_send_to_friend). */

namespace {

struct Viewer {
    bool want_open = false;
    std::vector<std::string> files;   // full paths, newest first
    int idx = 0;
    std::string game_key, game_name;
    ShotTexFn tex;
    bool confirm_delete = false;
    std::vector<std::string> del_queue;   // confirmed deletes, run at the start of the next frame (outside any Begin/End: the shell can pump messages)
    bool focus_first = false;   // land the pad on the first action each time the viewer (or its delete question) shows
};
Viewer V;

/* Decode a PNG/BMP into 32-bit RGBA (top-down). */
bool load_rgba(const std::string& path, std::vector<unsigned char>* px, int* w, int* h) {
    std::string lp = path;
    if (lp.size() > 4 && _stricmp(lp.c_str() + lp.size() - 4, ".mp4") == 0) lp += ".jpg";   // a clip: use its thumbnail
    std::ifstream f(std::filesystem::u8path(lp), std::ios::binary);
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

/* Set the game's cover to this picture (customize_ui.cpp custom_set_cover: copies it into the vault so it syncs). */
bool shot_set_cover(const std::string& game_key, const std::string& path) {
    return custom_set_cover(game_key, path);   // customize_ui.cpp: copies it into the vault (synced) and stores that path
}

/* "Send to a friend": opens the friend picker (friend_pick_ui.cpp); once friends are ticked the picture is uploaded
 * (hub::attach, <= 8 MB) and each friend gets a DM with the attachment. Returns false when the file cannot be sent at all. */
namespace {
hubui::FriendPicker g_pick;
std::string g_pick_path;
}  // namespace
bool shot_send_to_friend(const std::string& path) {
    if (path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".mp4") == 0) {
        push_toast("Only pictures can be sent to friends, not clips.", COL_BAD, 5.0);
        return false;
    }
    std::error_code ec;
    auto sz = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    if (!ec) {
        std::string why = hub::attach::attach_size_problem((int64_t)sz, "shot");
        if (!why.empty()) { push_toast(why, COL_BAD, 7.0); return false; }
    }
    g_pick_path = path;
    g_pick.open("Send this picture", "Pick who should get it. They see it in your chat and can open it for 7 days (max 8 MB).");
    return true;
}

/* A small cached texture for a picture (chat thumbnails). */
namespace hubui {
SDL_Texture* shot_thumb_tex(const std::string& path, int* w, int* h) {
    struct T { SDL_Texture* t = nullptr; int w = 0, h = 0; unsigned long long used = 0; };
    static std::map<std::string, T> cache;
    static unsigned long long tick = 0;
    auto it = cache.find(path);
    if (it != cache.end()) { it->second.used = ++tick; *w = it->second.w; *h = it->second.h; return it->second.t; }
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (!ren) return nullptr;
    while (cache.size() >= 24) {
        auto old = cache.begin();
        for (auto i = cache.begin(); i != cache.end(); ++i) if (i->second.used < old->second.used) old = i;
        if (old->second.t) SDL_DestroyTexture(old->second.t);
        cache.erase(old);
    }
    T nt; nt.used = ++tick;
    std::vector<unsigned char> px; int pw = 0, ph = 0;
    if (load_rgba(path, &px, &pw, &ph) && pw > 0 && ph > 0) {
        nt.t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, pw, ph);
        if (nt.t) { SDL_UpdateTexture(nt.t, nullptr, px.data(), pw * 4); SDL_SetTextureBlendMode(nt.t, SDL_BLENDMODE_BLEND); nt.w = pw; nt.h = ph; }
    }
    cache[path] = nt;
    *w = nt.w; *h = nt.h;
    return nt.t;
}
}  // namespace hubui

void shot_viewer_open(const hub::GameEntry& e, const std::vector<std::string>& paths, int idx, ShotTexFn tex) {
    if (paths.empty()) return;
    V.files = paths;
    V.idx = std::max(0, std::min(idx, (int)paths.size() - 1));
    V.game_key = e.key;
    V.game_name = e.title;
    V.tex = tex;
    V.confirm_delete = false;
    V.want_open = true;
    V.focus_first = true;
}

static bool shot_viewer_draw_inner();

/* Returns true if a picture was deleted (the strip should refresh). The delete itself runs here, at the start of the frame after the
 * confirm click, before any ImGui window of the viewer is open. */
bool shot_viewer_draw() {
    bool deleted = false;
    if (!V.del_queue.empty()) {
        std::vector<std::string> q; q.swap(V.del_queue);
        int failed = 0;
        for (const std::string& f : q) if (!hubrb::delete_user_file(f)) ++failed;
        deleted = true;   // rescan either way: a file that could not be deleted shows up again
        if (failed) push_toast("Couldn't delete that picture", COL_BAD, 4.0);
    }
    shot_viewer_draw_inner();
    return deleted;
}

static bool shot_viewer_draw_inner() {
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
    const bool is_clip = path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".mp4") == 0;

    ImGuiIO& io = ImGui::GetIO();
    const bool typing = io.WantTextInput;
    if (!typing && !V.confirm_delete) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadL1, true))
            V.idx = (V.idx + n - 1) % n;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1, true))
            V.idx = (V.idx + 1) % n;
    }
    if (ui::modal_back()) {
        if (V.confirm_delete) { V.confirm_delete = false; V.focus_first = true; }
        else { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return false; }
    }

    if (g_headless && std::getenv("HUB_SHOT_FRIEND_PICK")) {   // screenshot aid: open the friend picker over the viewer once
        static bool once = false;
        if (!once && !V.files.empty()) { once = true; shot_send_to_friend(V.files[(size_t)V.idx]); }
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
        if (V.focus_first) { ImGui::SetKeyboardFocusHere(0); V.focus_first = false; }
        if (ui::button("Copy to clipboard", bsz, true, true, ui::Icon::Check)) {
            if (shot_copy_to_clipboard(path)) push_toast("Copied to the clipboard", COL_GOOD, 3.0);
            else push_toast("Couldn't copy that picture", COL_BAD, 4.0);
        }
        if (ui::button("Set as cover", bsz)) {
            if (shot_set_cover(V.game_key, is_clip ? path + ".jpg" : path)) push_toast("Saved as this game's cover", COL_GOOD, 3.0);
            else push_toast("Couldn't save the cover", COL_BAD, 4.0);
        }
        if (ui::button("Send to a friend", bsz)) shot_send_to_friend(path);
        if (g_pick.draw("Send to friends##shotpick") && !g_pick_path.empty()) {
            std::string fname = std::filesystem::u8path(g_pick_path).filename().u8string();
            hubui::attach_send_start(g_pick_path, "shot", fname, g_pick.chosen(),
                                     V.game_name.empty() ? std::string("Sent a screenshot") : "Sent a screenshot of " + V.game_name);
        }
        { std::string st = hubui::attach_send_status(); if (!st.empty()) ImGui::TextDisabled("%s", st.c_str()); }
        if (ui::button(is_clip ? "Play clip" : "Open in viewer", bsz, is_clip, true, is_clip ? ui::Icon::Play : ui::Icon::None))
            ShellExecuteW(nullptr, L"open", std::filesystem::u8path(path).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ui::button("Delete", bsz)) { V.confirm_delete = true; V.focus_first = true; }
        if (ui::button("Close", bsz)) { ImGui::EndChild(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return false; }
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        ImGui::TextDisabled("Left / Right: next picture\nB: close");
    } else {
        /* Network and removable drives (or a switched-off bin) delete for good: say so instead of promising the Recycle Bin. */
        const bool to_bin = hubrb::recycle_expected(path);
        ImGui::TextWrapped("%s", to_bin ? "Move this picture to the Recycle Bin?" : "Delete this picture? This cannot be undone.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (V.focus_first) { ImGui::SetKeyboardFocusHere(0); V.focus_first = false; }
        if (ui::button(to_bin ? "Yes, move it" : "Yes, delete", bsz, true)) {
            V.del_queue.push_back(path);   // the shell delete runs at the start of the next frame (shot_viewer_draw), outside Begin/End
            if (is_clip) V.del_queue.push_back(path + ".jpg");
            V.confirm_delete = false;
            V.files.erase(V.files.begin() + V.idx);
            if (V.files.empty()) { ImGui::EndChild(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return false; }
            if (V.idx >= (int)V.files.size()) V.idx = (int)V.files.size() - 1;
        }
        if (ui::button("Cancel", bsz)) { V.confirm_delete = false; V.focus_first = true; }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
    return deleted;
}
