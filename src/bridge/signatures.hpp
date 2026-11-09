#pragma once

// Pontos do DD2.exe que a DLL usa, achados por assinatura de bytes em vez de endereço fixo.
// "??" é curinga: deslocamentos relativos e operandos relativos ao RIP, que mudam entre versões.
// As mesmas assinaturas ficam no instalador (install.ps1), que confere o executável antes de mexer nos PAKs.
// Conferidas nas versões 4.1.0.0 e 4.2.0.0 (executável da Steam e modificado).

namespace dd2sig {

// ResourceManager::create_resource. Os 6 primeiros bytes são curinga porque o REFramework pode ter posto um jmp ali.
inline constexpr char create_resource[] =
    "?? ?? ?? ?? ?? ?? 41 54 56 57 55 53 48 83 EC 48 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 40 4D 85 "
    "C0 0F 84 ?? ?? ?? ?? 4C 89 C3 41 0F B7 28 66 85 ED 0F 84 ?? ?? ?? ?? 49 89 D6 48 89 CE";
inline constexpr int create_resource_offset = 0;

// Retorno da chamada a create_resource que é o aviso seletivo de streaming.
inline constexpr char streaming_notice[] =
    "0F 8D ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 46 08 83 7E 24 0C 72 03 4D 8B 00 48 8B 15 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 48 89 44 24 38 48 85 C0 0F 84 ?? ?? ?? ?? 48 89 C3 80 78 39 00 0F 84";
inline constexpr int streaming_notice_offset = 38;

// Leitor de TEX, que entrega o trecho já descompactado.
inline constexpr char tex_reader[] =
    "48 89 D0 48 C1 E8 10 48 83 E0 F8 48 8B 0D ?? ?? ?? ?? 48 C1 E9 10 48 83 E1 F8 48 39 C8 74 08 48 8B "
    "08 E9 ?? ?? ?? ?? 31 C0 C3";
inline constexpr int tex_reader_offset = 0;

// Retorno da chamada ao leitor de TEX feita pelo carregador de texturas.
inline constexpr char tex_reader_caller[] =
    "E8 ?? ?? ?? ?? 49 89 C6 48 83 3D ?? ?? ?? ?? 00 74 0E FF 15 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 74 04 4C "
    "01 76 10 48 8B 4C 24 30 48 31 E1";
inline constexpr int tex_reader_caller_offset = 5;

// Endereço único da assinatura no .text do executável principal, mais o deslocamento; nullptr se 0 ou 2+ ocorrências.
unsigned char* find(const char* pattern, int offset, unsigned* matches = nullptr);

}  // namespace dd2sig
