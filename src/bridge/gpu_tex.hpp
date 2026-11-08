#pragma once

#include "tex_recipe.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct ID3D12Device;

struct GpuTiming {
    double file = 0, upload = 0, record = 0, wait = 0, readback = 0, total = 0;
};

// Reconstrói um TEX na GPU: decodifica o NTC e codifica BCn em compute passes do LibNTC, numa fila própria.
class GpuTexBuilder {
public:
    virtual ~GpuTexBuilder() = default;
    // Devolve false se a receita precisar da rota de CPU. Erros da GPU viram exceção.
    virtual bool build(const TexRecipe& recipe, std::vector<unsigned char>& bytes, GpuTiming& timing,
                       std::string& weights) = 0;
    // Chamar depois de uma exceção de build, antes do próximo TEX.
    virtual void recover() = 0;
};

// O LibNTC é carregado de `data`. O objeto guarda referência ao dispositivo.
std::unique_ptr<GpuTexBuilder> make_gpu_tex_builder(ID3D12Device* device, const std::filesystem::path& data);
bool gpu_tex_supported(const TexRecipe& recipe);
