/* Resume + save states UI: the Home "Resume" button, library sort cycling and the details-modal strip of save states
 * (slots 0-9 + auto with thumbnail, date and play time; Resume from this / Delete). Pure listing logic lives in state_slots.cpp.
 * Screenshot aid: HUB_SHOT_STATES=1 seeds fake states + thumbnails + play history for the fake library (fake:1 GBA, fake:4 GB). */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#include "app.h"
#include "../saves/savestore.h"
#include "state_slots.h"

namespace {

struct Tex {
    SDL_Texture* t = nullptr;
    int w = 0, h = 0;
    uint64_t stamp = 0;
};
std::map<std::string, Tex> g_tex;

SDL_Texture* thumb_tex(const hubui::StateSlot& s, int* w, int* h) {
    if (s.thumb_path.empty()) return nullptr;
    auto it = g_tex.find(s.thumb_path);
    if (it != g_tex.end() && it->second.stamp == s.time + s.bytes) { *w = it->second.w; *h = it->second.h; return it->second.t; }
    if (it != g_tex.end()) { if (it->second.t) SDL_DestroyTexture(it->second.t); g_tex.erase(it); }
    if (g_tex.size() > 48) state_textures_clear();
    Tex t;
    t.stamp = s.time + s.bytes;
    std::ifstream f(std::filesystem::u8path(s.thumb_path), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int iw = 0, ih = 0, comp = 0;
    unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &iw, &ih, &comp, 4);
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (px && ren && iw > 0 && ih > 0) {
        t.t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
        if (t.t) {
            SDL_UpdateTexture(t.t, nullptr, px, iw * 4);
            SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
            t.w = iw;
            t.h = ih;
        }
    }
    if (px) stbi_image_free(px);
    g_tex[s.thumb_path] = t;
    *w = t.w;
    *h = t.h;
    return t.t;
}

std::string slot_path(const hub::GameEntry& e, int slot) {
    std::string d = with_slash(hub::saves::states_dir(sys_short(e), e.key));
    return slot < 0 ? d + "auto.state" : d + "slot" + std::to_string(slot) + ".state";
}

}  // namespace

void state_textures_clear() {
    for (auto& kv : g_tex) if (kv.second.t) SDL_DestroyTexture(kv.second.t);
    g_tex.clear();
}

bool has_resume(const hub::GameEntry& e) {
    if (!hubui::system_has_states(sys_short(e), is_native(e))) return false;
    struct C { double at = -100; bool v = false; };
    static std::map<std::string, C> cache;
    C& c = cache[e.key];
    double now = ImGui::GetTime();
    if (now - c.at > 3.0 || now < c.at) { c.at = now; c.v = file_exists(slot_path(e, -1)); }
    return c.v;
}

void resume_entry(const hub::GameEntry& e, int slot) {
    std::string p = slot_path(e, slot);
    A.resume_key.clear();
    A.resume_state.clear();
    if (hubui::system_has_states(sys_short(e), is_native(e)) && file_exists(p)) {
        A.resume_key = e.key;
        A.resume_state = p;
    }
    shot_trace("resume", e.key + " slot=" + std::to_string(slot) + " state=" + (A.resume_key.empty() ? "none" : p));
    hub::GameEntry c = e;
    play_entry(c);
}

void cycle_sort(int dir) {
    hubui::SortMode m = hubui::next_sort(hubui::sort_from_key(A.st.library_sort), dir);
    A.st.library_sort = hubui::sort_key(m);
    mark_dirty();
    A.vis_dirty = true;
    A.grid_scroll_top = true;
    shot_trace("sort", A.st.library_sort);
}

void draw_state_strip(const hub::GameEntry& e) {
    const std::string sys = sys_short(e);
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Save states");
    ImGui::PopFont();
    if (!hubui::system_has_states(sys, is_native(e))) {
        text_wrapped_disabled(hubui::no_states_reason(sys, is_native(e)));
        shot_trace_once("act", e.key + " states explained:" + (is_native(e) ? "native" : "external-emulator"));
        return;
    }
    shot_trace_once("act", e.key + " states enabled count=" + std::to_string(hubui::count_states(A.info_states, true)));
    const std::string dir = hub::saves::states_dir(sys, e.key);
    const bool idle = A.proc == nullptr;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto find_slot = [&](int s) -> const hubui::StateSlot* {
        for (auto& x : A.info_states) if (x.slot == s) return &x;
        return nullptr;
    };
    if (A.state_sel == -3 || !find_slot(A.state_sel)) {
        A.state_sel = -1;
        for (auto& x : A.info_states) if (x.present) { A.state_sel = x.slot; break; }
        A.state_confirm = -2;
    }

    const float gap = sp(1.f), avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail > sp(70.f) ? 6 : 4;
    const float cw = (avail - gap * (cols - 1)) / cols, th = cw * 0.625f, cap = ui::F.cap->FontSize, ch = th + cap * 2.6f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    int n = 0;
    for (auto& s : A.info_states) {
        if (n % cols) ImGui::SameLine();
        ++n;
        ImGui::PushID(s.slot + 2);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("cell", ImVec2(cw, ch));
        bool focused = ImGui::IsItemFocused();
        if (clicked || focused) {
            if (A.state_sel != s.slot) A.state_confirm = -2;
            A.state_sel = s.slot;
        }
        bool sel = A.state_sel == s.slot;
        ImVec2 t1(p0.x + cw, p0.y + th), p1(p0.x + cw, p0.y + ch);
        float rad = sp(1.f);
        dl->AddRectFilled(p0, p1, sel ? ui::PANEL3 : ui::PANEL2, rad);
        int tw = 0, thh = 0;
        SDL_Texture* tex = s.present ? thumb_tex(s, &tw, &thh) : nullptr;
        if (tex) {
            ui::draw_art(dl, tex, tw, thh, p0, t1, rad);
        } else if (s.present) {
            dl->AddRectFilled(p0, t1, ui::mix(ui::ACCENT, ui::BG, 0.75f), rad, ImDrawFlags_RoundCornersTop);
            ui::icon(dl, ui::Icon::Play, ImVec2(p0.x + cw * 0.5f, p0.y + th * 0.5f), sp(2.f), ui::TEXT3);
        } else {
            const char* em = "Empty";
            ImVec2 ts = ui::F.cap->CalcTextSizeA(cap, FLT_MAX, 0.f, em);
            dl->AddText(ui::F.cap, cap, ImVec2(p0.x + (cw - ts.x) * 0.5f, p0.y + (th - cap) * 0.5f), ui::TEXT3, em);
        }
        dl->AddText(ui::F.cap, cap, ImVec2(p0.x + sp(0.75f), t1.y + cap * 0.25f), s.present ? ui::TEXT : ui::TEXT3, hubui::state_slot_label(s.slot).c_str());
        if (s.present)
            dl->AddText(ui::F.cap, cap, ImVec2(p0.x + sp(0.75f), t1.y + cap * 1.35f), ui::TEXT2,
                        ui::ellipsize(ui::F.cap, cap, hubui::ago_text(unix_now(), s.time), cw - sp(1.5f)).c_str());
        if (sel || focused) dl->AddRect(p0, p1, ui::ACCENT, rad, 0, focused ? 2.5f : 1.5f);
        else dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    const hubui::StateSlot* cur = find_slot(A.state_sel);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!cur || !cur->present) {
        if (hubui::count_states(A.info_states, true) == 0)
            text_wrapped_disabled("No save states yet. States you save in-game (and the automatic resume point when you quit) show up here.");
        else
            text_wrapped_disabled(hubui::state_slot_label(A.state_sel) + " is empty.");
        return;
    }
    std::string info = hubui::state_slot_label(cur->slot) + "   " + fmt_time(cur->time);
    if (cur->playtime) info += "   -   played " + hubui::playtime_text(cur->playtime);
    info += "   -   " + size_text(cur->bytes);
    ImGui::TextUnformatted(info.c_str());
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    float bh = sp(5.f);
    if (ui::button("Resume from this", ImVec2(0, bh), true, idle, ui::Icon::Play)) {
        ImGui::CloseCurrentPopup();
        resume_entry(e, cur->slot);
    }
    ImGui::SameLine(0, sp(1.5f));
    const bool confirm = A.state_confirm == cur->slot;
    if (ui::button(confirm ? "Really delete?" : "Delete", ImVec2(0, bh), false, true, ui::Icon::Close)) {
        if (confirm) {
            bool ok = hubui::delete_state(dir, cur->slot);
            shot_trace("state-delete", e.key + " slot=" + std::to_string(cur->slot) + (ok ? " ok" : " failed"));
            A.state_confirm = -2;
            refresh_save_info(e);
            A.state_sel = -3;
        } else {
            A.state_confirm = cur->slot;
        }
    }
    if (!idle) ImGui::SameLine(0, sp(1.5f)), text_wrapped_disabled("A game is running.");
}

/* ---- screenshot aid ---- */
namespace {
uint32_t crc32_of(const unsigned char* d, size_t n, uint32_t c = 0) {
    c = ~c;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
void put_be(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s += (char)(v >> (8 * i)); }
void chunk(std::string& out, const char* type, const std::string& data) {
    put_be(out, (uint32_t)data.size());
    std::string td = std::string(type, 4) + data;
    out += td;
    put_be(out, crc32_of((const unsigned char*)td.data(), td.size()));
}
/* Uncompressed (stored-deflate) RGB PNG of a diagonal gradient with a bright block: enough for a thumbnail. */
std::string fake_png(int w, int h, int seed) {
    std::string raw;
    for (int y = 0; y < h; ++y) {
        raw += (char)0;
        for (int x = 0; x < w; ++x) {
            bool block = x > w / 3 && x < w * 2 / 3 && y > h / 2 && y < h * 5 / 6;
            int r = (x * 255 / w + seed * 40) & 255, g = (y * 255 / h + seed * 25) & 255, b = (140 + seed * 17) & 255;
            if (block) { r = 240; g = 200; b = 60; }
            raw += (char)r; raw += (char)g; raw += (char)b;
        }
    }
    std::string z = "\x78\x01";
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - i);
        z += (char)(i + n >= raw.size() ? 1 : 0);
        z += (char)(n & 255); z += (char)(n >> 8); z += (char)(~n & 255); z += (char)((~n >> 8) & 255);
        z.append(raw, i, n);
    }
    put_be(z, (b << 16) | a);
    std::string png = "\x89PNG\r\n\x1a\n", ihdr;
    put_be(ihdr, (uint32_t)w); put_be(ihdr, (uint32_t)h);
    ihdr += (char)8; ihdr += (char)2; ihdr += (char)0; ihdr += (char)0; ihdr += (char)0;
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}
void write_file(const std::string& p, const std::string& s) {
    std::ofstream f(std::filesystem::u8path(p), std::ios::binary);
    f << s;
}
}  // namespace

void states_shot_seed() {
    if (!g_headless || !std::getenv("HUB_SHOT_STATES")) return;
    struct G { const char* key; const char* sys; };
    const uint64_t now = unix_now();
    int gi = 0;
    for (G g : {G{"fake:1", "gba"}, G{"fake:4", "gb"}}) {
        std::string dir = with_slash(hub::saves::states_dir(g.sys, g.key));
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        const int slots[] = {-1, 0, 1, 3, 4, 7};
        for (int s : slots) {
            if (gi == 1 && s > 1) continue;   // the second game has only a couple
            std::string b = dir + (s < 0 ? std::string("auto") : "slot" + std::to_string(s));
            write_file(b + ".state", std::string(40000 + s * 1000, 'x'));
            if (s != 4) write_file(b + ".png", fake_png(160, 100, s + 2 + gi));   // slot 4: state without a thumbnail
            uint64_t t = now - (s < 0 ? 600 : 3600ull * (s + 2) * 5);
            write_file(b + ".json", "{\"game_key\":\"" + std::string(g.key) + "\",\"time\":" + std::to_string(t) + ",\"playtime\":" + std::to_string(s < 0 ? 5025 : 700 + s * 311) + ",\"disc\":0}");
        }
        hub::record_play(g.key, now - 2 * 3600 - gi * 86400ull, 5400 + gi * 120000);
        ++gi;
    }
}
