// main.cpp —— Harmonica Player（Phase 7 起的真正的程序入口）
//
// 这是 GUI 子系统程序，入口必须是 wWinMain（/SUBSYSTEM:WINDOWS）。
// 各测试程序（input_test / timeline_test / ... / ui_test）仍然是独立的控制台 exe，
// 它们是自检工具，不属于交付物的一部分功能。

#include <string>

// 注意顺序：先 Platform.h（windows.h），再 commctrl.h —— 反过来 prsht.h 会因为
// CALLBACK / LPCSTR 还没定义而报一大片"未声明的标识符"
#include "core/Platform.h"

#include <commctrl.h>
#include "ui/App.h"
#include "ui/UiTheme.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR commandLine, int showCommand) {
    (void)previous;
    (void)commandLine;
    (void)showCommand;

    // 公共控件（ListView 等）。清单里声明了 v6，这里只是让库初始化。
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    // 先按系统 DPI 建一套字体与画刷，窗口建好后会用 GetDpiForWindow 的真实值重建
    hp::ui::SetUiDpi(GetDpiForSystem());
    hp::ui::RebuildUiFonts();
    hp::ui::RebuildThemeBrushes();

    hp::ui::App app;
    std::string error;
    if (!app.Initialize(instance, &error)) {
        MessageBoxW(nullptr, hp::ui::Utf8ToWide(error).c_str(), L"Harmonica Player",
                    MB_OK | MB_ICONERROR);
        hp::ui::DestroyUiFonts();
        hp::ui::DestroyThemeBrushes();
        return 1;
    }

    if (app.ElevationRelaunchRequested()) {
        // 已发起"以管理员身份重启"：新实例接着跑，本实例安静退出（不弹任何窗口）
        hp::ui::DestroyUiFonts();
        hp::ui::DestroyThemeBrushes();
        return 0;
    }

    const int code = app.Run();
    hp::ui::DestroyUiFonts();
    hp::ui::DestroyThemeBrushes();
    return code;
}
