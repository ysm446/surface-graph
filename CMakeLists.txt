cmake_minimum_required(VERSION 3.25)

# --- vcpkg ツールチェーンの解決 -------------------------------------------
# VCPKG_ROOT が設定されていればそれを使い、なければ C:/vcpkg にフォールバックする。
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(sg_vcpkgRoot "$ENV{VCPKG_ROOT}")
    else()
        set(sg_vcpkgRoot "C:/vcpkg")
    endif()
    set(sg_vcpkgToolchain "${sg_vcpkgRoot}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${sg_vcpkgToolchain}")
        set(CMAKE_TOOLCHAIN_FILE "${sg_vcpkgToolchain}" CACHE STRING "vcpkg toolchain file")
        message(STATUS "vcpkg toolchain: ${sg_vcpkgToolchain}")
    else()
        message(FATAL_ERROR "vcpkg のツールチェーンが見つかりません: ${sg_vcpkgToolchain}")
    endif()
endif()

project(surface_graph VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "本プロジェクトは Windows 専用です。")
endif()

enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# マルチコンフィグジェネレータでも出力先を揃える。
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" cfgUpper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfgUpper} "${CMAKE_BINARY_DIR}/bin/${cfg}")
endforeach()

# --- Windows SDK の d3d12.lib を先に解決する -------------------------------
# Visual Studio ジェネレータでは Windows SDK の lib パスが find_library の探索対象に
# 入らないため、directx12-agility の config が失敗する。先にキャッシュへ入れておく。
if(NOT D3D12_LIB)
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(sg_sdkVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    else()
        set(sg_sdkVersion "${CMAKE_SYSTEM_VERSION}")
    endif()

    cmake_host_system_information(RESULT sg_kitsRoot
        QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE "KitsRoot10"
        VIEW 32
        ERROR_VARIABLE sg_kitsRootError)
    if(NOT sg_kitsRoot)
        set(sg_kitsRoot "C:/Program Files (x86)/Windows Kits/10")
    endif()
    file(TO_CMAKE_PATH "${sg_kitsRoot}" sg_kitsRoot)

    find_library(D3D12_LIB
        NAMES d3d12
        HINTS
            "${sg_kitsRoot}/Lib/${sg_sdkVersion}/um/x64"
            "$ENV{WindowsSdkDir}Lib/${sg_sdkVersion}/um/x64"
        PATHS ENV LIB
    )
    if(NOT D3D12_LIB)
        message(FATAL_ERROR "Windows SDK の d3d12.lib が見つかりません (SDK ${sg_sdkVersion})")
    endif()
    message(STATUS "d3d12.lib: ${D3D12_LIB}")
endif()

find_package(directx-headers CONFIG REQUIRED)
find_package(directx12-agility CONFIG REQUIRED)
find_package(winpixevent CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(D3D12MemoryAllocator CONFIG REQUIRED)
find_package(directx-dxc CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

# --- 全ターゲット共通のコンパイル設定 ---------------------------------------
add_library(sg_common_options INTERFACE)
target_compile_options(sg_common_options INTERFACE /W4 /permissive- /utf-8 /Zc:preprocessor /MP)
target_compile_definitions(sg_common_options INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # PIX マーカーを有効にする。
    USE_PIX
    SG_APP_VERSION="${PROJECT_VERSION}"
    # シェーダはホットリロードのためソースツリーを直接参照する。
    SG_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    # 確認用の画像（3D ビューの UV チェッカーなど）。シェーダと同じくソースツリーを直接参照する。
    SG_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    # 手元のデータ置き場（スクリーンショットや CLI の出力の既定の保存先）。.gitignore で外してある。
    SG_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    $<$<CONFIG:Debug>:SG_DEBUG=1>
)
target_include_directories(sg_common_options INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${Stb_INCLUDE_DIR}"
)

# --- グラフモデル（GPU も UI も使わない） ------------------------------------
add_library(sg_graph STATIC
    src/graph/Graph.cpp
    src/graph/Graph.h
    src/graph/GraphLayout.cpp
    src/graph/GraphLayout.h
    src/graph/NodeDefinitions.cpp
    src/io/GraphJson.cpp
    src/io/GraphJson.h
    src/core/Log.cpp
    src/core/Log.h
)
target_link_libraries(sg_graph PUBLIC sg_common_options nlohmann_json::nlohmann_json)

# --- 評価エンジン（DX12 ラッパーを含む。UI に依存しない） ----------------------
add_library(sg_engine STATIC
    src/engine/Evaluator.cpp
    src/engine/Evaluator.h
    src/engine/Export.cpp
    src/engine/Export.h
    src/engine/MaterialPreview.cpp
    src/engine/MaterialPreview.h
    src/core/ImageIo.cpp
    src/core/ImageIo.h
    src/rhi/Common.cpp
    src/rhi/Common.h
    src/rhi/ComputeQueue.cpp
    src/rhi/ComputeQueue.h
    src/rhi/DeletionQueue.cpp
    src/rhi/DeletionQueue.h
    src/rhi/DescriptorHeap.cpp
    src/rhi/DescriptorHeap.h
    src/rhi/Device.cpp
    src/rhi/Device.h
    src/rhi/GpuResource.cpp
    src/rhi/GpuResource.h
    src/rhi/PipelineCache.cpp
    src/rhi/PipelineCache.h
    src/rhi/ShaderCompiler.cpp
    src/rhi/ShaderCompiler.h
    src/rhi/TextureReadback.cpp
    src/rhi/TextureReadback.h
    src/rhi/UploadRing.cpp
    src/rhi/UploadRing.h
)
target_link_libraries(sg_engine PUBLIC
    sg_graph
    Microsoft::DirectX-Headers
    Microsoft::DirectX-Guids
    Microsoft::DirectX12-Agility
    Microsoft::WinPixEventRuntime
    Microsoft::DirectXShaderCompiler
    GPUOpen::D3D12MemoryAllocator
    unofficial::tinyexr::tinyexr
    d3d12
    dxgi
    dxguid
)

# シェーダはビルド対象ではないが、VS のソリューションから編集できるようにしておく。
file(GLOB_RECURSE sg_shaders CONFIGURE_DEPENDS "shaders/*.hlsl" "shaders/*.hlsli")
set_source_files_properties(${sg_shaders} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/shaders" PREFIX "shaders" FILES ${sg_shaders})

# Agility SDK の DLL を実行ファイル横の D3D12/ に、DXC の署名用 DLL を実行ファイル横に置く。
function(sg_deploy_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PROPERTY:Microsoft::DirectX12-Core,IMPORTED_LOCATION_RELEASE>"
            "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PROPERTY:Microsoft::DirectX12-Layers,IMPORTED_LOCATION_DEBUG>"
            "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PROPERTY:Microsoft::DXIL,IMPORTED_LOCATION_DEBUG>"
            "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Agility SDK と dxil.dll を配置"
    )
endfunction()

# --- ヘッドレス CLI ---------------------------------------------------------
# グラフを読み込んで全ノードの出力を PNG に書き出す。仕様は docs/reference/cli.md。
add_executable(surfgraph-cli src/cli/CliMain.cpp ${sg_shaders})
target_link_libraries(surfgraph-cli PRIVATE sg_engine)
sg_deploy_runtime(surfgraph-cli)

# --- エディタ ---------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/Main.cpp")
    add_executable(surface_graph WIN32
        src/app/SurfaceGraph.rc
        src/app/Main.cpp
        src/app/Application.cpp
        src/app/Application.h
        src/app/GraphPanel.cpp
        src/app/Panels.cpp
        src/core/FileDialog.cpp
        src/core/FileDialog.h
        src/core/FrameLimiter.cpp
        src/core/FrameLimiter.h
        src/core/Shell.cpp
        src/core/Shell.h
        src/core/Window.cpp
        src/core/Window.h
        src/ui/ImGuiLayer.cpp
        src/ui/ImGuiLayer.h
        src/ui/Toast.cpp
        src/ui/Toast.h
        src/ui/UiStyle.cpp
        src/ui/UiStyle.h
    )
    target_link_libraries(surface_graph PRIVATE
        sg_engine
        imgui::imgui
        unofficial::imgui-node-editor::imgui-node-editor
        shell32
        ole32
    )
    sg_deploy_runtime(surface_graph)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT surface_graph)
    set_target_properties(surface_graph PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:surface_graph>"
    )
endif()

# --- テスト ---------------------------------------------------------------
enable_testing()

# tests/ と graphs/tests/ は .gitignore で外してあり、リポジトリには入っていない。
# 手元にあるときだけテストを組み込む。

# GPU を使わない単体テスト（グラフモデル、JSON、パラメータの詰め方）。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/TestMain.cpp")
    add_executable(surface_graph_tests tests/TestMain.cpp)
    target_link_libraries(surface_graph_tests PRIVATE sg_graph)
    add_test(NAME unit COMMAND surface_graph_tests)
endif()

# GPU を使う検査。テスト用グラフを CLI で評価し、決定論・シームレス・解像度非依存を確かめる。
file(GLOB sg_testGraphs CONFIGURE_DEPENDS "graphs/tests/*.sgraph")
foreach(graphFile ${sg_testGraphs})
    get_filename_component(graphName "${graphFile}" NAME_WE)
    add_test(NAME check-${graphName} COMMAND surfgraph-cli check "${graphFile}" --res 512)
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/graphs/tests/golden/${graphName}")
        add_test(NAME golden-${graphName}
            COMMAND surfgraph-cli golden "${graphFile}" --res 256
                    --dir "${CMAKE_CURRENT_SOURCE_DIR}/graphs/tests/golden/${graphName}")
    endif()
endforeach()
