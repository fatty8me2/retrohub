/* HUB_SHOT_BOOTART=<n64|gb|gbc|gba|ds|3ds|gc|ps1|ps2|wiiu|all|shelf>: full-window gallery of the boot-animation art over the hub,
 * with a procedural sample cover. Screenshot aid only (headless mode). */
#include "app.h"
#include "boot_art.h"

void bootart_shot_draw(SDL_Renderer* ren) {
    const char* which = std::getenv("HUB_SHOT_BOOTART");
    if (!which || !*which) return;
    static SDL_Texture* cover = nullptr;
    if (!cover && ren) {
        const int N = 128;
        std::vector<uint32_t> px(N * N);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float fx = x / (float)N, fy = y / (float)N;
                int r = (int)(250 - 120 * fy), g = (int)(150 - 70 * fx), b = (int)(60 + 170 * fy);
                float dx = fx - 0.5f, dy = fy - 0.42f;
                if (dx * dx + dy * dy < 0.045f) { r = 255; g = 235; b = 130; }
                if (fy > 0.7f && ((x / 8) % 2)) { r /= 2; g /= 2; b = b * 2 / 3; }
                if (x < 3 || y < 3 || x > N - 4 || y > N - 4) { r = g = b = 250; }
                px[y * N + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        cover = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
        if (cover) SDL_UpdateTexture(cover, nullptr, px.data(), N * 4);
    }
    ImGuiIO& io = ImGui::GetIO();
    bootanim::draw_gallery(ImGui::GetForegroundDrawList(), ImVec2(0, 0), io.DisplaySize, which, (ImTextureID)cover);
}
