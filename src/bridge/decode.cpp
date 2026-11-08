#include <windows.h>
#include <reframework/API.h>
#include <libntc/ntc.h>
#include "preread.hpp"

#include <filesystem>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <vector>

namespace {
int module_anchor;

}

std::filesystem::path ntc_data_directory() {
    HMODULE self = nullptr;
    wchar_t path[32768];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&module_anchor), &self)) {
        return {};
    }
    const DWORD length = GetModuleFileNameW(self, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) {
        return {};
    }
    return std::filesystem::path(path).parent_path() / L"dd2_ntc_data";
}

struct DecodeSession::State {
    HMODULE library = nullptr;
    decltype(&ntc::DestroyContext) destroy = nullptr;
    decltype(&ntc::GetLastErrorMessage) last_error = nullptr;
    ntc::IContext* context = nullptr;
    ntc::ITextureSet* packed_texture = nullptr;
    std::filesystem::path packed_path;
    unsigned packed_channels_read = 0;

    ~State() {
        if (packed_texture) context->DestroyTextureSet(packed_texture);
        if (context) destroy(context);
        if (library) FreeLibrary(library);
    }
};

DecodeSession::DecodeSession() = default;
DecodeSession::~DecodeSession() = default;
void DecodeSession::reset() { state_.reset(); }

bool DecodeSession::decode(const std::filesystem::path& path, const REFrameworkPluginFunctions& log,
                    int channels, TextureMips& mips, DecodeTiming& timing,
                    int packed_first_channel, int packed_channels, int width,
                    int height, int mip_count, int channel_bytes) {
    const bool packed = packed_channels != 0;
    const bool reuse = packed && state_ && state_->packed_texture && state_->packed_path == path;
    const int total_channels = packed ? packed_channels : channels;
    const ntc::TextureSetDesc expected{width, height ? height : width, (total_channels + 1) & ~1,
                                      mip_count ? mip_count : texture_mip_count(width)};
    if (expected.mips <= 0 || expected.mips > static_cast<int>(mips.size()) ||
        (channel_bytes != 1 && channel_bytes != 2 && channel_bytes != 4)) return false;
    using Clock = std::chrono::steady_clock;
    auto phase = Clock::now();
    const auto elapsed = [&] {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - phase).count();
        phase = now;
        return ms;
    };
    const auto directory = path.parent_path();
    if (directory.empty()) {
        log.log_error("[dd2_ntc_bridge] Cannot locate plugin data directory");
        return false;
    }
    std::vector<unsigned char> compressed;
    if (!reuse) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (size <= 0 || size > 16 * 1024 * 1024) {
            log.log_error("[dd2_ntc_bridge] Missing or invalid NTC file");
            return false;
        }
        compressed.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(compressed.data()), size)) {
            log.log_error("[dd2_ntc_bridge] Failed to read NTC file");
            return false;
        }
        timing.file = elapsed();
    }

    if (!state_) {
        auto state = std::make_unique<State>();
        const auto library_path = directory / L"libntc.dll";
        state->library = LoadLibraryExW(library_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!state->library) {
            log.log_error("[dd2_ntc_bridge] Cannot load libntc.dll: %lu", GetLastError());
            return false;
        }
        const auto create = reinterpret_cast<decltype(&ntc::CreateContext)>(
            GetProcAddress(state->library, "CreateContext"));
        state->destroy = reinterpret_cast<decltype(&ntc::DestroyContext)>(
            GetProcAddress(state->library, "DestroyContext"));
        state->last_error = reinterpret_cast<decltype(&ntc::GetLastErrorMessage)>(
            GetProcAddress(state->library, "GetLastErrorMessage"));
        timing.library = elapsed();
        if (!create || !state->destroy || !state->last_error) {
            log.log_error("[dd2_ntc_bridge] LibNTC exports are incomplete");
            return false;
        }
        const auto status = create(&state->context, {});
        timing.context = elapsed();
        if (status != ntc::Status::Ok) {
            log.log_error("[dd2_ntc_bridge] CreateContext failed (%d): %s",
                          static_cast<int>(status), state->last_error());
            return false;
        }
        state_ = std::move(state);
    }
    auto* context = state_->context;
    const auto check = [&](ntc::Status status, const char* operation) {
        if (status == ntc::Status::Ok) {
            return true;
        }
        log.log_error("[dd2_ntc_bridge] %s failed (%d): %s", operation,
                      static_cast<int>(status), state_->last_error());
        return false;
    };

    ntc::TextureSetFeatures features{};
    features.stagingBytesPerPixel = (std::max)(4, total_channels * channel_bytes);
    features.enableCompression = false;
    ntc::ITextureSet* raw_texture = nullptr;
    const auto destroy_texture = [&](ntc::ITextureSet* value) { context->DestroyTextureSet(value); };
    std::unique_ptr<ntc::ITextureSet, decltype(destroy_texture)> owned_texture(nullptr, destroy_texture);
    if (!reuse) {
        const auto load_status = context->CreateCompressedTextureSetFromMemory(
            compressed.data(), compressed.size(), features, &raw_texture);
        owned_texture.reset(raw_texture);
        if (!check(load_status, "Load NTC")) return false;
    } else {
        raw_texture = state_->packed_texture;
    }
    auto* texture = raw_texture;
    const auto& desc = texture->GetDesc();
    const int first = packed ? packed_first_channel : 0;
    const ntc::ITextureMetadata* metadata = nullptr;
    if (packed || texture->GetTextureCount() == 1) {
        for (int i = 0; i < texture->GetTextureCount(); ++i) {
            const auto* candidate = texture->GetTexture(i);
            if (candidate && candidate->GetFirstChannel() == first && candidate->GetNumChannels() == channels) {
                metadata = candidate;
                break;
            }
        }
    }
    if (desc != expected || !metadata) {
        log.log_error("[dd2_ntc_bridge] Unexpected NTC layout: %dx%d, %d channels, %d mips",
                      desc.width, desc.height, desc.channels, desc.mips);
        return false;
    }
    // Match the offline CLI output, preserving the original RGB sample values.
    const auto color_space = channel_bytes == 4 ? ntc::ColorSpace::Linear : metadata->GetRgbColorSpace();
    const ntc::ColorSpace color_spaces[] = {color_space, color_space, color_space,
        channel_bytes == 4 ? ntc::ColorSpace::Linear : metadata->GetAlphaColorSpace()};
    if (!reuse) {
        timing.load = elapsed();
        ntc::DecompressionStats stats{};
        if (!check(texture->Decompress(&stats), "CUDA decompress")) return false;
        timing.decode = elapsed();
        timing.gpu = stats.gpuTimeMilliseconds;
    }

    for (int mip = 0; mip < expected.mips; ++mip) {
        ntc::ReadChannelsParameters params{};
        params.mipLevel = mip;
        params.width = (std::max)(1, expected.width >> mip);
        params.height = (std::max)(1, expected.height >> mip);
        params.firstChannel = first;
        params.numChannels = channels;
        params.pixelStride = channels * channel_bytes;
        params.rowPitch = params.width * params.pixelStride;
        params.channelFormat = channel_bytes == 4 ? ntc::ChannelFormat::FLOAT32 :
                               channel_bytes == 2 ? ntc::ChannelFormat::UNORM16 : ntc::ChannelFormat::UNORM8;
        params.dstColorSpaces = color_spaces;
        params.useDithering = channel_bytes == 1;
        auto& pixels = mips[mip];
        pixels.resize(params.rowPitch * params.height);
        params.pOutData = pixels.data();
        const auto start = Clock::now();
        if (!check(texture->ReadChannels(params), "Read mip")) {
            return false;
        }
        timing.readback += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        unsigned long long hash = 14695981039346656037ull;
        for (const auto byte : pixels) {
            hash = (hash ^ byte) * 1099511628211ull;
        }
        log.log_info("[dd2_ntc_bridge] NTC mip %d %dx%d channels=%d FNV1a=%016llX",
                     mip, params.width, params.height, channels, hash);
    }
    if (packed) {
        if (!reuse) {
            state_->packed_path = path;
            if (state_->packed_texture) context->DestroyTextureSet(state_->packed_texture);
            state_->packed_texture = owned_texture.release();
            state_->packed_channels_read = 0;
        }
        log.log_info("[dd2_ntc_bridge] Packed NTC %s %s channels=%d..%d",
                     path.stem().string().c_str(), reuse ? "reused" : "decoded", first, first + channels - 1);
        state_->packed_channels_read |= ((1u << channels) - 1) << first;
        if (state_->packed_channels_read == (1u << packed_channels) - 1) {
            context->DestroyTextureSet(state_->packed_texture);
            state_->packed_texture = nullptr;
        }
    }
    return true;
}
