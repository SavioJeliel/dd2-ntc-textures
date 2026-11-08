#pragma once

#include <reframework/API.h>

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

using TextureMips = std::array<std::vector<unsigned char>, 16>;
constexpr int texture_mip_count(int side) {
    int count = 0;
    for (; side >= 8; side >>= 1) ++count;
    return count;
}
struct DecodeTiming {
    double file = 0, library = 0, context = 0, load = 0, decode = 0, gpu = 0, readback = 0;
};

bool install_preread_hook(const REFrameworkPluginFunctions& functions, const REFrameworkRendererData* renderer);
// `streaming_notice` indica o chamador que anuncia os mips altos (+0x527bc10). Só o modo de lote o usa.
void request_texture_prepare(const wchar_t* logical_path, bool streaming_notice = false);
std::filesystem::path ntc_data_directory();
class DecodeSession {
public:
    DecodeSession();
    ~DecodeSession();
    bool decode(const std::filesystem::path& file, const REFrameworkPluginFunctions& log,
                int channels, TextureMips& mips, DecodeTiming& timing,
                int packed_first_channel = 0, int packed_channels = 0, int width = 2048,
                int height = 0, int mip_count = 0, int channel_bytes = 1);
    void reset();

private:
    struct State;
    std::unique_ptr<State> state_;
};
