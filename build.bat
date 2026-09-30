@echo off
setlocal enabledelayedexpansion

rem ===========================================================================
rem  Harmonica Player - build.bat
rem  One-shot build of Phase 1 (Input Test) with MSVC cl.exe.
rem  No CMake required. Usage:  build.bat  [Release|Debug]
rem  Output:  dist\\HarmonicaPlayer.exe  (the application)\nrem           dist\\*_test.exe           (self-check tools)
rem
rem  NOTE: keep this file pure ASCII. cmd.exe reads .bat in the OEM code page,
rem        so UTF-8 comments would be parsed as broken commands.
rem ===========================================================================

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "ROOT=%~dp0"
pushd "%ROOT%"

rem ---- 1. locate vcvars64.bat (x64 toolchain) ----
set "VCVARS="

rem 1a. ask vswhere first (works for any VS 2019/2022/2026 install)
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)

rem 1b. fallback: known install roots
if not defined VCVARS (
    for %%D in (
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
        "C:\Program Files\Microsoft Visual Studio\2022\Community"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    ) do (
        if not defined VCVARS if exist "%%~D\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS=%%~D\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

if not defined VCVARS (
    echo.
    echo [ERROR] vcvars64.bat not found.
    echo         Install "Visual Studio Build Tools" with the
    echo         "Desktop development with C++" workload, then retry.
    echo.
    popd
    exit /b 1
)

echo [1/3] Using toolchain: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] failed to initialize MSVC environment.
    popd
    exit /b 1
)

rem ---- 2. prepare output folders ----
if not exist "%ROOT%dist"     mkdir "%ROOT%dist"
if not exist "%ROOT%dist\obj" mkdir "%ROOT%dist\obj"

rem ---- 3. compile ----
if /i "%CONFIG%"=="Debug" (
    set "OPTFLAGS=/Od /Zi /MTd /D_DEBUG"
) else (
    set "OPTFLAGS=/O2 /MT /DNDEBUG"
)

rem shared sources used by every executable
set "SHARED=src\core\Types.cpp src\core\Time.cpp src\core\Log.cpp src\core\PrecisionWait.cpp src\input\KeyboardController.cpp src\input\MouseController.cpp src\input\InputController.cpp src\song\Note.cpp src\song\Song.cpp src\song\Timeline.cpp src\song\BeatMath.cpp src\song\TextScore.cpp src\song\TestSongs.cpp src\song\SongSerializer.cpp src\player\PlaybackClock.cpp src\player\SongPlayer.cpp src\hotkey\HotkeyBinding.cpp src\hotkey\GlobalHotkeyManager.cpp src\monitor\InputMonitor.cpp src\ui\UiFormat.cpp src\ui\UiModel.cpp"
set "UISRC=src\ui\UiTheme.cpp src\ui\MainWindow.cpp src\ui\App.cpp src\ui\NewSongWindow.cpp src\ui\SettingsWindow.cpp src\ui\SettingsStore.cpp src\ui\OsdWindow.cpp"

rem ---- 3a. resource script (icon + manifest: Common Controls v6 + PerMonitorV2 DPI) ----
echo [1/11] Compiling resources (%CONFIG%) ...
rc /nologo /I src\ui /fo "%ROOT%dist\obj\harmonica.res" src\ui\harmonica.rc
if errorlevel 1 (
    echo.
    echo [ERROR] build failed: harmonica.rc
    popd
    exit /b 1
)

rem ---- 3b. the GUI application itself (Phase 7) ----
echo [2/11] Compiling HarmonicaPlayer.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% %UISRC% src\main.cpp ^
   "%ROOT%dist\obj\harmonica.res" ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\HarmonicaPlayer.exe" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib gdi32.lib comctl32.lib shell32.lib comdlg32.lib advapi32.lib ole32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: HarmonicaPlayer.exe
    popd
    exit /b 1
)

rem ---- 3b-2. app only:  build.bat app  (skip every self-check tool) ----
if /I "%~1"=="app" goto :app_only

rem ---- 3c. self-check tools ----
echo [3/11] Compiling input_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\input_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\input_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: input_test.exe
    popd
    exit /b 1
)

echo [3/11] Compiling timeline_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\timeline_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\timeline_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: timeline_test.exe
    popd
    exit /b 1
)

echo [4/11] Compiling player_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\player_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\player_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: player_test.exe
    popd
    exit /b 1
)

echo [5/11] Compiling song_json_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\song_json_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\song_json_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: song_json_test.exe
    popd
    exit /b 1
)

echo [6/11] Compiling hotkey_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\hotkey_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\hotkey_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: hotkey_test.exe
    popd
    exit /b 1
)

echo [7/11] Compiling hook_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\hook_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\hook_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: hook_test.exe
    popd
    exit /b 1
)

echo [8/11] Compiling ui_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\ui_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\ui_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: ui_test.exe
    popd
    exit /b 1
)

echo [9/11] Compiling beats_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\beats_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\beats_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: beats_test.exe
    popd
    exit /b 1
)

echo [10/12] Compiling text_score_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\tools\text_score_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\text_score_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: text_score_test.exe
    popd
    exit /b 1
)

echo [11/12] Compiling final_test.exe (%CONFIG%) ...
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /MP ^
   %OPTFLAGS% /DUNICODE /D_UNICODE /Isrc /Ithird_party ^
   %SHARED% src\ui\SettingsStore.cpp src\tools\final_test.cpp ^
   /Fo:"%ROOT%dist\obj\\" /Fe:"%ROOT%dist\final_test.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib advapi32.lib shell32.lib psapi.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed: final_test.exe
    popd
    exit /b 1
)

echo [12/12] OK -^> %ROOT%dist\HarmonicaPlayer.exe
echo           -^> %ROOT%dist\timeline_test.exe
echo           -^> %ROOT%dist\player_test.exe
echo           -^> %ROOT%dist\song_json_test.exe
echo           -^> %ROOT%dist\hotkey_test.exe
echo           -^> %ROOT%dist\hook_test.exe
echo           -^> %ROOT%dist\ui_test.exe
echo           -^> %ROOT%dist\beats_test.exe
echo           -^> %ROOT%dist\text_score_test.exe
echo           -^> %ROOT%dist\final_test.exe
echo.
echo Next:  cd /d "%ROOT%" ^&^& dist\input_test.exe
popd
endlocal
exit /b 0

rem ---- exit for "build.bat app" (jumped to from above) ----
:app_only
echo.
echo [app only] done -^> %ROOT%dist\HarmonicaPlayer.exe
echo   self-check tools were skipped; run build.bat with no arguments to build them
popd
endlocal
exit /b 0
