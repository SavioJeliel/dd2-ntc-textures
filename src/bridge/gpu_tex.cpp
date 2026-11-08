// Rota de GPU: NTC até TEX com compute passes do LibNTC em D3D12. Usada pela DLL e pela bancada.
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <libntc/ntc.h>
#include "gpu_tex.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {
UINT D3D12CalcSubresource(UINT mip, UINT layer, UINT plane, UINT mips, UINT layers) {
    return mip + layer * mips + plane * mips * layers;
}
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

struct BcInfo {
    ntc::BlockCompressedFormat format;
    unsigned block_bytes;
    int channel_bytes;
};

bool gpu_format(unsigned dxgi, BcInfo& info) {
    switch (dxgi) {
        case 71: case 72: info = {ntc::BlockCompressedFormat::BC1, 8, 1}; return true;
        case 77: case 78: info = {ntc::BlockCompressedFormat::BC3, 16, 1}; return true;
        case 80: info = {ntc::BlockCompressedFormat::BC4, 8, 1}; return true;
        case 83: info = {ntc::BlockCompressedFormat::BC5, 16, 1}; return true;
        case 95: info = {ntc::BlockCompressedFormat::BC6, 16, 4}; return true;
        case 98: case 99: info = {ntc::BlockCompressedFormat::BC7, 16, 1}; return true;
        default: return false; // BC6H com sinal e formatos sem blocos ficam na rota da CPU.
    }
}

class UploadRing {
public:
    void create(ID3D12Device* device, UINT64 size) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)), "upload ring");
        check(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map upload");
        capacity = size;
    }
    UINT64 allocate(UINT64 size, UINT64 alignment) {
        const UINT64 start = (used + alignment - 1) & ~(alignment - 1);
        if (start + size > capacity) throw std::runtime_error("upload ring cheio");
        used = start + size;
        return start;
    }
    void reset() { used = 0; }
    ComPtr<ID3D12Resource> buffer;
    unsigned char* mapped = nullptr;
    UINT64 capacity = 0, used = 0;
};

class Pipeline final : public GpuTexBuilder {
public:
    Pipeline(ID3D12Device* game_device, const fs::path& data) : device(game_device) {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE; // Fila própria, como a DLL usaria junto do jogo.
        check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "fila");
        check(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)), "alocador");
        check(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "lista");
        check(list->Close(), "fechar lista");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        upload.create(device.Get(), 64ull * 1024 * 1024);

        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = readback_size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = descriptor_capacity;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors)), "descritores");
        descriptor_step = device->GetDescriptorHandleIncrementSize(heap_desc.Type);

        library = LoadLibraryExW((data / L"libntc.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library) throw std::runtime_error("libntc.dll");
        const auto create = reinterpret_cast<decltype(&ntc::CreateContext)>(GetProcAddress(library, "CreateContext"));
        last_error = reinterpret_cast<decltype(&ntc::GetLastErrorMessage)>(GetProcAddress(library, "GetLastErrorMessage"));
        ntc::ContextParameters params{};
        params.cudaDevice = ntc::DisableCudaDevice;
        params.graphicsApi = ntc::GraphicsAPI::D3D12;
        params.d3d12Device = device.Get();
        params.enableCooperativeVector = false; // O jogo não carrega o Agility SDK com CoopVec.
        // Sem CUDA o contexto é criado normalmente, e o retorno CudaUnavailable é o esperado.
        const auto context_status = create(&context, params);
        if (context_status != ntc::Status::CudaUnavailable) ntc_check(context_status, "CreateContext");
        make_root_signatures();
    }

    bool build(const TexRecipe& recipe, std::vector<unsigned char>& bytes, GpuTiming& timing, std::string& weight_name) override {
        if (!gpu_tex_supported(recipe)) return false;
        BcInfo bc{};
        gpu_format(recipe.format, bc);
        weights_in_ring = false;
        const auto started = Clock::now();
        bytes.assign(recipe.length, 0);
        std::copy(recipe.header.begin(), recipe.header.end(), bytes.begin());

        struct Copy {
            const TexRegion* region;
            UINT64 offset;
            UINT row_pitch, rows, row_bytes;
        };
        std::vector<Copy> copies;
        UINT64 readback_used = 0;

        check(allocator->Reset(), "reset alocador");
        check(list->Reset(allocator.Get(), nullptr), "reset lista");
        upload.reset();
        descriptor_used = 0;
        ID3D12DescriptorHeap* heaps[] = {descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);

        std::vector<ComPtr<ID3D12Resource>> keep_alive;
        for (const auto& source : recipe.sources) {
            load_package(source.path, timing);
            const auto record_start = Clock::now();
            const auto* metadata = find_metadata(source);
            const auto format = bc.channel_bytes == 4 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            auto& output = pooled_texture(source.width, source.height, static_cast<UINT16>(source.mips), format);
            const int blocks_w = (source.width + 3) / 4, blocks_h = (source.height + 3) / 4;
            auto& blocks = pooled_texture(blocks_w, blocks_h, 1,
                bc.block_bytes == 8 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT);

            std::vector<bool> needed(source.mips, false);
            for (const auto& region : source.regions) needed[region.mip] = true;
            for (int mip = 0; mip < source.mips; ++mip) {
                if (needed[mip]) dispatch_decode(source, metadata, output.Get(), format, mip, bc.channel_bytes);
            }
            uav_barrier(output.Get());

            for (const auto& region : source.regions) {
                transition(output.Get(), D3D12CalcSubresource(region.mip, 0, 0, source.mips, 1),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                dispatch_encode(region, source, recipe, bc, output.Get(), format, blocks.Get());
                transition(output.Get(), D3D12CalcSubresource(region.mip, 0, 0, source.mips, 1),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                const UINT region_blocks_w = (region.width + 3) / 4, region_blocks_h = (region.height + 3) / 4;
                const UINT row_bytes = region_blocks_w * bc.block_bytes;
                const UINT row_pitch = (row_bytes + 255) & ~255u;
                const UINT64 offset = (readback_used + 511) & ~511ull;
                if (offset + UINT64(row_pitch) * region_blocks_h > readback_size) {
                    throw std::runtime_error("readback pequeno demais");
                }
                readback_used = offset + UINT64(row_pitch) * region_blocks_h;
                if (region.offset + UINT64(region.pitch) * (region_blocks_h - 1) + row_bytes > recipe.length ||
                    row_bytes > region.pitch) throw std::runtime_error("região fora do TEX");

                uav_barrier(blocks.Get());
                transition(blocks.Get(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = offset;
                dst.PlacedFootprint.Footprint.Format = bc.block_bytes == 8 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
                dst.PlacedFootprint.Footprint.Width = region_blocks_w;
                dst.PlacedFootprint.Footprint.Height = region_blocks_h;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = blocks.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                const D3D12_BOX box{0, 0, 0, region_blocks_w, region_blocks_h, 1};
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                transition(blocks.Get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                copies.push_back({&region, offset, row_pitch, region_blocks_h, row_bytes});
            }
            keep_alive.push_back(latent);
            timing.record += ms_since(record_start);
        }
        check(list->Close(), "fechar lista");

        const auto wait_start = Clock::now();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fence_value;
        check(queue->Signal(fence.Get(), value), "signal");
        if (fence->GetCompletedValue() < value) {
            check(fence->SetEventOnCompletion(value, event), "evento");
            if (WaitForSingleObject(event, 120000) != WAIT_OBJECT_0) throw std::runtime_error("GPU sem resposta");
        }
        timing.wait = ms_since(wait_start);
        check(device->GetDeviceRemovedReason(), "dispositivo removido");

        const auto copy_start = Clock::now();
        unsigned char* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(readback_used)};
        check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map readback");
        for (const auto& copy : copies) {
            for (UINT y = 0; y < copy.rows; ++y) {
                std::memcpy(bytes.data() + copy.region->offset + std::size_t(y) * copy.region->pitch,
                            mapped + copy.offset + UINT64(y) * copy.row_pitch, copy.row_bytes);
            }
        }
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        timing.readback = ms_since(copy_start);
        timing.total = ms_since(started);
        weight_name = weight_label;
        return true;
    }

private:
    void ntc_check(ntc::Status status, const char* what) {
        if (status != ntc::Status::Ok) {
            throw std::runtime_error(std::string(what) + ": " + (last_error ? last_error() : "?"));
        }
    }

    void make_root_signatures() {
        D3D12_DESCRIPTOR_RANGE1 latent_range{};
        latent_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        latent_range.NumDescriptors = 1;
        latent_range.BaseShaderRegister = 1;
        latent_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        D3D12_DESCRIPTOR_RANGE1 output_range{};
        output_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        output_range.NumDescriptors = UINT_MAX;
        output_range.RegisterSpace = 1;
        output_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        D3D12_ROOT_PARAMETER1 params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &latent_range};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 2;
        params[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable = {1, &output_range};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 3;
        decode_root = make_root(params, 4, &sampler, 1);

        D3D12_DESCRIPTOR_RANGE1 input_range{};
        input_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        input_range.NumDescriptors = 1;
        input_range.BaseShaderRegister = 1;
        input_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        D3D12_DESCRIPTOR_RANGE1 blocks_range{};
        blocks_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        blocks_range.NumDescriptors = 1;
        blocks_range.BaseShaderRegister = 3;
        blocks_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        D3D12_ROOT_PARAMETER1 bc_params[3]{};
        bc_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        bc_params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        bc_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bc_params[1].DescriptorTable = {1, &input_range};
        bc_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bc_params[2].DescriptorTable = {1, &blocks_range};
        encode_root = make_root(bc_params, 3, nullptr, 0);
    }

    ComPtr<ID3D12RootSignature> make_root(D3D12_ROOT_PARAMETER1* params, UINT count,
                                          const D3D12_STATIC_SAMPLER_DESC* samplers, UINT sampler_count) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = count;
        desc.Desc_1_1.pParameters = params;
        desc.Desc_1_1.NumStaticSamplers = sampler_count;
        desc.Desc_1_1.pStaticSamplers = samplers;
        ComPtr<ID3DBlob> blob, error;
        const HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
        if (FAILED(hr)) throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "root signature");
        ComPtr<ID3D12RootSignature> root;
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
        return root;
    }

    ID3D12PipelineState* pipeline(const ntc::ComputePassDesc& pass, ID3D12RootSignature* root) {
        auto& entry = pipelines[pass.computeShader];
        if (!entry) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root;
            desc.CS = {pass.computeShader, pass.computeShaderSize};
            check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&entry)), "PSO");
        }
        return entry.Get();
    }

    ComPtr<ID3D12Resource>& pooled_texture(int width, int height, UINT16 mips, DXGI_FORMAT format) {
        auto& entry = pool[std::make_tuple(width, height, mips, format)];
        if (!entry) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = mips;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&entry)), "textura");
        }
        return entry;
    }

    void load_package(const fs::path& path, GpuTiming& timing) {
        if (path == current_package && metadata_object) return;
        const auto file_start = Clock::now();
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto size = file.tellg();
        if (size <= 0) throw std::runtime_error("NTC ausente: " + path.string());
        package.resize(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(package.data()), size);
        timing.file += ms_since(file_start);

        const auto upload_start = Clock::now();
        if (metadata_object) {
            context->DestroyTextureSetMetadata(metadata_object);
            metadata_object = nullptr;
        }
        ntc::IStream* stream = nullptr;
        ntc_check(context->OpenReadOnlyMemory(package.data(), package.size(), &stream), "OpenReadOnlyMemory");
        const auto status = context->CreateTextureSetMetadataFromStream(stream, &metadata_object);
        context->CloseMemory(stream);
        ntc_check(status, "metadados");
        current_package = path;

        const auto weight = metadata_object->GetBestSupportedWeightType();
        if (weight == ntc::InferenceWeightType::Unknown) throw std::runtime_error("sem pesos compatíveis");
        weight_type = weight;
        weight_label = weight == ntc::InferenceWeightType::GenericInt8 ? "int8" :
                       weight == ntc::InferenceWeightType::GenericFP8 ? "fp8" : "coopvec";
        const void* weight_data = nullptr;
        std::size_t weight_size = 0, converted = 0;
        ntc_check(metadata_object->GetInferenceWeights(weight, &weight_data, &weight_size, &converted), "pesos");
        if (converted) throw std::runtime_error("pesos exigem conversão");

        const auto latent_desc = metadata_object->GetLatentTextureDesc();
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = latent_desc.width;
        desc.Height = latent_desc.height;
        desc.DepthOrArraySize = static_cast<UINT16>(latent_desc.arraySize);
        desc.MipLevels = static_cast<UINT16>(latent_desc.mipLevels);
        desc.Format = DXGI_FORMAT_B4G4R4A4_UNORM;
        desc.SampleDesc.Count = 1;
        latent.Reset();
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&latent)), "latentes");
        latent_mips = latent_desc.mipLevels;
        latent_layers = latent_desc.arraySize;

        std::vector<unsigned char> inflated;
        for (int mip = 0; mip < latent_desc.mipLevels; ++mip) {
            for (int layer = 0; layer < latent_desc.arraySize; ++layer) {
                ntc::LatentTextureFootprint footprint{};
                ntc_check(metadata_object->GetLatentTextureFootprint(mip, layer, footprint), "footprint");
                const unsigned char* source = package.data() + footprint.buffer.rangeInStream.offset;
                if (footprint.buffer.compressionType != ntc::CompressionType::None) {
                    inflated.resize(footprint.buffer.uncompressedSize);
                    ntc_check(context->DecompressBuffer(footprint.buffer.compressionType, source,
                        footprint.buffer.rangeInStream.size, inflated.data(), inflated.size(),
                        footprint.buffer.uncompressedCrc32), "DecompressBuffer");
                    source = inflated.data();
                }
                const UINT row_bytes = static_cast<UINT>(footprint.width) * 2;
                const UINT pitch = (row_bytes + 255) & ~255u;
                const UINT64 offset = upload.allocate(UINT64(pitch) * footprint.height, 512);
                for (int y = 0; y < footprint.height; ++y) {
                    std::memcpy(upload.mapped + offset + UINT64(y) * pitch, source + footprint.rowPitch * y, row_bytes);
                }
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = latent.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = D3D12CalcSubresource(mip, layer, 0, latent_desc.mipLevels, latent_desc.arraySize);
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = upload.buffer.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = offset;
                src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B4G4R4A4_UNORM;
                src.PlacedFootprint.Footprint.Width = footprint.width;
                src.PlacedFootprint.Footprint.Height = footprint.height;
                src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
        }
        transition(latent.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // O anel de upload é zerado a cada TEX, então os pesos são copiados para ele a cada uso do pacote.
        weight_bytes.assign(static_cast<const unsigned char*>(weight_data),
                            static_cast<const unsigned char*>(weight_data) + weight_size);
        weights_in_ring = false;
        timing.upload += ms_since(upload_start);
    }

    const ntc::ITextureMetadata* find_metadata(const TexSource& source) {
        for (int i = 0; i < metadata_object->GetTextureCount(); ++i) {
            const auto* texture = metadata_object->GetTexture(i);
            if (texture && texture->GetFirstChannel() == source.first_channel && texture->GetNumChannels() == source.channels) {
                return texture;
            }
        }
        throw std::runtime_error("canais não encontrados no pacote");
    }

    D3D12_GPU_VIRTUAL_ADDRESS push_constants(const ntc::ComputePassDesc& pass) {
        const UINT64 offset = upload.allocate(pass.constantBufferSize, 256);
        std::memcpy(upload.mapped + offset, pass.constantBufferData, pass.constantBufferSize);
        return upload.buffer->GetGPUVirtualAddress() + offset;
    }

    UINT next_descriptor() {
        if (descriptor_used >= descriptor_capacity) throw std::runtime_error("descritores esgotados");
        return descriptor_used++;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(UINT index) {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(index) * descriptor_step;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(UINT index) {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64(index) * descriptor_step;
        return handle;
    }

    void dispatch_decode(const TexSource& source, const ntc::ITextureMetadata* texture, ID3D12Resource* output,
                         DXGI_FORMAT format, int mip, int channel_bytes) {
        ntc::OutputTextureDesc out{};
        out.firstChannel = source.first_channel;
        out.numChannels = source.channels;
        out.rgbColorSpace = channel_bytes == 4 ? ntc::ColorSpace::Linear : texture->GetRgbColorSpace();
        out.alphaColorSpace = channel_bytes == 4 ? ntc::ColorSpace::Linear : texture->GetAlphaColorSpace();
        out.ditherScale = channel_bytes == 1 ? 1.f / 255.f : 0.f; // Igual à leitura com dithering da DLL.
        ntc::MakeDecompressionComputePassParameters params{};
        params.textureSetMetadata = metadata_object;
        params.weightType = weight_type;
        params.mipLevel = mip;
        params.pOutputTextures = &out;
        params.numOutputTextures = 1;
        ntc::ComputePassDesc pass{};
        ntc_check(context->MakeDecompressionComputePass(params, &pass), "passo de decodificação");

        if (!weights_in_ring) {
            weight_offset = upload.allocate(weight_bytes.size(), 256);
            std::memcpy(upload.mapped + weight_offset, weight_bytes.data(), weight_bytes.size());
            weights_in_ring = true;
        }
        const UINT latent_index = next_descriptor();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_B4G4R4A4_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = latent_mips;
        srv.Texture2DArray.ArraySize = latent_layers;
        device->CreateShaderResourceView(latent.Get(), &srv, cpu_handle(latent_index));
        const UINT output_index = next_descriptor();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = mip;
        device->CreateUnorderedAccessView(output, nullptr, &uav, cpu_handle(output_index));

        list->SetComputeRootSignature(decode_root.Get());
        list->SetPipelineState(pipeline(pass, decode_root.Get()));
        list->SetComputeRootConstantBufferView(0, push_constants(pass));
        list->SetComputeRootDescriptorTable(1, gpu_handle(latent_index));
        list->SetComputeRootShaderResourceView(2, upload.buffer->GetGPUVirtualAddress() + weight_offset);
        list->SetComputeRootDescriptorTable(3, gpu_handle(output_index));
        list->Dispatch(pass.dispatchWidth, pass.dispatchHeight, 1);
    }

    void dispatch_encode(const TexRegion& region, const TexSource& source, const TexRecipe&, const BcInfo& bc,
                         ID3D12Resource* output, DXGI_FORMAT format, ID3D12Resource* blocks) {
        ntc::MakeBlockCompressionComputePassParameters params;
        params.srcRect.left = region.x;
        params.srcRect.top = region.y;
        params.srcRect.width = region.width;
        params.srcRect.height = region.height;
        params.dstFormat = bc.format;
        params.alphaThreshold = source.channels == 4 ? 0.5f : 0.f; // Mesma referência de alfa do DirectXTex.
        ntc::ComputePassDesc pass{};
        ntc_check(context->MakeBlockCompressionComputePass(params, &pass), "passo BCn");
        const UINT input_index = next_descriptor();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = region.mip;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(output, &srv, cpu_handle(input_index));
        const UINT blocks_index = next_descriptor();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = bc.block_bytes == 8 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32B32A32_UINT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(blocks, nullptr, &uav, cpu_handle(blocks_index));
        list->SetComputeRootSignature(encode_root.Get());
        list->SetPipelineState(pipeline(pass, encode_root.Get()));
        list->SetComputeRootConstantBufferView(0, push_constants(pass));
        list->SetComputeRootDescriptorTable(1, gpu_handle(input_index));
        list->SetComputeRootDescriptorTable(2, gpu_handle(blocks_index));
        list->Dispatch(pass.dispatchWidth, pass.dispatchHeight, 1);
    }

    void uav_barrier(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        list->ResourceBarrier(1, &barrier);
    }
    void transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }

public:
    // Depois de uma exceção, a lista pode ter ficado aberta e sem o envio dos latentes do pacote atual.
    void recover() override {
        list->Close();
        current_package.clear();
    }

private:
    static constexpr UINT descriptor_capacity = 4096;
    static constexpr UINT64 readback_size = 64ull * 1024 * 1024;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    UploadRing upload;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT descriptor_step = 0, descriptor_used = 0;
    ComPtr<ID3D12RootSignature> decode_root, encode_root;
    std::map<const void*, ComPtr<ID3D12PipelineState>> pipelines;
    std::map<std::tuple<int, int, UINT16, DXGI_FORMAT>, ComPtr<ID3D12Resource>> pool;
    HMODULE library = nullptr;
    decltype(&ntc::GetLastErrorMessage) last_error = nullptr;
    ntc::IContext* context = nullptr;
    ntc::ITextureSetMetadata* metadata_object = nullptr;
    fs::path current_package;
    std::vector<unsigned char> package, weight_bytes;
    ComPtr<ID3D12Resource> latent;
    UINT latent_mips = 0, latent_layers = 0;
    ntc::InferenceWeightType weight_type = ntc::InferenceWeightType::Unknown;
    std::string weight_label;
    UINT64 weight_offset = 0;
    bool weights_in_ring = false;
};
}

bool gpu_tex_supported(const TexRecipe& recipe) {
    BcInfo bc{};
    if (!gpu_format(recipe.format, bc)) return false;
    for (const auto& source : recipe.sources) {
        if (source.channel_bytes != bc.channel_bytes) return false;
        // O LibNTC 0.10.1 recusa first + num >= 16 nos compute passes, e o canal 15 fica inacessível.
        if (source.first_channel + source.channels >= 16) return false;
    }
    return true;
}

std::unique_ptr<GpuTexBuilder> make_gpu_tex_builder(ID3D12Device* device, const fs::path& data) {
    return std::make_unique<Pipeline>(device, data);
}
