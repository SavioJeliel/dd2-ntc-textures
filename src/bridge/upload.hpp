#pragma once

#include <d3d12.h>
#include <reframework/API.h>

#include <array>
#include <vector>

using RgbMips = std::array<std::vector<unsigned char>, 9>;

bool install_upload_hook(const REFrameworkPluginInitializeParam& param);
bool prepare_upload(const RgbMips& mips);
void request_tx074_upload(const REFrameworkPluginFunctions& functions);
