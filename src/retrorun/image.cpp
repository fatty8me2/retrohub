#include "image.h"

#include <algorithm>
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

bool write_frame_image_scaled(const std::string& path, const FrameBuf& f, int max_w) {
    if (f.w <= max_w || max_w <= 0) return write_frame_image(path, f);
    if (f.h <= 0 || f.px.size() < (size_t)f.w * f.h) return false;
    FrameBuf o;
    o.w = max_w;
    o.h = std::max(1, (int)((long long)f.h * max_w / f.w));
    o.px.resize((size_t)o.w * o.h);
    for (int y = 0; y < o.h; y++) {
        int y0 = (int)((long long)y * f.h / o.h), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * f.h / o.h));
        for (int x = 0; x < o.w; x++) {
            int x0 = (int)((long long)x * f.w / o.w), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * f.w / o.w));
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < f.h; yy++)
                for (int xx = x0; xx < x1 && xx < f.w; xx++) {
                    uint32_t p = f.px[(size_t)yy * f.w + xx];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
                }
            if (!n) n = 1;
            o.px[(size_t)y * o.w + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return write_frame_image(path, o);
}
