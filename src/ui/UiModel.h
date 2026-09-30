#pragma once

// 界面用的应用状态（Phase 7）。
//
// 同样**不含 Win32**：设置项、曲谱编辑模型、状态快照都能在没有窗口的情况下自检。
// 窗口只负责“把这里的内容画出来”和“把用户的输入写回来”。
//
// 需求书依据：
//   三十六节 主界面要显示 当前歌曲 / 控制按钮 / 状态 / 当前音符 / 进度 / BPM / 速度 / 模式 / 音符表
//   四十二节 用户切换窗口要可配置：不处理 / 暂停 / 停止（默认建议停止或暂停）

#include <string>
#include <vector>

#include "core/Types.h"
#include "song/Song.h"
#include "song/Timeline.h"

namespace hp {
namespace ui {

// ---------------------------------------------------------------------------
// 四十二节：前台窗口变化时的策略
// ---------------------------------------------------------------------------
enum class ForegroundPolicy : int {
    Ignore = 0,   // 不处理
    Pause = 1,    // 暂停
    Stop = 2,     // 停止
};

const char* ForegroundPolicyName(ForegroundPolicy policy);        // "不处理" / "暂停" / "停止"
const char* ForegroundPolicyJsonName(ForegroundPolicy policy);    // "Ignore" / "Pause" / "Stop"
bool ParseForegroundPolicy(const std::string& text, ForegroundPolicy& out);

// 需求书四十二节：默认建议“停止或暂停，可配置”。这里默认暂停（更温和，位置不丢）。
inline ForegroundPolicy DefaultForegroundPolicy() { return ForegroundPolicy::Pause; }

// ---------------------------------------------------------------------------
// 设置（存内存；落盘属 Phase 11）
// ---------------------------------------------------------------------------
struct AppSettings {
    std::string playHotkey = "F6";
    std::string pauseHotkey = "F5";   // 暂停 / 继续
    std::string stopHotkey = "F7";
    std::string restartHotkey = "F8";
    std::string previousHotkey = "F9";     // 演奏中切上一首
    std::string nextHotkey = "F10";        // 演奏中切下一首

    // 热键是否"独占"（默认 false = 放行模式）：
    //   false 放行：用低层钩子认热键，**不吞键**——工具切歌的同时游戏照样收到这个键；
    //   true  独占：RegisterHotKey 全局注册，键被工具接走、游戏收不到（老行为）。
    // 放行模式必须提权：低权限进程的钩子看不到发给管理员游戏的按键。
    bool exclusiveHotkeys = false;

    double speed = 1.0;                 // 播放速度（1.0 = 100%）
    bool dryRun = false;                // 干跑：不真的注入按键（排练/演示用）
    // 时间轴模式（Phase 2 的选项）：精确 = 不打乱；自然 = 每个音符叠加 ±jitterMs 抖动
    bool naturalMode = false;
    double jitterMs = 3.0;

    bool monitorKeyboard = true;        // Phase 6 三项开关
    bool monitorMouseButtons = true;
    bool monitorMouseMove = false;

    // 排除键位（Phase 13 追加）：这些键按下时**不**触发"用户输入急停"。
    // 游戏里的走路 / 跳跃 / 切枪键放进来，演奏就不会被打断。
    std::vector<std::string> ignoredKeys;

    ForegroundPolicy foregroundPolicy = ForegroundPolicy::Pause;

    // 游戏以管理员身份运行时，普通权限的进程发不出按键（UIPI 拦截）：
    // 勾上它，启动时就自动向系统申请管理员权限（弹一次 UAC），一次选择以后都省事
    bool runAsAdmin = false;

    std::string songPath;               // 最近一次打开的曲谱文件

    static AppSettings Default() { return AppSettings(); }

    // 一行摘要（状态栏/日志用）
    std::string Summary() const;
    // 多行详情（"设置"窗口的预览与自检都会用）
    std::string DetailText() const;
};

// ---------------------------------------------------------------------------
// 曲谱编辑模型：表格里显示“拍”，保存时换回绝对时间
//
// 说明：编辑表格存的是**拍位置 + 拍时值**（作者视角，与 JSON 文件一致），
// 用 double 直接存，往返无损；下拉框给出的时值名只是“填入一个常用值”的快捷方式。
// ---------------------------------------------------------------------------
struct EditorRow {
    Key key = Key::Z;
    PlayMode mode = PlayMode::Normal;
    double beat = 0.0;            // 从歌曲开始的拍数
    double durationBeats = 1.0;   // 持续多少拍
    bool rest = false;            // 休止行：不出声，只占时间（key/mode 无意义）
};

// 绝对时间轴 → 编辑表格（按 startTime 排序）
std::vector<EditorRow> BuildEditorRows(const Song& song);

// 编辑表格 → 歌曲（一次性换算成绝对时间；name/bpm/beatUnit/拍号 由参数给出）
Song BuildSongFromRows(const std::string& name, double bpm, NoteValue beatUnit,
                       const TimeSignature& signature, const std::vector<EditorRow>& rows);

// 表格行的显示文本（需求书三十六节的四列 + 起始时间 + Phase 8 的“小节”）
struct EditorRowText {
    std::string bar;         // "3"（第几小节，由拍号算出）
    std::string time;        // "0.250"
    std::string beat;        // "0.5"
    std::string key;         // "Z"
    std::string mode;        // "Normal"
    std::string value;       // "Quarter 1/4（500 ms）"；非标准时值写成 "约 0.7 拍（350 ms）"
};
// 需要一个“只有曲头信息”的 Song 来算毫秒/小节（bpm、拍单位、拍号）
Song DescribeContext(const std::string& name, double bpm, NoteValue beatUnit, const TimeSignature& signature);
EditorRowText DescribeRow(const EditorRow& row, const Song& song);

// 时值单元格（Phase 8）：按曲谱的拍单位换算，给出音符名 + 分数写法 + 真实毫秒，
// 例如 "四分音符 1/4（500 ms）"；对不上标准时值就给近似值和毫秒。
std::string NoteValueCellForSong(const Song& song, double quarterBeats);

// ---------------------------------------------------------------------------
// "新建曲谱"窗口用的点选表（纯逻辑，自检可查）
//   音调按钮：把键盘 Z X C V B N M , 翻译成口琴的 1 2 3 4 5 6 7 i
//   演奏方式：本音 / 左键（降调）/ 中键（半音）/ 右键（升调）
//   时值：一拍 / 两拍 / 半拍 / 休止一拍（休止只占时间、不出声）
// ---------------------------------------------------------------------------
std::string KeyNoteName(Key key);              // "1" … "7"、"i"
std::string PlayModeButtonName(PlayMode mode); // "不按鼠标（本音）" 等

struct DurationChoice {
    const char* label;   // 按钮文字
    double beats;        // 占多少拍
    bool rest;           // true = 只占时间，不出声
};
constexpr int kDurationChoiceCount = 4;
extern const DurationChoice kDurationChoices[kDurationChoiceCount];

// 时值按钮下标 ↔ 拍数互查：新建曲谱窗口的"编辑选中行"要把选中行的时值反填进按钮。
//   DurationChoiceIndexFor(2.0) == 1（两拍）；对不上任何一档（例如曲谱里手工写的 1.5 拍）返回 -1。
//   DurationUserCell 用用户的话说时值："两拍（1 秒）"——不出现"四分音符""毫秒"这些术语。
int DurationChoiceIndexFor(double beats);
std::string DurationUserCell(double beats, double bpm);

// ---------------------------------------------------------------------------
// 主界面音符表的一格（Phase 11 大型曲谱：虚拟列表按需生成，不预建十万行）
//   column：0 序号 / 1 时间 / 2 音符 / 3 模式 / 4 时值
//   行号越界返回空串（控件问什么就答什么，不抛异常）
// ---------------------------------------------------------------------------
std::string NoteRowCellText(const Timeline& timeline, const Song& song, int row, int column);

// ---- Phase 8：BPM / 拍号 / 小节 / 速度 / 模式 的界面文案（窗口与自检共用）----
std::string TempoText(const Song& song);       // "BPM：120（1 拍 = 500 ms）"
std::string SignatureLine(const Song& song);   // "拍号：4/4　每小节 4 拍 = 2.000 秒"
std::string BarText(int bar, int total);       // "小节：3 / 8"
std::string ModeText(bool naturalMode, double jitterMs);   // "模式：精确" / "模式：自然（抖动 3 ms）"
std::string SpeedChoiceText(double speed);     // "100%"
int SpeedPresetIndexFor(double speed);         // 命中六档之一返回 0..5，否则 -1（自定义）
std::string SpeedCaption();                    // "播放速度（六档预置或自定义 50% ~ 200%）："

// BPM 校验（"120" / "120.5"），返回是否成功
bool ParseBpmText(const std::string& text, double& out, std::string& error);

// ---------------------------------------------------------------------------
// 界面文案：窗口与自检**共用这一份**（自检会扫描它们，确保没有一个 emoji）
// ---------------------------------------------------------------------------
namespace labels {
extern const char* kAppTitle;          // 主窗口标题
extern const char* kSongPrefix;        // "当前歌曲："
extern const char* kPlay;              // 播放
extern const char* kPause;             // 暂停
extern const char* kStop;              // 停止
extern const char* kRestart;           // 重播
extern const char* kOpenSong;          // 打开曲谱
extern const char* kEditSong;
extern const char* kNewSong;      // 无曲谱时主界面按钮的文案
extern const char* kNewSongName;  // 新建曲谱的默认曲名
extern const char* kNewSongHint;  // 新建曲谱时编辑器里的操作提示          // 编辑曲谱
extern const char* kTextScore;         // 文本曲谱（Phase 9）
extern const char* kSettings;          // 设置

// 文本曲谱窗口（Phase 9）
extern const char* kTextScoreTitle;    // 文本曲谱（一行一个音符）
extern const char* kTextScoreHint;     // 格式说明（两行）
extern const char* kTextScoreName;     // 曲名：
extern const char* kTextScoreParse;    // 解析预览
extern const char* kTextScoreExample;  // 载入示例
extern const char* kTextScoreApply;    // 载入为主曲谱
extern const char* kTextScoreClose;    // 关闭
extern const char* kSaveAs;            // 另存为
extern const char* kLoad;              // 载入
extern const char* kApply;             // 应用
extern const char* kCancel;            // 取消
extern const char* kClose;             // 关闭
extern const char* kRestoreDefaults;   // 恢复默认
extern const char* kAddNote;           // 新增
extern const char* kDeleteNote;        // 删除
extern const char* kApplyRow;          // 应用到选中
extern const char* kMoveUp;            // 上移
extern const char* kMoveDown;          // 下移
extern const char* kDryRun;            // 干跑（不注入按键）
extern const char* kReady;             // 就绪提示
extern const char* kNoSong;            // "（没有曲谱：请先打开或编辑一首）"
extern const char* kGroupPlayback;     // 分组标题：播放控制
extern const char* kGroupStatus;       // 分组标题：状态
extern const char* kGroupNotes;        // 分组标题：音符表
extern const char* kGroupInputGuard;   // 分组标题：用户输入急停
extern const char* kGroupForeground;   // 分组标题：前台窗口变化
extern const char* kGroupHotkeys;      // 分组标题：全局热键
extern const char* kGroupPlaybackOpt;  // 分组标题：播放选项
// Phase 8：BPM / 拍号 / 小节 / 速度 / 模式
extern const char* kBpmPrefix;         // "BPM："
extern const char* kSignaturePrefix;   // "拍号："
extern const char* kBarPrefix;         // "小节："
extern const char* kSpeedPrefix;       // "速度："
extern const char* kModePrefix;        // "模式："
extern const char* kExactMode;         // "精确"
extern const char* kNaturalMode;       // "自然"
extern const char* kCustomSpeed;       // "自定义（在设置里填）"
}  // namespace labels

}  // namespace ui
}  // namespace hp
