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
// 按钮文字测量：按钮宽度一律由**文字实测宽度**推出来，不用魔法数字
//
// 起因：新建/编辑曲谱窗口"演奏方式"第二排的长按钮（按住左键+中键（降调+半音））
//       原来按等分算宽度，中文比算出来的宽，左端被切掉一个"按"字；时值档位加多以后
//       同类问题会更多。所以宽度必须量出来：内存 DC + GetTextExtentPoint32W，
//       用该按钮**实际使用的字体**（UiFonts().body，PaintFlatButton 也用它画字）。
// 约定：窗口用这里的函数排版，自检（ui_test）用同一套函数断言
//       "文字宽 + 左右内边距 ≤ 按钮宽度"，这样以后再改文案也会被拦住。
// ---------------------------------------------------------------------------
constexpr int kNewSongWindowWidth = 900;   // 新建/编辑曲谱窗口的客户区基准宽度（96 DPI）
//   900 而不是 790：时值从 4 档扩到 10 档（标准乐理）后，按钮多了一倍，
//   宽一点才能在 2 行里放下，窗口不至于变得又窄又高。

int MeasureTextWidth(HFONT font, const std::wstring& text);   // 文字实测宽度（像素）
int ButtonTextPadding();                                      // 按钮文字左右内边距合计（像素）
int ButtonWidthForText(const std::wstring& text);             // 文字宽 + 内边距（正文按钮字体）

// 新建曲谱窗口里"按钮列"的可用宽度（客户区宽度去掉左边距、标签列、右边距）
int NewSongButtonsAvailableWidth(int clientWidth);

// 一组按钮的排版结果：宽度按文字实测，行号按可用宽度贪心折行
struct ButtonRowPlan {
    std::vector<int> widths;   // 每个按钮的宽度
    std::vector<int> rows;     // 每个按钮排在第几行（0 起）
    int rowCount = 0;          // 一共几行
    bool overflow = false;     // 有按钮自己就比整行可用宽度还宽（必须报出来，不能默默截字）
};

// forceNewRowAt：这些下标必须**另起一行**（例如"按住左键+中键 / 按住右键+中键"这一对组合模式
//   要挨在一起，不要被贪心折行拆散）。可以为空。
ButtonRowPlan PlanButtonRows(const std::vector<std::wstring>& labels, int availableWidth,
                             const std::vector<int>& forceNewRowAt = {});

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
