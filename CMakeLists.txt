# Harmonica Player —— 游戏口琴自动演奏工具
# Phase 1：输入系统（InputController / KeyboardController / MouseController / Input Test）
#
# 三种打开方式，任选其一：
#   A. Visual Studio 直接“打开文件夹”（VS 自带 CMake 支持，不用自己配任何东西）
#   B. 命令行生成 VS 工程：cmake -S . -B build -G "Visual Studio 18 2026" -A x64
#   C. 不用 CMake，双击 build.bat（最省事，只用 MSVC 的 cl.exe）

cmake_minimum_required(VERSION 3.20)
project(HarmonicaPlayer VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 生成的 exe 统一放到 dist/，方便双击测试
set(HP_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/dist")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${HP_OUTPUT_DIR}")

# -----------------------------------------------------------------------------
# 核心静态库：输入层 + 通用基础设施
# 后续 Phase 的 Song / Player / Hotkey / InputMonitor / GUI 都往这里加
# -----------------------------------------------------------------------------
add_library(harmonica_core STATIC
    src/core/Types.cpp
    src/core/Time.cpp
    src/core/Log.cpp
    src/core/PrecisionWait.cpp
    src/input/KeyboardController.cpp
    src/input/MouseController.cpp
    src/input/InputController.cpp
    src/song/Note.cpp
    src/song/Song.cpp
    src/song/Timeline.cpp
    src/song/BeatMath.cpp
    src/song/TextScore.cpp
    src/song/TestSongs.cpp
    src/song/SongSerializer.cpp
    src/player/PlaybackClock.cpp
    src/player/SongPlayer.cpp
    src/hotkey/HotkeyBinding.cpp
    src/hotkey/GlobalHotkeyManager.cpp
    src/monitor/InputMonitor.cpp
    src/ui/UiFormat.cpp
    src/ui/UiModel.cpp
)

target_include_directories(harmonica_core PUBLIC "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/third_party")
# nlohmann/json 只在需要它的目标里单独关掉警告（第三方头文件不参与本项目的 /W4 标准）
target_compile_options(harmonica_core PRIVATE /external:I"${CMAKE_SOURCE_DIR}/third_party" /external:W0)
target_compile_definitions(harmonica_core PUBLIC UNICODE _UNICODE)
target_link_libraries(harmonica_core PUBLIC user32.lib advapi32.lib shell32.lib)

if (MSVC)
    # /utf-8  必须加：源码里有中文字符串，否则会报 C4819 或输出乱码
    # /W4     严格一点，尽早发现类型/签名问题
    # /permissive-  关闭 MSVC 的宽容模式
    target_compile_options(harmonica_core PUBLIC /utf-8 /W4 /EHsc /permissive-)
    # 静态链接 CRT → 单文件 exe，目标机器不需要装任何运行时
    set_property(TARGET harmonica_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# -----------------------------------------------------------------------------
# Phase 1 测试程序
# -----------------------------------------------------------------------------
add_executable(input_test src/tools/input_test.cpp)
target_link_libraries(input_test PRIVATE harmonica_core)

# 多配置生成器（Visual Studio）默认会往 dist/<Config>/ 里放，这里统一成 dist/
foreach(config_name DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(input_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_name} "${HP_OUTPUT_DIR}")
endforeach()

set_target_properties(input_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HP_OUTPUT_DIR}"
    VS_DEBUGGER_WORKING_DIRECTORY "$(TargetDir)")

# -----------------------------------------------------------------------------
# Phase 2 测试程序（时间轴 / 播放时钟 / 精等待）
# -----------------------------------------------------------------------------
add_executable(timeline_test src/tools/timeline_test.cpp)
target_link_libraries(timeline_test PRIVATE harmonica_core shell32)

foreach(config_name DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(timeline_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_name} "${HP_OUTPUT_DIR}")
endforeach()

set_target_properties(timeline_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HP_OUTPUT_DIR}"
    VS_DEBUGGER_WORKING_DIRECTORY "$(TargetDir)")

# -----------------------------------------------------------------------------
# Phase 3 测试程序（SongPlayer：播放/暂停/继续/停止/重播/急停）
# -----------------------------------------------------------------------------
add_executable(player_test src/tools/player_test.cpp)
target_link_libraries(player_test PRIVATE harmonica_core shell32)

foreach(config_name DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(player_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_name} "${HP_OUTPUT_DIR}")
endforeach()

set_target_properties(player_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HP_OUTPUT_DIR}"
    VS_DEBUGGER_WORKING_DIRECTORY "$(TargetDir)")

# -----------------------------------------------------------------------------
# Phase 7：图形界面程序本体（原生 Win32，无第三方依赖，单文件 exe）
# -----------------------------------------------------------------------------
add_executable(HarmonicaPlayer WIN32
    src/main.cpp
    src/ui/UiTheme.cpp
    src/ui/MainWindow.cpp
    src/ui/App.cpp
    src/ui/NewSongWindow.cpp
    src/ui/SettingsStore.cpp
    src/ui/SettingsWindow.cpp
    src/ui/harmonica.rc)
target_link_libraries(HarmonicaPlayer PRIVATE harmonica_core comctl32 comdlg32 ole32 shell32 gdi32 user32)
# 清单由 .rc 嵌入（Common Controls v6 + PerMonitorV2），所以关掉 CMake 自己生成的清单
set_target_properties(HarmonicaPlayer PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# -----------------------------------------------------------------------------
# Phase 7 测试程序（界面逻辑，无窗口）
# -----------------------------------------------------------------------------
add_executable(ui_test src/tools/ui_test.cpp)
target_link_libraries(ui_test PRIVATE harmonica_core)

# -----------------------------------------------------------------------------
# Phase 6 测试程序（用户输入立即停止：WH_KEYBOARD_LL）
# -----------------------------------------------------------------------------
add_executable(hook_test src/tools/hook_test.cpp)
target_link_libraries(hook_test PRIVATE harmonica_core)

# -----------------------------------------------------------------------------
# Phase 5 测试程序（全局热键：F6/F7/F8 + 自定义）
# -----------------------------------------------------------------------------
add_executable(hotkey_test src/tools/hotkey_test.cpp)
target_link_libraries(hotkey_test PRIVATE harmonica_core)

# -----------------------------------------------------------------------------
# Phase 4 测试程序（JSON 曲谱：Save / Load / 序列化 / 反序列化）
# -----------------------------------------------------------------------------
add_executable(song_json_test src/tools/song_json_test.cpp)
target_link_libraries(song_json_test PRIVATE harmonica_core shell32)

foreach(config_name DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(song_json_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_name} "${HP_OUTPUT_DIR}")
endforeach()

set_target_properties(song_json_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HP_OUTPUT_DIR}"
    VS_DEBUGGER_WORKING_DIRECTORY "$(TargetDir)")

# -----------------------------------------------------------------------------
# 后续 Phase 的占位说明（暂不创建目标，避免现在编译报错）
#   Phase 5    Hotkey（GlobalHotkeyManager：F6/F7/F8 + 自定义）
#   Phase 6    InputMonitor（WH_KEYBOARD_LL）+ 急停钩子（调用 SongPlayer::EmergencyStop）
#   Phase 7    UI（MainWindow / SongEditor / StatusPanel / Settings）
#   Phase 8    BPM / 拍号 / 时值 / 速度的完整语义
#   Phase 9    文本曲谱
#   Phase 10   Recorder（录制真实演奏 → 绝对时间轴）
# -----------------------------------------------------------------------------
