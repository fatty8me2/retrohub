#pragma once
#include <string>

#include "core.h"

/* Writes the frame as PNG (default) or BMP (path ends in .bmp). */
bool write_frame_image(const std::string& path, const FrameBuf& f);
