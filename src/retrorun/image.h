#pragma once
#include <string>

#include "core.h"

/* Writes the frame as PNG (default) or BMP (path ends in .bmp). */
bool write_frame_image(const std::string& path, const FrameBuf& f);
/* Same, box-downscaled so the width is at most max_w (never upscaled): save-state thumbnails. */
bool write_frame_image_scaled(const std::string& path, const FrameBuf& f, int max_w);
