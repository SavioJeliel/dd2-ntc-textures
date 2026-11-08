// Bancada fora do jogo: mede do NTC até o TEX pronto, em CPU (código da DLL) e em GPU (LibNTC + D3D12).
// Uso: ntc_gpu_bench <cpu|gpu|compare|bc6h> <pasta_dados> <lista.tsv> <saida.csv> [limite]
#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXTex.h>
#include <libntc/ntc.h>
#include "tex_recipe.hpp"
#include "gpu_tex.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {
double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[160];
        std::snprintf(text, sizeof(text), "%s falhou: 0x%08lX", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}
void quiet(const char*, ...) {}
void loud(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}
REFrameworkPluginFunctions make_log() {
    REFrameworkPluginFunctions log{};
    log.log_error = loud;
    log.log_warn = quiet;
    log.log_info = quiet;
    return log;
}

struct Item {
    std::string asset, variant;
    TexRecipe recipe;
};

std::vector<Item> load_items(const fs::path& data, const fs::path& list_path, int limit) {
    const auto bundle = read_tex_recipe_bundle(data);
    std::ifstream list(list_path);
    std::vector<Item> items;
    std::string line;
    int assets = 0;
    while (std::getline(list, line) && (limit <= 0 || assets < limit)) {
        std::istringstream row(line);
        std::string asset, base, streaming;
        if (!(row >> asset >> base >> streaming)) continue;
        ++assets;
        if (base != "-") items.push_back({asset, "base", read_tex_recipe(data, base, bundle)});
        if (streaming != "-") items.push_back({asset, "streaming", read_tex_recipe(data, streaming, bundle)});
    }
    return items;
}

DXGI_FORMAT plain_format(unsigned format) {
    switch (format) {
        case 72: return DXGI_FORMAT_BC1_UNORM;
        case 78: return DXGI_FORMAT_BC3_UNORM;
        case 99: return DXGI_FORMAT_BC7_UNORM;
        default: return static_cast<DXGI_FORMAT>(format);
    }
}

struct Quality {
    double cpu_rmse = -1, gpu_rmse = -1, gpu_alpha_mean = -1;
};

// Compara, no maior mip, cada TEX com os pixels decodificados do NTC. Só blocos BC de 8 bits.
Quality measure_quality(const Item& item, DecodeSession& session, const REFrameworkPluginFunctions& log,
                        const std::vector<unsigned char>& cpu, const std::vector<unsigned char>& gpu) {
    Quality quality;
    const auto& source = item.recipe.sources.front();
    const TexRegion* top = &source.regions.front();
    for (const auto& region : source.regions) {
        if (region.mip < top->mip) top = &region;
    }
    if (source.channel_bytes != 1) return quality;
    TextureMips mips;
    DecodeTiming timing;
    if (!session.decode(source.path, log, source.channels, mips, timing, source.first_channel, source.total_channels,
                        source.width, source.height, source.mips, source.channel_bytes)) return quality;
    const unsigned w = (std::max)(1, source.width >> top->mip);
    const DXGI_FORMAT format = plain_format(item.recipe.format);
    const auto rmse = [&](const std::vector<unsigned char>& bytes, double* alpha) {
        std::size_t row_bytes = 0, slice = 0;
        DirectX::ComputePitch(format, top->width, top->height, row_bytes, slice);
        const unsigned rows = (top->height + 3) / 4;
        std::vector<unsigned char> packed(row_bytes * rows);
        for (unsigned y = 0; y < rows; ++y) {
            std::memcpy(packed.data() + y * row_bytes, bytes.data() + top->offset + std::size_t(y) * top->pitch, row_bytes);
        }
        const DirectX::Image image{top->width, top->height, format, row_bytes, packed.size(), packed.data()};
        DirectX::ScratchImage out;
        if (FAILED(DirectX::Decompress(image, DXGI_FORMAT_R8G8B8A8_UNORM, out))) return -1.0;
        const auto* decoded = out.GetImage(0, 0, 0);
        double sum = 0, alpha_sum = 0;
        std::size_t count = 0;
        for (unsigned y = 0; y < top->height; ++y) {
            for (unsigned x = 0; x < top->width; ++x) {
                const auto* a = decoded->pixels + y * decoded->rowPitch + std::size_t(x) * 4;
                const auto* b = mips[top->mip].data() +
                    (std::size_t(y + top->y) * w + x + top->x) * source.channels;
                for (int c = 0; c < source.channels; ++c) {
                    const double d = double(a[c]) - double(b[c]);
                    sum += d * d;
                    ++count;
                }
                alpha_sum += a[3];
            }
        }
        if (alpha) *alpha = alpha_sum / (double(top->width) * top->height);
        return std::sqrt(sum / count);
    };
    quality.cpu_rmse = rmse(cpu, nullptr);
    quality.gpu_rmse = rmse(gpu, &quality.gpu_alpha_mean);
    return quality;
}

// Decodifica uma região de um TEX para RGBA8. Devolve vazio se o formato não for BC de 8 bits.
std::vector<unsigned char> decode_region(const std::vector<unsigned char>& bytes, const TexRegion& region, unsigned format) {
    const DXGI_FORMAT dxgi = plain_format(format);
    std::size_t row_bytes = 0, slice = 0;
    if (!DirectX::IsCompressed(dxgi) || FAILED(DirectX::ComputePitch(dxgi, region.width, region.height, row_bytes, slice))) return {};
    const unsigned rows = (region.height + 3) / 4;
    std::vector<unsigned char> packed(row_bytes * rows);
    for (unsigned y = 0; y < rows; ++y) {
        std::memcpy(packed.data() + y * row_bytes, bytes.data() + region.offset + std::size_t(y) * region.pitch, row_bytes);
    }
    const DirectX::Image image{region.width, region.height, dxgi, row_bytes, packed.size(), packed.data()};
    DirectX::ScratchImage out;
    if (FAILED(DirectX::Decompress(image, DXGI_FORMAT_R8G8B8A8_UNORM, out))) return {};
    const auto* decoded = out.GetImage(0, 0, 0);
    std::vector<unsigned char> pixels(std::size_t(region.width) * region.height * 4);
    for (unsigned y = 0; y < region.height; ++y) {
        std::memcpy(pixels.data() + y * region.width * 4ull, decoded->pixels + y * decoded->rowPitch, region.width * 4ull);
    }
    return pixels;
}

double rmse_channel(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b, int channel) {
    double sum = 0;
    for (std::size_t i = channel; i < a.size(); i += 4) {
        const double d = double(a[i]) - double(b[i]);
        sum += d * d;
    }
    return std::sqrt(sum / (a.size() / 4));
}

// Erro na diferença entre dois quadros seguidos: é o que aparece como tremor em animação.
double rmse_delta(const std::vector<unsigned char>& now, const std::vector<unsigned char>& before,
                  const std::vector<unsigned char>& ref_now, const std::vector<unsigned char>& ref_before, int channel) {
    double sum = 0;
    for (std::size_t i = channel; i < now.size(); i += 4) {
        const double d = (double(now[i]) - double(before[i])) - (double(ref_now[i]) - double(ref_before[i]));
        sum += d * d;
    }
    return std::sqrt(sum / (now.size() / 4));
}

// Compara o TEX original com o reconstruído em GPU e em CPU, região por região.
int run_audit(const fs::path& data, const fs::path& list_path, const fs::path& originals, const fs::path& output, bool gpu_only) {
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI");
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "adaptador");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "dispositivo");
    auto gpu = make_gpu_tex_builder(device.Get(), data);
    auto log = make_log();
    DecodeSession session;
    std::ofstream csv(output);
    csv << "asset,variant,format,source,region,mip,width,height,gpu_r,gpu_g,gpu_b,gpu_a,cpu_r,cpu_g,cpu_b,cpu_a,"
           "orig_delta_r,orig_delta_g,gpu_delta_err_r,gpu_delta_err_g,cpu_delta_err_r,cpu_delta_err_g\n";
    const auto bundle = read_tex_recipe_bundle(data);
    std::ifstream list(list_path);
    std::string line;
    while (std::getline(list, line)) {
        std::istringstream row(line);
        std::string asset, variant, recipe_name;
        if (!(row >> asset >> variant >> recipe_name)) continue;
        const auto recipe = read_tex_recipe(data, recipe_name, bundle);
        std::ifstream file(originals / (recipe_name + ".tex"), std::ios::binary);
        std::vector<unsigned char> original((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (original.size() != recipe.length) {
            std::fprintf(stderr, "%s: original com %zu bytes, receita com %zu\n", recipe_name.c_str(), original.size(), recipe.length);
            continue;
        }
        std::vector<unsigned char> from_gpu, from_cpu;
        GpuTiming timing;
        std::string weights;
        try {
            if (!gpu->build(recipe, from_gpu, timing, weights)) continue;
        } catch (const std::exception& error) {
            gpu->recover();
            std::fprintf(stderr, "%s %s: %s\n", asset.c_str(), variant.c_str(), error.what());
            continue;
        }
        DecodeTiming decode_timing;
        const bool cpu_ok = !gpu_only && build_recipe_tex(recipe, session, log, decode_timing, from_cpu);
        for (std::size_t s = 0; s < recipe.sources.size(); ++s) {
            const auto& regions = recipe.sources[s].regions;
            std::vector<unsigned char> before_ref, before_gpu, before_cpu;
            for (std::size_t r = 0; r < regions.size(); ++r) {
                const auto& region = regions[r];
                auto ref = decode_region(original, region, recipe.format);
                auto a = decode_region(from_gpu, region, recipe.format);
                auto b = cpu_ok ? decode_region(from_cpu, region, recipe.format) : std::vector<unsigned char>{};
                if (ref.empty() || a.empty()) continue;
                csv << asset << ',' << variant << ',' << recipe.format << ',' << s << ',' << r << ',' << region.mip << ','
                    << region.width << ',' << region.height;
                for (int c = 0; c < 4; ++c) csv << ',' << rmse_channel(ref, a, c);
                for (int c = 0; c < 4; ++c) csv << ',' << (b.empty() ? -1.0 : rmse_channel(ref, b, c));
                const bool has_previous = before_ref.size() == ref.size();
                for (int c = 0; c < 2; ++c) {
                    double delta = -1;
                    if (has_previous) {
                        double sum = 0;
                        for (std::size_t i = c; i < ref.size(); i += 4) {
                            const double d = double(ref[i]) - double(before_ref[i]);
                            sum += d * d;
                        }
                        delta = std::sqrt(sum / (ref.size() / 4));
                    }
                    csv << ',' << delta;
                }
                for (int c = 0; c < 2; ++c) csv << ',' << (has_previous ? rmse_delta(a, before_gpu, ref, before_ref, c) : -1.0);
                for (int c = 0; c < 2; ++c) {
                    csv << ',' << (has_previous && !b.empty() ? rmse_delta(b, before_cpu, ref, before_ref, c) : -1.0);
                }
                csv << '\n';
                before_ref = std::move(ref);
                before_gpu = std::move(a);
                before_cpu = std::move(b);
            }
        }
        csv.flush();
        std::printf("%s %s ok\n", asset.c_str(), variant.c_str());
        std::fflush(stdout);
    }
    return 0;
}

int run_bc6h(const fs::path& data, const fs::path& list_path, const fs::path& output) {
    // Mede o encoder por GPU do DirectXTex (D3D11) contra o caminho atual, nos BC6H.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    D3D_FEATURE_LEVEL level{};
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                            &device, &level, &immediate), "D3D11");
    auto log = make_log();
    DecodeSession session;
    std::ofstream csv(output);
    csv << "asset,variant,format,width,height,decode_ms,cpu_encode_ms,gpu_encode_ms,cpu_rmse_log,gpu_rmse_log,status\n";
    for (const auto& item : load_items(data, list_path, 0)) {
        const auto& source = item.recipe.sources.front();
        const TexRegion* top = &source.regions.front();
        for (const auto& region : source.regions) {
            if (region.mip < top->mip) top = &region;
        }
        const auto format = static_cast<DXGI_FORMAT>(item.recipe.format);
        TextureMips mips;
        DecodeTiming timing;
        const auto decode_start = Clock::now();
        std::string status = "ok";
        double cpu_ms = -1, gpu_ms = -1, cpu_rmse = -1, gpu_rmse = -1;
        const bool decoded = session.decode(source.path, log, source.channels, mips, timing, source.first_channel,
            source.total_channels, source.width, source.height, source.mips, source.channel_bytes);
        const double decode_ms = ms_since(decode_start);
        if (!decoded) status = "decode_failed";
        else {
            const unsigned w = (std::max)(1, source.width >> top->mip);
            std::vector<float> pixels(std::size_t(top->width) * top->height * 4);
            for (unsigned y = 0; y < top->height; ++y) {
                for (unsigned x = 0; x < top->width; ++x) {
                    const auto* from = reinterpret_cast<const float*>(mips[top->mip].data()) +
                        (std::size_t(y + top->y) * w + x + top->x) * source.channels;
                    auto* to = pixels.data() + (std::size_t(y) * top->width + x) * 4;
                    for (int c = 0; c < 4; ++c) to[c] = c < source.channels ? from[c] : 1.f;
                }
            }
            const DirectX::Image image{top->width, top->height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                       std::size_t(top->width) * 16, pixels.size() * 4,
                                       reinterpret_cast<unsigned char*>(pixels.data())};
            const auto error_of = [&](const DirectX::ScratchImage& encoded) {
                DirectX::ScratchImage out;
                if (FAILED(DirectX::Decompress(*encoded.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT, out))) return -1.0;
                const auto* d = reinterpret_cast<const float*>(out.GetImage(0, 0, 0)->pixels);
                double sum = 0;
                const std::size_t count = std::size_t(top->width) * top->height;
                for (std::size_t i = 0; i < count; ++i) {
                    for (int c = 0; c < 3; ++c) {
                        const auto log1 = [](float v) { return std::log2(1.0 + (v < 0 ? -v : v)) * (v < 0 ? -1 : 1); };
                        const double e = log1(d[i * 4 + c]) - log1(pixels[i * 4 + c]);
                        sum += e * e;
                    }
                }
                return std::sqrt(sum / (count * 3));
            };
            DirectX::ScratchImage cpu_out, gpu_out;
            auto start = Clock::now();
            HRESULT hr = DirectX::Compress(image, format, DirectX::TEX_COMPRESS_DEFAULT, 0.5f, cpu_out);
            cpu_ms = ms_since(start);
            if (SUCCEEDED(hr)) cpu_rmse = error_of(cpu_out);
            start = Clock::now();
            hr = DirectX::Compress(device.Get(), image, format, DirectX::TEX_COMPRESS_DEFAULT, 1.0f, gpu_out);
            gpu_ms = ms_since(start);
            if (SUCCEEDED(hr)) gpu_rmse = error_of(gpu_out);
            else status = "gpu_failed";
        }
        csv << item.asset << ',' << item.variant << ',' << item.recipe.format << ',' << source.width << ','
            << source.height << ',' << decode_ms << ',' << cpu_ms << ',' << gpu_ms << ','
            << cpu_rmse << ',' << gpu_rmse << ',' << status << '\n';
        csv.flush();
    }
    return 0;
}
}

int main(int argc, char** argv) try {
    if (argc < 5) {
        std::fprintf(stderr, "uso: ntc_gpu_bench <cpu|gpu|compare|bc6h> <dados> <lista.tsv> <saida.csv> [limite]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const fs::path data = argv[2];
    const int limit = argc > 5 ? std::atoi(argv[5]) : 0;
    if (mode == "bc6h") return run_bc6h(data, argv[3], argv[4]);
    // audit <dados> <lista: ativo variante receita> <saida.csv> <pasta de originais>
    if (mode == "audit") return run_audit(data, argv[3], argc > 5 ? argv[5] : ".", argv[4], argc > 6 && std::string(argv[6]) == "gpu-only");

    const auto items = load_items(data, argv[3], limit);
    std::ofstream csv(argv[4]);
    csv << "index,asset,variant,format,width,height,tex_bytes,route,file_ms,upload_ms,record_ms,wait_ms,readback_ms,total_ms,"
           "weights,cpu_rmse,gpu_rmse,gpu_alpha_mean,same_bytes,status\n";
    auto log = make_log();
    DecodeSession session;
    std::unique_ptr<GpuTexBuilder> gpu;
    ComPtr<ID3D12Device> device;
    if (mode != "cpu") {
        ComPtr<IDXGIFactory6> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI");
        ComPtr<IDXGIAdapter1> adapter;
        check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "adaptador");
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "dispositivo");
        gpu = make_gpu_tex_builder(device.Get(), data);
    }

    const auto wall = Clock::now();
    int index = 0, failures = 0;
    for (const auto& item : items) {
        const auto& source = item.recipe.sources.front();
        GpuTiming timing;
        std::string route = mode == "cpu" ? "cpu" : "gpu", status = "ok", weights;
        std::vector<unsigned char> bytes, cpu_bytes;
        Quality quality;
        int same = -1;
        try {
            bool built = false;
            if (gpu) {
                built = gpu->build(item.recipe, bytes, timing, weights);
            }
            if (!built) {
                route = gpu ? "cpu_fallback" : "cpu";
                const auto start = Clock::now();
                DecodeTiming decode_timing;
                if (!build_recipe_tex(item.recipe, session, log, decode_timing, bytes)) status = "cpu_failed";
                timing.total = ms_since(start);
                timing.file = decode_timing.file;
                timing.upload = decode_timing.load;
                timing.wait = decode_timing.decode;
                timing.readback = decode_timing.readback;
            } else if (mode == "compare") {
                DecodeTiming decode_timing;
                if (build_recipe_tex(item.recipe, session, log, decode_timing, cpu_bytes)) {
                    same = cpu_bytes == bytes;
                    quality = measure_quality(item, session, log, cpu_bytes, bytes);
                }
            }
        } catch (const std::exception& error) {
            status = error.what();
            std::replace(status.begin(), status.end(), ',', ';');
            ++failures;
            if (gpu) gpu->recover();
            std::fprintf(stderr, "%s %s: %s\n", item.asset.c_str(), item.variant.c_str(), status.c_str());
        }
        csv << index++ << ',' << item.asset << ',' << item.variant << ',' << item.recipe.format << ','
            << source.width << ',' << source.height << ',' << item.recipe.length << ',' << route << ','
            << timing.file << ',' << timing.upload << ',' << timing.record << ',' << timing.wait << ','
            << timing.readback << ',' << timing.total << ',' << weights << ',' << quality.cpu_rmse << ','
            << quality.gpu_rmse << ',' << quality.gpu_alpha_mean << ',' << same << ',' << status << '\n';
        if (index % 50 == 0) {
            csv.flush();
            std::printf("%d/%zu %.1f s\n", index, items.size(), ms_since(wall) / 1000);
            std::fflush(stdout);
        }
    }
    csv.flush();
    std::printf("%s: %zu TEX em %.1f s, falhas=%d\n", mode.c_str(), items.size(), ms_since(wall) / 1000, failures);
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "erro: %s\n", error.what());
    return 1;
}
