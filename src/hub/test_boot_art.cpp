// Console test: every hub::System maps to boot-animation art (console + media) and draws real geometry without crashing.
#include "boot_art.h"
#include <cstdio>
#include <cmath>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using hub::System;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 800);
    unsigned char* pix; int fw, fh; io.Fonts->GetTexDataAsRGBA32(&pix, &fw, &fh);
    ImGui::NewFrame();
    ImDrawList* d = ImGui::GetForegroundDrawList();

    const System all[] = {System::Unknown, System::N64, System::GBA, System::GB, System::GBC, System::GC, System::N3DS, System::WiiU, System::NDS, System::PS2, System::PS1};
    for (System s : all) {
        const char* nm = bootanim::art_name(s);
        CHECK(nm && *nm);
        if (s != System::Unknown) CHECK(bootanim::media_for(s) != bootanim::Media::None);
        if (s != System::Unknown) { CHECK(bootanim::art_dedicated(s)); CHECK(bootanim::art_system_from_name(nm) == s); }
        for (float lid : {0.f, 0.5f, 1.f}) {
            int v0 = d->VtxBuffer.Size;
            bootanim::draw_console(s, d, ImVec2(400, 400), 1.f, lid, lid);
            CHECK(d->VtxBuffer.Size - v0 > 150);
        }
        int v0 = d->VtxBuffer.Size;
        bootanim::draw_media(s, d, ImVec2(400, 400), 1.f, 0.3f, (ImTextureID)0, ImVec2(0, 0), ImVec2(1, 1));
        CHECK(d->VtxBuffer.Size - v0 > 60);
        bootanim::draw_media(s, d, ImVec2(400, 400), 1.f, 0.3f, (ImTextureID)(intptr_t)7, ImVec2(0, 0), ImVec2(1, 1));
    }
    bootanim::draw_shelf(d, ImVec2(0, 0), ImVec2(1280, 800), 1.5f);
    bootanim::draw_gallery(d, ImVec2(0, 0), ImVec2(1280, 800), "all", (ImTextureID)0);
    for (int i = 0; i < d->VtxBuffer.Size; i++) { CHECK(std::isfinite(d->VtxBuffer[i].pos.x) && std::isfinite(d->VtxBuffer[i].pos.y)); }
    CHECK(bootanim::art_system_from_name("PS2") == System::PS2);
    CHECK(bootanim::art_system_from_name("nope") == System::Unknown);
    ImGui::EndFrame();
    ImGui::DestroyContext();
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("boot_art_test OK\n");
    return 0;
}
