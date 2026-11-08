#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <reframework/API.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include "preread.hpp"

namespace {
constexpr DWORD resource_manager_rva = 0x527C240;
constexpr DWORD streaming_notice_rva = 0x527BC10;
constexpr wchar_t target_texture[] = L"Environment/Textures/tx_074_ALBD.tex";
constexpr unsigned char expected_prologue[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x48
};

using CreateResource = void* (*)(void*, void*, const wchar_t*);
CreateResource original_create_resource = nullptr;
const REFrameworkPluginFunctions* framework = nullptr;
const void* streaming_notice_return = nullptr;
std::atomic_flag logged_success = ATOMIC_FLAG_INIT;

void* find_create_resource() {
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) {
        return nullptr;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->FileHeader.TimeDateStamp != 0x6ACE5933 ||
        pe->OptionalHeader.SizeOfImage != 0x0F7FE000) {
        return nullptr;
    }
    auto* entry = image + resource_manager_rva;
    if (std::memcmp(entry, expected_prologue, sizeof(expected_prologue)) == 0) {
        return entry;
    }

    // The installed REFramework can redirect this entry before plugins initialize.
    constexpr std::size_t patched_size = 6;
    if (entry[0] != 0xE9 || entry[5] != 0x90 ||
        std::memcmp(entry + patched_size, expected_prologue + patched_size,
                    sizeof(expected_prologue) - patched_size) != 0) {
        return nullptr;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, entry + 1, sizeof(displacement));
    const auto destination = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(entry + 5) + displacement);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(destination, &region, sizeof(region)) == 0 ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return nullptr;
    }
    switch (region.Protect & 0xff) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return entry;
    default:
        return nullptr;
    }
}

void* observe_create_resource(void* manager, void* type_info, const wchar_t* name) {
    const bool is_target = name && _wcsicmp(name, target_texture) == 0;
    if (name) request_texture_prepare(name, _ReturnAddress() == streaming_notice_return);
    void* resource = original_create_resource(manager, type_info, name);
    if (is_target && resource && !logged_success.test_and_set(std::memory_order_relaxed)) {
        framework->log_info("[dd2_ntc_bridge] TX_074_ALBD requested; original returned %p", resource);
    }
    return resource;
}
}

extern "C" __declspec(dllexport) void reframework_plugin_required_version(
    REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(
    const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions) {
        return false;
    }
    framework = param->functions;
    streaming_notice_return = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + streaming_notice_rva;
    void* entry = find_create_resource();
    if (!entry) {
        framework->log_error("[dd2_ntc_bridge] Unsupported executable or unexpected prologue");
        return false;
    }
    auto status = MH_Initialize();
    if (status != MH_OK) {
        framework->log_error("[dd2_ntc_bridge] MH_Initialize: %s", MH_StatusToString(status));
        return false;
    }
    void* trampoline = nullptr;
    status = MH_CreateHook(entry, reinterpret_cast<void*>(&observe_create_resource), &trampoline);
    if (status == MH_OK) {
        original_create_resource = reinterpret_cast<CreateResource>(trampoline);
        status = MH_EnableHook(entry);
    }
    if (status != MH_OK) {
        MH_Uninitialize();
        framework->log_error("[dd2_ntc_bridge] Hook setup: %s", MH_StatusToString(status));
        return false;
    }
    framework->log_info("[dd2_ntc_bridge] Observation hook active at RVA 0x%X", resource_manager_rva);
    if (!install_preread_hook(*framework, param->renderer_data)) {
        framework->log_error("[dd2_ntc_bridge] Could not install TEX reader hook; using original");
        return true;
    }
    return true;
}
