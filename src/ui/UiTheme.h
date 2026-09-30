#pragma once

// 界面外观：颜色、字体、DPI 缩放、平面按钮绘制（Phase 7）。
//
// 这是本项目唯一允许包含 Win32 的 UI 头（窗口与控件都靠它）。
// 配色与字号集中在这里，改主题只动这一个文件。
//
// 约定（用户明确要求）：中文、**禁 emoji**、状态用文字 + 颜色表达、界面不刷屏。

#include <string>
#include <vector>

#include "core/Platform.h"

namespace hp {
namespace ui {

// ---------------------------------------------------------------------------
// 颜色（平面风格：浅底 + 一点强调色）
// ---------------------------------------------------------------------------
struct Rgb {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};

COLORREF ToColorRef(Rgb color);

// 主题配色
extern const Rgb kColorBackground;   // 窗口底色
extern const Rgb kColorPanel;        // 分组框内的底色
extern const Rgb kColorText;         // 正文
extern const Rgb kColorMuted;        // 次要说明
extern const Rgb kColorBorder;       // 边框/分隔线
extern const Rgb kColorAccent;       // 强调（进度条、当前行）
extern const Rgb kColorGreen;        // 演奏中
extern const Rgb kColorOrange;       // 已暂停 / 警告
extern const Rgb kColorRed;          // 被用户打断 / 错误
extern const Rgb kColorBlue;         // 次要按钮
extern const Rgb kColorBorderStrong; // 分组框描边（比普通分隔线清楚一点）

// ---------------------------------------------------------------------------
// DPI
// ---------------------------------------------------------------------------
void SetUiDpi(unsigned dpi);          // 窗口建好后用 GetDpiForWindow 拿到的真实值
unsigned UiDpi();                     // 默认 96
int S(int value);                     // 按 DPI 缩放（96 → 原值）

// 窗口外框尺寸（用 AdjustWindowRectExForDpi，拿不到时退回 AdjustWindowRectEx）
void AdjustWindowRectForDpi(RECT& rect, DWORD style, bool hasMenu);

// ---------------------------------------------------------------------------
// 字体（按 DPI 重建；界面字体用微软雅黑 UI）
// ---------------------------------------------------------------------------
struct FontSet {
    HFONT title = nullptr;   // 曲名
    HFONT body = nullptr;    // 正文/按钮
    HFONT small = nullptr;   // 说明文字
    HFONT mono = nullptr;    // 数字/时间（等宽，跳字不抖）
};

FontSet& UiFonts();
void RebuildUiFonts();     // 字体尺寸变了（DPI 变化）后调用
void DestroyUiFonts();

// 窗口底色画刷（WM_CTLCOLORSTATIC / WM_ERASEBKGND 用）
HBRUSH BackgroundBrush();
HBRUSH PanelBrush();       // 分组框内部的白底（空状态提示等叠在表格上时用）
void RebuildThemeBrushes();
void DestroyThemeBrushes();

// ---------------------------------------------------------------------------
// 文本工具
// ---------------------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

// ---------------------------------------------------------------------------
// 平面按钮（BS_OWNERDRAW）与分组框绘制
// ---------------------------------------------------------------------------
enum class ButtonKind {
    Primary,   // 主操作（播放）—— 绿
    Secondary, // 次要（暂停/重播）—— 蓝
    Danger,    // 停止 —— 红
    Neutral,   // 普通（打开/编辑/设置/应用）—— 灰
};

struct ButtonPaintInfo {
    ButtonKind kind = ButtonKind::Neutral;
    std::wstring text;
    bool enabled = true;
};

// 在 WM_DRAWITEM 里调用
void PaintFlatButton(const DRAWITEMSTRUCT& item, const ButtonPaintInfo& info);

// 画分组框（1 px 圆角边框 + 标题；标题处的底色先擦干净）
void PaintGroupFrame(HDC dc, const RECT& rect, const wchar_t* title);

// 在 WM_DRAWITEM 里画进度条（SS_OWNERDRAW 的静态控件）
void PaintProgress(const DRAWITEMSTRUCT& item, int percent, const std::wstring& caption,
                   Rgb fill);

// 状态颜色（文字 + 颜色表达状态，不用符号）
Rgb StatusColor(int playbackState, int stopReason);

}  // namespace ui
}  // namespace hp
