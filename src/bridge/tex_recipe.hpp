#pragma once

#include "preread.hpp"
#include <unordered_map>

struct TexRegion {
    unsigned mip, x, y, width, height, pitch;
    std::size_t offset;
};

struct TexSource {
    std::filesystem::path path;
    int width, height, mips, channels, first_channel, total_channels, channel_bytes;
    std::vector<TexRegion> regions;
};

struct TexRecipe {
    std::size_t length = 0;
    unsigned format = 0;
    bool shared = false;
    std::string shared_file;
    std::vector<unsigned char> header;
    std::vector<TexSource> sources;
};

using TexRecipeBundle = std::unordered_map<std::string, std::string>;
TexRecipeBundle read_tex_recipe_bundle(const std::filesystem::path& directory);
TexRecipe read_tex_recipe(const std::filesystem::path& directory, const std::string& filename,
                          const TexRecipeBundle& bundle);
void encode_bc7_map(const unsigned char* pixels, unsigned width, unsigned height,
                    std::size_t stride, unsigned channels, unsigned char* output, std::size_t pitch);
bool build_recipe_tex(const TexRecipe& recipe, DecodeSession& decoder,
                      const REFrameworkPluginFunctions& log, DecodeTiming& timing,
                      std::vector<unsigned char>& bytes);
