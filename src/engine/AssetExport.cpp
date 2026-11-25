#include "engine/AssetExport.h"

#include "engine/Export.h"
#include "mesh/GlbWriter.h"
#include "ui/Localization.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <system_error>

namespace sg::engine {

namespace {

// ファイル名に使えない文字を _ にする（UTF-8 のまま）。
std::string FileName(std::string text) {
    for (char& c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("\\/:*?\"<>|", c) != nullptr) c = '_';
    }
    return text.empty() ? std::string("untitled") : text;
}

bool EndsWith(const std::u8string& text, std::u8string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool ExportAsset(rhi::Device& device, const project::Project& project, const mesh::Asset& asset,
                 const MaterialEvaluatorProvider& evaluatorFor, const std::filesystem::path& directory, uint32_t resolution,
                 AssetExportResult& outResult) {
    outResult = {};
    if (asset.layers.empty()) {
        outResult.error = ui::Tr("アセットにレイヤーがありません（アセット出力に、メッシュを繋いだマテリアルを繋ぐ）",
                                 "The asset has no layers (connect materials with meshes to the asset output)");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    // レイヤーが使っているマテリアルを、1 つずつ評価して書き出す。
    std::vector<mesh::GlbMaterial> glbMaterials;
    std::vector<std::string> usedNames;
    for (const mesh::AssetLayer& layer : asset.layers) {
        if (std::any_of(glbMaterials.begin(), glbMaterials.end(), [&](const mesh::GlbMaterial& m) { return m.id == layer.materialId; })) continue;
        const project::Material* material = project.FindMaterial(layer.materialId);
        Evaluator* evaluator = (material != nullptr) ? evaluatorFor(*material) : nullptr;
        if (material == nullptr || evaluator == nullptr) {
            outResult.error = ui::Tr("マテリアルを評価できません", "Could not evaluate a material");
            return false;
        }
        std::string baseName = FileName(material->name);
        if (std::find(usedNames.begin(), usedNames.end(), baseName) != usedNames.end()) baseName += "_" + std::to_string(material->id);
        usedNames.push_back(baseName);

        ExportOptions options;
        options.resolution = resolution;
        options.packOrm = true;
        options.opacityInAlbedoAlpha = true;
        options.normalOpenGl = true;
        ExportResult result;
        if (!ExportMaterial(device, *evaluator, material->graph, directory, baseName, options, result)) {
            outResult.error = material->name + ": " + result.error;
            return false;
        }
        mesh::GlbMaterial glb;
        glb.id = material->id;
        glb.name = material->name;
        for (const std::filesystem::path& path : result.files) {
            const std::u8string name = path.filename().u8string();
            if (EndsWith(name, u8"_Albedo.png")) glb.baseColor = path;
            else if (EndsWith(name, u8"_Normal.png")) glb.normal = path;
            else if (EndsWith(name, u8"_ORM.png")) glb.orm = path;
            else if (EndsWith(name, u8"_Opacity.png") && glb.baseColor.empty()) {
                // Albedo の後に Opacity が来るので、ここで Albedo がまだ無ければ繋がっていない。
                outResult.warnings.push_back(material->name + ": " +
                                             ui::Tr("Albedo が繋がっていないので、.glb に Opacity が入りません",
                                                    "Opacity is not stored in the .glb because Albedo is not connected"));
            }
            outResult.files.push_back(path);
        }
        glbMaterials.push_back(std::move(glb));
    }

    const std::string fileName = FileName(project.name) + ".glb";
    const std::filesystem::path glbPath = directory / std::filesystem::path(std::u8string(fileName.begin(), fileName.end()));
    if (!mesh::WriteGlb(glbPath, asset, glbMaterials, outResult.error)) return false;
    outResult.files.push_back(glbPath);
    return true;
}

}  // namespace sg::engine
