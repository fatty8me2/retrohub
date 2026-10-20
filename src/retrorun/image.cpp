#include "image.h"

#include <cstdio>
#include <filesystem>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

static void to_file(void* ctx, void* data, int size) { fwrite(data, 1, (size_t)size, (FILE*)ctx); }

bool write_frame_image(const std::string& path, const FrameBuf& f) {
    if (f.w <= 0 || f.h <= 0 || f.px.size() < (size_t)f.w * f.h) return false;
    std::vector<uint8_t> rgb((size_t)f.w * f.h * 3);
    for (size_t i = 0; i < (size_t)f.w * f.h; i++) {
        uint32_t p = f.px[i];
        rgb[i * 3 + 0] = (uint8_t)(p >> 16);
        rgb[i * 3 + 1] = (uint8_t)(p >> 8);
        rgb[i * 3 + 2] = (uint8_t)p;
    }
    std::filesystem::path fp = std::filesystem::u8path(path);
    if (fp.has_parent_path()) { std::error_code ec; std::filesystem::create_directories(fp.parent_path(), ec); }
    FILE* fh = _wfopen(fp.c_str(), L"wb");
    if (!fh) return false;
    bool bmp = fp.extension() == ".bmp" || fp.extension() == ".BMP";
    int ok = bmp ? stbi_write_bmp_to_func(to_file, fh, f.w, f.h, 3, rgb.data())
                 : stbi_write_png_to_func(to_file, fh, f.w, f.h, 3, rgb.data(), f.w * 3);
    fclose(fh);
    return ok != 0;
}
