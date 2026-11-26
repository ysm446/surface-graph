#include "app/Application.h"

#include "ui/Localization.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <string>

// --- DirectX 12 Agility SDK ------------------------------------------------
// 実行ファイルからエクスポートすることで、D3D12/ 配下の新しいランタイムが使われる。
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

// 使い方:
//   surface_graph.exe [<graph.sgraph | project.sgproj>] [--open <graph.sgraph | project.sgproj>]
//                     [--screenshot-ui <path>] [--screenshot-frame <n>]
//                     [--select-node <id>] [--enter-node <id>] [--preview-shape sphere|plane]
//                     [--language ja|en] [--gpu-validation]
sg::StartupOptions ParseCommandLine() {
    sg::StartupOptions options;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) return options;

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--open" && i + 1 < argc) {
            options.graphPath = argv[++i];
        } else if (argument == L"--screenshot-ui" && i + 1 < argc) {
            options.uiScreenshotPath = argv[++i];
        } else if (argument == L"--screenshot-frame" && i + 1 < argc) {
            options.screenshotFrame = static_cast<uint32_t>(std::clamp(::_wtoi(argv[++i]), 1, 100000));
        } else if (argument == L"--select-node" && i + 1 < argc) {
            options.selectNode = static_cast<uint64_t>(::_wtoi64(argv[++i]));
        } else if (argument == L"--view-pitch" && i + 1 < argc) {
            options.viewPitch = std::clamp(static_cast<float>(::_wtof(argv[++i])), -1.5f, 1.5f);
            options.hasViewPitch = true;
        } else if (argument == L"--view-project-node" && i + 1 < argc) {
            options.viewProjectNode = static_cast<uint64_t>(::_wtoi64(argv[++i]));
        } else if (argument == L"--mesh-material") {
            options.meshMaterial = true;
        } else if (argument == L"--no-grid") {
            options.noGrid = true;
        } else if (argument == L"--no-ground") {
            options.noGround = true;
        } else if (argument == L"--wireframe") {
            options.wireframe = true;
        } else if (argument == L"--select-path-point" && i + 1 < argc) {
            options.selectPathPoint = ::_wtoi(argv[++i]);
        } else if (argument == L"--enter-node" && i + 1 < argc) {
            options.enterNode = static_cast<uint64_t>(::_wtoi64(argv[++i]));
        } else if (argument == L"--language" && i + 1 < argc) {
            const std::wstring language = argv[++i];
            if (language == L"ja" || language == L"en") options.language = (language == L"en") ? "en" : "ja";
        } else if (argument == L"--ui-scale" && i + 1 < argc) {
            options.uiScale = std::clamp(static_cast<float>(::_wtof(argv[++i])), 0.5f, 3.0f);
        } else if (argument == L"--preview-shape" && i + 1 < argc) {
            const std::wstring shape = argv[++i];
            options.previewShape = (shape == L"plane") ? 1 : (shape == L"cube") ? 2 : 0;
        } else if (argument == L"--show-light-gizmo") {
            options.showLightGizmo = true;
        } else if (argument == L"--view-tiling") {
            options.viewTiling = true;
        } else if (argument == L"--view-grid") {
            options.viewGrid = true;
        } else if (argument == L"--uv-checker") {
            options.uvChecker = true;
        } else if (argument == L"--show-settings") {
            options.showSettings = true;
        } else if (argument == L"--show-confirm") {
            options.showConfirm = true;
        } else if (argument == L"--arrange") {
            options.arrange = true;
        } else if (argument == L"--gpu-validation") {
            options.gpuValidation = true;
        } else if (!argument.empty() && argument[0] != L'-' && options.graphPath.empty()) {
            // 関連付けやドロップで渡されたファイル。
            options.graphPath = argument;
        } else {
            SG_LOG_WARN(sg::ui::Tr("不明な引数です: %ls", "Unknown argument: %ls"), argument.c_str());
        }
    }
    ::LocalFree(argv);
    return options;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const sg::StartupOptions options = ParseCommandLine();
    sg::Application app;
    if (!app.Initialize(options)) {
        app.Shutdown();
        return 1;
    }
    const int result = app.Run();
    app.Shutdown();
    return result;
}
