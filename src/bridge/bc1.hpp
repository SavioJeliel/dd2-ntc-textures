#pragma once

#include <cstddef>
#include <cstring>
#include <stb_dxt.h>

// The current pilot and first batch use mip dimensions divisible by four.
inline void encode_bc1(const unsigned char* rgb, unsigned width, unsigned height,
                       unsigned char* output, std::size_t row_pitch) {
    for (unsigned y = 0; y < height; y += 4) {
        auto* row = output + (y / 4) * row_pitch;
        for (unsigned x = 0; x < width; x += 4) {
            unsigned char block[64];
            for (unsigned pixel = 0; pixel < 16; ++pixel) {
                const auto source = ((y + pixel / 4) * width + x + pixel % 4) * 3;
                std::memcpy(block + pixel * 4, rgb + source, 3);
                block[pixel * 4 + 3] = 255;
            }
            unsigned char bc1[8];
            stb_compress_dxt_block(bc1, block, 0, STB_DXT_HIGHQUAL);
            std::memcpy(row + (x / 4) * sizeof(bc1), bc1, sizeof(bc1));
        }
    }
}
