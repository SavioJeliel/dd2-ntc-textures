#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <reframework/API.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>
#include "preread.hpp"
#include "signatures.hpp"

namespace {
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
    // Por assinatura, não por endereço: funciona em qualquer build em que o código desta função seja o mesmo.
    unsigned matches = 0;
    auto* entry = dd2sig::find(dd2sig::create_resource, dd2sig::create_resource_offset, &matches);
    if (!entry) {
        framework->log_error("[dd2_ntc_bridge] create_resource signature found %u times (expected 1)", matches);
        return nullptr;
    }
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

namespace {
// Logger com fila: os hooks ligam no carregamento da DLL, antes de o REFramework entregar o logger dele.
// As mensagens ficam guardadas e saem pelo logger real em reframework_plugin_initialize.
std::mutex early_mutex;
std::vector<std::string> early_lines;
const REFrameworkPluginFunctions* real_framework = nullptr;

void early_log(const char* level, const char* format, va_list args) {
    char line[1024];
    std::vsnprintf(line, sizeof(line), format, args);
    std::lock_guard lock(early_mutex);
    if (real_framework) {
        real_framework->log_info("%s%s", level, line);
    } else if (early_lines.size() < 200000) {
        early_lines.emplace_back(std::string(level) + line);
    }
}
void early_error(const char* format, ...) { va_list a; va_start(a, format); early_log("(early) ERROR ", format, a); va_end(a); }
void early_warn(const char* format, ...) { va_list a; va_start(a, format); early_log("(early) WARN ", format, a); va_end(a); }
void early_info(const char* format, ...) { va_list a; va_start(a, format); early_log("(early) ", format, a); va_end(a); }
REFrameworkPluginFunctions early_functions{};

std::atomic<int> early_state{0};  // 0 = rodando, 1 = hooks instalados, 2 = falhou
HANDLE early_thread = nullptr;

bool install_hooks(const REFrameworkRendererData* renderer);

DWORD WINAPI early_install(void*) {
    early_state.store(install_hooks(nullptr) ? 1 : 2);
    return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Liga os hooks já no carregamento (cerca de 4 s após abrir o jogo), não na inicialização dos plugins
        // (cerca de 15 s): texturas lidas nesse intervalo iam direto ao PAK, e travavam se estivessem zeradas.
        // A thread começa depois que o loader solta o lock, então pode usar MinHook e ler arquivos.
        early_functions.log_error = early_error;
        early_functions.log_warn = early_warn;
        early_functions.log_info = early_info;
        framework = &early_functions;
        early_thread = CreateThread(nullptr, 0, early_install, nullptr, 0, nullptr);
    }
    return TRUE;
}

namespace {
bool install_hooks(const REFrameworkRendererData* renderer) {
    unsigned notice_matches = 0;
    streaming_notice_return = dd2sig::find(dd2sig::streaming_notice, dd2sig::streaming_notice_offset, &notice_matches);
    if (!streaming_notice_return) {
        // Sem o aviso, o streaming ainda entra pela própria leitura: só perde antecedência.
        framework->log_warn("[dd2_ntc_bridge] streaming notice signature found %u times; continuing without it", notice_matches);
    }
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
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    framework->log_info("[dd2_ntc_bridge] Observation hook active at RVA 0x%llX (streaming notice RVA 0x%llX)",
                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(entry) - image),
                        streaming_notice_return ? static_cast<unsigned long long>(
                            reinterpret_cast<std::uintptr_t>(streaming_notice_return) - image) : 0ull);
    if (!install_preread_hook(*framework, renderer)) {
        framework->log_error("[dd2_ntc_bridge] Could not install TEX reader hook; using original");
    }
    return true;
}
}  // namespace

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(
    const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions) {
        return false;
    }
    if (early_thread) {
        WaitForSingleObject(early_thread, 60000);
        CloseHandle(early_thread);
        early_thread = nullptr;
    }
    {
        // Troca para o logger real e despeja o que foi registrado antes.
        std::lock_guard lock(early_mutex);
        real_framework = param->functions;
        for (const auto& line : early_lines) real_framework->log_info("%s", line.c_str());
        early_lines.clear();
    }
    framework = param->functions;
    set_preread_logger(*param->functions);
    set_renderer_data(param->renderer_data);
    if (early_state.load() == 1) {
        framework->log_info("[dd2_ntc_bridge] Hooks were installed early, at DLL load");
        return true;
    }
    // Reserva: se a instalação antecipada falhou, tenta de novo agora, como antes.
    framework->log_warn("[dd2_ntc_bridge] Early hook install failed; retrying at plugin initialize");
    return install_hooks(param->renderer_data);
}
