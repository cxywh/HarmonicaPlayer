#pragma once

// 屏幕提示（OSD）：按下"上一首 / 下一首"热键时，在屏幕中央偏下显示"当前歌曲：xxx"。
//
// 需求（用户原话）：
//   1 切歌后显示"当前歌曲：xxx"
//   2 尽量不遮挡游戏视野，又要清晰可见
//   3 几秒后自己消失，不是一直在
//   4 连续按只显示最新的名字，旧的不残留（不堆叠）
//   5 以管理员身份运行、游戏全屏时也要能正确显示
//
// 实现要点：
//   * WS_EX_LAYERED + WS_EX_TOPMOST + WS_EX_TRANSPARENT + WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW
//     整窗半透明、置顶、点击穿透、不进 Alt+Tab、**不抢焦点**（游戏里不能被打断）
//   * LWA_ALPHA 是整窗统一透明度，糊不出圆角，所以圆角靠 SetWindowRgn 把四角真裁掉
//   * 只有一个窗口对象：重复调用 ShowText 就是换文字 + 重新计时，天然不堆叠
//   * 位置取"前台窗口所在显示器"的中央偏下（62% 高度），避开准星

#include <string>

#include "core/Platform.h"

namespace hp {
namespace ui {

class OsdWindow {
public:
    bool Create(HINSTANCE instance, std::string* error);
    // 显示（重复调用 = 换文字并重新计时）
    void ShowText(const std::string& utf8Text);
    void Hide();
    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void RecalcLayout();
    int Scale(int dip) const;

    HWND hwnd_ = nullptr;
    std::wstring text_;
    HFONT font_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    RECT card_{};
    UINT dpi_ = 96;
    int radius_ = 0;
};

}  // namespace ui
}  // namespace hp
