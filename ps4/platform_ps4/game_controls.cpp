// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_ps4/game_controls.hpp"

#include "runtime/game_settings.h"
#include "runtime/prospero_profile.h"

#include <algorithm>
#include <iterator>
#include <cerrno>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" void playden_ps4_log_text(const char *line);

namespace rt = playden::runtime;

namespace
{

constexpr const char *kRoot = "/data/PLDN10001";
// One file per game, in the shared record format (SerializeGameSettingsRecord). PS5's single
// game-settings.json goes through OpenSafeParent's private-folder ownership checks, which the PS4
// title's /data folders do not meet ("cannot read game settings", 01.95).
constexpr const char *kSettingsDir = "/data/PLDN10001/user/game-settings";
constexpr const char *kRecipeDir = "/data/PLDN10001/recipes";

// What the Orbis-Win runtime applies beyond the base profile: the touchpad pointer. Rows that need
// anything else stay off the Controls page and out of the profile.
rt::KitCapabilities Ps4Kit()
{
    rt::KitCapabilities kit;
    kit.input_touchpad = true;
    kit.input_keyboard_stick = true;  // [input] keyboard_stick, upstream prospero-win #736
    // DXVK reads dxvk.conf from the game folder by itself: Frame limit, Frame latency, Reported GPU and VRAM,
    // Overlay detail.
    kit.dxvk_config = true;
    return kit;
}

std::string InstallRoot(std::uint32_t app_id) { return std::string(kRoot) + "/games/" + std::to_string(app_id); }
std::string ProfilePath(std::uint32_t app_id) { return std::string(kRoot) + "/profiles/" + std::to_string(app_id) + ".profile"; }
std::string RecipePath(std::uint32_t app_id) { return std::string(kRecipeDir) + "/steam-" + std::to_string(app_id) + ".json"; }

bool EnsureDirectory(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno == EEXIST) {
        struct stat info {};
        if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode)) return false;
        // Folders made by earlier builds (0700) are opened up to the mode asked for.
        if ((info.st_mode & 0777) != mode) (void)chmod(path, mode);
        return true;
    }
    return false;
}

std::optional<std::string> ReadFile(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return std::nullopt;
    std::string bytes;
    char chunk[4096];
    for (std::size_t count; (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) bytes.append(chunk, count);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) return std::nullopt;
    return bytes;
}

// Replaces path atomically: a temporary file, flushed and synced, then renamed over it.
bool WriteFileAtomic(const std::string &path, const std::string &bytes)
{
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool failed = std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size();
    if (std::fflush(file) != 0) failed = true;
    if (!failed && fsync(fileno(file)) != 0) failed = true;
    if (std::fclose(file) != 0) failed = true;
    if (failed || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

std::string RecordPath(std::uint32_t app_id) { return std::string(kSettingsDir) + "/" + std::to_string(app_id) + ".json"; }

// The saved record; a missing file is no choices. False (with reason) when the file is unreadable.
bool LoadRecord(std::uint32_t app_id, rt::GameSettingsRecord *record, std::string *reason)
{
    *record = {};
    struct stat info {};
    if (stat(RecordPath(app_id).c_str(), &info) != 0 && errno == ENOENT) return true;
    const auto text = ReadFile(RecordPath(app_id));
    if (!text) {
        *reason = "cannot read " + RecordPath(app_id);
        return false;
    }
    if (!rt::ParseGameSettingsRecord(*text, record, reason)) return false;
    return true;
}

bool SaveRecord(std::uint32_t app_id, const rt::GameSettingsRecord &record, std::string *reason)
{
    // Readable over FTP like profiles/: nothing here is secret, and players fix settings by hand.
    if (!EnsureDirectory((std::string(kRoot) + "/user").c_str(), 0777) || !EnsureDirectory(kSettingsDir, 0777)) {
        *reason = "Playden could not create its settings folder.";
        return false;
    }
    if (!WriteFileAtomic(RecordPath(app_id), rt::SerializeGameSettingsRecord(record))) {
        *reason = "Playden could not write " + RecordPath(app_id) + ".";
        return false;
    }
    return true;
}

rt::GameSettingsRecord RecordFor(std::uint32_t app_id)
{
    rt::GameSettingsRecord record;
    std::string reason;
    if (!LoadRecord(app_id, &record, &reason)) return {};
    return record;
}
std::optional<rt::GameRecipe> LoadRecipe(std::uint32_t app_id)
{
    const auto text = ReadFile(RecipePath(app_id));
    if (!text) return std::nullopt;
    rt::GameRecipe recipe;
    std::string reason;
    if (!rt::ParseGameRecipe(*text, &recipe, &reason)) return std::nullopt;
    return recipe;
}

// The prefix the game's current profile names; a new game gets its own.
std::string ExistingPrefix(std::uint32_t app_id)
{
    const auto text = ReadFile(ProfilePath(app_id));
    if (text) {
        static constexpr char key[] = "\nprefix = ";
        const auto at = text->find(key);
        if (at != std::string::npos) {
            const auto start = at + sizeof(key) - 1;
            const auto end = text->find_first_of("\r\n", start);
            std::string name = text->substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!name.empty()) return name;
        }
    }
    return "steam-" + std::to_string(app_id);
}

// Renders the profile from the recipe and the saved choices and writes it. The shared renderer names
// the prefix after the install folder; PS4 keeps the Playden prefix this game already uses.
bool RenderAndWrite(std::uint32_t app_id, const rt::GameRecipe &recipe, const std::string &prefix_name,
                    rt::EffectiveSettings *settings_out, std::string *reason)
{
    const auto record = RecordFor(app_id);
    rt::EffectiveSettings settings;
    rt::SettingsError error;
    if (!rt::BuildEffectiveSettings(recipe, nullptr, record.overrides, Ps4Kit(), &settings, &error)) {
        *reason = "Saved setting " + error.setting_id + " is invalid: " + error.message;
        return false;
    }
    rt::ProsperoProfile profile;
    const rt::ProfileTarget target{std::to_string(app_id), InstallRoot(app_id), ""};
    if (!rt::RenderProsperoProfile(recipe, settings, target, &profile, reason)) return false;
    std::string text = profile.text;
    const auto at = text.find("\nprefix = ");
    if (at == std::string::npos) {
        *reason = "The generated profile names no Wine prefix.";
        return false;
    }
    const auto start = at + 10;
    const auto end = text.find('\n', start);
    text.replace(start, end == std::string::npos ? std::string::npos : end - start, prefix_name);
    if (!EnsureDirectory((std::string(kRoot) + "/profiles").c_str(), 0777) ||
        !WriteFileAtomic(ProfilePath(app_id), text)) {
        *reason = "Playden could not write the game's launch profile.";
        return false;
    }
    // dxvk.conf in the folder the game starts in (DXVK reads it from there), as ApplyGameSettings does on the
    // PS5. One Playden didn't write is left alone; an empty config removes Playden's.
    if (settings.launch) {
        if (const auto folder = rt::LaunchWorkingFolder(*settings.launch)) {
            const std::string path = InstallRoot(app_id) + (folder->empty() ? "" : "/" + *folder) + "/dxvk.conf";
            const std::string config = rt::RenderDxvkConfig(settings);
            const auto existing = ReadFile(path);
            static const char kHeader[] = "# Generated by Playden";
            const bool ours = existing && existing->rfind(kHeader, 0) == 0;
            if (existing && !ours) {
                if (!config.empty())
                    playden_ps4_log_text(("game settings app=" + std::to_string(app_id) +
                                          " dxvk.conf left alone: the game folder has its own").c_str());
            } else if (config.empty()) {
                if (existing) std::remove(path.c_str());
            } else if (!existing || *existing != config) {
                if (!WriteFileAtomic(path, config)) {
                    *reason = "Playden could not write the game's dxvk.conf.";
                    return false;
                }
            }
        }
    }
    if (settings_out) *settings_out = std::move(settings);
    return true;
}

// The Controls page rows, in order.
struct Row
{
    std::string id;
    std::string label;
    std::vector<rt::SettingChoice> choices;  // for choice rows; empty for a button
    bool button = false;
    bool text = false;  // edited with the system keyboard
    const char *ps4_off = nullptr;  // shown, but the PS4 port does not apply it yet: why
};

// The game's Windows programs, from the installer's file list (.playden-files, "f<TAB>path"): the
// title cannot list folders. Redistributable installers are left out.
// One choice of the library overrides picker: a DLL loaded from the game first, then Wine's ("name=n,b").
struct OverrideItem
{
    std::string name;   // lower case, without .dll
    std::string label;
};

// DLL names Wine has its own copy of, that games often bring a version of their own: those are the ones an
// override changes.
bool WineBuiltinName(const std::string &name)
{
    for (const char *prefix : {"d3dx9_", "d3dx10", "d3dx11_", "d3dcompiler_", "xinput", "xaudio2_", "x3daudio",
                               "xapofx", "dinput", "dsound", "ddraw", "d3dim", "msvcp", "msvcr", "vcruntime",
                               "mfc", "msvbvm", "d3dxof", "dplay", "wmvcore"})
        if (name.rfind(prefix, 0) == 0) return true;
    return false;
}

// The picker's choices: the game's own copies of Wine's DLLs first, then common ones.
std::vector<OverrideItem> OverrideChoices(std::uint32_t app_id)
{
    std::vector<OverrideItem> items;
    const auto has = [&](const std::string &name) {
        for (const auto &item : items)
            if (item.name == name) return true;
        return false;
    };
    if (FILE *list = std::fopen((InstallRoot(app_id) + "/.playden-files").c_str(), "rb")) {
        char line[4096];
        while (items.size() < 24 && std::fgets(line, sizeof(line), list)) {
            if (line[0] != 'f' || line[1] != '\t') continue;
            std::string path(line + 2);
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
            std::string lower = path;
            for (char &ch : lower)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".dll") != 0) continue;
            if (lower.rfind("_commonredist/", 0) == 0 || lower.find("redist") != std::string::npos) continue;
            const auto slash = lower.rfind('/');
            const std::string name = lower.substr(slash == std::string::npos ? 0 : slash + 1,
                                                  lower.size() - 4 - (slash == std::string::npos ? 0 : slash + 1));
            if (!WineBuiltinName(name) || has(name)) continue;
            items.push_back({name, name + ".dll (the game's own copy)"});
        }
        std::fclose(list);
    }
    for (const char *name : {"dinput8", "dsound", "xinput1_3", "d3dcompiler_43", "d3dx9_43"})
        if (!has(name)) items.push_back({name, std::string(name) + ".dll (if the game brings one)"});
    return items;
}

// The names an override list sets ("a,b=n;c=n,b" sets a, b and c), lower case.
std::vector<std::string> OverrideNames(const std::string &list)
{
    std::vector<std::string> names;
    for (const auto &entry : rt::SplitDllOverrides(list)) {
        const auto equals = entry.find('=');
        std::string left = entry.substr(0, equals);
        std::size_t start = 0;
        while (start <= left.size()) {
            const auto comma = std::min(left.find(',', start), left.size());
            std::string name = left.substr(start, comma - start);
            for (char &ch : name)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (!name.empty()) names.push_back(name);
            start = comma + 1;
        }
    }
    return names;
}

// The list with name added as "name=n,b", or taken out of whichever entry names it.
std::string ToggleOverride(const std::string &list, const std::string &name)
{
    const auto names = OverrideNames(list);
    if (std::find(names.begin(), names.end(), name) == names.end())
        return list.empty() ? name + "=n,b" : list + ";" + name + "=n,b";
    std::string out;
    for (const auto &entry : rt::SplitDllOverrides(list)) {
        const auto equals = entry.find('=');
        const std::string left = entry.substr(0, equals);
        const std::string right = equals == std::string::npos ? std::string() : entry.substr(equals);
        std::string kept;
        std::size_t start = 0;
        while (start <= left.size()) {
            const auto comma = std::min(left.find(',', start), left.size());
            std::string part = left.substr(start, comma - start), lower = part;
            for (char &ch : lower)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (!part.empty() && lower != name) kept += (kept.empty() ? "" : ",") + part;
            start = comma + 1;
        }
        if (!kept.empty()) out += (out.empty() ? "" : ";") + kept + right;
    }
    return out;
}

std::vector<std::string> GamePrograms(std::uint32_t app_id)
{
    std::vector<std::string> programs;
    FILE *list = std::fopen((InstallRoot(app_id) + "/.playden-files").c_str(), "rb");
    if (!list) return programs;
    char line[4096];
    while (programs.size() < 40 && std::fgets(line, sizeof(line), list)) {
        if (line[0] != 'f' || line[1] != '\t') continue;
        std::string path(line + 2);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        std::string lower = path;
        for (char &ch : lower)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".exe") != 0) continue;
        if (lower.rfind("_commonredist/", 0) == 0 || lower.find("redist") != std::string::npos ||
            lower.find("vcredist") != std::string::npos || lower.find("dxsetup") != std::string::npos)
            continue;
        programs.push_back(path);
    }
    std::fclose(list);
    return programs;
}

// What a button can send: the runtime's key names (src/pw_game_profile.c in prospero-win).
const std::vector<std::string> &ButtonValues()
{
    static const std::vector<std::string> values = [] {
        std::vector<std::string> list = {"none", "mouse_left", "mouse_right", "mouse_middle", "enter", "escape",
                                         "space", "tab", "backspace", "shift", "ctrl", "alt", "up", "down",
                                         "left", "right", "pageup", "pagedown", "home", "end", "insert",
                                         "delete", "pause"};
        for (char c = 'a'; c <= 'z'; ++c) list.emplace_back(1, c);
        for (char c = '0'; c <= '9'; ++c) list.emplace_back(1, c);
        for (int f = 1; f <= 12; ++f) list.push_back("f" + std::to_string(f));
        for (const char *name : {"comma", "period", "slash", "semicolon", "quote", "minus", "equals",
                                 "lbracket", "rbracket", "backslash", "backquote"})
            list.emplace_back(name);
        return list;
    }();
    return values;
}

std::vector<Row> BuildRows(std::uint32_t app_id, bool with_controls)
{
    std::vector<Row> rows;
    // Program: Steam's launch option, or another program from the game folder (a setup tool the
    // game would start itself, which the runtime cannot: Mafia's setup.exe).
    {
        Row program{"program", "Program", {}, false};
        program.choices.push_back({"", "Steam's launch option", ""});
        for (const auto &path : GamePrograms(app_id)) program.choices.push_back({path, path, ""});
        rows.push_back(std::move(program));
    }
    for (const char *id : {"resolution", "scaling"}) {
        if (const auto *definition = rt::FindSetting(id))
            rows.push_back(Row{definition->id, definition->label, definition->choices, false});
    }
    // Library overrides added after the recipe's (WINEDLLOVERRIDES syntax, "d3dx9_27=n"): for a game
    // whose own DLL must win over Wine's (Dreamfall's d3dx9_27.dll).
    if (const auto *overrides = rt::FindSetting("library_overrides")) {
        Row row{overrides->id, overrides->label, {}, false};
        row.text = true;
        rows.push_back(std::move(row));
    }
    // Logging: the profile's [debug] winedebug, which takes precedence over the Settings tab's
    // Wine log level for this game only.
    if (const auto *logging = rt::FindSetting("logging"))
        rows.push_back(Row{logging->id, "Wine log detail", logging->choices, false});
    // Every other setting Playden has, as on the PS5: the ones this runtime can't take show why (the effective
    // settings mark them unavailable), and the few the PS4 port doesn't apply yet say so.
    {
        static const char *const kHandled[] = {"program", "launch_option", "resolution", "scaling", "library_overrides",
                                               "logging", "controller", "controller_preset", "pointer_stick",
                                               "movement_stick", "pointer_speed", "program_folder", "program_arguments",
                                               "arguments", "architecture", "dll_overrides"};
        for (const auto &definition : rt::SettingsCatalogue()) {
            if (!rt::SettingShown(definition, Ps4Kit())) continue;
            if (std::any_of(std::begin(kHandled), std::end(kHandled),
                            [&](const char *id) { return definition.id == id; }))
                continue;
            const bool controls = definition.group == rt::SettingGroup::kControls ||
                                  definition.group == rt::SettingGroup::kButtonMapping ||
                                  definition.group == rt::SettingGroup::kControllerTuning;
            if (controls && !with_controls) continue;
            Row row{definition.id, definition.label, definition.choices, false};
            row.text = definition.kind == rt::SettingKind::kText;
            // Playden's PS4 Steam setup always emulates; it doesn't read these rows yet.
            if (definition.id == "steam_features" || definition.id == "steam_input" || definition.id == "start_check")
                row.ps4_off = "The PS4 port doesn't apply this setting yet.";
            rows.push_back(std::move(row));
        }
    }
    if (!with_controls) return rows;
    for (const char *id : {"controller", "controller_preset", "pointer_stick", "movement_stick", "pointer_speed"}) {
        const auto *definition = rt::FindSetting(id);
        if (!definition) continue;
        Row row{definition->id, definition->label, {}, false};
        for (const auto &choice : definition->choices)
            // The PS4 runtime has no PlayStation-controller mode (kit capability input-playstation).
            if (!(row.id == "controller" && choice.value == "playstation")) row.choices.push_back(choice);
        rows.push_back(std::move(row));
    }
    for (const auto &button : rt::ProsperoButtons()) {
        const auto *definition = rt::FindSetting("button." + button);
        if (definition) rows.push_back(Row{definition->id, definition->label, {}, true});
    }
    return rows;
}

struct ControlsPage
{
    std::mutex mutex;
    std::uint32_t app_id = 0;
    std::optional<rt::GameRecipe> recipe;
    std::vector<Row> rows;
    std::vector<std::string> labels, values;
    std::vector<int> custom;
    std::string status;
    std::vector<OverrideItem> overrides;  // the picker's choices; a "Type your own" row follows them
};

ControlsPage &Page()
{
    static ControlsPage page;
    return page;
}

// Why the shared recipe was refused, per game, for the Game Settings page.
// ⚠ A function-local static, never a namespace-scope object with a constructor: the UI PRX is
// entered through its descriptor (ps4/runtime_entry.c), so its .init_array does not run. A global
// std::map stayed all-zero and crashed on first use (01.91, SIGSEGV in NoteRefusal).
struct Refusals
{
    std::mutex mutex;
    std::map<std::uint32_t, std::string> reasons;
};

Refusals &RefusalTable()
{
    static Refusals table;
    return table;
}

void NoteRefusal(std::uint32_t app_id, const std::string &reason)
{
    auto &table = RefusalTable();
    const std::lock_guard<std::mutex> guard(table.mutex);
    if (reason.empty()) table.reasons.erase(app_id);
    else table.reasons[app_id] = reason;
}

std::string Refusal(std::uint32_t app_id)
{
    auto &table = RefusalTable();
    const std::lock_guard<std::mutex> guard(table.mutex);
    const auto found = table.reasons.find(app_id);
    return found == table.reasons.end() ? std::string() : found->second;
}

// Refreshes the shown values from the saved choices (page lock held).
void RefreshValues(ControlsPage &page)
{
    page.labels.clear();
    page.values.clear();
    page.custom.clear();
    const auto record = RecordFor(page.app_id);
    rt::EffectiveSettings settings;
    rt::SettingsError error;
    const bool built = page.recipe &&
        rt::BuildEffectiveSettings(*page.recipe, nullptr, record.overrides, Ps4Kit(), &settings, &error);
    const auto preset = built && settings.values.count("controller_preset")
        ? rt::ChosenValue(settings.values.at("controller_preset")) : std::string("none");
    for (const auto &row : page.rows) {
        page.labels.push_back(row.label);
        const auto saved = record.overrides.find(row.id);
        page.custom.push_back(saved != record.overrides.end() ? 1 : 0);
        std::string value;
        if (built && settings.values.count(row.id)) value = rt::ChosenValue(settings.values.at(row.id));
        if (row.ps4_off) {
            value = "Not on the PS4 yet";
        } else if (built && settings.values.count(row.id) && !settings.values.at(row.id).available) {
            value = "Not available";
        } else if (row.text) {
            if (saved != record.overrides.end()) value = saved->second;
            if (value.empty()) value = "None";
        } else if (row.button) {
            if (value.empty()) value = preset != "none" && !preset.empty() ? "From preset" : "Not set";
        } else {
            for (const auto &choice : row.choices)
                if (choice.value == value) { value = choice.label; break; }
            if (value.empty()) value = "Default";
        }
        page.values.push_back(value);
    }
}

// Saves (or, for an empty value, removes) the player's choice and rewrites the profile.
bool Save(ControlsPage &page, const std::string &id, const std::string &value)
{
    if (!page.recipe) return false;
    std::string reason;
    if (!value.empty() && !rt::ValidateSettingValue(*page.recipe, id, value, &reason)) {
        page.status = reason;
        return false;
    }
    rt::GameSettingsRecord record;
    if (!LoadRecord(page.app_id, &record, &reason)) {
        page.status = "Saved settings are unreadable: " + reason;
        return false;
    }
    const auto previous = record;
    if (value.empty()) record.overrides.erase(id);
    else record.overrides[id] = value;
    ++record.revision;
    if (!SaveRecord(page.app_id, record, &reason)) {
        page.status = "Could not save: " + reason;
        return false;
    }
    if (!RenderAndWrite(page.app_id, *page.recipe, ExistingPrefix(page.app_id), nullptr, &reason)) {
        // Keep the saved choices and the profile in agreement.
        std::string ignored;
        (void)SaveRecord(page.app_id, previous, &ignored);
        page.status = reason;
        return false;
    }
    page.status = "Saved. Applies the next time the game starts.";
    return true;
}

}  // namespace

namespace playden::ps4
{

bool WriteSharedLaunchProfile(std::uint32_t app_id, const std::string &title, const LaunchSpec &launch,
                              const std::string &prefix_name, std::string *reason)
{
    rt::LaunchEntry entry;
    entry.id = "steam-default";
    entry.label = title;
    entry.executable = launch.executable;
    entry.working_directory = launch.working_directory;
    entry.arguments = launch.arguments;
    rt::GameRecipe recipe;
    const auto record = RecordFor(app_id);
    if (!rt::GenerateGameRecipe(app_id, title, {entry}, InstallRoot(app_id), &recipe, reason, Ps4Kit(),
                                rt::RecipeChoicesFrom(record))) {
        NoteRefusal(app_id, *reason);
        return false;
    }
    NoteRefusal(app_id, {});
    if (!EnsureDirectory(kRecipeDir, 0777) || !WriteFileAtomic(RecipePath(app_id), rt::SerializeGameRecipe(recipe))) {
        *reason = "Playden could not save the game's recipe.";
        return false;
    }
    return RenderAndWrite(app_id, recipe, prefix_name, nullptr, reason);
}

std::string DescribeGamePath(std::uint32_t app_id, const std::string &relative)
{
    std::string out;
    std::string folder = InstallRoot(app_id);
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto end = std::min(relative.find('/', start), relative.size());
        const std::string part = relative.substr(start, end - start);
        start = end + 1;
        errno = 0;
        DIR *directory = opendir(folder.c_str());
        out += " dir=" + folder + (directory ? "" : " opendir_errno=" + std::to_string(errno));
        if (directory) {
            int entries = 0;
            std::string names;
            errno = 0;
            while (const dirent *entry = readdir(directory)) {
                ++entries;
                if (names.size() < 160) names += std::string(names.empty() ? "" : ",") + entry->d_name;
            }
            out += " entries=" + std::to_string(entries) + " end_errno=" + std::to_string(errno) + " names=" + names;
            closedir(directory);
        }
        const std::string exact = folder + "/" + part;
        struct stat info {};
        const int rc = lstat(exact.c_str(), &info);
        out += " lstat(" + part + ")=" + std::to_string(rc) + (rc == 0 ? " mode=" + std::to_string(info.st_mode) : " errno=" + std::to_string(errno));
        if (end == relative.size()) break;
        folder = exact;
    }
    return out;
}

void InstallControllerPresets()
{
    const auto *definition = rt::FindSetting("controller_preset");
    if (!definition || !EnsureDirectory((std::string(kRoot) + "/input").c_str(), 0777)) return;
    for (const auto &choice : definition->choices) {
        if (choice.value == "none") continue;
        const auto packaged = ReadFile("/app0/runtime/input/" + choice.value + ".input");
        if (!packaged) continue;
        const std::string installed = std::string(kRoot) + "/input/" + choice.value + ".input";
        const auto current = ReadFile(installed);
        if (!current || *current != *packaged) (void)WriteFileAtomic(installed, *packaged);
    }
}

}  // namespace playden::ps4

extern "C" const char *playden_ps4_wine_debug_level();

extern "C" int playden_ps4_controls_open(std::uint32_t app_id, int with_controls)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    page.app_id = app_id;
    page.recipe = LoadRecipe(app_id);
    page.rows = BuildRows(app_id, with_controls != 0);
    const auto refusal = Refusal(app_id);
    page.status = !page.recipe ? (refusal.empty() ? "Install the game, then open its details once to change its settings."
                                                  : "Settings unavailable: " + refusal)
        : with_controls ? "Changes apply the next time the game starts."
                        : "Steam lists full controller support, so this game needs no button mapping.";
    // The Settings tab's Wine log replaces this page's unless it is "profile" (Orbis-Win runtime).
    if (page.recipe) {
        const std::string global = playden_ps4_wine_debug_level();
        if (global != "profile")
            page.status = "Wine log detail applies only while Settings > Wine log is Profile (now " + global + ").";
    }
    RefreshValues(page);
    return page.recipe ? 1 : 0;
}

extern "C" std::size_t playden_ps4_controls_count(void)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    return page.recipe ? page.rows.size() : 0;
}

extern "C" const char *playden_ps4_controls_label(std::size_t row)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    return row < page.labels.size() ? page.labels[row].c_str() : "";
}

extern "C" const char *playden_ps4_controls_value(std::size_t row)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    return row < page.values.size() ? page.values[row].c_str() : "";
}

extern "C" int playden_ps4_controls_custom(std::size_t row)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    return row < page.custom.size() ? page.custom[row] : 0;
}

extern "C" int playden_ps4_controls_step(std::size_t row, int direction)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    if (!page.recipe || row >= page.rows.size() || direction == 0) return 0;
    const auto &definition = page.rows[row];
    if (definition.ps4_off) {
        page.status = definition.ps4_off;
        return 0;
    }
    if (definition.text) return 2;  // the caller opens the system keyboard (playden_ps4_controls_edit)
    const auto record = RecordFor(page.app_id);
    const auto saved = record.overrides.find(definition.id);
    std::vector<std::string> values;
    if (definition.button) {
        values.push_back("");  // not set: the preset's key, or nothing
        for (const auto &value : ButtonValues()) values.push_back(value);
    } else {
        for (const auto &choice : definition.choices) values.push_back(choice.value);
    }
    if (values.empty()) return 0;
    std::string current;
    if (saved != record.overrides.end()) current = saved->second;
    else if (!definition.button) {
        rt::EffectiveSettings settings;
        rt::SettingsError error;
        if (rt::BuildEffectiveSettings(*page.recipe, nullptr, record.overrides, Ps4Kit(), &settings, &error) &&
            settings.values.count(definition.id))
            current = rt::ChosenValue(settings.values.at(definition.id));
    }
    long index = 0;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (values[i] == current) index = static_cast<long>(i);
    const long count = static_cast<long>(values.size());
    index = ((index + direction) % count + count) % count;
    const bool saved_ok = Save(page, definition.id, values[static_cast<std::size_t>(index)]);
    RefreshValues(page);
    // A row the game cannot use (Scaling for a DXVK game) keeps the saved choice for later; say why.
    {
        rt::EffectiveSettings settings;
        rt::SettingsError error;
        const auto saved = RecordFor(page.app_id);
        if (rt::BuildEffectiveSettings(*page.recipe, nullptr, saved.overrides, Ps4Kit(), &settings, &error) &&
            settings.values.count(definition.id) && !settings.values.at(definition.id).available &&
            !settings.values.at(definition.id).reason.empty())
            page.status = "Not available: " + settings.values.at(definition.id).reason;
    }
    return saved_ok ? 1 : 0;
}

extern "C" int playden_ps4_controls_reset(std::size_t row)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    if (!page.recipe || row >= page.rows.size()) return 0;
    const bool saved_ok = Save(page, page.rows[row].id, "");
    if (saved_ok) page.status = "Back to the default. Applies the next time the game starts.";
    RefreshValues(page);
    return saved_ok ? 1 : 0;
}

extern "C" int playden_ps4_edit_text(int user_id, const char *title, const char *placeholder,
                                     const char *initial, char *out, std::size_t out_size);

// The library overrides picker for the Game Settings row `row` (a text row): returns its choice count; the
// row after the last choice is "Type your own".
extern "C" std::size_t playden_ps4_overrides_open(std::size_t row)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    if (!page.recipe || row >= page.rows.size() || page.rows[row].id != "library_overrides") return 0;
    page.overrides = OverrideChoices(page.app_id);
    page.status = "CROSS turns an override on or off; it applies the next time the game starts.";
    return page.overrides.size();
}

extern "C" const char *playden_ps4_overrides_label(std::size_t item)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    return item < page.overrides.size() ? page.overrides[item].label.c_str() : "Type your own…";
}

extern "C" int playden_ps4_overrides_checked(std::size_t item)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    if (item >= page.overrides.size()) return 0;
    const auto record = RecordFor(page.app_id);
    const auto found = record.overrides.find("library_overrides");
    if (found == record.overrides.end()) return 0;
    const auto names = OverrideNames(found->second);
    return std::find(names.begin(), names.end(), page.overrides[item].name) != names.end() ? 1 : 0;
}

extern "C" int playden_ps4_overrides_toggle(std::size_t item)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    if (!page.recipe || item >= page.overrides.size()) return 0;
    const auto record = RecordFor(page.app_id);
    const auto found = record.overrides.find("library_overrides");
    const std::string current = found == record.overrides.end() ? std::string() : found->second;
    const bool saved_ok = Save(page, "library_overrides", ToggleOverride(current, page.overrides[item].name));
    RefreshValues(page);
    return saved_ok ? 1 : 0;
}

// The current override list, for the picker's summary line.
extern "C" const char *playden_ps4_overrides_value(void)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    static std::string value;
    const auto record = RecordFor(page.app_id);
    const auto found = record.overrides.find("library_overrides");
    value = found == record.overrides.end() || found->second.empty() ? std::string("None") : found->second;
    return value.c_str();
}

extern "C" int playden_ps4_controls_edit(std::size_t row, int user_id)
{
    std::string id, current, label;
    {
        auto &page = Page();
        const std::lock_guard<std::mutex> guard(page.mutex);
        if (!page.recipe || row >= page.rows.size() || !page.rows[row].text) return 0;
        id = page.rows[row].id;
        label = page.rows[row].label;
        const auto record = RecordFor(page.app_id);
        if (const auto found = record.overrides.find(id); found != record.overrides.end()) current = found->second;
    }
    // The keyboard blocks this thread until it closes; the page lock is not held meanwhile.
    char edited[256] = {};
    const char *example = id == "library_overrides" ? "For example d3dx9_27=n"
                        : id == "extra_arguments" ? "For example -windowed -nointro" : "";
    const int result = playden_ps4_edit_text(user_id, label.c_str(), example, current.c_str(),
                                             edited, sizeof(edited));
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    if (result == 0) {
        page.status = "No change.";
        return 0;
    }
    if (result < 0) {
        page.status = "Use plain letters, digits and = , ; only.";
        return 0;
    }
    const bool saved_ok = Save(page, id, edited);
    RefreshValues(page);
    return saved_ok ? 1 : 0;
}

extern "C" const char *playden_ps4_controls_status(void)
{
    auto &page = Page();
    const std::lock_guard<std::mutex> guard(page.mutex);
    return page.status.c_str();
}
