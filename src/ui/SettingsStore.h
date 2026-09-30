#pragma once

// 配置保存（Phase 11“配置保存”项）。
//
// 文件位置：%APPDATA%\HarmonicaPlayer\settings.json
//   - 拿不到 APPDATA 时退回当前目录（并如实说明，不假装成功）。
//
// 读取原则（Phase 11“异常处理”项）：**任何坏文件都不能让程序起不来**。
//   缺文件 / 空文件 / 坏 JSON / 字段类型不对 / 值超范围 → 一律退回默认值并把原因
//   写进 warnings，绝不抛异常、绝不半途而废。
// 写入原则：临时文件 + 替换，避免写一半断电留下半个文件；写失败返回 false 带原因。
//
// 约定：字段顺序固定（用 ordered_json），中文提示，无 emoji。

#include <string>
#include <vector>

#include "ui/UiModel.h"

namespace hp {
namespace ui {

struct SettingsLoadResult {
    bool ok = false;                       // 是否成功读到可用配置（ok == false 时用的是 defaultSettings）
    bool fileExists = false;               // 文件是否存在
    AppSettings settings;                  // 结果（失败时等于传入的默认值）
    std::vector<std::string> warnings;     // 读取过程中的问题（中文，可直接显示）
    std::string error;                     // 致命原因（ok == false 时非空）
};

// %APPDATA%\HarmonicaPlayer\settings.json；环境变量缺失时返回 "settings.json"
std::string DefaultSettingsPath();

// 读取；文件不存在不算错误（ok=false、fileExists=false、warning 说明"还没有配置文件"）
SettingsLoadResult LoadSettings(const AppSettings& defaults, const std::string& path);

// 保存；成功返回 true
bool SaveSettings(const AppSettings& settings, const std::string& path, std::string& error);

// 序列化 / 反序列化（自检直接测这两个，不用碰磁盘）
std::string SettingsToJson(const AppSettings& settings);
SettingsLoadResult SettingsFromJson(const AppSettings& defaults, const std::string& json);

}  // namespace ui
}  // namespace hp
