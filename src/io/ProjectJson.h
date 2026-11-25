#pragma once

#include "project/Project.h"

#include <filesystem>
#include <string>
#include <vector>

// プロジェクトの JSON 入出力。形式は docs/reference/project-format.md。
namespace sg::io {

inline constexpr int kProjectFormatVersion = 1;
inline constexpr const wchar_t* kProjectFileExtension = L"sgproj";

std::string ProjectToJson(const project::Project& project);
// 失敗したら false を返し、error に理由を入れる。読み飛ばしたもの（未知のノード、張れないリンク）は warnings に積む。
// 外部参照のマテリアルは読み込まない（path だけが入る）。LoadExternalMaterials を続けて呼ぶ。
bool ProjectFromJson(const std::string& text, project::Project& outProject, std::string& error,
                     std::vector<std::string>* warnings = nullptr);

// 外部参照のマテリアル（.sgraph）を、directory からの相対パスで読み込む。
// 読めなかったものは Material::loadError に理由が入る（パスは保ったまま）。
void LoadExternalMaterials(project::Project& project, const std::filesystem::path& directory,
                           std::vector<std::string>* warnings = nullptr);

// 外部参照のマテリアルの中身は書かない（.sgraph は個別に保存する）。
bool SaveProject(const std::filesystem::path& path, const project::Project& project);
// 外部参照のマテリアルも、プロジェクトファイルのあるフォルダから読み込む。
bool LoadProject(const std::filesystem::path& path, project::Project& outProject, std::string& error,
                 std::vector<std::string>* warnings = nullptr);

}  // namespace sg::io
