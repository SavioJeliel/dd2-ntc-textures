#pragma once

// FNV-1a samples from the original TEX payloads, mip order.
constexpr unsigned long long base_fingerprints[] = {
    0xA31DA63E6CC0E7FDull,
    0x840157F9D83D05BAull,
    0x9105B209A0B6D290ull,
    0x47C19C9E5BAA661Aull,
    0xF16D9CB1456D748Bull,
    0x61ADC0E0B88487FAull,
    0x84D355553A5EAE4Dull,
};
constexpr unsigned long long streaming_fingerprints[] = {
    0xD53D151FF57AB73Cull,
    0xF951A4714AB9A368ull,
    0xA31DA63E6CC0E7FDull,
    0x840157F9D83D05BAull,
    0x9105B209A0B6D290ull,
    0x47C19C9E5BAA661Aull,
    0xF16D9CB1456D748Bull,
    0x61ADC0E0B88487FAull,
    0x84D355553A5EAE4Dull,
};
