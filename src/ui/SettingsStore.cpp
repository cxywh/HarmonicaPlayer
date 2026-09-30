#include "ui/SettingsStore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Log.h"
#include "song/BeatMath.h"
#include "song/SongSerializer.h"   // ExeDirectory()：设置/日志要跟 exe 放一起
#include "ui/UiFormat.h"

#include "nlohmann/json.hpp"

namespace hp {
namespace ui {

// 真正干活的实现（外面包一层异常护栏，见 SaveSettings）；放命名空间作用域，
// 不能塞进下面的匿名命名空间，否则链接不上（踩过）
bool SaveSettingsUnlocked(const AppSettings& settings, const std::string& path, std::string& error);

namespace {

using Json = nlohmann::ordered_json;

constexpr int kSettingsVersion = 1;

// 读取容错：类型不对就退回默认并把原因记下来
// 字符串数组（目前只有"排除键位"用）：类型不对就用默认值，坏项跳过并记警告
void ReadStringArray(const Json& root, const char* key, std::vector<std::string>& target,
                     const std::vector<std::string>& fallback,
                     std::vector<std::string>& warnings) {
    target = fallback;
    const auto found = root.find(key);
    if (found == root.end()) {
        return;
    }
    if (!found->is_array()) {
        warnings.push_back(std::string("配置项 ") + key + " 类型不对，已用默认值");
        return;
    }
    target.clear();
    for (const Json& item : *found) {
        if (item.is_string()) {
            target.push_back(item.get<std::string>());
        } else {
            warnings.push_back(std::string("配置项 ") + key + " 里有一项不是字符串，已跳过");
        }
    }
}

template <typename T>
void ReadField(const Json& root, const char* key, T& target, const T& fallback,
               std::vector<std::string>& warnings) {
    const auto found = root.find(key);
    if (found == root.end()) {
        target = fallback;
        return;
    }
    if (!found->is_boolean() && !found->is_number() && !found->is_string()) {
        warnings.push_back(std::string("配置项 ") + key + " 类型不对，已用默认值");
        target = fallback;
        return;
    }
    try {
        target = found->get<T>();
    } catch (const std::exception&) {
        warnings.push_back(std::string("配置项 ") + key + " 读不出来，已用默认值");
        target = fallback;
    }
}

std::string TrimAscii(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

// 读环境变量：MSVC 下走 _dupenv_s，避免 C4996（本项目要求 0 警告）
std::string AppDataDirectory() {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, "APPDATA") != 0 || buffer == nullptr) {
        return std::string();
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv("APPDATA");
    return (value != nullptr) ? std::string(value) : std::string();
#endif
}

// ---- 兜底小工具：判存在 / 判可写（真建一个临时文件试，比查权限位可靠）----
bool FileExists(const std::string& path) {
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

bool DirectoryIsWritable(const std::string& directory) {
    const std::string probe = directory + "hp_write_probe.tmp";
    std::FILE* file = nullptr;
    if (fopen_s(&file, probe.c_str(), "wb") != 0 || file == nullptr) {
        return false;
    }
    std::fclose(file);
    std::remove(probe.c_str());   // 探针自己清掉，别在用户目录里留垃圾
    return true;
}

// 老版本的设置放在 %APPDATA%\HarmonicaPlayer\settings.json：第一次搬到程序目录，
// 免得升级后用户"设置丢了"。原件保留（程序目录被清掉还能找回来）。
void MigrateLegacySettings(const std::string& target) {
    if (FileExists(target)) {
        return;
    }
    const std::string appData = AppDataDirectory();
    if (appData.empty()) {
        return;
    }
    std::string legacy = appData;
    if (legacy.back() != '\\' && legacy.back() != '/') {
        legacy += '\\';
    }
    legacy += "HarmonicaPlayer\\settings.json";
    if (!FileExists(legacy)) {
        return;
    }
    std::ifstream in(legacy, std::ios::binary);
    std::ofstream out(target, std::ios::binary);
    out << in.rdbuf();
    out.flush();
}

std::string DefaultSettingsPath() {
    // 1) 优先跟 exe 放一起：自带、看得见、好备份；被删了会自己重建
    const std::string exeDirectory = ExeDirectory();
    if (!exeDirectory.empty() && DirectoryIsWritable(exeDirectory)) {
        const std::string path = exeDirectory + "settings.json";
        MigrateLegacySettings(path);
        return path;
    }
    // 2) 兜底：程序目录不可写（例如装在 Program Files）→ 退回用户目录
    const std::string path0 = AppDataDirectory();
    if (path0.empty()) {
        return "settings.json";   // 拿不到就落当前目录，不假装写到了用户目录
    }
    std::string path = path0;
    if (path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    path += "HarmonicaPlayer";
    path += "\\settings.json";   // 目录不存在时由 SaveSettings 建
    return path;
}

std::string SettingsToJson(const AppSettings& settings) {
    Json root;
    root["version"] = kSettingsVersion;
    root["playHotkey"] = settings.playHotkey;
    root["pauseHotkey"] = settings.pauseHotkey;
    root["stopHotkey"] = settings.stopHotkey;
    root["restartHotkey"] = settings.restartHotkey;
    root["previousHotkey"] = settings.previousHotkey;
    root["nextHotkey"] = settings.nextHotkey;
    root["speed"] = settings.speed;
    root["dryRun"] = settings.dryRun;
    root["naturalMode"] = settings.naturalMode;
    root["jitterMs"] = settings.jitterMs;
    root["monitorKeyboard"] = settings.monitorKeyboard;
    root["monitorMouseButtons"] = settings.monitorMouseButtons;
    root["monitorMouseMove"] = settings.monitorMouseMove;
    root["foregroundPolicy"] = ForegroundPolicyJsonName(settings.foregroundPolicy);
    root["runAsAdmin"] = settings.runAsAdmin;
    root["exclusiveHotkeys"] = settings.exclusiveHotkeys;
    root["songPath"] = settings.songPath;
    root["ignoredKeys"] = settings.ignoredKeys;   // 数组：排除键位
    return root.dump(2) + "\n";
}

SettingsLoadResult SettingsFromJson(const AppSettings& defaults, const std::string& json) {
    SettingsLoadResult result;
    result.settings = defaults;

    const std::string trimmed = TrimAscii(json);
    if (trimmed.empty()) {
        result.error = "配置文件是空的，已用默认设置";
        result.warnings.push_back(result.error);
        return result;
    }

    Json root;
    try {
        root = Json::parse(trimmed);
    } catch (const std::exception& exception) {
        result.error = std::string("配置文件不是合法 JSON（") + exception.what() + "），已用默认设置";
        result.warnings.push_back(result.error);
        return result;
    }
    if (!root.is_object()) {
        result.error = "配置文件顶层不是对象，已用默认设置";
        result.warnings.push_back(result.error);
        return result;
    }

    AppSettings loaded = defaults;
    ReadField(root, "playHotkey", loaded.playHotkey, defaults.playHotkey, result.warnings);
    ReadField(root, "pauseHotkey", loaded.pauseHotkey, defaults.pauseHotkey, result.warnings);
    ReadField(root, "stopHotkey", loaded.stopHotkey, defaults.stopHotkey, result.warnings);
    ReadField(root, "restartHotkey", loaded.restartHotkey, defaults.restartHotkey, result.warnings);
    ReadField(root, "previousHotkey", loaded.previousHotkey, defaults.previousHotkey, result.warnings);
    ReadField(root, "nextHotkey", loaded.nextHotkey, defaults.nextHotkey, result.warnings);
    ReadStringArray(root, "ignoredKeys", loaded.ignoredKeys, defaults.ignoredKeys, result.warnings);
    ReadField(root, "speed", loaded.speed, defaults.speed, result.warnings);
    ReadField(root, "dryRun", loaded.dryRun, defaults.dryRun, result.warnings);
    ReadField(root, "naturalMode", loaded.naturalMode, defaults.naturalMode, result.warnings);
    ReadField(root, "jitterMs", loaded.jitterMs, defaults.jitterMs, result.warnings);
    ReadField(root, "monitorKeyboard", loaded.monitorKeyboard, defaults.monitorKeyboard,
              result.warnings);
    ReadField(root, "monitorMouseButtons", loaded.monitorMouseButtons,
              defaults.monitorMouseButtons, result.warnings);
    ReadField(root, "monitorMouseMove", loaded.monitorMouseMove, defaults.monitorMouseMove,
              result.warnings);
    ReadField(root, "songPath", loaded.songPath, defaults.songPath, result.warnings);

    const auto policy = root.find("foregroundPolicy");
    if (policy == root.end()) {
        loaded.foregroundPolicy = defaults.foregroundPolicy;
    } else if (policy->is_string()) {
        ForegroundPolicy parsed = defaults.foregroundPolicy;
        if (ParseForegroundPolicy(policy->get<std::string>(), parsed)) {
            loaded.foregroundPolicy = parsed;
        } else {
            result.warnings.push_back("配置项 foregroundPolicy 认不出来（" +
                                      policy->get<std::string>() + "），已用默认值");
            loaded.foregroundPolicy = defaults.foregroundPolicy;
        }
    } else {
        result.warnings.push_back("配置项 foregroundPolicy 类型不对，已用默认值");
        loaded.foregroundPolicy = defaults.foregroundPolicy;
    }

    const auto runAsAdmin = root.find("runAsAdmin");
    if (runAsAdmin == root.end()) {
        loaded.runAsAdmin = defaults.runAsAdmin;
    } else if (runAsAdmin->is_boolean()) {
        loaded.runAsAdmin = runAsAdmin->get<bool>();
    }
    const auto exclusiveHotkeys = root.find("exclusiveHotkeys");
    if (exclusiveHotkeys == root.end()) {
        loaded.exclusiveHotkeys = defaults.exclusiveHotkeys;
    } else if (exclusiveHotkeys->is_boolean()) {
        loaded.exclusiveHotkeys = exclusiveHotkeys->get<bool>();
    } else {
        result.warnings.push_back("配置项 runAsAdmin 类型不对，已用默认值");
        loaded.runAsAdmin = defaults.runAsAdmin;
    }

    // 值域校验走与界面同一条路（速度 50%~200%）
    double speed = loaded.speed;
    std::string speedError;
    if (!beat::ClampSpeed(loaded.speed, speed, speedError)) {
        // 超出 50%~200% 时收到边界（比"悄悄回默认"更容易让用户看明白），说明原因
        const double bound = (loaded.speed > beat::kSpeedMax) ? beat::kSpeedMax : beat::kSpeedMin;
        result.warnings.push_back("配置里的速度 " + std::to_string(loaded.speed) +
                                  " 超出允许范围，已收到 " + FormatSpeedText(bound));
        loaded.speed = bound;
    } else if (speed != loaded.speed) {
        result.warnings.push_back("配置里的速度超出 50%~200%，已收到 " + FormatSpeedText(speed));
        loaded.speed = speed;
    }
    if (loaded.jitterMs < 0.0 || loaded.jitterMs > 50.0) {
        result.warnings.push_back("配置里的抖动 " + std::to_string(loaded.jitterMs) +
                                  " ms 超出 0~50，已用默认值");
        loaded.jitterMs = defaults.jitterMs;
    }
    // 五个动作的热键两两不许撞车（撞了的整组回退默认值：宁可用默认，也不要一按两动作）
    {
        const std::string keys[] = {loaded.playHotkey,  loaded.pauseHotkey, loaded.stopHotkey,
                                    loaded.restartHotkey, loaded.previousHotkey, loaded.nextHotkey};
        const std::string fallbacks[] = {defaults.playHotkey,  defaults.pauseHotkey,
                                         defaults.stopHotkey,  defaults.restartHotkey,
                                         defaults.previousHotkey, defaults.nextHotkey};
        bool clash = false;
        for (int i = 0; i < 6 && !clash; ++i) {
            if (keys[i].empty()) {
                continue;   // 空 = 未绑定，允许（也不和别的撞）
            }
            for (int j = i + 1; j < 6; ++j) {
                if (!keys[j].empty() && keys[i] == keys[j]) {
                    clash = true;
                    break;
                }
            }
        }
        if (clash) {
            result.warnings.push_back("配置里有两个动作共用了同一把热键，已全部用默认值");
            loaded.playHotkey = fallbacks[0];
            loaded.pauseHotkey = fallbacks[1];
            loaded.stopHotkey = fallbacks[2];
            loaded.restartHotkey = fallbacks[3];
            loaded.previousHotkey = fallbacks[4];
            loaded.nextHotkey = fallbacks[5];
        }
    }

    result.settings = loaded;
    result.ok = true;
    return result;
}

SettingsLoadResult LoadSettings(const AppSettings& defaults, const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        SettingsLoadResult result;
        result.settings = defaults;
        result.fileExists = false;
        result.ok = false;
        result.error = "还没有配置文件（" + path + "），用的是默认设置";
        result.warnings.push_back(result.error);
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    SettingsLoadResult result = SettingsFromJson(defaults, buffer.str());
    // 配置坏了要留痕：否则用户只看到"设置回到默认了"，不知道为什么
    if (!result.error.empty()) {
        LogWarn("设置文件有问题：" + result.error);
    } else {
        for (const std::string& warning : result.warnings) {
            LogWarn("设置文件：" + warning);
        }
    }
    result.fileExists = true;
    return result;
}

bool SaveSettings(const AppSettings& settings, const std::string& path, std::string& error) {
    error.clear();
    // Phase 11 异常安全：配置写盘是最容易碰到"怪路径 / 只读目录 / 权限"的地方，
    // 这里整体兜住异常 —— 任何情况下都只返回 false + 中文原因，绝不把异常抛给调用者。
    try {
        return SaveSettingsUnlocked(settings, path, error);
    } catch (const std::exception& exception) {
        error = std::string("写配置时出现异常：") + exception.what();
        return false;
    } catch (...) {
        error = "写配置时出现未知异常";
        return false;
    }
}

bool SaveSettingsUnlocked(const AppSettings& settings, const std::string& path,
                          std::string& error) {
    const std::string json = SettingsToJson(settings);

    // 目录不存在就建（用 std::filesystem，既不弹控制台也不依赖 Win32；
    // 建不出来不算致命 —— 真正写文件时会报出原因）
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos && slash > 0) {
        const std::string directory = path.substr(0, slash);
        // 必须用 u8path：本项目内部字符串一律 UTF-8，而 std::filesystem 会把窄字符串按
        // 当前 ANSI 码页解释 —— 路径里有中文时会直接抛异常（"No mapping for the Unicode
        // character exists in the target multi-byte code page"，实测踩到过）。
        std::error_code code;
        std::filesystem::create_directories(std::filesystem::u8path(directory), code);
        if (code) {
            // 建不出目录就直接说清楚（例如盘符不存在），不去写文件、也不抛异常
            error = "建配置目录失败（" + directory + "）：" + code.message();
            return false;
        }
    }

    const std::string temporary = path + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "写不进临时文件（" + temporary + "）";
            return false;
        }
        stream << json;
        if (!stream) {
            error = "写临时文件时出错（" + temporary + "）";
            return false;
        }
    }
    if (std::remove(path.c_str()) != 0) {
        // 目标不存在也算正常
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        error = "替换配置文件失败（" + path + "）";
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace ui
}  // namespace hp
