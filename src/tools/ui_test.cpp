// ui_test.cpp —— Phase 7 自检程序（界面逻辑，无窗口）
//
// 为什么界面也要有自检：
//   窗口本身靠肉眼与 UIA 控件树验证（ui-e2e.sh），但界面里所有**数字与文案的换算**
//   （进度百分比、当前音符、时间格式、音符表格的 时间/音符/模式/时值、速度解析、
//   设置校验、表格 ↔ 绝对时间轴往返）都是纯函数，可以在这里自动验证。
//   把这一层测干净，窗口里剩下的只有"画出来"和"把输入写回去"。
//
// 需求书依据：
//   三十六节 主界面要素（当前歌曲 / 控制按钮 / 状态 / 当前音符 / 进度 / BPM / 速度 / 模式 / 音符表）
//   四十二节 前台窗口变化策略（不处理 / 暂停 / 停止）
//   用户约定 中文、禁 emoji、状态用文字表达
//
// 命令行：--all / --format / --status / --table / --settings / --roundtrip / --labels / --list / --help

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"
#include "hotkey/HotkeyBinding.h"
#include "song/SongSerializer.h"
#include "song/TestSongs.h"
#include "song/Timeline.h"
#include "ui/UiFormat.h"
#include "song/BeatMath.h"
#include "ui/UiModel.h"
#include "ui/UiTheme.h"

#include <shellapi.h>

namespace {

int g_passed = 0;
int g_failed = 0;

void Section(const std::string& title) {
    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, std::string("── ") + title + " ──");
}

void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_passed;
        hp::LogLine(hp::LogLevel::Ok, std::string("通过  ") + what);
    } else {
        ++g_failed;
        hp::LogLine(hp::LogLevel::Error, std::string("错误  ") + what);
    }
}

void CheckNear(double actual, double expected, double tolerance, const std::string& what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    Check(ok, what + (ok ? "" : ("（实际 " + std::to_string(actual) + "，期望 " +
                                 std::to_string(expected) + "）")));
}

void Note(const std::string& text) {
    hp::LogLine(hp::LogLevel::Plain, std::string("      ") + text);
}

std::vector<std::string> Utf8Arguments() {
    std::vector<std::string> result;
    int wideCount = 0;
    LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (wideArgs == nullptr) {
        return result;
    }
    for (int i = 0; i < wideCount; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, nullptr, 0, nullptr,
                                             nullptr);
        if (size <= 1) {
            result.push_back(std::string());
            continue;
        }
        std::string utf8(static_cast<size_t>(size - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, utf8.data(), size, nullptr, nullptr);
        result.push_back(utf8);
    }
    LocalFree(wideArgs);
    return result;
}

bool HasFlag(const std::vector<std::string>& args, const std::string& flag) {
    for (const std::string& arg : args) {
        if (arg == flag) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. 数字与时间格式
// ---------------------------------------------------------------------------
void RunFormatTest() {
    Section("数字与时间格式");

    Check(hp::ui::FormatSeconds3(0.25) == "0.250", "秒数三位小数：0.25 → 0.250");
    Check(hp::ui::FormatSeconds3(0.0) == "0.000", "秒数三位小数：0 → 0.000");
    Check(hp::ui::FormatClock(12.345) == "00:12.345", "时钟格式：12.345 → 00:12.345");
    Check(hp::ui::FormatClock(75.5) == "01:15.500", "时钟格式：75.5 → 01:15.500");
    Check(hp::ui::FormatClock(-3.0) == "00:00.000", "负数按 0 处理（不让界面出现负时间）");
    Check(hp::ui::FormatBeats(1.25) == "1.25", "拍数：1.25 → 1.25");
    Check(hp::ui::FormatBeats(1.0) == "1", "拍数：1 → 1（去掉没意义的 .0）");

    Check(hp::ui::PercentOf(23, 128) == 18, "进度：23/128 → 18%");
    Check(hp::ui::PercentOf(0, 128) == 0, "进度：0/128 → 0%");
    Check(hp::ui::PercentOf(128, 128) == 100, "进度：128/128 → 100%");
    Check(hp::ui::PercentOf(5, 0) == 0, "进度：总数为 0 时按 0% 处理（空曲不算错）");
    Check(hp::ui::PercentOf(999, 100) == 100, "进度不会超过 100%");

    Check(hp::ui::FormatSpeedText(1.0) == "100%", "速度：1.0 → 100%");
    Check(hp::ui::FormatSpeedText(0.75) == "75%", "速度：0.75 → 75%");
    Check(hp::ui::FormatSpeedText(1.333) == "133%", "速度：1.333 → 133%（四舍五入）");

    Check(hp::ui::ProgressBarText(65, 10) == "[######.... ] 65%" ||
              hp::ui::ProgressBarText(65, 10) == "[#######...] 65%",
          "进度条文本：" + hp::ui::ProgressBarText(65, 10));
    Check(hp::ui::ProgressBarText(0, 8) == "[........] 0%", "进度条文本（0%）：" +
                                                                 hp::ui::ProgressBarText(0, 8));
    Check(hp::ui::ProgressBarText(100, 8) == "[########] 100%",
          "进度条文本（100%）：" + hp::ui::ProgressBarText(100, 8));
}

// ---------------------------------------------------------------------------
// 2. 速度解析
// ---------------------------------------------------------------------------
void RunSpeedTest() {
    Section("速度输入解析");

    double speed = 0.0;
    std::string error;

    Check(hp::ui::ParseSpeedText("100%", speed, error) && std::fabs(speed - 1.0) < 1e-9,
          "\"100%\" → 1.0");
    Check(hp::ui::ParseSpeedText("1.0", speed, error) && std::fabs(speed - 1.0) < 1e-9,
          "\"1.0\" → 1.0");
    Check(hp::ui::ParseSpeedText("100", speed, error) && std::fabs(speed - 1.0) < 1e-9,
          "\"100\" 按百分比理解 → 1.0");
    Check(hp::ui::ParseSpeedText("0.5", speed, error) && std::fabs(speed - 0.5) < 1e-9,
          "\"0.5\" → 0.5 倍");
    Check(hp::ui::ParseSpeedText(" 150% ", speed, error) && std::fabs(speed - 1.5) < 1e-9,
          "两边空格能容忍 → 1.5 倍");
    Check(hp::ui::ParseSpeedText("2", speed, error) && std::fabs(speed - 2.0) < 1e-9,
          "\"2\" → 2 倍（不是 2%）");

    Check(!hp::ui::ParseSpeedText("", speed, error) && !error.empty(), "空字符串被拒绝：" + error);
    Check(!hp::ui::ParseSpeedText("快一点", speed, error) && error.find("数字") != std::string::npos,
          "文字被拒绝：" + error);
    Check(!hp::ui::ParseSpeedText("10x", speed, error) && !error.empty(),
          "带单位被拒绝：" + error);
    const bool zeroRejected = !hp::ui::ParseSpeedText("0", speed, error);
    Check(zeroRejected && error.find("50%") != std::string::npos,
          "0 被拒绝（不能让演奏静止）：" + error);
    Check(!hp::ui::ParseSpeedText("900%", speed, error) && !error.empty(),
          "过大被拒绝：" + error);
}

// ---------------------------------------------------------------------------
// 3. 状态文案
// ---------------------------------------------------------------------------
void RunStatusTest() {
    Section("状态文案（二十六节：要能看出为什么停）");

    const std::string playing =
        hp::ui::StatusLine(hp::PlaybackState::Playing, hp::StopReason::None, "");
    Check(playing == "状态：演奏中", "演奏中：" + playing);

    const std::string paused =
        hp::ui::StatusLine(hp::PlaybackState::Paused, hp::StopReason::None, "");
    Check(paused == "状态：已暂停", "已暂停：" + paused);

    const std::string stopped = hp::ui::StatusLine(hp::PlaybackState::Stopped,
                                                   hp::StopReason::UserInput, "Space");
    Check(stopped == "状态：已停止　原因：检测到用户输入：Space",
          "被用户输入打断：" + stopped);

    const std::string manual =
        hp::ui::StatusLine(hp::PlaybackState::Stopped, hp::StopReason::UserStop, "");
    Check(manual == "状态：已停止　原因：用户手动停止", "手动停止：" + manual);

    const std::string initial =
        hp::ui::StatusLine(hp::PlaybackState::Stopped, hp::StopReason::None, "");
    Check(initial == "状态：已停止", "还没弹过时不显示原因：" + initial);

    Check(hp::ui::CurrentNoteText(23, 128) == "当前音符：23 / 128",
          "当前音符文案：" + hp::ui::CurrentNoteText(23, 128));
    Check(hp::ui::CurrentNoteText(-5, 128) == "当前音符：0 / 128",
          "负数归零：" + hp::ui::CurrentNoteText(-5, 128));
}

// ---------------------------------------------------------------------------
// 4. 音符表格四列
// ---------------------------------------------------------------------------
void RunTableTest() {
    Section("音符表格（三十六节：时间 / 音符 / 模式 / 时值）");

    Check(hp::ui::NoteTimeCell(0.25) == "0.250", "时间列：" + hp::ui::NoteTimeCell(0.25));
    Check(hp::ui::NoteKeyCell(hp::Key::Z) == "Z", "音符列：" + hp::ui::NoteKeyCell(hp::Key::Z));
    Check(hp::ui::NoteKeyCell(hp::Key::Comma) == ",", "音符列（逗号）：" +
                                                          hp::ui::NoteKeyCell(hp::Key::Comma));
    Check(hp::ui::NoteModeCell(hp::PlayMode::Left) == "Left",
          "模式列：" + hp::ui::NoteModeCell(hp::PlayMode::Left));
    Check(hp::ui::NoteModeCell(hp::PlayMode::LeftMiddle) == "LeftMiddle" &&
              hp::ui::NoteModeCell(hp::PlayMode::RightMiddle) == "RightMiddle",
          "模式列（组合模式）：" + hp::ui::NoteModeCell(hp::PlayMode::LeftMiddle) + " / " +
              hp::ui::NoteModeCell(hp::PlayMode::RightMiddle));
    Check(hp::ui::PlayModeButtonName(hp::PlayMode::LeftMiddle) == "按住左键+中键（降调+半音）" &&
              hp::ui::PlayModeButtonName(hp::PlayMode::RightMiddle) == "按住右键+中键（升调+半音）",
          "组合模式按钮文案：" + hp::ui::PlayModeButtonName(hp::PlayMode::LeftMiddle));

    // 120 BPM、四分音符为一拍：四分音符 = 1 拍，八分音符 = 0.5 拍
    Check(hp::ui::NearestValueName(1.0, hp::NoteValue::Quarter) == "Quarter",
          "1 拍 → Quarter");
    Check(hp::ui::NearestValueName(0.5, hp::NoteValue::Quarter) == "Eighth",
          "0.5 拍 → Eighth");
    Check(hp::ui::NearestValueName(2.0, hp::NoteValue::Quarter) == "Half",
          "2 拍 → Half");
    Check(hp::ui::NearestValueName(4.0, hp::NoteValue::Quarter) == "Whole",
          "4 拍 → Whole");
    // 6/8：以八分音符为一拍时，四分音符 = 2 拍
    Check(hp::ui::NearestValueName(2.0, hp::NoteValue::Eighth) == "Quarter",
          "6/8 里 2 拍 → Quarter（拍单位不同结果不同）");

    Check(hp::ui::NoteValueCell(1.0, hp::NoteValue::Quarter) == "Quarter（1 拍）",
          "时值列：" + hp::ui::NoteValueCell(1.0, hp::NoteValue::Quarter));

    double beats = 0.0;
    Check(hp::ui::ValueBeatsByName("1/4", hp::NoteValue::Quarter, beats) &&
              std::fabs(beats - 1.0) < 1e-9,
          "时值名 \"1/4\" → 1 拍");
    Check(hp::ui::ValueBeatsByName("八分", hp::NoteValue::Quarter, beats) &&
              std::fabs(beats - 0.5) < 1e-9,
          "时值名简写 \"八分\" → 0.5 拍");
    Check(hp::ui::ValueBeatsByName("八分音符", hp::NoteValue::Quarter, beats) &&
              std::fabs(beats - 0.5) < 1e-9,
          "时值名全称 \"八分音符\" → 0.5 拍");
    Check(hp::ui::ValueBeatsByName("四分", hp::NoteValue::Eighth, beats) &&
              std::fabs(beats - 2.0) < 1e-9,
          "6/8 里 \"四分\" → 2 拍（按拍单位换算）");
    Check(hp::ui::ValueBeatsByName("0.25", hp::NoteValue::Quarter, beats) &&
              std::fabs(beats - 0.25) < 1e-9,
          "直接填拍数 \"0.25\" → 0.25 拍");
    Check(!hp::ui::ValueBeatsByName("不是时值", hp::NoteValue::Quarter, beats),
          "乱填被拒绝");
}

// ---------------------------------------------------------------------------
// 5. 设置与前台窗口策略
// ---------------------------------------------------------------------------
void RunSettingsTest() {
    Section("设置与前台窗口策略（四十二节）");

    const hp::ui::AppSettings defaults;
    Check(defaults.ignoredKeys.size() == 7, "排除键位默认含 W A S D Q E 空格（用户当前设置）");
    Check(defaults.playHotkey == "Num5" && defaults.pauseHotkey == "Num6" && defaults.stopHotkey == "Num4" &&
              defaults.restartHotkey == "Num0" && defaults.previousHotkey == "Up" &&
              defaults.nextHotkey == "Down",
          "默认热键 Num5 / Num6 / Num4 / Num0 + 换曲 Up / Down（用户当前设置）");
    Check(std::fabs(defaults.speed - 1.0) < 1e-9, "默认速度 100%");
    Check(defaults.monitorKeyboard && defaults.monitorMouseButtons && !defaults.monitorMouseMove &&
              !defaults.stopOnMouseWheel,
          "默认输入监控：键盘开、鼠标键开、鼠标移动关、鼠标滚轮开（Phase 6 + 滚轮打断演奏）");
    const std::string settingsDetail = defaults.DetailText();
    const size_t guardLine = settingsDetail.find("用户输入急停");
    Check(settingsDetail.find("鼠标滚轮") != std::string::npos,
          "设置详情里有鼠标滚轮这一项（新设置项 stopOnMouseWheel 默认开）；详情：" +
              ((guardLine == std::string::npos) ? settingsDetail : settingsDetail.substr(guardLine)));
    Check(defaults.foregroundPolicy == hp::ui::ForegroundPolicy::Pause,
          "默认前台窗口策略 = 暂停（四十二节：默认建议停止或暂停）");

    Check(std::string(hp::ui::ForegroundPolicyName(hp::ui::ForegroundPolicy::Ignore)) == "不处理" &&
              std::string(hp::ui::ForegroundPolicyName(hp::ui::ForegroundPolicy::Pause)) == "暂停" &&
              std::string(hp::ui::ForegroundPolicyName(hp::ui::ForegroundPolicy::Stop)) == "停止",
          "三种策略的中文名齐全（不处理 / 暂停 / 停止）");

    hp::ui::ForegroundPolicy policy = hp::ui::ForegroundPolicy::Ignore;
    Check(hp::ui::ParseForegroundPolicy("停止", policy) && policy == hp::ui::ForegroundPolicy::Stop,
          "解析 \"停止\"");
    Check(hp::ui::ParseForegroundPolicy("Pause", policy) && policy == hp::ui::ForegroundPolicy::Pause,
          "解析 \"Pause\"");
    Check(hp::ui::ParseForegroundPolicy("Ignore", policy) &&
              policy == hp::ui::ForegroundPolicy::Ignore,
          "解析 \"Ignore\"");
    Check(!hp::ui::ParseForegroundPolicy("随便", policy), "乱填被拒绝");

    Note("设置摘要：" + defaults.Summary());
    hp::ui::AppSettings custom = defaults;
    custom.speed = 0.75;
    custom.dryRun = true;
    Check(custom.Summary().find("75%") != std::string::npos &&
              custom.Summary().find("干跑") != std::string::npos,
          "摘要反映速度与干跑：" + custom.Summary());
    Note("设置详情：");
    std::string detail = custom.DetailText();
    size_t start = 0;
    while (start < detail.size()) {
        const size_t end = detail.find('\n', start);
        Note(detail.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    double bpm = 0.0;
    std::string error;
    Check(hp::ui::ParseBpmText("120", bpm, error) && std::fabs(bpm - 120.0) < 1e-9, "BPM 120 可解析");
    Check(hp::ui::ParseBpmText("120.5", bpm, error) && std::fabs(bpm - 120.5) < 1e-9,
          "BPM 120.5 可解析");
    Check(!hp::ui::ParseBpmText("很快", bpm, error) && !error.empty(), "BPM 文字被拒绝：" + error);
    Check(!hp::ui::ParseBpmText("0", bpm, error), "BPM 0 被拒绝");
    Check(!hp::ui::ParseBpmText("5000", bpm, error), "BPM 过大被拒绝");
}

// ---------------------------------------------------------------------------
// 6. 编辑表格 ↔ 绝对时间轴 往返
// ---------------------------------------------------------------------------
void RunRoundTripTest() {
    Section("编辑表格 ↔ 绝对时间轴（往返必须无损）");

    const std::vector<std::string> names = hp::test_songs::Names();
    int totalNotes = 0;
    double worstError = 0.0;
    bool allSame = true;
    std::string worstName;

    for (const std::string& songName : names) {
        const hp::Song song = hp::test_songs::ByName(songName);
        const std::vector<hp::ui::EditorRow> rows = hp::ui::BuildEditorRows(song);
        const hp::Song rebuilt = hp::ui::BuildSongFromRows(song.name, song.bpm, song.beatUnit,
                                                           song.timeSignature, rows);
        if (rebuilt.notes.size() != song.notes.size()) {
            allSame = false;
            Check(false, song.name + "：音符数变了（" + std::to_string(song.notes.size()) + " → " +
                             std::to_string(rebuilt.notes.size()) + "）");
            continue;
        }
        for (size_t i = 0; i < song.notes.size(); ++i) {
            if (song.notes[i].key != rebuilt.notes[i].key ||
                song.notes[i].mode != rebuilt.notes[i].mode) {
                allSame = false;
            }
            const double error = std::max(std::fabs(song.notes[i].startTime - rebuilt.notes[i].startTime),
                                          std::fabs(song.notes[i].duration - rebuilt.notes[i].duration));
            if (error > worstError) {
                worstError = error;
                worstName = song.name;
            }
            ++totalNotes;
        }
        if (std::fabs(rebuilt.bpm - song.bpm) > 1e-9 ||
            rebuilt.timeSignature.numerator != song.timeSignature.numerator ||
            rebuilt.timeSignature.denominator != song.timeSignature.denominator ||
            rebuilt.beatUnit != song.beatUnit || rebuilt.name != song.name) {
            allSame = false;
            Check(false, song.name + "：曲名/BPM/拍号/拍单位没有原样带回来");
        }
    }

    Check(allSame, std::to_string(names.size()) +
                       " 首内置曲的 键/模式/曲名/BPM/拍号/拍单位 往返一致");
    Check(worstError < 1e-9, "表格往返最大时间误差 " + std::to_string(worstError) + " 秒（共 " +
                                 std::to_string(totalNotes) + " 个音符，最差 " +
                                 (worstName.empty() ? std::string("无") : worstName) + "）");

    // 反向：从零构造一首（0 拍 Z、0.5 拍 X、1 拍 C），检查换算正确
    std::vector<hp::ui::EditorRow> rows;
    rows.push_back({hp::Key::Z, hp::PlayMode::Normal, 0.0, 1.0});
    rows.push_back({hp::Key::X, hp::PlayMode::Normal, 0.5, 1.0});
    rows.push_back({hp::Key::C, hp::PlayMode::Left, 1.0, 0.5});
    const hp::Song made = hp::ui::BuildSongFromRows("自检曲", 120.0, hp::NoteValue::Quarter,
                                                    hp::TimeSignature{4, 4}, rows);
    Check(made.notes.size() == 3, "构造出 3 个音符");
    if (made.notes.size() == 3) {
        Check(std::fabs(made.notes[0].startTime - 0.0) < 1e-9 &&
                  std::fabs(made.notes[1].startTime - 0.25) < 1e-9 &&
                  std::fabs(made.notes[2].startTime - 0.5) < 1e-9,
              "120 BPM 下 0/0.5/1 拍 → 0.000/0.250/0.500 秒");
        Check(std::fabs(made.notes[2].duration - 0.25) < 1e-9,
              "第三音符 0.5 拍 = 0.250 秒（八分音符）");
        Check(made.notes[2].mode == hp::PlayMode::Left, "演奏模式带过来了");
    }

    // 表格里的行文本（界面要显示的东西）
    const hp::Song rowContext = hp::ui::DescribeContext("自检曲", 120.0, hp::NoteValue::Quarter,
                                                        hp::TimeSignature{4, 4});
    const hp::ui::EditorRowText rowText = hp::ui::DescribeRow(rows[1], rowContext);
    Check(rowText.bar == "1" && rowText.time == "0.250" && rowText.beat == "0.5" &&
              rowText.key == "X" && rowText.mode == "Normal" &&
              rowText.value == "四分音符 1/4（500 ms）",
          "行文本：" + rowText.bar + " | " + rowText.time + " | " + rowText.beat + " | " +
              rowText.key + " | " + rowText.mode + " | " + rowText.value);

    // 与 JSON 文本往返（界面“另存为/载入”走的就是这两个函数）
    const std::string json = hp::SongToJsonText(made, 4);
    hp::Song loaded;
    const hp::SongIoResult result = hp::ParseSongFromJsonText(json, loaded);
    Check(result.ok, "界面用 JSON 文本往返成功");
    Check(loaded.notes.size() == made.notes.size(), "JSON 往返音符数一致");
    double jsonWorst = 0.0;
    for (size_t i = 0; i < std::min(loaded.notes.size(), made.notes.size()); ++i) {
        jsonWorst = std::max(jsonWorst, std::fabs(loaded.notes[i].startTime - made.notes[i].startTime));
    }
    Check(jsonWorst < 1e-9, "JSON 文本往返最大误差 " + std::to_string(jsonWorst) + " 秒");
}

// ---------------------------------------------------------------------------
// 7. 热键文本框（设置窗口里能填的东西）
// ---------------------------------------------------------------------------
void RunHotkeyTextTest() {
    Section("设置窗口的热键文本框");

    hp::HotkeyBinding binding;
    std::string error;
    Check(hp::ParseHotkey("F6", binding, &error) && binding.vk == VK_F6, "\"F6\" 可解析");
    Check(hp::ParseHotkey("Ctrl+Shift+F9", binding, &error) && binding.vk == VK_F9 &&
              binding.modifiers == (hp::kModifierCtrl | hp::kModifierShift),
          "\"Ctrl+Shift+F9\" 可解析");
    Check(hp::ParseHotkey("f6", binding, &error) && binding.vk == VK_F6,
          "小写 \"f6\" 也能解析");
    Check(hp::ParseHotkey("none", binding, &error) && binding.vk == 0,
          "\"none\" = 取消绑定");
    Check(!hp::ParseHotkey("按一下", binding, &error) && !error.empty(),
          "乱填被拒绝并给出原因：" + error);
}

// ---------------------------------------------------------------------------
// 8. 界面文案守卫（中文、无 emoji）
// ---------------------------------------------------------------------------
void RunLabelTest() {
    Section("界面文案（中文、无 emoji、无刷屏）");

    const std::vector<std::pair<std::string, std::string>> all = {
        {"窗口标题", hp::ui::labels::kAppTitle},
        {"当前歌曲前缀", hp::ui::labels::kSongPrefix},
        {"播放", hp::ui::labels::kPlay},
        {"暂停", hp::ui::labels::kPause},
        {"停止", hp::ui::labels::kStop},
        {"重播", hp::ui::labels::kRestart},
        {"打开曲谱", hp::ui::labels::kOpenSong},
        {"编辑曲谱", hp::ui::labels::kEditSong},
        {"新建曲谱", hp::ui::labels::kNewSong},
        {"设置", hp::ui::labels::kSettings},
        {"另存为", hp::ui::labels::kSaveAs},
        {"载入", hp::ui::labels::kLoad},
        {"应用", hp::ui::labels::kApply},
        {"取消", hp::ui::labels::kCancel},
        {"关闭", hp::ui::labels::kClose},
        {"恢复默认", hp::ui::labels::kRestoreDefaults},
        {"新增", hp::ui::labels::kAddNote},
        {"删除", hp::ui::labels::kDeleteNote},
        {"应用到选中", hp::ui::labels::kApplyRow},
        {"上移", hp::ui::labels::kMoveUp},
        {"下移", hp::ui::labels::kMoveDown},
        {"干跑", hp::ui::labels::kDryRun},
        {"就绪提示", hp::ui::labels::kReady},
        {"没有曲谱", hp::ui::labels::kNoSong},
        {"分组：播放控制", hp::ui::labels::kGroupPlayback},
        {"分组：状态", hp::ui::labels::kGroupStatus},
        {"分组：音符表", hp::ui::labels::kGroupNotes},
        {"分组：用户输入急停", hp::ui::labels::kGroupInputGuard},
        {"鼠标滚轮复选框", hp::ui::labels::kMouseWheelGuard},
        {"分组：前台窗口变化", hp::ui::labels::kGroupForeground},
        {"分组：全局热键", hp::ui::labels::kGroupHotkeys},
        {"分组：播放选项", hp::ui::labels::kGroupPlaybackOpt},
        {"延时按钮", hp::ui::labels::kExtendAction},
    };

    int emojiFree = 0;
    for (const auto& item : all) {
        if (!hp::ui::ContainsEmoji(item.second)) {
            ++emojiFree;
        } else {
            Check(false, std::string("文案里有 emoji：") + item.first + " = " + item.second);
        }
    }
    Check(emojiFree == static_cast<int>(all.size()),
          "界面文案 " + std::to_string(emojiFree) + "/" + std::to_string(all.size()) +
              " 条：无 emoji");

    // 守卫本身要能抓到 emoji（否则这个断言是假的）
    Check(hp::ui::ContainsEmoji("播放 ▶"), "守卫能识别 ▶（U+25B6 装饰符号）");
    Check(hp::ui::ContainsEmoji("完成 ✓"), "守卫能识别 ✓");
    Check(!hp::ui::ContainsEmoji("演奏中"), "中文不会被误判为 emoji");

    // 新复选框（鼠标滚轮打断演奏）：文案必须存在、是中文、且提到滚轮
    Check(hp::ui::labels::kMouseWheelGuard != nullptr &&
              std::string(hp::ui::labels::kMouseWheelGuard).find("滚轮") != std::string::npos,
          std::string("新复选框文案存在且提到滚轮：") + hp::ui::labels::kMouseWheelGuard);

    for (const auto& item : all) {
        Note(item.first + "：" + item.second);
    }

    const std::string hint = hp::ui::HotkeyHint("F6", "F5", "F7", "F8", "F9", "F10");
    Check(hint == "热键：F6 播放　F5 暂停　F7 停止　F8 重播　F9 上一首　F10 下一首（可在设置里改）",
          "热键提示：" + hint);
    Check(hint.find("F5 暂停") != std::string::npos, "提示里带上暂停热键：F5");
}

void RunBeatMathTest() {
    Section("Phase 8：BPM / 拍号 / 小节 / 时值 / 速度");

    // ---- BPM → 一拍多长（需求书二十七节：BPM = 120 → 1 拍 = 500 ms）----
    {
        hp::Song song = hp::ui::DescribeContext("测速", 120.0, hp::NoteValue::Quarter,
                                                 hp::TimeSignature{4, 4});
        CheckNear(hp::beat::BeatMs(song), 500.0, 1e-9, "120 BPM → 1 拍 = 500 ms");
        Check(hp::beat::BeatMsText(song) == "1 拍 = 500 ms",
              "文案：" + hp::beat::BeatMsText(song));
        song.bpm = 90.0;
        CheckNear(hp::beat::BeatMs(song), 666.6666667, 1e-6, "90 BPM → 1 拍 = 666.667 ms");
        song.bpm = 60.0;
        CheckNear(hp::beat::BeatMs(song), 1000.0, 1e-9, "60 BPM → 1 拍 = 1000 ms");
        song.bpm = 0.0;
        CheckNear(hp::beat::BeatMs(song), 0.0, 1e-12, "BPM 非法时不算瞎数（给 0）");
    }

    // ---- 时值 → 毫秒（全 / 二分 / 四分 / 八分 / 十六分）----
    {
        hp::Song song = hp::ui::DescribeContext("时值", 120.0, hp::NoteValue::Quarter,
                                                 hp::TimeSignature{4, 4});
        CheckNear(hp::beat::NoteDurationMs(song, hp::NoteValue::Whole), 2000.0, 1e-9,
                  "120 BPM 全音符 = 2000 ms");
        CheckNear(hp::beat::NoteDurationMs(song, hp::NoteValue::Half), 1000.0, 1e-9,
                  "二分音符 = 1000 ms");
        CheckNear(hp::beat::NoteDurationMs(song, hp::NoteValue::Quarter), 500.0, 1e-9,
                  "四分音符 = 500 ms");
        CheckNear(hp::beat::NoteDurationMs(song, hp::NoteValue::Eighth), 250.0, 1e-9,
                  "八分音符 = 250 ms");
        CheckNear(hp::beat::NoteDurationMs(song, hp::NoteValue::Sixteenth), 125.0, 1e-9,
                  "十六分音符 = 125 ms");
        Check(hp::beat::NoteFractionText(hp::NoteValue::Quarter) == "1/4", "四分音符的分数写法 1/4");
        Check(hp::beat::NoteFractionText(hp::NoteValue::Sixteenth) == "1/16",
              "十六分音符的分数写法 1/16");
        Check(hp::beat::NoteValueCellText(song, hp::NoteValue::Quarter) == "四分音符 1/4（500 ms）",
              "时值单元格：" + hp::beat::NoteValueCellText(song, hp::NoteValue::Quarter));

        // 6/8：一拍就是八分音符，所以八分 = 1 拍、四分 = 2 拍
        hp::Song jig = hp::ui::DescribeContext("吉格", 120.0, hp::NoteValue::Eighth,
                                                hp::TimeSignature{6, 8});
        CheckNear(hp::beat::BeatMs(jig), 500.0, 1e-9, "6/8 曲目 120 BPM → 1 拍（八分）= 500 ms");
        CheckNear(hp::beat::NoteDurationMs(jig, hp::NoteValue::Eighth), 500.0, 1e-9,
                  "6/8 里八分音符 = 1 拍 = 500 ms");
        CheckNear(hp::beat::NoteDurationMs(jig, hp::NoteValue::Quarter), 1000.0, 1e-9,
                  "6/8 里四分音符 = 2 拍 = 1000 ms");
    }

    // ---- 拍号（二十八节：4/4、3/4、6/8）----
    {
        hp::Song four = hp::ui::DescribeContext("四四", 120.0, hp::NoteValue::Quarter,
                                                 hp::TimeSignature{4, 4});
        CheckNear(hp::beat::BeatsPerBar(four), 4.0, 1e-12, "4/4 每小节 4 拍");
        CheckNear(hp::beat::SecondsPerBar(four), 2.0, 1e-9, "120 BPM + 4/4 → 每小节 2.000 秒");
        Check(hp::beat::SignatureText(four) == "4/4　每小节 4 拍 = 2 秒",
              "拍号描述：" + hp::beat::SignatureText(four));

        hp::Song three = hp::ui::DescribeContext("三四", 100.0, hp::NoteValue::Quarter,
                                                  hp::TimeSignature{3, 4});
        CheckNear(hp::beat::BeatsPerBar(three), 3.0, 1e-12, "3/4 每小节 3 拍");
        CheckNear(hp::beat::SecondsPerBar(three), 1.8, 1e-9, "100 BPM + 3/4 → 每小节 1.800 秒");

        hp::Song six = hp::ui::DescribeContext("六八", 120.0, hp::NoteValue::Eighth,
                                                hp::TimeSignature{6, 8});
        CheckNear(hp::beat::BeatsPerBar(six), 6.0, 1e-12, "6/8 每小节 6 拍（一拍 = 八分音符）");
        CheckNear(hp::beat::SecondsPerBar(six), 3.0, 1e-9, "120 BPM + 6/8 → 每小节 3.000 秒");
        Check(hp::beat::SignatureText(six) == "6/8　每小节 6 拍 = 3 秒",
              "拍号描述：" + hp::beat::SignatureText(six));
    }

    // ---- 小节定位（主界面“小节：3 / 8”、编辑窗口的“小节”列）----
    {
        hp::Song four = hp::ui::DescribeContext("四四", 120.0, hp::NoteValue::Quarter,
                                                 hp::TimeSignature{4, 4});
        Check(hp::beat::LocateBarByBeat(four, 0.0).bar == 1, "第 0 拍在第 1 小节");
        Check(hp::beat::LocateBarByBeat(four, 3.9).bar == 1, "第 3.9 拍还在第 1 小节");
        Check(hp::beat::LocateBarByBeat(four, 4.0).bar == 2, "第 4 拍进入第 2 小节");
        const hp::beat::BarSpot spot = hp::beat::LocateBarByBeat(four, 5.5);
        Check(spot.bar == 2 && std::fabs(spot.beatInBar - 1.5) < 1e-9,
              "第 5.5 拍 = 第 2 小节第 1.5 拍");
        const hp::beat::BarSpot byTime = hp::beat::LocateBar(four, 2.0);
        Check(byTime.bar == 2 && std::fabs(byTime.beatInBar) < 1e-9,
              "120 BPM + 4/4：第 2.000 秒正好是第 2 小节开头");
        Check(hp::beat::BarOfBeat(four, 8.0) == 3, "第 8 拍在第 3 小节");

        hp::Song six = hp::ui::DescribeContext("六八", 120.0, hp::NoteValue::Eighth,
                                                hp::TimeSignature{6, 8});
        Check(hp::beat::LocateBarByBeat(six, 5.9).bar == 1, "6/8 第 5.9 拍还在第 1 小节");
        Check(hp::beat::LocateBarByBeat(six, 6.0).bar == 2, "6/8 第 6 拍进入第 2 小节");

        // 整首小节数
        hp::Song whole = four;
        whole.notes.clear();
        for (int i = 0; i < 16; ++i) {   // 16 拍 = 4 小节（120 BPM 下每拍 0.5 秒）
            hp::Note note;
            note.key = hp::Key::Z;
            note.startTime = i * 0.5;
            note.duration = 0.5;
            whole.notes.push_back(note);
        }
        Check(hp::beat::BarCount(whole) == 4, "16 拍 = 4 小节");
        whole.notes.clear();
        Check(hp::beat::BarCount(whole) == 0, "空曲 = 0 小节");
    }

    // ---- 速度控制（二十九节：50% ~ 200%）----
    {
        CheckNear(hp::beat::SpeedPreset(0), 0.5, 1e-12, "第一档 = 50%");
        CheckNear(hp::beat::SpeedPreset(1), 0.75, 1e-12, "第二档 = 75%");
        CheckNear(hp::beat::SpeedPreset(2), 1.0, 1e-12, "第三档 = 100%");
        CheckNear(hp::beat::SpeedPreset(3), 1.25, 1e-12, "第四档 = 125%");
        CheckNear(hp::beat::SpeedPreset(4), 1.5, 1e-12, "第五档 = 150%");
        CheckNear(hp::beat::SpeedPreset(5), 2.0, 1e-12, "第六档 = 200%");
        Check(hp::beat::SpeedPresetText(0) == "50%" && hp::beat::SpeedPresetText(5) == "200%",
              "档位文案：" + hp::beat::SpeedPresetText(0) + " … " + hp::beat::SpeedPresetText(5));

        double out = 0.0;
        std::string error;
        Check(hp::beat::ClampSpeed(0.5, out, error) && std::fabs(out - 0.5) < 1e-12,
              "50% 在范围内");
        Check(hp::beat::ClampSpeed(2.0, out, error) && std::fabs(out - 2.0) < 1e-12,
              "200% 在范围内");
        const bool lowRejected = !hp::beat::ClampSpeed(0.4, out, error);
        Check(lowRejected, "40% 被拒：" + error);
        const bool highRejected = !hp::beat::ClampSpeed(2.5, out, error);
        Check(highRejected, "250% 被拒：" + error);

        hp::Song song = hp::ui::DescribeContext("测速", 120.0, hp::NoteValue::Quarter,
                                                 hp::TimeSignature{4, 4});
        CheckNear(hp::beat::EffectiveBpm(song, 2.0), 240.0, 1e-9, "120 BPM @ 200% → 有效 BPM 240");
        CheckNear(hp::beat::EffectiveBpm(song, 0.5), 60.0, 1e-9, "120 BPM @ 50% → 有效 BPM 60");

        // 界面的速度输入框走同一套范围
        double parsed = 0.0;
        std::string parseError;
        Check(hp::ui::ParseSpeedText("150%", parsed, parseError) &&
                  std::fabs(parsed - 1.5) < 1e-9,
              "速度输入 150% 通过");
        const bool p300 = !hp::ui::ParseSpeedText("300%", parsed, parseError);
        Check(p300, "速度输入 300% 被拒：" + parseError);
        const bool p40 = !hp::ui::ParseSpeedText("40%", parsed, parseError);
        Check(p40, "速度输入 40% 被拒：" + parseError);
        Check(hp::ui::SpeedPresetIndexFor(1.25) == 3, "1.25 命中第 4 档");
        Check(hp::ui::SpeedPresetIndexFor(0.9) == -1, "0.9 不在六档里 → 自定义");
    }

    // ---- 界面文案（主界面分行显示的这几行）----
    {
        hp::Song song = hp::ui::DescribeContext("文案", 120.0, hp::NoteValue::Quarter,
                                                 hp::TimeSignature{4, 4});
        Check(hp::ui::TempoText(song) == "BPM：120（1 拍 = 500 ms）",
              "BPM 行：" + hp::ui::TempoText(song));
        Check(hp::ui::SignatureLine(song) == "拍号：4/4　每小节 4 拍 = 2 秒",
              "拍号行：" + hp::ui::SignatureLine(song));
        Check(hp::ui::BarText(3, 8) == "小节：3 / 8", "小节行：" + hp::ui::BarText(3, 8));
        Check(hp::ui::BarText(0, 0) == "小节：0 / 0", "没有曲谱时：" + hp::ui::BarText(0, 0));

        // 新建曲谱窗口的点选表：8 个音的名字必须和游戏里的键位一一对应（用户给的映射：
        // Z X C V B N M , ↔ 1 2 3 4 5 6 7 i），一个错就会让整首曲子错音。
        const std::string noteNames[] = {"1", "2", "3", "4", "5", "6", "7", "i"};
        bool namesOk = hp::kKeyCount == 8;
        for (int i = 0; i < hp::kKeyCount && namesOk; ++i) {
            namesOk = hp::ui::KeyNoteName(static_cast<hp::Key>(i)) == noteNames[i];
        }
        Check(namesOk, "8 个音调按钮的名字 = 1 2 3 4 5 6 7 i");
        Check(hp::ui::KeyNoteName(hp::Key::Comma) == "i", "逗号键 = 高音 1（i）");
        Check(hp::ui::KeyNoteName(hp::Key::Z) == "1" && hp::ui::KeyNoteName(hp::Key::M) == "7",
              "Z = 1、M = 7");

        // 演奏方式按钮：6 个（4 个单键 + 2 个“八度+半音”组合），名字互不相同，
        // 并且把"左/中/右"与组合（左+中、右+中）说清楚（用户不懂左/中/右的含义）
        std::string modeNames[hp::kPlayModeCount];
        bool modesOk = hp::kPlayModeCount == 6;
        for (int i = 0; i < hp::kPlayModeCount; ++i) {
            modeNames[i] = hp::ui::PlayModeButtonName(static_cast<hp::PlayMode>(i));
            modesOk = modesOk && !modeNames[i].empty();
            for (int j = 0; j < i; ++j) {
                modesOk = modesOk && modeNames[i] != modeNames[j];
            }
        }
        Check(modesOk, "6 个演奏方式按钮名字互不相同");
        Check(modeNames[0].find("本音") != std::string::npos &&
                  modeNames[1].find("左") != std::string::npos &&
                  modeNames[2].find("中") != std::string::npos &&
                  modeNames[3].find("右") != std::string::npos,
              "演奏方式名字点明 本音/左/中/右");
        // 组合模式必须两个键的名字都出现，否则用户只会按一个键（音就不对了）
        Check(modeNames[4].find("左") != std::string::npos &&
                  modeNames[4].find("中") != std::string::npos &&
                  modeNames[5].find("右") != std::string::npos &&
                  modeNames[5].find("中") != std::string::npos,
              "组合模式名字点明 左键+中键 / 右键+中键：" + modeNames[4] + " / " + modeNames[5]);

        // 时值按钮：标准乐理（全 / 二分 / 四分 / 八分 / 十六分 + 附点 + 休止）共 10 档。
        // 名字里的拍数必须和 beats 对得上——否则用户"照名字选"就会加错时值。
        Check(hp::ui::kDurationChoiceCount == 10, "时值 10 档（标准乐理拓展后）");
        Check(hp::ui::kDurationChoices[0].beats == 4.0 && !hp::ui::kDurationChoices[0].rest &&
                  std::string(hp::ui::kDurationChoices[0].label) == "全音符 4 拍",
              "第 1 档 = 全音符 4 拍");
        Check(hp::ui::kDurationChoices[1].beats == 2.0 &&
                  std::string(hp::ui::kDurationChoices[1].label) == "二分音符 2 拍",
              "第 2 档 = 二分音符 2 拍");
        Check(hp::ui::kDurationChoices[2].beats == 1.0 && !hp::ui::kDurationChoices[2].rest &&
                  std::string(hp::ui::kDurationChoices[2].label) == "四分音符 1 拍",
              "第 3 档 = 四分音符 1 拍");
        Check(hp::ui::kDurationChoices[3].beats == 0.5 &&
                  std::string(hp::ui::kDurationChoices[3].label) == "八分音符 1/2 拍",
              "第 4 档 = 八分音符 1/2 拍");
        Check(hp::ui::kDurationChoices[4].beats == 0.25 &&
                  std::string(hp::ui::kDurationChoices[4].label) == "十六分音符 1/4 拍",
              "第 5 档 = 十六分音符 1/4 拍");
        // 附点 = 原时值 × 1.5（附点四分 = 1.5 拍、附点八分 = 0.75 拍）
        Check(hp::ui::kDurationChoices[5].beats == 1.5 && !hp::ui::kDurationChoices[5].rest &&
                  std::string(hp::ui::kDurationChoices[5].label).find("附点四分") != std::string::npos,
              "第 6 档 = 附点四分 1.5 拍（四分 1 拍 × 1.5）");
        Check(hp::ui::kDurationChoices[6].beats == 0.75 && !hp::ui::kDurationChoices[6].rest &&
                  std::string(hp::ui::kDurationChoices[6].label).find("附点八分") != std::string::npos,
              "第 7 档 = 附点八分 0.75 拍（八分 0.5 拍 × 1.5）");
        // 休止同理：至少要有 两拍 / 一拍 / 半拍 三档，且只占时间不出声
        Check(hp::ui::kDurationChoices[7].rest && hp::ui::kDurationChoices[7].beats == 2.0 &&
                  std::string(hp::ui::kDurationChoices[7].label) == "休止两拍",
              "第 8 档 = 休止两拍");
        Check(hp::ui::kDurationChoices[8].rest && hp::ui::kDurationChoices[8].beats == 1.0,
              "第 9 档 = 休止一拍");
        Check(hp::ui::kDurationChoices[9].rest && hp::ui::kDurationChoices[9].beats == 0.5,
              "第 10 档 = 休止半拍");
        int restCount = 0;
        for (int i = 0; i < hp::ui::kDurationChoiceCount; ++i) {
            if (hp::ui::kDurationChoices[i].rest) {
                ++restCount;
            }
        }
        Check(restCount == 3, "休止档三个（两拍 / 一拍 / 半拍）");
        // 标准叫法：用户明确说要用"四分、八分、十六分"这些词
        Check(std::string(hp::ui::kDurationChoices[2].label).find("四分") != std::string::npos &&
                  std::string(hp::ui::kDurationChoices[3].label).find("八分") != std::string::npos &&
                  std::string(hp::ui::kDurationChoices[4].label).find("十六分") != std::string::npos &&
                  std::string(hp::ui::kDurationChoices[0].label).find("全音符") != std::string::npos,
              "时值用标准乐理叫法（全音符 / 四分 / 八分 / 十六分）");
        // 默认档 = 四分音符：新建曲谱点一下音调按钮加的是 1 拍，
        // （数组按从长到短排，拿下标 0 当默认会变成"全音符 4 拍"，那就错了）
        Check(hp::ui::kDurationChoices[hp::ui::kDefaultDurationChoiceIndex].beats == 1.0 &&
                  !hp::ui::kDurationChoices[hp::ui::kDefaultDurationChoiceIndex].rest,
              "默认时值档 = 四分音符 1 拍");
        Check(hp::ui::WrapLabelText("短句", 40.0, 2) == "短句", "短句不折行");
        const std::string longCn = "一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十";
        const std::string wrapped3 = hp::ui::WrapLabelText(longCn, 10.0, 3);
        int newlines = 0;
        for (char ch : wrapped3) {
            if (ch == '\n') {
                ++newlines;
            }
        }
        Check(newlines == 2, "30 个中文字按 10 字一行正好折 3 行（没有多余换行）");
        Check(wrapped3.find("…") == std::string::npos, "正好放得下就不加省略号");
        const std::string tooLong = longCn + longCn;   // 60 字，3 行放不下
        const std::string wrapped60 = hp::ui::WrapLabelText(tooLong, 10.0, 3);
        int newlines60 = 0;
        for (char ch : wrapped60) {
            if (ch == '\n') {
                ++newlines60;
            }
        }
        Check(newlines60 == 2 && wrapped60.size() > 4 &&
                  wrapped60.compare(wrapped60.size() - 3, 3, "…") == 0,
              "放不下时最多 3 行并以省略号收尾：" + hp::ui::WrapLabelText(tooLong, 10.0, 3));
        Check(hp::ui::WrapLabelText(longCn, 10.0, 2).find("\n") != std::string::npos,
              "两行上限也折得开");
        // 中文折行禁则：行首不能是收尾标点（否则下一行孤零零一个"。"），行尾不能是开引号
        {
            const std::string punct = "还没有曲谱：点下面的「新建曲谱」建一首（这是很长的一句"
                                      "话，用来把标点挤到行首位置）。";
            const std::string wrapped = hp::ui::WrapLabelText(punct, 10.0, 6);
            bool badStart = false;
            bool badEnd = false;
            size_t pos = 0;
            while (pos <= wrapped.size()) {
                const size_t end = wrapped.find('\n', pos);
                const std::string line = wrapped.substr(pos, end == std::string::npos ? end : end - pos);
                if (!line.empty()) {
                    const char* closings[] = {"。", "，", "、", "）", "」", "！", "？", "；", "："};
                    for (const char* mark : closings) {
                        if (line.compare(0, std::string(mark).size(), mark) == 0) {
                            badStart = true;
                        }
                    }
                }
                if (end == std::string::npos) {
                    break;
                }
                pos = end + 1;
            }
            Check(!badStart, "折行禁则：没有一行以收尾标点开头（" + wrapped + "）");
            // 行尾不能是开引号 / 开括号（"（"「"等被留在上一行会很难看）
            const std::string opens = "abcdefghij（很长的一句话";
            const std::string wrapped2 = hp::ui::WrapLabelText(opens, 8.0, 5);
            size_t p2 = 0;
            while (true) {
                const size_t e2 = wrapped2.find('\n', p2);
                const std::string line = wrapped2.substr(p2, e2 == std::string::npos ? e2 : e2 - p2);
                if (!line.empty()) {
                    const char* tails[] = {"（", "「", "『", "【", "《"};
                    for (const char* mark : tails) {
                        if (line.size() >= std::string(mark).size() &&
                            line.compare(line.size() - std::string(mark).size(),
                                         std::string(mark).size(), mark) == 0) {
                            badEnd = true;
                        }
                    }
                }
                if (e2 == std::string::npos) {
                    break;
                }
                p2 = e2 + 1;
            }
            Check(!badEnd, "折行禁则：没有一行以开引号结尾（" + wrapped2 + "）");
        }
        Check(hp::ui::WrapLabelText(longCn, 10.0, 2).size() < longCn.size(),
              "折到两行上限会截短（不会越折越长）");
        Check(hp::ui::WrapLabelText("", 10.0, 2).empty(), "空串还是空串");

        // 休止行：进编辑器表格、存进 Song::rests、后面的音符跟着挪、读回来还是休止行
        std::vector<hp::ui::EditorRow> restRows;
        hp::ui::EditorRow first;
        first.key = hp::Key::Z;
        first.beat = 0.0;
        first.durationBeats = 1.0;
        restRows.push_back(first);
        hp::ui::EditorRow middle;
        middle.rest = true;
        middle.beat = 1.0;
        middle.durationBeats = 1.0;
        restRows.push_back(middle);
        hp::ui::EditorRow last;
        last.key = hp::Key::C;
        last.beat = 2.0;
        last.durationBeats = 1.0;
        restRows.push_back(last);
        const hp::Song restSong = hp::ui::BuildSongFromRows(
            "休止测试", 120.0, hp::NoteValue::Quarter, hp::TimeSignature{4, 4}, restRows);
        Check(restSong.notes.size() == 2 && restSong.rests.size() == 1,
              "休止不进演奏列表，单独记在 rests 里");
        Check(std::fabs(restSong.notes[1].startTime - 1.0) < 1e-9,
              "休止后面的音符从第 2 拍（1 秒）开始");
        const std::vector<hp::ui::EditorRow> backRows = hp::ui::BuildEditorRows(restSong);
        Check(backRows.size() == 3 && backRows[1].rest, "读回来还是三行、中间那行是休止");
        const hp::ui::EditorRowText restText = hp::ui::DescribeRow(backRows[1], restSong);
        Check(restText.key == "休止" && restText.mode == "—",
              "休止行的音调列写休止、演奏方式列写—：" + restText.key + "/" + restText.mode);
        Check(hp::ui::DescribeRow(backRows[0], restSong).key != "休止",
              "普通音符行不受影响：" + hp::ui::DescribeRow(backRows[0], restSong).key);
        // 半角按 0.55 个字宽算：宽度 11 个字 ≈ 20 个 ASCII 字符；单行放不下就截断加省略号
        const std::string ascii = hp::ui::WrapLabelText("abcdefghijklmnopqrst", 11.0, 1);
        Check(ascii.compare(ascii.size() - 3, 3, "…") == 0 && ascii.compare(0, 5, "abcde") == 0 &&
                  ascii.find("abcdefghijklmnopqrs") == std::string::npos,
              "半角单行截断到不超宽并以省略号收尾：" + ascii);

        Check(hp::ui::DurationChoiceIndexFor(4.0) == 0, "4 拍命中全音符");
        Check(hp::ui::DurationChoiceIndexFor(2.0) == 1, "2 拍命中二分音符");
        Check(hp::ui::DurationChoiceIndexFor(1.0) == 2, "1 拍命中四分音符");
        Check(hp::ui::DurationChoiceIndexFor(0.5) == 3, "0.5 拍命中八分音符");
        Check(hp::ui::DurationChoiceIndexFor(0.25) == 4, "0.25 拍命中十六分音符");
        Check(hp::ui::DurationChoiceIndexFor(1.5) == 5, "1.5 拍命中附点四分");
        Check(hp::ui::DurationChoiceIndexFor(0.75) == 6, "0.75 拍命中附点八分");
        Check(hp::ui::DurationChoiceIndexFor(1.2) == -1, "1.2 拍不属于任何一档");
        // 休止档不能顶替音符档：同样拍数必须命中音符那一档（否则改时值会把音符变成休止）
        Check(!hp::ui::kDurationChoices[hp::ui::DurationChoiceIndexFor(1.0)].rest &&
                  !hp::ui::kDurationChoices[hp::ui::DurationChoiceIndexFor(2.0)].rest &&
                  !hp::ui::kDurationChoices[hp::ui::DurationChoiceIndexFor(0.5)].rest,
              "1 / 2 / 0.5 拍优先命中音符档（休止档不顶替）");
        Check(hp::ui::DurationUserCell(2.0, 120.0) == "二分音符 2 拍（1 秒）",
              "用户视角时值：二分音符 2 拍 = 1 秒（120 BPM）");
        Check(hp::ui::DurationUserCell(1.0, 60.0) == "四分音符 1 拍（1 秒）", "60 BPM 下一拍 = 1 秒");
        Check(hp::ui::DurationUserCell(0.5, 120.0) == "八分音符 1/2 拍（0.25 秒）",
              "八分音符 = 0.25 秒");
        Check(hp::ui::DurationUserCell(1.5, 120.0) == "附点四分 1.5 拍（0.75 秒）",
              "附点四分：标准叫法 + 秒数");
        Check(hp::ui::DurationUserCell(1.2, 120.0) == "1.2 拍（0.6 秒）",
              "不在档位里的时值也用用户看得懂的说法");

        // ---- 「延时」（简谱的 -）：把上一个音符延长一拍 ----
        {
            std::vector<hp::ui::EditorRow> extendRows;
            hp::ui::EditorRow firstRow;
            firstRow.key = hp::Key::Z;
            firstRow.beat = 0.0;
            firstRow.durationBeats = 1.0;
            extendRows.push_back(firstRow);
            double cursor = 1.0;
            const int extended = hp::ui::ExtendLastRow(extendRows, &cursor);
            Check(extended == 0 && extendRows.size() == 1,
                  "延时作用在最后一个音符上，且**不新增行**（模型里仍是一个音符）");
            Check(std::fabs(extendRows[0].durationBeats - 2.0) < 1e-9,
                  "延时把时值 1 拍 → 2 拍");
            Check(std::fabs(cursor - 2.0) < 1e-9, "延时的续写位置跟到第 2 拍（下一个音符接在它后面）");
            const hp::Song extendedSong = hp::ui::BuildSongFromRows(
                "延时测试", 120.0, hp::NoteValue::Quarter, hp::TimeSignature{4, 4}, extendRows);
            Check(extendedSong.notes.size() == 1 &&
                      std::fabs(extendedSong.notes[0].duration - 1.0) < 1e-9,
                  "延时后存进曲谱的仍是一个音符、时长 1.000 秒（2 拍 @120 BPM）");
            hp::ui::ExtendLastRow(extendRows, &cursor);
            Check(std::fabs(extendRows[0].durationBeats - 3.0) < 1e-9 &&
                      std::fabs(cursor - 3.0) < 1e-9,
                  "连按两次延时 = 延长两拍（3 拍 = 简谱的两个 -）");
            std::vector<hp::ui::EditorRow> emptyRows;
            Check(hp::ui::ExtendLastRow(emptyRows, nullptr) == -1,
                  "空表按延时返回 -1（界面据此提示先加音符，不静默什么都不做）");
            // 旧曲谱兼容：延时只是改浮点拍数，JSON 里没有新字段
            const std::string json = hp::SongToJsonText(extendedSong, 4);
            hp::Song reloaded;
            const hp::SongIoResult js = hp::ParseSongFromJsonText(json, reloaded);
            Check(js.ok && reloaded.notes.size() == 1 &&
                      std::fabs(reloaded.notes[0].duration - 1.0) < 1e-9,
                  "延时出来的曲谱按原来的 JSON 格式存/读都正常（没有新增字段）");
        }
        Check(hp::ui::ModeText(false, 3.0) == "模式：精确", "精确模式：" + hp::ui::ModeText(false, 3.0));
        Check(hp::ui::ModeText(true, 3.0) == "模式：自然（抖动 3 ms 以内）",
              "自然模式：" + hp::ui::ModeText(true, 3.0));
    }
}

// ---------------------------------------------------------------------------
// 9. 按钮文字实测宽度（防截断）
//
// 为什么要有这一组：新建/编辑曲谱窗口"演奏方式"第二排的长按钮
// （按住左键+中键（降调+半音））原来按等分算宽度，中文比算出来的宽，左端被切掉一个"按"字。
// 修法是把宽度改成"用内存 DC + GetTextExtentPoint32W 按按钮实际字体量文字宽 + 内边距"，
// 这里就用**同一套函数**（窗口排版用的就是它们）把每个按钮量一遍并断言放得下——
// 以后再改文案（例如时值从 4 档扩到 10 档）会自动被拦住。
// ---------------------------------------------------------------------------
void CheckButtonFit(const std::string& group, const std::vector<std::string>& utf8Labels,
                    HFONT font, int available, const std::vector<int>& forceNewRowAt = {}) {
    std::vector<std::wstring> labels;
    labels.reserve(utf8Labels.size());
    for (const std::string& text : utf8Labels) {
        labels.push_back(hp::ui::Utf8ToWide(text));
    }
    const hp::ui::ButtonRowPlan plan = hp::ui::PlanButtonRows(labels, available, forceNewRowAt);
    const int padding = hp::ui::ButtonTextPadding();

    int clipped = 0;
    int widestText = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        const int textWidth = hp::ui::MeasureTextWidth(font, labels[i]);
        widestText = std::max(widestText, textWidth);
        // 核心断言：文字宽 + 左右内边距 ≤ 按钮宽度（否则文字会被截）
        if (textWidth + padding > plan.widths[i]) {
            ++clipped;
            Check(false, group + "[" + std::to_string(i) + "]「" + utf8Labels[i] +
                             "」放不下：文字宽 " + std::to_string(textWidth) + " + 内边距 " +
                             std::to_string(padding) + " > 按钮宽 " +
                             std::to_string(plan.widths[i]));
        } else {
            Note(group + "[" + std::to_string(i) + "] 文字宽 " + std::to_string(textWidth) +
                 " px，按钮宽 " + std::to_string(plan.widths[i]) + " px（内边距 " +
                 std::to_string(padding) + " px）：" + utf8Labels[i]);
        }
    }
    Check(clipped == 0, group + "：全部 " + std::to_string(labels.size()) +
                            " 个按钮都是「文字宽 + 内边距 ≤ 按钮宽」（会截断的 " +
                            std::to_string(clipped) + " 个，最宽文字 " + std::to_string(widestText) +
                            " px）");
    Check(!plan.overflow, group + "：没有单个按钮本身就比可用宽度还宽");
    Check(plan.rowCount >= 1, group + "：排成了 " + std::to_string(plan.rowCount) + " 行");

    // 每一行的按钮宽度之和（含间隙）不能超过可用宽度——否则按钮会跑到窗口外面
    const int gap = hp::ui::S(8);
    std::vector<int> rowWidth(static_cast<size_t>(plan.rowCount), 0);
    std::vector<int> rowCount(static_cast<size_t>(plan.rowCount), 0);
    for (size_t i = 0; i < plan.widths.size(); ++i) {
        const size_t row = static_cast<size_t>(plan.rows[i]);
        rowWidth[row] += plan.widths[i];
        ++rowCount[row];
    }
    int overflowRows = 0;
    for (size_t row = 0; row < rowWidth.size(); ++row) {
        const int total = rowWidth[row] + gap * (rowCount[row] - 1);
        if (total > available) {
            ++overflowRows;
            Check(false, group + "第 " + std::to_string(row + 1) + " 行溢出：" +
                             std::to_string(total) + " px > 可用 " + std::to_string(available) + " px");
        } else {
            Note(group + "第 " + std::to_string(row + 1) + " 行：" + std::to_string(rowCount[row]) +
                 " 个按钮共占 " + std::to_string(total) + " px（可用 " + std::to_string(available) + " px）");
        }
    }
    Check(overflowRows == 0, group + "：每一行都没超过可用宽度");
}

void RunButtonFitTest() {
    Section("按钮文字实测宽度（新建/编辑曲谱窗口：文字宽 + 内边距 ≤ 按钮宽）");

    hp::ui::SetUiDpi(96);
    hp::ui::RebuildUiFonts();
    const HFONT body = hp::ui::UiFonts().body;
    Check(body != nullptr, "正文按钮字体已建好（测量必须按按钮实际字体算）");

    const int clientWidth = hp::ui::S(hp::ui::kNewSongWindowWidth);
    const int available = hp::ui::NewSongButtonsAvailableWidth(clientWidth);
    Note("客户区宽 " + std::to_string(clientWidth) + " px，按钮区可用宽 " + std::to_string(available) +
         " px，左右内边距合计 " + std::to_string(hp::ui::ButtonTextPadding()) + " px");
    Check(available > 0 && available < clientWidth, "可用宽度是从客户区里算出来的");

    // ---- 演奏方式 6 个（含被截过的那两个组合模式）----
    std::vector<std::string> modeLabels;
    for (int i = 0; i < hp::kPlayModeCount; ++i) {
        modeLabels.push_back(hp::ui::PlayModeButtonName(static_cast<hp::PlayMode>(i)));
    }
    CheckButtonFit("演奏方式", modeLabels, body, available,
                   {static_cast<int>(hp::PlayMode::LeftMiddle)});
    // 组合模式那一对必须真的排在同一行（第二排），不要被拆散
    {
        const hp::ui::ButtonRowPlan plan =
            hp::ui::PlanButtonRows({hp::ui::Utf8ToWide(modeLabels[4]), hp::ui::Utf8ToWide(modeLabels[5])},
                                   available, {});
        Check(plan.rowCount == 1,
              "两个组合模式按钮同行排下（" + std::to_string(plan.widths[0] + hp::ui::S(8) +
                                                          plan.widths[1]) +
                  " px ≤ 可用 " + std::to_string(available) + " px）");
    }
    // 那两个长按钮必须真的被量到"要比 96 px 宽"，否则说明测量没生效（原来写死 96 就截了字）
    const std::wstring longMode = hp::ui::Utf8ToWide(modeLabels[4]);
    Check(hp::ui::MeasureTextWidth(body, longMode) + hp::ui::ButtonTextPadding() >
              hp::ui::S(96),
          "「" + modeLabels[4] + "」实测要 > 96 px（原来写死 96 px 就是截字的原因）");

    // ---- 时值 10 档 + 延时（窗口把「延时」排在时值按钮后面，同一套排版）----
    std::vector<std::string> durationLabels;
    for (int i = 0; i < hp::ui::kDurationChoiceCount; ++i) {
        durationLabels.push_back(hp::ui::kDurationChoices[i].label);
    }
    durationLabels.push_back(hp::ui::labels::kExtendAction);
    CheckButtonFit("时值+延时", durationLabels, body, available);
    {
        // 「延时」必须是排得下的最后一个：文字宽 + 内边距 ≤ 它的按钮宽
        const std::wstring extendText = hp::ui::Utf8ToWide(hp::ui::labels::kExtendAction);
        const hp::ui::ButtonRowPlan plan = hp::ui::PlanButtonRows(
            {extendText}, available, {});
        const int textWidth = hp::ui::MeasureTextWidth(body, extendText);
        Check(textWidth + hp::ui::ButtonTextPadding() <= plan.widths[0],
              "延时按钮：文字宽 " + std::to_string(textWidth) + " + 内边距 " +
                  std::to_string(hp::ui::ButtonTextPadding()) + " ≤ 按钮宽 " +
                  std::to_string(plan.widths[0]));
    }

    // ---- 时值列的表格宽度：最长的时值文字要能显示下 ----
    int widestCell = 0;
    std::string widestText;
    for (int i = 0; i < hp::ui::kDurationChoiceCount; ++i) {
        const std::string cell = hp::ui::DurationUserCell(hp::ui::kDurationChoices[i].beats, 120.0);
        const int px = hp::ui::MeasureTextWidth(body, hp::ui::Utf8ToWide(cell));
        if (px > widestCell) {
            widestCell = px;
            widestText = cell;
        }
    }
    Note("最宽的时值单元格文字：" + widestText + " = " + std::to_string(widestCell) + " px");
    Check(widestCell + hp::ui::S(24) <= hp::ui::S(200) + hp::ui::S(24),
          "时值列按实测最宽文字定宽（" + std::to_string(widestCell) + " px + 内边距）：" + widestText);
}

void PrintList() {
    Note("界面逻辑自检（无窗口）覆盖：");
    Note("--format     数字与时间格式（秒/时钟/拍/百分比/进度条）");
    Note("--status     状态文案（演奏中/已暂停/已停止+原因）");
    Note("--speed      速度输入解析与拒绝");
    Note("--table      音符表四列 + 时值名换算");
    Note("--settings   设置默认值、前台窗口策略、BPM 校验");
    Note("--roundtrip  编辑表格 ↔ 绝对时间轴 ↔ JSON 文本 往返");
    Note("--hotkey     设置窗口热键文本框解析");
    Note("--labels     界面文案（中文、无 emoji）");
    Note("--buttons    按钮文字实测宽度（文字宽 + 内边距 ≤ 按钮宽，防截断）");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    const std::vector<std::string> args = Utf8Arguments();

    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "游戏口琴自动演奏工具 —— Phase 7 / 8 自检（界面逻辑，无窗口）");
    Note("界面里所有数字与文案的换算都在这里自动验证；窗口本身由 ui-e2e.sh 用 UIA 控件树核对");

    const bool runAll = args.size() <= 1 || HasFlag(args, "--all");
    if (HasFlag(args, "--help") || HasFlag(args, "-h")) {
        PrintList();
        return 0;
    }
    if (HasFlag(args, "--list")) {
        PrintList();
        return 0;
    }

    if (runAll || HasFlag(args, "--format")) {
        RunFormatTest();
    }
    if (runAll || HasFlag(args, "--status")) {
        RunStatusTest();
    }
    if (runAll || HasFlag(args, "--speed")) {
        RunSpeedTest();
    }
    if (runAll || HasFlag(args, "--table")) {
        RunTableTest();
    }
    if (runAll || HasFlag(args, "--settings")) {
        RunSettingsTest();
    }
    if (runAll || HasFlag(args, "--roundtrip")) {
        RunRoundTripTest();
    }
    if (runAll || HasFlag(args, "--hotkey")) {
        RunHotkeyTextTest();
    }
    if (runAll || HasFlag(args, "--labels")) {
        RunLabelTest();
    }
    if (runAll || HasFlag(args, "--beats")) {
        RunBeatMathTest();
    }
    if (runAll || HasFlag(args, "--buttons")) {
        RunButtonFitTest();
    }

    hp::LogBlank();
    hp::LogLine(hp::LogLevel::Info, "本次测试：通过 " + std::to_string(g_passed) + " 项，失败 " +
                                        std::to_string(g_failed) + " 项。");
    return (g_failed == 0) ? 0 : 1;
}
