#include <windows.h>
#include <MinHook.h>
#include <wrl/client.h>

#include "upload.hpp"
#include "fingerprints.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <list>
#include <mutex>

DWORD WINAPI decode_tx074(void* functions);

#define STB_DXT_IMPLEMENTATION
#include "bc1.hpp"

namespace {
using Microsoft::WRL::ComPtr;
using CopyTextureRegion = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList*,
    const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
    const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using ResetList = HRESULT (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList*,
    ID3D12CommandAllocator*, ID3D12PipelineState*);
using ExecuteLists = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr GUID upload_key{0xe7db2174, 0xdcd8, 0x457d, {0x84, 0x31, 0x96, 0x27, 0x08, 0x11, 0x49, 0xdd}};
constexpr auto request_timeout = 900ms;
constexpr auto idle_timeout = 15s;
constexpr UINT64 upload_budget = 4 * 1024 * 1024;
std::atomic<unsigned> live_uploads{0};

class Upload final : public IUnknown {
public:
    Upload() { ++live_uploads; }
    STDMETHODIMP QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *result = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    ComPtr<ID3D12Resource> buffer;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 9> footprints;

private:
    ~Upload() {
        buffer.Reset();
        --live_uploads;
    }
    std::atomic<ULONG> references_{1};
};

struct Submission {
    ComPtr<IUnknown> upload;
    ComPtr<ID3D12Fence> fence;
};

CopyTextureRegion original_copy = nullptr;
ResetList original_reset = nullptr;
ExecuteLists original_execute = nullptr;
ComPtr<ID3D12Device> device;
const REFrameworkPluginFunctions* log_functions = nullptr;
std::atomic<bool> upload_hook_enabled{false};
std::mutex state_mutex;
std::condition_variable upload_changed;
ComPtr<Upload> cached_upload;
ComPtr<IUnknown> emergency_hold;
std::list<Submission> pending;
Clock::time_point last_use;
Clock::time_point deadline;
Clock::time_point retry_after;
bool preparing = false;

ComPtr<IUnknown> list_upload(ID3D12CommandList* list) {
    ComPtr<IUnknown> reference;
    UINT size = sizeof(IUnknown*);
    list->GetPrivateData(upload_key, &size, reference.GetAddressOf());
    return reference;
}

void collect_uploads() {
    std::lock_guard lock(state_mutex);
    pending.remove_if([](const Submission& submission) {
        if (!submission.fence) return false;
        const UINT64 completed = submission.fence->GetCompletedValue();
        return completed != UINT64_MAX && completed >= 1;
    });
    if (cached_upload && Clock::now() - last_use >= idle_timeout) {
        cached_upload.Reset();
        log_functions->log_info("[dd2_ntc_bridge] TX_074_ALBD idle cache entry retired");
    }
}

HRESULT STDMETHODCALLTYPE intercept_reset(ID3D12GraphicsCommandList* list,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* state) {
    const auto result = original_reset(list, allocator, state);
    if (SUCCEEDED(result)) {
        list->SetPrivateDataInterface(upload_key, nullptr);
    }
    return result;
}

void STDMETHODCALLTYPE intercept_execute(ID3D12CommandQueue* queue, UINT count,
                                        ID3D12CommandList* const* lists) {
    // This pilot permits only one live upload allocation. Lists pin it through private data.
    ComPtr<IUnknown> reference;
    for (UINT index = 0; index < count && !reference; ++index) {
        if (lists[index]) reference = list_upload(lists[index]);
    }
    if (!reference) {
        original_execute(queue, count, lists);
        return;
    }

    std::list<Submission> submission;
    try {
        submission.emplace_back();
        submission.front().upload = reference;
    } catch (const std::bad_alloc&) {
        std::lock_guard lock(state_mutex);
        emergency_hold = reference;
        upload_hook_enabled.store(false, std::memory_order_release);
    }
    if (!submission.empty()) {
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission.front().fence));
    }
    original_execute(queue, count, lists);
    if (!submission.empty()) {
        auto& fence = submission.front().fence;
        if (!fence || FAILED(queue->Signal(fence.Get(), 1))) {
            // Keep the buffer pinned if GPU completion cannot be established.
            fence.Reset();
            upload_hook_enabled.store(false, std::memory_order_release);
            log_functions->log_error("[dd2_ntc_bridge] Upload fence failed; retaining buffer and disabling substitution");
        }
        std::lock_guard lock(state_mutex);
        pending.splice(pending.end(), submission);
    }
}

struct UploadSequence {
    ID3D12GraphicsCommandList* command_list;
    ID3D12Resource* destination;
    ID3D12Resource* source;
    UINT next_mip;
};
thread_local UploadSequence sequence{};

bool matches(const D3D12_TEXTURE_COPY_LOCATION& source, unsigned long long expected) {
    const auto& placed = source.PlacedFootprint;
    const auto& layout = placed.Footprint;
    const UINT rows = layout.Height / 4;
    const UINT row_bytes = layout.Width * 2;
    const UINT sample_size = (std::min)(32u, row_bytes);
    const UINT column = (row_bytes - sample_size) / 2;
    const auto desc = source.pResource->GetDesc();
    const UINT64 span = UINT64(rows - 1) * layout.RowPitch + column + sample_size;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        layout.RowPitch < row_bytes || placed.Offset > desc.Width ||
        span > desc.Width - placed.Offset) {
        return false;
    }
    const D3D12_RANGE read{placed.Offset + column, placed.Offset + span};
    void* mapped = nullptr;
    if (FAILED(source.pResource->Map(0, &read, &mapped))) {
        return false;
    }
    unsigned long long hash = 14695981039346656037ull;
    if (mapped) {
        const auto* bytes = static_cast<const unsigned char*>(mapped) + read.Begin;
        for (const UINT row : {0u, rows / 4, rows / 2, rows - 1}) {
            for (UINT byte = 0; byte < sample_size; ++byte) {
                hash = (hash ^ bytes[UINT64(row) * layout.RowPitch + byte]) * 1099511628211ull;
            }
        }
    }
    const D3D12_RANGE no_writes{};
    source.pResource->Unmap(0, &no_writes);
    return mapped && hash == expected;
}

const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* replacement_footprint(
    ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION& dst,
    const D3D12_TEXTURE_COPY_LOCATION& src, const Upload& upload) {
    const UINT mip = dst.SubresourceIndex;
    if (mip == 0) {
        sequence = {};
    } else if (sequence.destination != dst.pResource || sequence.source != src.pResource ||
               sequence.command_list != list || sequence.next_mip != mip) {
        return nullptr;
    }

    const auto desc = dst.pResource->GetDesc();
    const bool base = desc.Width == 512;
    const UINT first = base ? 2 : 0;
    const auto& source = src.PlacedFootprint.Footprint;
    if ((!base && desc.Width != 2048) || desc.Height != desc.Width ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.DepthOrArraySize != 1 || desc.Format != DXGI_FORMAT_BC1_UNORM_SRGB ||
        mip >= desc.MipLevels || mip >= upload.footprints.size() - first) {
        return nullptr;
    }
    const auto& replacement = upload.footprints[first + mip];
    if (source.Width != replacement.Footprint.Width ||
        source.Height != replacement.Footprint.Height || source.Depth != 1) {
        return nullptr;
    }
    if (mip == 0) {
        ComPtr<ID3D12Device> owner;
        if (FAILED(dst.pResource->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device.Get()) {
            return nullptr;
        }
    }
    const auto expected = base ? base_fingerprints[mip] : streaming_fingerprints[mip];
    if (!matches(src, expected)) {
        return nullptr;
    }
    sequence = mip + 1 < desc.MipLevels
        ? UploadSequence{list, dst.pResource, src.pResource, mip + 1} : UploadSequence{};
    return &replacement;
}

void STDMETHODCALLTYPE intercept_copy(ID3D12GraphicsCommandList* list,
    const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (upload_hook_enabled.load(std::memory_order_acquire) && dst && src &&
        dst->pResource && src->pResource && x == 0 && y == 0 && z == 0 && !box &&
        dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
        src->PlacedFootprint.Footprint.Format == DXGI_FORMAT_BC1_UNORM_SRGB) {
        ComPtr<Upload> upload;
        {
            std::lock_guard lock(state_mutex);
            upload = cached_upload;
        }
        const auto* layout = upload ? replacement_footprint(list, *dst, *src, *upload.Get()) : nullptr;
        if (layout && SUCCEEDED(list->SetPrivateDataInterface(upload_key, upload.Get()))) {
            D3D12_TEXTURE_COPY_LOCATION replacement = *src;
            replacement.pResource = upload->buffer.Get();
            replacement.PlacedFootprint = *layout;
            original_copy(list, dst, x, y, z, &replacement, nullptr);
            {
                std::lock_guard lock(state_mutex);
                last_use = Clock::now();
            }
            log_functions->log_info("[dd2_ntc_bridge] TX_074_ALBD NTC upload %p mip %u (%ux%u)",
                dst->pResource, dst->SubresourceIndex, layout->Footprint.Width, layout->Footprint.Height);
            return;
        }
    }
    original_copy(list, dst, x, y, z, src, box);
}
}

bool install_upload_hook(const REFrameworkPluginInitializeParam& param) {
    const auto* renderer = param.renderer_data;
    if (!renderer || renderer->renderer_type != REFRAMEWORK_RENDERER_D3D12 ||
        !renderer->device || !renderer->command_queue) {
        return false;
    }
    device = static_cast<ID3D12Device*>(renderer->device);
    log_functions = param.functions;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                        nullptr, IID_PPV_ARGS(&list)))) {
        return false;
    }
    if (FAILED(list->Close()) || !param.functions->on_present(collect_uploads)) {
        return false;
    }
    auto** list_methods = *reinterpret_cast<void***>(list.Get());
    auto** queue_methods = *reinterpret_cast<void***>(renderer->command_queue);
    void* execute_entry = queue_methods[10];
    // Require all queue types that can submit texture copies to share the hooked method.
    for (const auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                            D3D12_COMMAND_LIST_TYPE_COPY}) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = type;
        ComPtr<ID3D12CommandQueue> probe;
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probe))) ||
            (*reinterpret_cast<void***>(probe.Get()))[10] != execute_entry) {
            return false;
        }
    }

    const auto install = [](void* entry, auto detour, auto& original) {
        void* trampoline = nullptr;
        auto status = MH_CreateHook(entry, reinterpret_cast<void*>(detour), &trampoline);
        if (status == MH_OK) {
            original = reinterpret_cast<std::remove_reference_t<decltype(original)>>(trampoline);
            status = MH_EnableHook(entry);
            if (status != MH_OK) MH_RemoveHook(entry);
        }
        if (status != MH_OK) {
            log_functions->log_error("[dd2_ntc_bridge] D3D12 hook: %s", MH_StatusToString(status));
        }
        return status == MH_OK;
    };
    if (!install(execute_entry, &intercept_execute, original_execute)) {
        return false;
    }
    if (!install(list_methods[10], &intercept_reset, original_reset)) {
        MH_RemoveHook(execute_entry);
        return false;
    }
    if (!install(list_methods[16], &intercept_copy, original_copy)) {
        MH_RemoveHook(list_methods[10]);
        MH_RemoveHook(execute_entry);
        return false;
    }
    upload_hook_enabled.store(true, std::memory_order_release);
    log_functions->log_info("[dd2_ntc_bridge] D3D12 upload and fence hooks active");
    return true;
}

void request_tx074_upload(const REFrameworkPluginFunctions& functions) {
    if (!upload_hook_enabled.load(std::memory_order_acquire)) return;
    std::unique_lock lock(state_mutex);
    if (cached_upload) {
        last_use = Clock::now();
        return;
    }
    const auto now = Clock::now();
    if (!preparing && now >= retry_after && live_uploads.load() == 0) {
        preparing = true;
        deadline = now + request_timeout;
        const auto worker = +[](void* param) -> DWORD {
            const auto result = decode_tx074(param);
            {
                std::lock_guard lock(state_mutex);
                preparing = false;
                if (result != 0) retry_after = Clock::now() + 5s;
            }
            upload_changed.notify_all();
            return result;
        };
        if (HANDLE thread = CreateThread(nullptr, 0, worker,
                const_cast<REFrameworkPluginFunctions*>(&functions), 0, nullptr)) {
            CloseHandle(thread);
            functions.log_info("[dd2_ntc_bridge] TX_074_ALBD on-demand decode requested");
        } else {
            preparing = false;
            retry_after = now + 5s;
            functions.log_error("[dd2_ntc_bridge] Cannot start decode worker: %lu", GetLastError());
        }
    }
    if (preparing && !upload_changed.wait_until(lock, deadline, [] { return !preparing; })) {
        functions.log_info("[dd2_ntc_bridge] TX_074_ALBD wait timed out; original loader resumed");
    }
}

bool prepare_upload(const RgbMips& mips) {
    ComPtr<Upload> upload;
    upload.Attach(new Upload);
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 2048;
    texture.Height = 2048;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = static_cast<UINT16>(mips.size());
    texture.Format = DXGI_FORMAT_BC1_UNORM_SRGB;
    texture.SampleDesc.Count = 1;
    UINT64 total_size = 0;
    device->GetCopyableFootprints(&texture, 0, texture.MipLevels, 0, upload->footprints.data(),
                                  nullptr, nullptr, &total_size);

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total_size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD,
        D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    if (!total_size || total_size > upload_budget ||
        device->GetResourceAllocationInfo(0, 1, &buffer).SizeInBytes > upload_budget ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload->buffer)))) {
        return false;
    }
    const D3D12_RANGE no_reads{};
    void* mapped = nullptr;
    if (FAILED(upload->buffer->Map(0, &no_reads, &mapped))) {
        return false;
    }
    for (std::size_t mip = 0; mip < mips.size(); ++mip) {
        const auto& layout = upload->footprints[mip];
        const UINT width = layout.Footprint.Width;
        const UINT height = layout.Footprint.Height;
        encode_bc1(mips[mip].data(), width, height,
                   static_cast<unsigned char*>(mapped) + layout.Offset, layout.Footprint.RowPitch);
    }
    const D3D12_RANGE written{0, total_size};
    upload->buffer->Unmap(0, &written);
    {
        std::lock_guard lock(state_mutex);
        cached_upload = std::move(upload);
        last_use = Clock::now();
    }
    return true;
}
