@echo off
chcp 65001 >nul
setlocal

rem surface-graph のエディタを起動する。
rem
rem   start.bat                 Release を起動する（無ければビルドしてから）
rem   start.bat debug           Debug を起動する
rem   start.bat build           ビルドし直してから起動する（debug と併用できる）
rem   start.bat path\to\a.sgraph   グラフを開いて起動する
rem
rem 上の 3 つ以外の引数は、そのままエディタへ渡す（例: --language en）。

cd /d "%~dp0"

set "CONFIG=Release"
set "PRESET=x64-release"
set "REBUILD=0"
set "APP_ARGS="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="debug" (
    set "CONFIG=Debug"
    set "PRESET=x64-debug"
) else if /i "%~1"=="build" (
    set "REBUILD=1"
) else (
    set APP_ARGS=%APP_ARGS% %1
)
shift
goto parse
:parsed

set "EXE=build\bin\%CONFIG%\surface_graph.exe"
if not exist "%EXE%" set "REBUILD=1"

if "%REBUILD%"=="1" (
    where cmake >nul 2>nul
    if errorlevel 1 (
        echo cmake が見つかりません。Visual Studio の「開発者コマンド プロンプト」から実行するか、cmake を PATH に入れてください。
        pause
        exit /b 1
    )
    if not exist "build\CMakeCache.txt" (
        echo 構成しています...
        cmake --preset x64
        if errorlevel 1 goto failed
    )
    echo %CONFIG% をビルドしています...
    cmake --build --preset %PRESET% --target surface_graph
    if errorlevel 1 goto failed
)

start "" "%EXE%" %APP_ARGS%
exit /b 0

:failed
echo ビルドに失敗しました。
pause
exit /b 1
