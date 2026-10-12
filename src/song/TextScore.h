#pragma once

// TextScore：文本曲谱（需求书三十三节 + Phase 9）。
//
// 格式：一行一个音符，`[演奏方式+]键名 [时值]`
//
//   Z                → Normal + Z，时值默认 1/4（一拍）
//   X                → Normal + X
//   L+V              → Left + V
//   R+N              → Right + N
//   MIDDLE+,         → Middle + ,（逗号键）
//   LM+V             → LeftMiddle + V（同时按住左键 + 中键）
//   RM+B             → RightMiddle + B（同时按住右键 + 中键）
//   左键+中键+V      → 同上（中文别名里带 '+' 也认：从最后一个 '+' 切开）
//   C 1/8            → Normal + C，八分音符
//   B 1/2            → Normal + B，二分音符
//
// 时值写法走 NoteValue 的通用解析：1/4、四分、四分音符、QUARTER、4 都认。
// 演奏方式缩写：N/NORMAL/正常、L/LEFT/左键、M/MIDDLE/中键、R/RIGHT/右键、
//   LM/LEFTMIDDLE/左中、RM/RIGHTMIDDLE/右中、数字 1..6。
//   （注意：只有写成 `M+` 时 M 才表示中键；单独一个 `M` 是音调 M —— 与
//    ParsePlayMode 的约定一致，那边故意不认单字母 M。）
//
// 空行、以 # 或 // 开头的整行注释、行尾注释都会被跳过。
// 时间轴由时值累加算出：第 i 个音符的起音 = 前 i 个音符的时值之和。
// 文本谱**不表示休止符**（没有空拍语法），也不写 BPM / 拍号 —— 这些由曲头提供。

#include <string>
#include <vector>

#include "song/Song.h"

namespace hp {
namespace text_score {

// 出错的一行（行号从 1 开始，按文本原文行号，含空行与注释行）
struct TextScoreIssue {
    int line = 0;
    std::string text;      // 该行原文（已去掉注释与首尾空白）
    std::string message;   // 中文原因
};

struct TextScoreResult {
    bool ok = false;
    Song song;                             // ok == true 时有效
    std::vector<TextScoreIssue> issues;    // 出错的行（最多留 24 条，其余合并计数）
    int noteLines = 0;                     // 成功解析的音符行数
    int skippedLines = 0;                  // 跳过的空行 / 注释行
    int totalLines = 0;                    // 文本总行数
    double totalBeats = 0.0;                // 累计拍数（以曲谱拍单位计）
    int moreIssues = 0;                    // 超过 24 条之后的错误条数

    // 给界面直接显示的一句话（成功或失败都能用）
    std::string Summary() const;
    // 失败时的多行详情（含行号与原文），行数已限流
    std::string IssueText() const;
};

// 解析文本谱；header 提供曲名 / BPM / 拍单位 / 拍号，notes 会被替换
TextScoreResult ParseTextScore(const std::string& text, const Song& header);

// 反向输出文本谱（供往返自检与"导出文本谱"）。
// 文本谱只支持标准时值、且不表示休止，所以遇到这两种情况会往 inexactReason 写原因，
// 但只要没写原因，ParseTextScore(SongToTextScore(song)) 必须与 song 完全一致。
std::string SongToTextScore(const Song& song, std::string* inexactReason = nullptr);

// 时值的分数写法统一用 beat::NoteFractionText（src/song/BeatMath.h），这里不再重复实现。

}  // namespace text_score
}  // namespace hp
