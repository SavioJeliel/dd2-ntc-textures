#include "tex_recipe.hpp"
#include "bc7enc.h"

#include <DirectXTex.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
bool safe_filename(const std::string& name) {
    return !name.empty() && name.size() <= 64 && name.find("..") == std::string::npos &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.-") == std::string::npos;
}

template<typename T>
T get(const unsigned char* bytes) {
    T result;
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}

DXGI_FORMAT linear_block_format(unsigned format) {
    switch (format) {
        case 72: return DXGI_FORMAT_BC1_UNORM;
        case 78: return DXGI_FORMAT_BC3_UNORM;
        case 99: return DXGI_FORMAT_BC7_UNORM;
        default: return static_cast<DXGI_FORMAT>(format);
    }
}

void encode_bc6h_unsigned(const unsigned char* pixels, unsigned width, unsigned height,
                          std::size_t stride, std::size_t pixel_bytes,
                          unsigned char* output, std::size_t pitch) {
    using namespace DirectX::PackedVector;
    for (unsigned y = 0; y < height; y += 4) {
        for (unsigned x = 0; x < width; x += 4) {
            int samples[16][3];
            int low[3]{0x7bff, 0x7bff, 0x7bff}, high[3]{};
            for (unsigned i = 0; i < 16; ++i) {
                const auto* pixel = pixels + (std::min)(y + i / 4, height - 1) * stride
                                           + (std::min)(x + i % 4, width - 1) * pixel_bytes;
                for (unsigned c = 0; c < 3; ++c) {
                    const float value = get<float>(pixel + c * 4);
                    const int half = XMConvertFloatToHalf(value > 0 ? (std::min)(value, 65504.0f) : 0.0f);
                    samples[i][c] = half;
                    low[c] = (std::min)(low[c], half);
                    high[c] = (std::max)(high[c], half);
                }
            }
            int span[3];
            std::uint32_t length2 = 0;
            for (unsigned c = 0; c < 3; ++c) {
                span[c] = high[c] - low[c];
                length2 += span[c] * span[c];
            }
            const float scale = length2 ? 15.0f / length2 : 0.0f;
            unsigned indices[16];
            for (unsigned i = 0; i < 16; ++i) {
                std::uint32_t dot = 0;
                for (unsigned c = 0; c < 3; ++c) dot += (samples[i][c] - low[c]) * span[c];
                indices[i] = (std::min)(15u, static_cast<unsigned>(dot * scale + 0.5f));
            }
            // BC6H mode 11 has two explicit 10-bit RGB endpoints and one 4-bit index per texel.
            // The first index omits its high bit, so reverse the endpoints when needed.
            const bool reverse = indices[0] >= 8;
            const auto quantize = [](int half) { return half == 0x7bff ? 1023u :
                (std::min)(1022u, static_cast<unsigned>(half / 31)); };
            std::uint64_t bits[2]{3, 0};
            unsigned position = 5;
            const auto append = [&](std::uint64_t value, unsigned count) {
                const unsigned word = position / 64, shift = position % 64;
                bits[word] |= value << shift;
                if (shift + count > 64) bits[word + 1] |= value >> (64 - shift);
                position += count;
            };
            for (const auto* endpoint : {reverse ? high : low, reverse ? low : high}) {
                for (unsigned c = 0; c < 3; ++c) append(quantize(endpoint[c]), 10);
            }
            for (unsigned i = 0; i < 16; ++i) append(reverse ? 15 - indices[i] : indices[i], i ? 4 : 3);
            std::memcpy(output + (y / 4) * pitch + (x / 4) * 16, bits, sizeof(bits));
        }
    }
}

bool encode_region(const TexRecipe& recipe, const TexSource& source, const TexRegion& region,
                   const TextureMips& mips, std::vector<unsigned char>& bytes) {
    const auto format = static_cast<DXGI_FORMAT>(recipe.format);
    const auto w = static_cast<unsigned>((std::max)(1, source.width >> region.mip));
    const auto h = static_cast<unsigned>((std::max)(1, source.height >> region.mip));
    const auto stride = static_cast<std::size_t>(source.channels * source.channel_bytes);
    if (!region.width || !region.height || region.x > w || region.y > h ||
        region.width > w - region.x || region.height > h - region.y ||
        mips[region.mip].size() != w * h * stride) return false;
    const auto& mip = mips[region.mip];
    const auto* pixels = mip.data() + (static_cast<std::size_t>(region.y) * w + region.x) * stride;
    std::size_t row_bytes = 0, slice = 0;
    if (FAILED(DirectX::ComputePitch(format, region.width, region.height, row_bytes, slice))) return false;
    const bool compressed = DirectX::IsCompressed(format);
    const auto rows = compressed ? (region.height + 3) / 4 : region.height;
    if (row_bytes > region.pitch || region.offset < recipe.header.size() ||
        region.offset > bytes.size() ||
        (rows - 1) * static_cast<std::size_t>(region.pitch) + row_bytes > bytes.size() - region.offset) {
        return false;
    }
    auto* dest = bytes.data() + region.offset;
    if (format == DXGI_FORMAT_BC6H_UF16 && source.channel_bytes == 4 && source.channels >= 3) {
        encode_bc6h_unsigned(pixels, region.width, region.height, w * stride, stride, dest, region.pitch);
        return true;
    }
    if ((format == DXGI_FORMAT_BC7_UNORM || format == DXGI_FORMAT_BC7_UNORM_SRGB)
        && source.channel_bytes == 1) {
        encode_bc7_map(pixels, region.width, region.height, w * stride, source.channels, dest, region.pitch);
        return true;
    }
    if (!compressed) {
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT && source.channels == 4 && source.channel_bytes == 4) {
            for (unsigned y = 0; y < rows; ++y) {
                for (unsigned x = 0; x < region.width * 4; ++x) {
                    const auto value = get<float>(pixels + y * w * stride + x * 4);
                    const auto half = DirectX::PackedVector::XMConvertFloatToHalf(value);
                    std::memcpy(dest + y * region.pitch + x * 2, &half, sizeof(half));
                }
            }
            return true;
        }
        if (row_bytes != region.width * stride) return false;
        for (unsigned y = 0; y < rows; ++y) {
            std::memcpy(dest + y * region.pitch, pixels + y * w * stride, row_bytes);
        }
        return true;
    }

    const bool hdr = format == DXGI_FORMAT_BC6H_UF16 || format == DXGI_FORMAT_BC6H_SF16;
    const auto image_format = hdr ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const auto pixel_bytes = hdr ? 16u : 4u;
    if ((hdr && source.channel_bytes != 4) || (!hdr && source.channel_bytes != 1)) return false;
    const auto image_pitch = static_cast<std::size_t>(region.width) * pixel_bytes;
    std::vector<unsigned char> rgba(image_pitch * region.height);
    for (unsigned y = 0; y < region.height; ++y) {
        for (unsigned x = 0; x < region.width; ++x) {
            auto* pixel = rgba.data() + y * image_pitch + x * pixel_bytes;
            std::memcpy(pixel, pixels + (static_cast<std::size_t>(y) * w + x) * stride, stride);
            if (source.channels < 4) {
                if (hdr) {
                    const float one = 1.0f;
                    std::memcpy(pixel + 12, &one, sizeof(one));
                } else {
                    pixel[3] = 255;
                }
            }
        }
    }
    const DirectX::Image image{region.width, region.height, image_format, image_pitch, rgba.size(), rgba.data()};
    DirectX::ScratchImage encoded;
    if (FAILED(DirectX::Compress(image, linear_block_format(recipe.format),
                                 DirectX::TEX_COMPRESS_DEFAULT, 0.5f, encoded))) return false;
    const auto* blocks = encoded.GetImage(0, 0, 0);
    if (!blocks || blocks->rowPitch < row_bytes) return false;
    for (unsigned y = 0; y < rows; ++y) {
        std::memcpy(dest + y * region.pitch, blocks->pixels + y * blocks->rowPitch, row_bytes);
    }
    return true;
}
}

void encode_bc7_map(const unsigned char* pixels, unsigned width, unsigned height,
                    std::size_t stride, unsigned channels, unsigned char* output, std::size_t pitch) {
    static std::once_flag initialized;
    std::call_once(initialized, bc7enc_compress_block_init);
    bc7enc_compress_block_params params{};
    bc7enc_compress_block_params_init(&params);
    bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_mode_mask = 1 << 6;
    for (unsigned y = 0; y < height; y += 4) {
        auto* row = output + (y / 4) * pitch;
        for (unsigned x = 0; x < width; x += 4) {
            unsigned char block[64]{};
            for (unsigned dy = 0; dy < 4; ++dy) {
                const auto* src = pixels + (std::min)(y + dy, height - 1) * stride;
                for (unsigned dx = 0; dx < 4; ++dx) {
                    auto* dst = block + (dy * 4 + dx) * 4;
                    std::memcpy(dst, src + (std::min)(x + dx, width - 1) * channels, channels);
                    if (channels < 4) dst[3] = 255;
                }
            }
            bc7enc_compress_block(row + (x / 4) * 16, block, &params);
        }
    }
}

TexRecipeBundle read_tex_recipe_bundle(const std::filesystem::path& directory) {
    const auto path = directory / "recipes.bundle";
    TexRecipeBundle result;
    if (!std::filesystem::exists(path)) return result;
    std::ifstream input(path, std::ios::binary);
    std::string magic;
    unsigned count = 0;
    if (!(input >> magic >> count) || magic != "TEXRECIPES1" || !count || count > 100000 ||
        input.get() != '\n') throw std::runtime_error("Invalid TEX recipe bundle");
    result.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        std::string name;
        unsigned size = 0;
        if (!(input >> name >> size) || !safe_filename(name) || !size || size > 2 * 1024 * 1024 ||
            input.get() != '\n') throw std::runtime_error("Invalid bundled TEX recipe");
        std::string recipe(size, '\0');
        if (!input.read(recipe.data(), size) || !result.emplace(std::move(name), std::move(recipe)).second) {
            throw std::runtime_error("Missing or duplicate bundled TEX recipe");
        }
    }
    if (input >> magic) throw std::runtime_error("Trailing TEX recipe bundle data");
    return result;
}

TexRecipe read_tex_recipe(const std::filesystem::path& directory, const std::string& filename,
                          const TexRecipeBundle& bundle) {
    if (!safe_filename(filename)) throw std::runtime_error("Invalid TEX recipe name");
    std::ifstream individual;
    std::istringstream bundled;
    const auto found = bundle.find(filename);
    if (found == bundle.end()) individual.open(directory / filename);
    else bundled.str(found->second);
    std::istream& file = found == bundle.end() ? static_cast<std::istream&>(individual) : bundled;
    TexRecipe recipe;
    std::string signature, hex;
    unsigned count = 0;
    if (!(file >> signature >> recipe.length >> recipe.format >> recipe.shared >> hex >> count) ||
        signature != "TEXRECIPE1" || recipe.length > 512u * 1024 * 1024 ||
        hex.size() < 80 || hex.size() % 2 || hex.size() / 2 > recipe.length ||
        hex.size() > 128u * 1024 || hex.find_first_not_of("0123456789abcdef") != std::string::npos ||
        count == 0 || count > 4096) {
        throw std::runtime_error("Invalid TEX recipe header");
    }
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        recipe.header.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    if (std::memcmp(recipe.header.data(), "TEX\0", 4) != 0 ||
        get<unsigned>(recipe.header.data() + 16) != recipe.format) {
        throw std::runtime_error("TEX recipe header format mismatch");
    }
    if (recipe.shared) recipe.shared_file = filename;
    for (unsigned i = 0; i < count; ++i) {
        TexSource source{};
        std::string path;
        unsigned regions = 0;
        if (!(file >> path >> source.width >> source.height >> source.mips >> source.channels
                   >> source.first_channel >> source.total_channels >> source.channel_bytes >> regions) ||
            !safe_filename(path) || source.width <= 0 || source.height <= 0 ||
            source.width > 16384 || source.height > 16384 || source.mips < 1 || source.mips > 16 ||
            source.channels < 1 || source.channels > 4 || source.first_channel < 0 ||
            source.total_channels > 16 || source.total_channels < source.first_channel + source.channels ||
            (source.channel_bytes != 1 && source.channel_bytes != 2 && source.channel_bytes != 4) ||
            regions == 0 || regions > 4096) throw std::runtime_error("Invalid TEX recipe source");
        source.path = directory / path;
        for (unsigned r = 0; r < regions; ++r) {
            TexRegion region{};
            if (!(file >> region.mip >> region.x >> region.y >> region.width >> region.height
                       >> region.offset >> region.pitch) || region.mip >= static_cast<unsigned>(source.mips)) {
                throw std::runtime_error("Invalid TEX recipe region");
            }
            source.regions.push_back(region);
        }
        recipe.sources.push_back(std::move(source));
    }
    if (file >> signature) throw std::runtime_error("Trailing data in TEX recipe");
    return recipe;
}

bool build_recipe_tex(const TexRecipe& recipe, DecodeSession& decoder,
                      const REFrameworkPluginFunctions& log, DecodeTiming& timing,
                      std::vector<unsigned char>& bytes) {
    bytes.assign(recipe.length, 0);
    std::copy(recipe.header.begin(), recipe.header.end(), bytes.begin());
    for (const auto& source : recipe.sources) {
        TextureMips mips;
        if (!decoder.decode(source.path, log, source.channels, mips, timing, source.first_channel,
                            source.total_channels, source.width, source.height, source.mips,
                            source.channel_bytes)) return false;
        for (const auto& region : source.regions) {
            if (!encode_region(recipe, source, region, mips, bytes)) {
                log.log_error("[dd2_ntc_bridge] TEX encoding failed format=%u mip=%u offset=%zu",
                              recipe.format, region.mip, region.offset);
                return false;
            }
        }
    }
    return true;
}
