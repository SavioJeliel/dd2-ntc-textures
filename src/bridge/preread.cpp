#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <dxgiformat.h>
#include <wrl/client.h>
#include <d3d12.h>

#include "preread.hpp"
#include "tex_recipe.hpp"
#include "gpu_tex.hpp"

#include <iomanip>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#define STB_DXT_IMPLEMENTATION
#include "bc1.hpp"

namespace {
using Clock = std::chrono::steady_clock;
using ReadTex = std::uint64_t (*)(void*, std::uintptr_t, void*, std::uint64_t, std::uint64_t);
using TexPair = std::array<std::vector<unsigned char>, 2>;
constexpr auto timeout = std::chrono::milliseconds(1500);
constexpr auto required_timeout = std::chrono::seconds(15);
constexpr auto required_on_demand_timeout = std::chrono::seconds(3);
constexpr std::uint64_t heavy_tex_bytes = 8 * 1024 * 1024;
constexpr std::uintptr_t reader_rva = 0x222640;
constexpr std::uintptr_t caller_rva = 0x222502;
constexpr unsigned char reader_prologue[] = {0x48, 0x89, 0xd0, 0x48, 0xc1, 0xe8, 0x10};

struct Variant {
    const char* name;
    std::uint64_t key, offset, packed, length, flags, hash;
    unsigned first_mip;
    std::array<unsigned char, 40> header;
};
constexpr Variant layout[] = {
    {"base", 18347148158647879888ull, 68250500942ull, 144936, 179864, 2,
     3459611124269350869ull, 2, {
        0x54,0x45,0x58,0x00,0x21,0x2f,0xf9,0x0e,0x00,0x02,0x00,0x02,0x01,0x00,0x01,0x70,
        0x48,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x01,0x45,0x00,0x00}},
    {"streaming", 15821680102874806015ull, 51068299416ull, 1947620, 2801336, 1026,
     3459611120835071741ull, 0, {
        0x54,0x45,0x58,0x00,0x21,0x2f,0xf9,0x0e,0x00,0x08,0x00,0x08,0x01,0x00,0x01,0x90,
        0x48,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x05,0x00,0x00}},
};
constexpr Variant height_layout{"streaming", 0, 51075348485ull, 2612649, 2801336, 1026,
    1837492159466892712ull, 0, {
    0x54,0x45,0x58,0x00,0x21,0x2f,0xf9,0x0e,0x00,0x08,0x00,0x08,0x01,0x00,0x01,0x90,
    0x50,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x00}};
constexpr Variant normal_layout{"streaming", 0, 51070247036ull, 5101449, 5594808, 1026,
    14275772827735448574ull, 0, {
    0x54,0x45,0x58,0x00,0x21,0x2f,0xf9,0x0e,0x00,0x08,0x00,0x08,0x01,0x00,0x01,0x90,
    0x62,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x80,0x06,0x00,0x00}};


struct Entry {
    std::uint64_t offset = 0, packed = 0, flags = 0, hash = 0, key = 0;
};

enum class Route { undecided, original, reconstructed, unavailable };
enum class Policy { optional, required_prewarm, required_on_demand };
struct Asset;
using CacheList = std::list<std::pair<Asset*, unsigned>>;
// Estado por variante do modo de lote, em que base e streaming decidem a rota separadamente.
struct LotState {
    std::atomic<Route> route{Route::undecided};
    Clock::time_point queued_at, deadline;
    std::atomic<bool> queued{false};
    bool finished = false;
    bool required = false; // O original foi zerado no PAK: atraso ou falha nunca caem nele.
    // TEX pronto, lido com std::atomic_load: um leitor no meio da cópia mantém o buffer vivo após o descarte.
    std::shared_ptr<const std::vector<unsigned char>> buffer;
    // Campos do cache, sob o mutex.
    bool cached = false, consumed = false, evicted = false;
    std::size_t cached_size = 0;
    CacheList::iterator position;
};
struct Asset {
    std::string name;
    std::wstring logical;
    std::filesystem::path ntc;
    std::array<Entry, 2> entries;
    std::array<const Variant*, 2> variants{&layout[0], &layout[1]};
    std::array<Variant, 2> catalog_layout{};
    std::array<TexRecipe, 2> recipes{};
    unsigned side = 2048;
    int channels = 3;
    int packed_first_channel = 0;
    int packed_channels = 0;
    TexPair buffers;
    std::atomic<Route> route{Route::undecided};
    Clock::time_point deadline;
    Clock::time_point queued_at;
    Policy policy = Policy::optional;
    bool queued = false;
    bool finished = false;
    std::atomic<unsigned> reported{0};
    std::atomic<unsigned> timing_reads{0};
    std::atomic<Clock::duration::rep> observed_request{0};
    std::array<LotState, 2> lot;
};
struct Match {
    Asset* asset;
    unsigned variant;
};
struct LogicalPathLess {
    bool operator()(const wchar_t* left, const wchar_t* right) const {
        return _wcsicmp(left, right) < 0;
    }
};
std::deque<Asset> assets;
std::unordered_multimap<std::uint64_t, Match> by_offset;
std::map<const wchar_t*, Asset*, LogicalPathLess> by_path;
struct WorkLane {
    const char* name;
    std::deque<Asset*> queue;
    bool running = false;
};
WorkLane normal_lane{"normal"};
WorkLane heavy_lane{"heavy"};
std::mutex mutex;
std::condition_variable changed;
std::atomic<bool> enabled{false};
bool gameplay_trace = false;
bool trace_original_only = false;
bool trace_allow_pak_fallback = false;
ReadTex original_read = nullptr;
const void* expected_caller = nullptr;
const REFrameworkPluginFunctions* logger = nullptr;
// Modo de lote: ativo quando gpu-lot.txt existe. Só os ativos listados passam pela DLL.
struct LotItem {
    Asset* asset;
    unsigned variant;
};
bool lot_mode = false;
std::unordered_set<std::string> lot_names;
std::unordered_set<std::string> lot_required; // gpu-lot-required.txt: originais zerados no PAK.
std::deque<LotItem> lot_base_queue, lot_streaming_queue;
const REFrameworkRendererData* renderer_data = nullptr;
constexpr auto lot_timeout = std::chrono::seconds(3);
constexpr auto lot_required_timeout = std::chrono::seconds(60);

void configure(Asset& asset, const std::filesystem::path& directory, const std::string& name) {
    asset.name = name;
    const std::wstring stem(name.begin(), name.end());
    asset.logical = L"Environment/Textures/" + stem + L".tex";
    asset.ntc = directory / (stem + L".ntc");
}

Variant catalog_variant(const Entry& entry, const std::string& hex, unsigned side,
                        int channels, unsigned index, unsigned first_mip) {
    Variant result{index == 0 ? "base" : "streaming", 0, entry.offset, entry.packed, 0,
                   entry.flags, entry.hash, first_mip, {}};
    if (hex.size() != 80 || hex.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw std::runtime_error("Invalid catalog TEX header");
    }
    for (std::size_t i = 0; i < result.header.size(); ++i) {
        result.header[i] = static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    const unsigned count = texture_mip_count(side) - first_mip;
    const unsigned width = side >> first_mip;
    const auto& h = result.header;
    const unsigned format = h[16];
    const bool valid_format = channels == 1 ? format == 80 : channels == 3
        ? format == 71 || format == 72 : format == 98 || format == 99;
    if (std::memcmp(h.data(), "TEX\0", 4) != 0 || (h[8] | (h[9] << 8)) != width ||
        (h[10] | (h[11] << 8)) != width || h[14] != 1 || h[15] != count * 16 ||
        !valid_format || h[17] || h[18] || h[19]) {
        throw std::runtime_error("Unsupported catalog TEX header");
    }
    result.length = 40 + 16 * count;
    for (unsigned mip = first_mip; mip < static_cast<unsigned>(texture_mip_count(side)); ++mip) {
        const unsigned dim = side >> mip;
        result.length += (std::max)(256u, dim * (channels == 4 ? 4u : 2u)) * (dim / 4);
    }
    return result;
}

void load_manifest() {
    const auto directory = ntc_data_directory();
    if (directory.empty()) throw std::runtime_error("Missing plugin data directory");
    auto& floor = assets.emplace_back();
    configure(floor, directory, "tx_074_albd");
    by_path.emplace(floor.logical.c_str(), &floor);
    for (int i = 0; i < 2; ++i) {
        const auto& v = layout[i];
        floor.entries[i] = {v.offset, v.packed, v.flags, v.hash, v.key};
        by_offset.emplace(v.offset, Match{&floor, static_cast<unsigned>(i)});
    }
    if (std::ifstream lot(directory / L"gpu-lot.txt"); lot) {
        for (std::string line; std::getline(lot, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lot_names.insert(line);
        }
        if (lot_names.empty()) throw std::runtime_error("Empty GPU lot");
        lot_mode = true;
        // Fora do lote, o chão fica no PAK.
        floor.lot[0].route.store(Route::original);
        floor.lot[1].route.store(Route::original);
        if (std::ifstream required(directory / L"gpu-lot-required.txt"); required) {
            for (std::string line; std::getline(required, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) lot_required.insert(line);
            }
        }
    }
    std::ifstream manifest(directory / L"batch-01.tsv");
    TexRecipeBundle recipe_bundle;
    unsigned packed_maps = 0;
    std::map<std::string, std::pair<int, unsigned>> packages;
    for (std::string line; std::getline(manifest, line);) {
        if (line == "# recipes.bundle") {
            if (!recipe_bundle.empty()) throw std::runtime_error("Duplicate TEX recipe bundle directive");
            recipe_bundle = read_tex_recipe_bundle(directory);
            if (recipe_bundle.empty()) throw std::runtime_error("Missing TEX recipe bundle");
            continue;
        }
        std::istringstream input(line);
        std::string name, extra;
        std::array<Entry, 2> entries;
        if (!(input >> name)) continue;
        if (name.front() == '#') continue;
        if (lot_mode && !lot_names.count(name)) continue;
        const bool catalog_map = name.size() == 10 && name.compare(0, 4, "map_") == 0 &&
            name.find_first_not_of("0123456789", 4) == std::string::npos;
        const Variant* streaming_only = name == "tx_074_nrra" ? &normal_layout
            : name == "tx_074_hgt" ? &height_layout : nullptr;
        if (!catalog_map && !streaming_only && (name.size() != 11 || name.compare(0, 3, "tx_") != 0 ||
            name.compare(6, 5, "_albd") != 0 || name.find_first_not_of("0123456789", 3) != 6)) {
            throw std::runtime_error("Invalid batch texture name");
        }
        for (auto& entry : entries) {
            if (!(input >> entry.offset >> entry.packed >> entry.flags >> entry.hash)) {
                throw std::runtime_error("Invalid batch PAK entry");
            }
        }
        const auto missing = [](const Entry& e) {
            return !e.offset && !e.packed && !e.flags && !e.hash;
        };
        const auto valid = [](const Entry& e) { return e.offset && e.packed && e.hash; };
        const bool paired = valid(entries[0]) && valid(entries[1]);
        const bool single = (valid(entries[0]) && missing(entries[1])) ||
                            (missing(entries[0]) && valid(entries[1]));
        if (streaming_only) {
            const auto& base = entries[0];
            const auto& stream = entries[1];
            if (base.offset || base.packed || base.flags || base.hash ||
                stream.offset != streaming_only->offset || stream.packed != streaming_only->packed ||
                stream.flags != streaming_only->flags || stream.hash != streaming_only->hash) {
                throw std::runtime_error("Invalid streaming-only PAK entry");
            }
        } else if (!paired && !(catalog_map && single)) {
            throw std::runtime_error("Invalid batch PAK entry");
        }
        auto policy = Policy::optional;
        if (input >> extra) {
            if (extra == "required") policy = Policy::required_prewarm;
            else if (extra == "required-on-demand") policy = Policy::required_on_demand;
            else if (extra != "optional") throw std::runtime_error("Invalid batch policy");
        }
        int packed_first = 0, packed_channels = 0;
        bool uses_recipe = false;
        std::string package;
        if (input >> extra) {
            if (extra == "recipe" && catalog_map) {
                uses_recipe = true;
            } else if (extra == "packed-tx074") {
                if (policy != Policy::required_prewarm || (name != floor.name && !streaming_only)) {
                    throw std::runtime_error("Invalid packed TX_074 map");
                }
                packed_first = name == floor.name ? 0 : streaming_only == &normal_layout ? 3 : 7;
                packed_channels = 8;
                package = "tx_074_material";
                ++packed_maps;
            } else {
                std::replace(extra.begin(), extra.end(), ':', ' ');
                std::istringstream spec(extra);
                std::string type, trailing;
                if (!(spec >> type >> package >> packed_first >> packed_channels) || spec >> trailing ||
                    type != "packed" || name == floor.name || streaming_only ||
                    package.size() < 8 || package.size() > 11 || package.compare(0, 5, "pack_") != 0 ||
                    package.find_first_not_of("0123456789", 5) != std::string::npos ||
                    packed_channels < 1 || packed_channels > 16 || packed_first < 0 ||
                    packed_first >= packed_channels || (!catalog_map &&
                    (packed_channels < 3 || packed_channels > 15 || packed_channels % 3 != 0 ||
                     packed_first > packed_channels - 3 || packed_first % 3 != 0))) {
                    throw std::runtime_error("Invalid packed BC1 map");
                }
            }
        }
        std::string logical_path;
        std::array<Variant, 2> custom{};
        std::array<TexRecipe, 2> recipes{};
        unsigned side = 2048;
        int channels = streaming_only ? (streaming_only == &normal_layout ? 4 : 1) : 3;
        if (catalog_map) {
            if ((!packed_channels && !uses_recipe) || !(input >> std::quoted(logical_path)) || logical_path.size() <= 4 ||
                logical_path.compare(logical_path.size() - 4, 4, ".tex") != 0 ||
                logical_path.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_/-. ", 0) != std::string::npos ||
                logical_path.front() == '/' || logical_path.find("..") != std::string::npos ||
                logical_path.find("//") != std::string::npos) {
                throw std::runtime_error("Invalid catalog texture path");
            }
            if (uses_recipe) {
                for (unsigned i = 0; i < recipes.size(); ++i) {
                    std::string file;
                    if (!(input >> file) || missing(entries[i]) != (file == "-")) {
                        throw std::runtime_error("TEX recipe does not match PAK entry");
                    }
                    if (file == "-") continue;
                    recipes[i] = read_tex_recipe(directory, file, recipe_bundle);
                    const auto& entry = entries[i];
                    custom[i] = {i == 0 ? "base" : "streaming", 0, entry.offset, entry.packed,
                                 recipes[i].length, entry.flags, entry.hash, 0, {}};
                }
            } else if (input >> extra) {
                std::replace(extra.begin(), extra.end(), ':', ' ');
                std::istringstream spec(extra);
                std::string type, base_header, stream_header, trailing;
                if (!(spec >> type >> side >> channels >> base_header >> stream_header) || spec >> trailing ||
                    type != "layout" || (side != 512 && side != 1024 && side != 2048) ||
                    (channels != 1 && channels != 3 && channels != 4)) {
                    throw std::runtime_error("Invalid catalog texture layout");
                }
                const std::array headers{base_header, stream_header};
                for (unsigned i = 0; i < headers.size(); ++i) {
                    if (missing(entries[i]) != (headers[i] == "-")) {
                        throw std::runtime_error("Catalog TEX header does not match PAK entry");
                    }
                    if (valid(entries[i])) {
                        custom[i] = catalog_variant(entries[i], headers[i], side, channels, i,
                                                    paired && i == 0 ? 2 : 0);
                    }
                }
                if (paired && custom[0].header[16] != custom[1].header[16]) {
                    throw std::runtime_error("Catalog variants use different formats");
                }
            } else if (!paired) {
                throw std::runtime_error("Single catalog variant requires a TEX layout");
            }
        }
        if (packed_channels && package != "tx_074_material") {
            if (packed_first + channels > packed_channels) throw std::runtime_error("Invalid packed channel range");
            auto& [total, mask] = packages[package];
            const unsigned channels_mask = ((1u << channels) - 1) << packed_first;
            if ((total && total != packed_channels) || (mask & channels_mask)) {
                throw std::runtime_error("Overlapping or inconsistent packed channels");
            }
            total = packed_channels;
            mask |= channels_mask;
        }
        if (input >> extra || entries[0].offset == entries[1].offset) {
            throw std::runtime_error("Invalid batch manifest");
        }
        if (name == assets[0].name) {
            if (policy != Policy::required_prewarm || assets[0].policy != Policy::optional) {
                throw std::runtime_error("Invalid floor policy");
            }
            for (unsigned i = 0; i < 2; ++i) {
                const auto& v = layout[i];
                const auto& e = entries[i];
                if (e.offset != v.offset || e.packed != v.packed || e.flags != v.flags || e.hash != v.hash) {
                    throw std::runtime_error("Floor metadata mismatch");
                }
            }
            assets[0].policy = policy;
            if (packed_channels) {
                floor.ntc = directory / L"tx_074_material.ntc";
                floor.packed_first_channel = packed_first;
                floor.packed_channels = packed_channels;
            }
            continue;
        }
        auto& asset = assets.emplace_back();
        configure(asset, directory, name);
        if (catalog_map) asset.logical.assign(logical_path.begin(), logical_path.end());
        if (custom[0].length || custom[1].length) {
            asset.catalog_layout = custom;
            asset.recipes = std::move(recipes);
            for (unsigned i = 0; i < custom.size(); ++i) {
                asset.variants[i] = custom[i].length ? &asset.catalog_layout[i] : nullptr;
            }
            asset.side = side;
        }
        asset.channels = channels;
        if (streaming_only) {
            asset.variants = {nullptr, streaming_only};
        }
        asset.policy = policy;
        if (packed_channels) {
            asset.ntc = directory / (package + ".ntc");
            asset.packed_first_channel = packed_first;
            asset.packed_channels = packed_channels;
        }
        asset.entries = entries;
        if (const auto [found, inserted] = by_path.emplace(asset.logical.c_str(), &asset); !inserted) {
            // O DLC repete caminhos do PAK principal com trechos próprios. A primeira entrada continua atendendo
            // pedidos por nome, e a outra é reconhecida pela leitura, pela identidade do trecho.
            const auto& other = *found->second;
            const bool same_entries = other.entries[0].offset == entries[0].offset && other.entries[1].offset == entries[1].offset;
            if (same_entries || !uses_recipe) throw std::runtime_error("Duplicate batch texture");
        }
        for (unsigned i = 0; i < entries.size(); ++i) {
            if (!asset.variants[i]) continue;
            const auto matches = by_offset.equal_range(entries[i].offset);
            for (auto match = matches.first; match != matches.second; ++match) {
                const auto& other = match->second;
                if (!asset.recipes[i].shared ||
                    asset.recipes[i].shared_file != other.asset->recipes[other.variant].shared_file ||
                    asset.variants[i]->length != other.asset->variants[other.variant]->length ||
                    entries[i].packed != other.asset->entries[other.variant].packed) {
                    throw std::runtime_error("Duplicate batch PAK offset without a shared recipe");
                }
            }
            by_offset.emplace(entries[i].offset, Match{&asset, i});
        }
    }
    if (manifest.bad()) throw std::runtime_error("Cannot read batch manifest");
    if (packed_maps != 0 && packed_maps != 3) {
        throw std::runtime_error("Packed TX_074 requires all three maps");
    }
    for (const auto& entry : packages) {
        const auto& [total, mask] = entry.second;
        if (mask != (1u << total) - 1) throw std::runtime_error("Incomplete packed BC1 channels");
    }
    if (lot_mode) {
        if (assets.size() - 1 != lot_names.size()) throw std::runtime_error("GPU lot has names outside the manifest");
        for (const auto& name : lot_required) {
            if (!lot_names.count(name)) throw std::runtime_error("Required GPU texture outside the lot: " + name);
        }
        for (std::size_t i = 1; i < assets.size(); ++i) {
            const bool required = lot_required.count(assets[i].name) != 0;
            assets[i].lot[0].required = required;
            assets[i].lot[1].required = required;
            for (unsigned v = 0; v < 2; ++v) {
                if (assets[i].variants[v] && !gpu_tex_supported(assets[i].recipes[v])) {
                    throw std::runtime_error("GPU lot has a recipe without GPU route: " + assets[i].name);
                }
            }
        }
    }
}

bool read_pak_row(std::uintptr_t token, std::uint64_t (&values)[6]) {
    __try {
        const auto* slot = reinterpret_cast<const unsigned char*>((token >> 19) << 3);
        const auto* context = *reinterpret_cast<const unsigned char* const*>(slot);
        const auto index = *reinterpret_cast<const std::uint32_t*>(slot + 8);
        if (!context || index > 0xffff0000u) return false;
        const auto* table = *reinterpret_cast<const std::uint64_t* const*>(context + 0x168);
        if (!table) return false;
        const auto* row = table + std::size_t(index) * 6;
        std::memcpy(values, row, sizeof(values));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Match find_variant(std::uintptr_t token) {
    std::uint64_t row[6];
    if (!read_pak_row(token, row)) return {};
    const auto matches = std::as_const(by_offset).equal_range(row[1]);
    for (auto found = matches.first; found != matches.second; ++found) {
        const auto match = found->second;
        const auto& entry = match.asset->entries[match.variant];
        const auto& variant = *match.asset->variants[match.variant];
        if ((!entry.key || row[0] == entry.key) && row[2] == entry.packed &&
            row[3] == variant.length && row[4] == entry.flags && row[5] == entry.hash) return match;
    }
    return {};
}

void encode_data_map(const unsigned char* pixels, unsigned width, int channels,
                     unsigned char* output, std::size_t pitch) {
    if (channels == 4) {
        encode_bc7_map(pixels, width, width, std::size_t(width) * channels, channels, output, pitch);
        return;
    }
    for (unsigned y = 0; y < width; y += 4) {
        auto* row = output + (y / 4) * pitch;
        for (unsigned x = 0; x < width; x += 4) {
            unsigned char block[16];
            for (unsigned line = 0; line < 4; ++line) {
                std::memcpy(block + line * 4 * channels,
                            pixels + ((y + line) * width + x) * channels, 4 * channels);
            }
            stb_compress_bc4_block(row + (x / 4) * 8, block);
        }
    }
}

bool build_tex(const Asset& asset, const TextureMips& mips, TexPair& prepared) {
    constexpr unsigned base = 0;
    constexpr unsigned streaming = 1;
    std::array<std::uint64_t, std::tuple_size_v<TextureMips>> streaming_offsets{};
    for (const unsigned i : {streaming, base}) {
        if (!asset.variants[i]) continue;
        const auto& variant = *asset.variants[i];
        auto& bytes = prepared[i];
        bytes.resize(variant.length);
        std::memcpy(bytes.data(), variant.header.data(), variant.header.size());
        const unsigned count = texture_mip_count(asset.side) - variant.first_mip;
        std::uint64_t offset = 40 + 16 * count;
        for (unsigned mip = 0; mip < count; ++mip) {
            const unsigned source_mip = mip + variant.first_mip;
            const unsigned width = asset.side >> source_mip;
            const std::uint32_t pitch = (std::max)(256u, width * (asset.channels == 4 ? 4u : 2u));
            const std::uint32_t size = pitch * (width / 4);
            if (offset + size > bytes.size()) {
                return false;
            }
            auto* entry = bytes.data() + 40 + 16 * mip;
            std::memcpy(entry, &offset, sizeof(offset));
            std::memcpy(entry + 8, &pitch, sizeof(pitch));
            std::memcpy(entry + 12, &size, sizeof(size));
            if (i == streaming || !asset.variants[streaming]) {
                if (mips[source_mip].size() != std::size_t(width) * width * asset.channels) return false;
                streaming_offsets[source_mip] = offset;
                const auto* pixels = mips[source_mip].data();
                if (asset.channels == 3) encode_bc1(pixels, width, width, bytes.data() + offset, pitch);
                else encode_data_map(pixels, width, asset.channels, bytes.data() + offset, pitch);
            } else {
                // Both layouts use the same pitch for a given source mip.
                std::memcpy(bytes.data() + offset,
                            prepared[streaming].data() + streaming_offsets[source_mip], size);
            }
            offset += size;
        }
        if (offset != bytes.size()) return false;
    }
    return true;
}

WorkLane& lane_for(const Asset& asset) {
    std::uint64_t bytes = 0;
    for (const auto* variant : asset.variants) {
        if (variant) bytes += variant->length;
    }
    const bool signed_bc6h = std::any_of(asset.recipes.begin(), asset.recipes.end(),
        [](const TexRecipe& recipe) { return recipe.format == DXGI_FORMAT_BC6H_SF16; });
    return bytes >= heavy_tex_bytes || signed_bc6h ? heavy_lane : normal_lane;
}

DWORD WINAPI worker(void* parameter) {
    auto& lane = *static_cast<WorkLane*>(parameter);
    DecodeSession session;
    for (;;) {
        Asset* asset = nullptr;
        {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(10), [&lane] { return !lane.queue.empty(); })) {
                lock.unlock();
                session.reset();
                lock.lock();
                if (lane.queue.empty()) {
                    lane.running = false;
                    return 0;
                }
            }
            asset = lane.queue.front();
            lane.queue.pop_front();
            if (asset->route.load() != Route::undecided) continue;
        }
        TexPair prepared;
        bool success = false;
        DecodeTiming timing;
        const auto started_at = Clock::now();
        double build_ms = 0;
        try {
            TextureMips mips;
            if (asset->recipes[0].length || asset->recipes[1].length) {
                const auto build_start = Clock::now();
                success = true;
                for (unsigned i = 0; i < asset->recipes.size() && success; ++i) {
                    if (asset->recipes[i].length) {
                        success = build_recipe_tex(asset->recipes[i], session, *logger, timing, prepared[i]);
                    }
                }
                build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
            } else if (session.decode(asset->ntc, *logger, asset->channels, mips, timing,
                               asset->packed_first_channel, asset->packed_channels, asset->side)) {
                const auto build_start = Clock::now();
                success = build_tex(*asset, mips, prepared);
                build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
            }
        } catch (const std::exception& error) {
            logger->log_error("[dd2_ntc_bridge] %s decode: %s", asset->name.c_str(), error.what());
        }
        {
            std::lock_guard lock(mutex);
            logger->log_info(
                "[dd2_ntc_bridge] %s timing_ms queue=%.3f file=%.3f library=%.3f context=%.3f "
                "load=%.3f decode=%.3f gpu=%.3f readback=%.3f tex=%.3f total=%.3f result=%s lane=%s",
                asset->name.c_str(),
                std::chrono::duration<double, std::milli>(started_at - asset->queued_at).count(),
                timing.file, timing.library, timing.context, timing.load, timing.decode, timing.gpu,
                timing.readback, build_ms,
                std::chrono::duration<double, std::milli>(Clock::now() - asset->queued_at).count(),
                success ? "ready" : "failed", lane.name);
            if (success && asset->route.load() == Route::undecided) {
                asset->buffers = std::move(prepared);
                for (unsigned i = 0; i < asset->recipes.size(); ++i) {
                    if (asset->recipes[i].length) {
                        logger->log_info("[dd2_ntc_bridge] %s TEX built %s bytes=%zu",
                                         asset->name.c_str(), asset->variants[i]->name,
                                         asset->buffers[i].size());
                    }
                }
                logger->log_info("[dd2_ntc_bridge] %s reconstructed TEX ready", asset->name.c_str());
            } else if (!success) {
                logger->log_warn("[dd2_ntc_bridge] %s preparation failed", asset->name.c_str());
            }
            asset->finished = true;
        }
        changed.notify_all();
    }
}

void enqueue(Asset& asset) {
    std::lock_guard lock(mutex);
    if (asset.queued) return;
    asset.queued = true;
    asset.queued_at = Clock::now();
    const char* policy_name = "on-demand";
    auto budget = timeout;
    switch (asset.policy) {
        case Policy::optional:
            break;
        case Policy::required_prewarm:
            policy_name = "required";
            budget = required_timeout;
            break;
        case Policy::required_on_demand:
            policy_name = "required-on-demand";
            budget = required_on_demand_timeout;
            break;
    }
    asset.deadline = asset.queued_at + budget;
    auto& lane = lane_for(asset);
    try {
        lane.queue.push_back(&asset);
    } catch (const std::bad_alloc&) {
        asset.finished = true;
        logger->log_error("[dd2_ntc_bridge] Cannot queue TEX preparation");
        return;
    }
    logger->log_info("[dd2_ntc_bridge] %s %s TEX preparation requested lane=%s",
                     asset.name.c_str(), policy_name, lane.name);
    if (lane.running) {
        changed.notify_all();
        return;
    }
    lane.running = true;
    if (HANDLE thread = CreateThread(nullptr, 0, worker, &lane, 0, nullptr)) {
        CloseHandle(thread);
    } else {
        lane.running = false;
        lane.queue.pop_back();
        asset.finished = true;
        logger->log_error("[dd2_ntc_bridge] Cannot start TEX worker: %lu", GetLastError());
    }
}

Route choose_route(Asset& asset) {
    auto selected = asset.route.load(std::memory_order_acquire);
    if (selected != Route::undecided) return selected;
    enqueue(asset);
    std::unique_lock lock(mutex);
    const auto wait_start = Clock::now();
    changed.wait_until(lock, asset.deadline, [&] {
        return asset.finished || asset.route.load() != Route::undecided;
    });
    selected = asset.route.load(std::memory_order_relaxed);
    const auto wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count();
    if (wait_ms >= 0.1) {
        logger->log_info("[dd2_ntc_bridge] %s reader_wait_ms=%.3f thread=%lu",
                         asset.name.c_str(), wait_ms, GetCurrentThreadId());
    }
    if (selected == Route::undecided) {
        const bool ready = asset.finished &&
            (!asset.variants[0] || !asset.buffers[0].empty()) &&
            (!asset.variants[1] || !asset.buffers[1].empty());
        selected = ready
            ? Route::reconstructed
            : (asset.policy == Policy::optional || trace_allow_pak_fallback ? Route::original : Route::unavailable);
        asset.route.store(selected, std::memory_order_release);
        if (selected == Route::original) {
            logger->log_warn("[dd2_ntc_bridge] %s TEX unavailable; using PAK for this session", asset.name.c_str());
        } else if (selected == Route::unavailable) {
            logger->log_error("[dd2_ntc_bridge] %s required NTC unavailable (%s); PAK read blocked",
                              asset.name.c_str(), asset.finished ? "preparation failed" : "deadline expired");
        }
        changed.notify_all();
    }
    return selected;
}

// Cache dos TEX prontos do lote. O teto vem da RAM da máquina: 3%, entre 256 MiB e 1 GiB.
// Só sai do cache o TEX que o jogo já leu até o fim; se ele for pedido de novo, a GPU refaz.
CacheList cache_order; // Mais antigo na frente.
std::size_t cache_bytes = 0, cache_peak = 0, cache_cap = 0;
std::uint64_t cache_evictions = 0, cache_rebuilds = 0;

void cache_log_locked(const char* why) {
    logger->log_info("[dd2_ntc_bridge] TEX cache %s bytes=%zu peak=%zu cap=%zu entries=%zu evictions=%llu rebuilds=%llu",
                     why, cache_bytes, cache_peak, cache_cap, cache_order.size(), cache_evictions, cache_rebuilds);
}

void cache_trim_locked() {
    for (auto it = cache_order.begin(); cache_bytes > cache_cap && it != cache_order.end();) {
        auto& state = it->first->lot[it->second];
        if (!state.consumed) {
            ++it;
            continue;
        }
        state.route.store(Route::undecided, std::memory_order_release);
        std::atomic_store(&state.buffer, std::shared_ptr<const std::vector<unsigned char>>{});
        state.evicted = true;
        state.cached = false;
        cache_bytes -= state.cached_size;
        it = cache_order.erase(it);
        if (++cache_evictions % 500 == 0) cache_log_locked("trim");
    }
}

void cache_insert_locked(Asset& asset, unsigned variant, std::size_t size) {
    auto& state = asset.lot[variant];
    if (state.cached) {
        cache_bytes -= state.cached_size;
        cache_order.erase(state.position);
    }
    state.position = cache_order.insert(cache_order.end(), {&asset, variant});
    state.cached = true;
    state.consumed = false;
    state.cached_size = size;
    cache_bytes += size;
    if (cache_bytes > cache_peak) {
        cache_peak = cache_bytes;
        if (cache_peak > cache_cap && cache_peak - size <= cache_cap) cache_log_locked("over-cap");
    }
    cache_trim_locked();
}

// O jogo leu o TEX até o fim: vira o mais recente e pode ser descartado quando o cache encher.
void cache_consumed(Asset& asset, unsigned variant) {
    std::lock_guard lock(mutex);
    auto& state = asset.lot[variant];
    if (!state.cached) return;
    state.consumed = true;
    cache_order.splice(cache_order.end(), cache_order, state.position);
    cache_trim_locked();
}

void lot_enqueue(Asset& asset, unsigned variant) {
    if (!asset.variants[variant]) return;
    auto& state = asset.lot[variant];
    // create_resource repete os nomes milhares de vezes: evita o mutex no caminho quente do jogo.
    if (state.queued.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(mutex);
        if (state.queued.load(std::memory_order_relaxed)) return;
        state.queued.store(true, std::memory_order_release);
        state.queued_at = Clock::now();
        state.deadline = state.queued_at + (state.required
            ? std::chrono::duration_cast<Clock::duration>(lot_required_timeout)
            : std::chrono::duration_cast<Clock::duration>(lot_timeout));
        (variant == 0 ? lot_base_queue : lot_streaming_queue).push_back({&asset, variant});
    }
    changed.notify_all();
}

Route lot_route(Asset& asset, unsigned variant) {
    auto& state = asset.lot[variant];
    auto selected = state.route.load(std::memory_order_acquire);
    if (selected != Route::undecided) return selected;
    {
        // Descartado do cache depois de lido: este pedido refaz o TEX na GPU.
        std::lock_guard lock(mutex);
        if (state.evicted && state.route.load(std::memory_order_relaxed) == Route::undecided) {
            state.evicted = false;
            state.finished = false;
            state.queued.store(false, std::memory_order_release);
            ++cache_rebuilds;
        }
    }
    lot_enqueue(asset, variant);
    std::unique_lock lock(mutex);
    const auto wait_start = Clock::now();
    changed.wait_until(lock, state.deadline, [&] {
        return state.finished || state.route.load() != Route::undecided;
    });
    selected = state.route.load(std::memory_order_relaxed);
    const auto wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count();
    if (gameplay_trace && wait_ms >= 0.1) {
        logger->log_info("[dd2_ntc_bridge] %s %s reader_wait_ms=%.3f thread=%lu", asset.name.c_str(),
                         asset.variants[variant]->name, wait_ms, GetCurrentThreadId());
    }
    if (selected == Route::undecided) {
        const bool ready = state.finished && std::atomic_load(&state.buffer) != nullptr;
        selected = ready ? Route::reconstructed : state.required ? Route::unavailable : Route::original;
        state.route.store(selected, std::memory_order_release);
        if (!ready && state.required) {
            logger->log_error("[dd2_ntc_bridge] %s %s required TEX unavailable (%s); PAK read blocked", asset.name.c_str(),
                              asset.variants[variant]->name, state.finished ? "preparation failed" : "deadline expired");
        } else if (!ready) {
            logger->log_warn("[dd2_ntc_bridge] %s %s TEX unavailable (%s); using PAK", asset.name.c_str(),
                             asset.variants[variant]->name, state.finished ? "preparation failed" : "deadline expired");
        }
        changed.notify_all();
    }
    return selected;
}

Microsoft::WRL::ComPtr<ID3D12Device> find_game_device() {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (renderer_data && renderer_data->device) {
        static_cast<IUnknown*>(renderer_data->device)->QueryInterface(IID_PPV_ARGS(&device));
    }
    if (!device) {
        // Depois que o jogo criou o dispositivo, esta chamada devolve o mesmo objeto para o adaptador.
        D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        logger->log_warn("[dd2_ntc_bridge] GPU lot: REFramework has no device; used D3D12CreateDevice");
    }
    return device;
}

DWORD WINAPI lot_worker(void*) {
    {
        // Espera o jogo criar o dispositivo: até o primeiro pedido, ou 20 s para preaquecer antes do save.
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(20),
                         [] { return !lot_base_queue.empty() || !lot_streaming_queue.empty(); });
    }
    std::unique_ptr<GpuTexBuilder> builder;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    try {
        const auto start = Clock::now();
        device = find_game_device();
        if (!device) throw std::runtime_error("no D3D12 device");
        builder = make_gpu_tex_builder(device.Get(), ntc_data_directory());
        // Compila antes um pipeline de cada formato, porque a primeira compilação chega a segundos.
        std::unordered_set<unsigned> warmed;
        for (std::size_t i = 1; i < assets.size(); ++i) {
            for (unsigned v = 0; v < 2; ++v) {
                const auto& recipe = assets[i].recipes[v];
                if (!assets[i].variants[v] || !warmed.insert(recipe.format).second) continue;
                std::vector<unsigned char> bytes;
                GpuTiming timing;
                std::string weights;
                builder->build(recipe, bytes, timing, weights);
                logger->log_info("[dd2_ntc_bridge] GPU warm format=%u record_ms=%.3f wait_ms=%.3f weights=%s",
                                 recipe.format, timing.record, timing.wait, weights.c_str());
            }
        }
        logger->log_info("[dd2_ntc_bridge] GPU lot ready warm_ms=%.3f",
                         std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    } catch (const std::exception& error) {
        builder.reset();
        logger->log_error("[dd2_ntc_bridge] GPU lot unavailable: %s", error.what());
    }
    for (;;) {
        LotItem item{};
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [] { return !lot_base_queue.empty() || !lot_streaming_queue.empty(); });
            // A base bloqueia os threads de carregamento, então passa à frente do streaming.
            auto& queue = !lot_base_queue.empty() ? lot_base_queue : lot_streaming_queue;
            item = queue.front();
            queue.pop_front();
            if (item.asset->lot[item.variant].route.load() != Route::undecided) continue;
        }
        std::vector<unsigned char> bytes;
        GpuTiming timing;
        std::string weights;
        const auto started = Clock::now();
        bool success = false;
        if (builder) {
            try {
                success = builder->build(item.asset->recipes[item.variant], bytes, timing, weights);
            } catch (const std::exception& error) {
                builder->recover();
                logger->log_error("[dd2_ntc_bridge] %s GPU build: %s", item.asset->name.c_str(), error.what());
            }
        }
        {
            std::lock_guard lock(mutex);
            auto& state = item.asset->lot[item.variant];
            if (gameplay_trace || !success) logger->log_info(
                "[dd2_ntc_bridge] %s:%s timing_ms queue=%.3f file=%.3f library=0 context=0 load=%.3f decode=%.3f "
                "gpu=%.3f readback=%.3f tex=0 total=%.3f result=%s lane=gpu",
                item.asset->name.c_str(), item.asset->variants[item.variant]->name,
                std::chrono::duration<double, std::milli>(started - state.queued_at).count(),
                timing.file, timing.upload, timing.record, timing.wait, timing.readback,
                std::chrono::duration<double, std::milli>(Clock::now() - state.queued_at).count(),
                success ? "ready" : "failed");
            if (success && state.route.load() == Route::undecided) {
                const auto size = bytes.size();
                std::atomic_store(&state.buffer, std::make_shared<const std::vector<unsigned char>>(std::move(bytes)));
                cache_insert_locked(*item.asset, item.variant, size);
            }
            state.finished = true;
        }
        changed.notify_all();
    }
}

std::uint64_t intercept_read(void* context, std::uintptr_t token, void* dst,
                             std::uint64_t count, std::uint64_t position) {
    if (enabled.load(std::memory_order_acquire) && _ReturnAddress() == expected_caller && dst && count) {
        const auto match = find_variant(token);
        if (!match.asset) return original_read(context, token, dst, count, position);
        auto& asset = *match.asset;
        const auto& recipe = asset.recipes[match.variant];
        unsigned event = 0;
        if (gameplay_trace) {
            const auto& variant = *asset.variants[match.variant];
            const std::size_t header_bytes = recipe.length ? recipe.header.size()
                : variant.header.size() + 16u * (texture_mip_count(asset.side) - variant.first_mip);
            const unsigned shift = match.variant * 3;
            const bool payload = position >= header_bytes || count > header_bytes - position;
            const bool reaches_end = position < variant.length && count >= variant.length - position;
            const unsigned mask = (1u | (payload ? 2u : 0u) | (reaches_end ? 4u : 0u)) << shift;
            event = (mask & ~asset.timing_reads.fetch_or(mask, std::memory_order_relaxed)) >> shift;
        }
        const auto start = event ? Clock::now() : Clock::time_point{};
        const auto observed = event && trace_original_only
            ? asset.observed_request.load(std::memory_order_relaxed) : 0;
        auto selected = trace_original_only ? Route::original
            : lot_mode ? lot_route(asset, match.variant) : choose_route(asset);
        std::shared_ptr<const std::vector<unsigned char>> held;
        if (lot_mode && selected == Route::reconstructed) {
            held = std::atomic_load(&asset.lot[match.variant].buffer);
            if (!held) {
                // Descartado entre a decisão e a cópia: decide de novo, o que refaz o TEX.
                selected = lot_route(asset, match.variant);
                held = std::atomic_load(&asset.lot[match.variant].buffer);
                if (selected == Route::reconstructed && !held) {
                    logger->log_error("[dd2_ntc_bridge] %s %s TEX cache miss without buffer", asset.name.c_str(),
                                      asset.variants[match.variant]->name);
                }
            }
        }
        const auto decision = event ? Clock::now() : Clock::time_point{};
        std::uint64_t length = 0;
        if (selected == Route::reconstructed) {
            static const std::vector<unsigned char> none;
            const auto& bytes = lot_mode ? (held ? *held : none) : asset.buffers[match.variant];
            if (position < bytes.size()) {
                length = (std::min)(count, bytes.size() - position);
                std::memcpy(dst, bytes.data() + position, length);
                if (lot_mode && position + length >= bytes.size()) cache_consumed(asset, match.variant);
                if (gameplay_trace && asset.reported.fetch_add(1, std::memory_order_relaxed) < 60) {
                    logger->log_info("[dd2_ntc_bridge] %s TEX in memory %s offset=%llu bytes=%llu",
                        asset.name.c_str(), layout[match.variant].name, position, length);
                }
            }
        } else if (selected == Route::original) {
            length = original_read(context, token, dst, count, position);
        }
        if (event) {
            const auto end = Clock::now();
            const auto ms = [](Clock::duration elapsed) {
                return std::chrono::duration<double, std::milli>(elapsed).count();
            };
            const auto& variant = *asset.variants[match.variant];
            const auto& lot_state = asset.lot[match.variant];
            const double lead = trace_original_only
                ? (observed ? (std::max)(0.0, ms(start.time_since_epoch() - Clock::duration(observed))) : -1.0)
                : (std::max)(0.0, ms(start - (lot_mode ? lot_state.queued_at : asset.queued_at)));
            const double budget = trace_original_only ? -1.0
                : lot_mode ? ms(lot_state.deadline - lot_state.queued_at) : ms(asset.deadline - asset.queued_at);
            logger->log_info(
                "[dd2_ntc_bridge] GAMEPLAY %s %s format=%u lane=%s event=%u route=%s "
                "lead_ms=%.3f decision_ms=%.3f service_ms=%.3f response_ms=%.3f "
                "budget_ms=%.3f tex_bytes=%llu offset=%llu asked=%llu returned=%llu thread=%lu",
                asset.name.c_str(), variant.name, recipe.length ? recipe.format : variant.header[16],
                lot_mode ? "gpu" : lane_for(asset).name, event,
                selected == Route::reconstructed ? "ntc" : selected == Route::original ? "pak" : "blocked",
                lead, ms(decision - start), ms(end - decision),
                ms(end - start), budget, variant.length,
                position, count, length, GetCurrentThreadId());
        }
        return length;
    }
    return original_read(context, token, dst, count, position);
}
}

bool install_preread_hook(const REFrameworkPluginFunctions& functions, const REFrameworkRendererData* renderer) {
    renderer_data = renderer;
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* entry = image + reader_rva;
    if (std::memcmp(entry, reader_prologue, sizeof(reader_prologue)) != 0) return false;
    expected_caller = image + caller_rva;
    logger = &functions;
    try {
        load_manifest();
        const auto data = ntc_data_directory();
        trace_original_only = std::filesystem::exists(data / L"trace-original-only.enabled");
        gameplay_trace = trace_original_only || std::filesystem::exists(data / L"trace-gameplay.enabled");
        if (gameplay_trace && !trace_original_only &&
            std::filesystem::exists(data / L"trace-intact-pak-fallback.enabled")) {
            const auto pak = data.parent_path().parent_path().parent_path() / L"re_chunk_000.pak";
            const auto attributes = GetFileAttributesW(pak.c_str());
            // Com gpu-lot-required.txt o PAK pode estar esparso: só os ativos listados foram zerados,
            // e eles nunca caem no PAK.
            const DWORD refused = lot_required.empty()
                ? (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_REPARSE_POINT) : FILE_ATTRIBUTE_REPARSE_POINT;
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & refused)) {
                throw std::runtime_error("Trace fallback requires the intact local PAK");
            }
            trace_allow_pak_fallback = true;
        }
        if (lot_mode && !trace_allow_pak_fallback) {
            // Modo normal, sem marcadores de teste: só o atraso de um ativo não zerado cai no PAK.
            // Um PAK esparso sem a lista dos zerados mandaria dados zerados ao jogo, então é recusado.
            const auto pak = data.parent_path().parent_path().parent_path() / L"re_chunk_000.pak";
            const auto attributes = GetFileAttributesW(pak.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((attributes & FILE_ATTRIBUTE_SPARSE_FILE) && lot_required.empty())) {
                throw std::runtime_error("Sparse PAK without gpu-lot-required.txt");
            }
        }
    } catch (const std::exception& error) {
        logger->log_error("[dd2_ntc_bridge] Cannot load batch manifest: %s", error.what());
        return false;
    }
    void* trampoline = nullptr;
    auto status = MH_CreateHook(entry, reinterpret_cast<void*>(&intercept_read), &trampoline);
    if (status == MH_OK) {
        original_read = reinterpret_cast<ReadTex>(trampoline);
        status = MH_EnableHook(entry);
        if (status != MH_OK) MH_RemoveHook(entry);
    }
    if (status != MH_OK) {
        logger->log_error("[dd2_ntc_bridge] TEX reader hook: %s", MH_StatusToString(status));
        return false;
    }
    if (gameplay_trace) logger->log_info("[dd2_ntc_bridge] GAMEPLAY TRACE START mode=%s",
        trace_original_only ? "original-only" : trace_allow_pak_fallback ? "ntc-intact-pak" : "ntc");
    enabled.store(true, std::memory_order_release);
    logger->log_info("[dd2_ntc_bridge] Pre-PAK TEX reader hook active (%zu textures)", assets.size());
    if (lot_mode) {
        MEMORYSTATUSEX memory{sizeof(memory)};
        const std::uint64_t total = GlobalMemoryStatusEx(&memory) ? memory.ullTotalPhys : 8ull << 30;
        cache_cap = static_cast<std::size_t>((std::clamp)(total * 3 / 100, 256ull << 20, 1ull << 30));
        logger->log_info("[dd2_ntc_bridge] GPU lot mode: %zu textures, %zu required; renderer device %p; "
                         "TEX cache cap=%zu (RAM %llu MiB)", lot_names.size(), lot_required.size(),
                         renderer_data ? renderer_data->device : nullptr, cache_cap, total >> 20);
        if (HANDLE thread = CreateThread(nullptr, 0, lot_worker, nullptr, 0, nullptr)) CloseHandle(thread);
        else logger->log_error("[dd2_ntc_bridge] Cannot start GPU worker: %lu", GetLastError());
        return true;
    }
    for (auto& asset : assets) {
        if (!trace_original_only && asset.policy == Policy::required_prewarm) enqueue(asset);
    }
    return true;
}

void request_texture_prepare(const wchar_t* logical_path, bool streaming_notice) {
    if (!enabled.load(std::memory_order_acquire)) return;
    if (lot_mode) {
        const auto found = std::as_const(by_path).find(logical_path);
        if (found != by_path.end() && found->second != &assets.front()) {
            // O streaming só entra pelo aviso seletivo ou pela própria leitura.
            lot_enqueue(*found->second, streaming_notice ? 1 : 0);
        }
        return;
    }
    if (trace_original_only && std::wcsncmp(logical_path, L"streaming/", 10) == 0) logical_path += 10;
    const auto found = std::as_const(by_path).find(logical_path);
    if (found != by_path.end() && found->second->route.load() == Route::undecided) {
        if (trace_original_only) {
            Clock::duration::rep unset = 0;
            found->second->observed_request.compare_exchange_strong(
                unset, Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        } else {
            enqueue(*found->second);
        }
    }
}
