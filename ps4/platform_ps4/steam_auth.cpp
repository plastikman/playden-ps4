// Playden PS4 port - Steam QR sign-in adapter.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_ps4/steam_auth.hpp"
#include "platform_ps4/game_controls.hpp"
#include "wine_runtime.h"

#include "steam/backend/auth_client.h"
#include "steam/backend/http_client.h"
#include "steam/store_details_service.h"
#include "homebrew/store_fetch.hpp"
#include "steam/backend/library_client.h"
#include "library/cache.h"
#include "library/service.h"
#include "library/organization.h"
#include "library/query.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/app_info.h"
#include "steam/backend/cdn_client.h"
#include "steam/backend/steam_install_runner.h"
#include "steam/backend/steam_cloud_runner.h"
#include "steam/cloud_service.h"
#include "steam/backend/content_language.h"
#include "installs/install_record.h"
#include "steam/backend/native_network.h"
#include "steam/secret.h"
#include <cctype>
#include "steam/service.h"
#include "steam/session_store.h"
#include "platform/storage.h"
#include "runtime/steamworks.h"
#include "runtime/pe_architecture.h"

#include <cstdio>
#include <algorithm>
#include <cstdarg>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <iterator>
#include <exception>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <execinfo.h>
#include <orbis/libkernel.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis_boot.h>
#include <orbis_log.h>
#include <orbis_thread.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

// SceLibc's heap statistics (SceLibcMallocManagedSize, version 1) and the flexible memory
// query. The SDK headers declare both with the wrong signature, so bind them by symbol.
struct PlaydenMallocManagedSize
{
    std::uint16_t size;
    std::uint16_t version;
    std::uint32_t reserved1;
    std::size_t max_system_size;
    std::size_t current_system_size;
    std::size_t max_inuse_size;
    std::size_t current_inuse_size;
};
extern "C" int playden_malloc_stats_fast(PlaydenMallocManagedSize *stats)
    __asm__("malloc_stats_fast") __attribute__((weak));
extern "C" std::int32_t playden_flexible_available(std::size_t *size)
    __asm__("sceKernelAvailableFlexibleMemorySize");
#include "playden_heap.h"
// Weak: only the UI PRX links Playden's heap (ps4/playden_heap.c).
extern "C" void playden_heap_stats(PlaydenHeapStats *stats) __attribute__((weak));

namespace
{
void LogNetworkLine(const std::string &line);

std::string HeapSummary()
{
    PlaydenMallocManagedSize stats{};
    stats.size = sizeof(stats);
    stats.version = 1;
    const int rc = &playden_malloc_stats_fast ? playden_malloc_stats_fast(&stats) : -1;
    std::size_t flexible = 0;
    const std::int32_t flexible_rc = playden_flexible_available(&flexible);
    const auto kib = [](std::size_t bytes) { return std::to_string(bytes / 1024); };
    char rc_text[64];
    std::snprintf(rc_text, sizeof(rc_text), " rc=0x%08x flex_rc=0x%08x", static_cast<unsigned>(rc),
                  static_cast<unsigned>(flexible_rc));
    std::string own = " playden_heap=absent";
    if (&playden_heap_stats) {
        PlaydenHeapStats heap{};
        playden_heap_stats(&heap);
        own = " playden_heap live=" + kib(heap.live_bytes) + "KiB peak_live=" + kib(heap.peak_live_bytes) +
              "KiB mapped=" + kib(heap.mapped_bytes) + "KiB peak_mapped=" + kib(heap.peak_mapped_bytes) +
              "KiB allocs=" + std::to_string(heap.allocations) + " frees=" + std::to_string(heap.frees) +
              " failures=" + std::to_string(heap.failures) + " foreign_frees=" +
              std::to_string(heap.foreign_frees) + " foreign_reallocs=" + std::to_string(heap.foreign_reallocs);
        if (heap.failures)
            own += " last_failed=" + std::to_string(heap.last_failed_bytes) + " last_map=" +
                   std::to_string(heap.last_map_bytes) + " map_errno=" + std::to_string(heap.last_map_errno);
    }
    return "libc system=" + kib(stats.current_system_size) + "KiB inuse=" + kib(stats.current_inuse_size) +
           "KiB max_inuse=" + kib(stats.max_inuse_size) + "KiB flex_avail=" + kib(flexible) + "KiB" + rc_text +
           own;
}

void LogHeap(const std::string &where)
{
    LogNetworkLine("heap [" + where + "] " + HeapSummary());
}

// Confirm at startup that malloc can go past libc's fixed 13 MiB heap: take 32 1 MiB
// blocks, record the heaps, then give every block back.
void ProbeHeapCapacity()
{
    constexpr std::size_t kBlock = 1024 * 1024;
    constexpr std::size_t kMaxBlocks = 32;
    static void *blocks[kMaxBlocks];
    LogHeap("probe-start");
    std::size_t held = 0;
    while (held < kMaxBlocks) {
        void *block = std::malloc(kBlock);
        if (!block) break;
        std::memset(block, 0xa5, 4096);
        blocks[held++] = block;
    }
    LogHeap("probe-held " + std::to_string(held) + "MiB" +
            (held == kMaxBlocks ? " limit-not-reached" : " malloc-refused"));
    while (held) std::free(blocks[--held]);
    LogHeap("probe-freed");
}

std::string JoinStoreItems(const std::vector<std::string> &items)
{
    std::string joined;
    for (const auto &item : items) {
        if (!joined.empty()) joined += ", ";
        joined += item;
    }
    return joined;
}

bool IsIpv4Address(const std::string &text)
{
    unsigned parts = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('.', start);
        const auto length = (end == std::string::npos ? text.size() : end) - start;
        if (length == 0 || length > 3 || parts == 4) return false;
        unsigned value = 0;
        for (std::size_t i = start; i < start + length; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            value = value * 10 + static_cast<unsigned>(text[i] - '0');
        }
        if (value > 255) return false;
        ++parts;
        if (end == std::string::npos) {
            start = text.size();
        } else {
            start = end + 1;
            if (start == text.size()) return false;
        }
    }
    return parts == 4;
}

// sceCommonDialogInitialize, once: the system dialogs need it before the first one opens (Orbis-Win's
// pw_osk_ps5.c does the same). libSceCommonDialog has no sysmodule id, so it is loaded by name; a
// failure is logged and the keyboard is still tried.
void EnsureCommonDialog()
{
    static bool tried = false;
    if (tried) return;
    tried = true;
    int (*initialize)(void) = nullptr;
    int rc = -1;
    for (const char *path : {"libSceCommonDialog.sprx", "/system/common/lib/libSceCommonDialog.sprx"}) {
        int start_result = 0;
        const auto module = static_cast<std::int32_t>(
            sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &start_result));
        void *address = nullptr;
        if (module >= 0 && sceKernelDlsym(module, "sceCommonDialogInitialize", &address) == 0 && address) {
            std::memcpy(&initialize, &address, sizeof(address));
            rc = initialize();
            break;
        }
    }
    char line[96];
    std::snprintf(line, sizeof(line), "keyboard common dialog init rc=0x%08x%s", static_cast<unsigned>(rc),
                  initialize ? "" : " (not found)");
    LogNetworkLine(line);
}

// Waits for the keyboard to close. NONE early on means it has not been drawn yet; past a short grace
// it means there is no dialog.
void WaitForKeyboard()
{
    for (int polls = 0;; ++polls) {
        const int status = sceImeDialogGetStatus();
        if (status == ORBIS_DIALOG_STATUS_RUNNING || (status == ORBIS_DIALOG_STATUS_NONE && polls < 50)) {
            usleep(10000);
            continue;
        }
        if (status != ORBIS_DIALOG_STATUS_STOPPED) {
            char line[80];
            std::snprintf(line, sizeof(line), "keyboard ended status=%d polls=%d", status, polls);
            LogNetworkLine(line);
        }
        return;
    }
}

// The system keyboard for one line of ASCII text (title and placeholder are ASCII). 1: accepted and
// stored in *result; 0: cancelled or unavailable; -1: the text was not plain ASCII.
// IME options (SceImeOption): no automatic capital letters, and a masked field.
constexpr std::uint32_t kImeNoAutoCapitalization = 0x2;
constexpr std::uint32_t kImePassword = 0x4;

int EditTextWithSystemKeyboard(int user_id, const char *title_ascii, const char *placeholder_ascii,
                               const std::string &initial, std::size_t max_length, std::string *result,
                               std::uint32_t option = 0)
{
    if (!result || user_id < 0 || max_length == 0 || max_length > 250 ||
        sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG) < 0)
        return 0;
    EnsureCommonDialog();
    // UTF-16 code units, as the IME takes them (wchar_t is 32-bit here; see the IP editor below).
    std::uint16_t input[256] = {};
    for (std::size_t i = 0; i < initial.size() && i < max_length; ++i)
        input[i] = static_cast<std::uint8_t>(initial[i]);
    std::uint16_t title[64] = {}, placeholder[96] = {};
    for (std::size_t i = 0; title_ascii && title_ascii[i] && i + 1 < 64; ++i)
        title[i] = static_cast<std::uint8_t>(title_ascii[i]);
    for (std::size_t i = 0; placeholder_ascii && placeholder_ascii[i] && i + 1 < 96; ++i)
        placeholder[i] = static_cast<std::uint8_t>(placeholder_ascii[i]);

    OrbisImeDialogSetting settings {};
    settings.userId = static_cast<std::uint32_t>(user_id);
    settings.type = ORBIS_TYPE_BASIC_LATIN;  // letters, digits and symbols, as Orbis-Win's keyboard
    settings.option = option;
    settings.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    settings.maxTextLength = static_cast<std::uint32_t>(max_length);
    settings.inputTextBuffer = reinterpret_cast<wchar_t *>(input);
    settings.horizontalAlignment = ORBIS_H_CENTER;
    settings.verticalAlignment = ORBIS_V_CENTER;
    settings.placeholder = reinterpret_cast<const wchar_t *>(placeholder);
    settings.title = reinterpret_cast<const wchar_t *>(title);
    int init_result = sceImeDialogInit(&settings, nullptr);
    // A keyboard opened right after another one (the password after the account name) was refused with
    // 0x80bc0008 on 02.11 while the first was still closing: wait for it, up to two seconds.
    for (int tries = 0; init_result == static_cast<int>(0x80bc0008) && tries < 20; ++tries) {
        usleep(100000);
        init_result = sceImeDialogInit(&settings, nullptr);
        if (init_result >= 0 || tries == 19) {
            char line[96];
            std::snprintf(line, sizeof(line), "keyboard init after %d waits rc=0x%08x", tries + 1,
                          static_cast<unsigned>(init_result));
            LogNetworkLine(line);
        }
    }
    if (init_result < 0 && (option & kImePassword)) {
        // Still refused: the hidden-text option may be what this firmware rejects. Ask without it
        // rather than not at all, and say so in the log.
        settings.option = option & ~kImePassword;
        const int hidden = init_result;
        init_result = sceImeDialogInit(&settings, nullptr);
        char line[112];
        std::snprintf(line, sizeof(line), "keyboard init rc=0x%08x; without the password option rc=0x%08x",
                      static_cast<unsigned>(hidden), static_cast<unsigned>(init_result));
        LogNetworkLine(line);
    }
    if (init_result < 0) {
        // Retry with the settings the receiver-IP keyboard uses, which open on this console.
        const int first = init_result;
        settings.enterLabel = ORBIS_BUTTON_LABEL_GO;
        settings.maxTextLength = static_cast<std::uint32_t>(std::min<std::size_t>(max_length, 64));
        init_result = sceImeDialogInit(&settings, nullptr);
        char line[160];
        std::snprintf(line, sizeof(line), "keyboard init rc=0x%08x retry(max=%u,go) rc=0x%08x",
                      static_cast<unsigned>(first), settings.maxTextLength, static_cast<unsigned>(init_result));
        LogNetworkLine(line);
    }
    if (init_result < 0) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_IME_DIALOG);
        return 0;
    }
    WaitForKeyboard();
    OrbisDialogResult dialog_result {};
    const bool accepted = sceImeDialogGetResult(&dialog_result) >= 0 &&
                          dialog_result.endstatus == ORBIS_DIALOG_OK;
    std::string text;
    bool ascii = true;
    if (accepted) {
        for (std::size_t i = 0; i < max_length && input[i] != 0; ++i) {
            std::uint16_t character = input[i];
            if (character >= 0xff01 && character <= 0xff5e) character -= 0xfee0;  // full-width ASCII
            if (character == 0x3002) character = '.';
            if (character < 0x20 || character > 0x7e) { ascii = false; break; }
            text.push_back(static_cast<char>(character));
        }
    }
    sceImeDialogTerm();
    sceSysmoduleUnloadModule(ORBIS_SYSMODULE_IME_DIALOG);
    if (!accepted) return 0;
    if (!ascii) return -1;
    const auto first = text.find_first_not_of(' ');
    const auto last = text.find_last_not_of(' ');
    *result = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    return 1;
}

int EditReceiverIpWithSystemKeyboard(int user_id, const std::string &initial, std::string *result)
{
    if (!result || user_id < 0 || sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG) < 0)
        return 0;
    EnsureCommonDialog();

    // The PS4 IME consumes UTF-16 code units. The cross-compiled host's
    // wchar_t is 32-bit, so passing wchar_t arrays makes the IME see each
    // character's low word followed by a zero high word (for example, only
    // "L" for the title and "1" for the prefilled address).
    std::uint16_t input[16] = {};
    for (std::size_t i = 0; i < initial.size() && i + 1 < sizeof(input) / sizeof(input[0]); ++i)
        input[i] = static_cast<std::uint8_t>(initial[i]);
    static constexpr char16_t placeholder[] = u"Enter an IPv4 address";
    static constexpr char16_t title[] = u"Log receiver IP";

    OrbisImeDialogSetting settings {};
    settings.userId = static_cast<std::uint32_t>(user_id);
    settings.type = ORBIS_TYPE_TYPE_URL;
    settings.enterLabel = ORBIS_BUTTON_LABEL_GO;
    settings.maxTextLength = 15;
    settings.inputTextBuffer = reinterpret_cast<wchar_t *>(input);
    settings.horizontalAlignment = ORBIS_H_CENTER;
    settings.verticalAlignment = ORBIS_V_CENTER;
    settings.placeholder = reinterpret_cast<const wchar_t *>(placeholder);
    settings.title = reinterpret_cast<const wchar_t *>(title);

    const int init_result = sceImeDialogInit(&settings, nullptr);
    if (init_result < 0) {
        sceSysmoduleUnloadModule(ORBIS_SYSMODULE_IME_DIALOG);
        return 0;
    }
    WaitForKeyboard();

    OrbisDialogResult dialog_result {};
    const bool accepted = sceImeDialogGetResult(&dialog_result) >= 0 &&
                          dialog_result.endstatus == ORBIS_DIALOG_OK;
    std::string address;
    if (accepted) {
        for (std::size_t i = 0; i < sizeof(input) / sizeof(input[0]) && input[i] != 0; ++i) {
            std::uint16_t character = input[i];
            // IME input can contain full-width ASCII characters, even when
            // they render like ordinary digits and periods in the text field.
            if (character >= 0xff01 && character <= 0xff5e) character -= 0xfee0;
            if (character == 0x3002) character = '.';
            if (character > 0x7f) { address.clear(); break; }
            address.push_back(static_cast<char>(character));
        }
    }
    while (!address.empty() && (address.back() == ' ' || address.back() == '\t' ||
                                address.back() == '\r' || address.back() == '\n'))
        address.pop_back();
    const auto first = address.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && first != 0) address.erase(0, first);
    sceImeDialogTerm();
    sceSysmoduleUnloadModule(ORBIS_SYSMODULE_IME_DIALOG);
    if (!accepted) return 0;
    if (!IsIpv4Address(address)) return -1;
    *result = std::move(address);
    return 1;
}

constexpr const char *kInternalGamesRoot = "/data/PLDN10001/games";
bool CleanWindowsRelativePath(std::string &path);
std::string LaunchExecutableRelative(std::uint32_t app_id);

std::string GameInstallRoot(std::uint32_t app_id)
{
    return std::string(kInternalGamesRoot) + "/" + std::to_string(app_id);
}

std::vector<std::string> GameInstallRoots(std::uint32_t app_id)
{
    return {GameInstallRoot(app_id)};
}

std::string InstalledGameRoot(std::uint32_t app_id)
{
    for (const auto &candidate : GameInstallRoots(app_id)) {
        const auto record = playden::installs::ReadInstallRecord(
            candidate + "/" + playden::installs::kInstallRecordName);
        struct stat journal {};
        const bool download_incomplete =
            lstat((candidate + "/" + playden::steam::DepotInstaller::kJournalDir).c_str(), &journal) == 0;
        if (record && record->app_id == app_id && !download_incomplete) return candidate;

    }
    return {};
}

std::string PrepareSteamworksForGame(const std::string &root, std::uint32_t app_id,
                                     std::uint64_t steam_id, const std::string &account_name)
{
    playden::runtime::SteamworksIdentity identity;
    identity.app_id = app_id;
    identity.steam_id = steam_id;
    identity.account_name = account_name;
    (void)playden::runtime::ReadSteamworksFacts(root, &identity);

    constexpr const char *emulator = "/app0/runtime/steamworks";
    const auto result = playden::runtime::EmulateSteamworks(root, emulator, identity);
    std::string changed;
    for (const auto &path : result.changed) changed += (changed.empty() ? "" : ",") + path;
    LogNetworkLine("steamworks app=" + std::to_string(app_id) + " mode=emulate found=" +
                   std::to_string(result.found.size()) + " changed=" + (changed.empty() ? "-" : changed) +
                   (result.ok ? "" : " failed=" + result.message));
    if (!result.ok) return result.message;

    if (result.found.empty()) {
        const auto program = LaunchExecutableRelative(app_id);
        if (program.empty())
            return "Playden couldn't read this game's launch path to prepare Steam features.";
        const std::vector<std::string> programs = {program};
        const auto legacy = playden::runtime::ProvideLegacySteam(root, emulator, programs);
        std::string added;
        for (const auto &path : legacy.changed) added += (added.empty() ? "" : ",") + path;
        LogNetworkLine("legacy steam app=" + std::to_string(app_id) + " program=" + program +
                       " added=" + (added.empty() ? "-" : added) +
                       (legacy.ok ? "" : " failed=" + legacy.message));
        if (!legacy.ok) return legacy.message;
    }
    return {};
}

bool HasInstalledRecord(std::uint32_t app_id)
{
    return !InstalledGameRoot(app_id).empty();
}

std::string ProfilePrefixName(std::uint32_t app_id)
{
    char profile_path[256];
    std::snprintf(profile_path, sizeof(profile_path), "/data/PLDN10001/profiles/%u.profile", app_id);
    std::string name;
    if (FILE *profile = std::fopen(profile_path, "rb")) {
        char line[256];
        static constexpr char key[] = "prefix = ";
        while (std::fgets(line, sizeof(line), profile)) {
            if (std::strncmp(line, key, sizeof(key) - 1) != 0) continue;
            name.assign(line + sizeof(key) - 1);
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
            break;
        }
        std::fclose(profile);
    }
    if (name.empty() || name.size() > 80 ||
        !std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        })) return {};
    return name;
}

std::string ConfiguredWinePrefix(std::uint32_t app_id)
{
    std::string name = ProfilePrefixName(app_id);
    // An old Half-Life 2 install may predate its generated launch profile.
    if (name.empty() && app_id == 220) name = "half-life-2";
    if (name.empty()) return {};
    const std::string path = "/data/PLDN10001/prefixes/" + name;
    struct stat info {};
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) ? path : std::string();
}

// Whether Steam has a newer version of an installed game: a depot the install recorded from this app's
// listing now names another manifest on the installed branch (the comparison of the shared
// CompareWithRecord, without licenses: only depots the install already holds). Borrowed depots are
// listed by their own app and are left out.
bool InstalledUpdateAvailable(std::uint32_t app_id, const playden::steam::AppInfo &app)
{
    const auto record = playden::installs::ReadInstallRecord(
        GameInstallRoot(app_id) + "/" + playden::installs::kInstallRecordName);
    if (!record || record->depots.empty()) return false;
    const std::string branch = record->branch.empty() ? "public" : record->branch;
    for (const auto &installed : record->depots) {
        if (installed.source != 0 && installed.source != app_id) continue;
        const auto depot = std::find_if(app.depots.begin(), app.depots.end(),
                                        [&](const auto &entry) { return entry.id == installed.id; });
        if (depot == app.depots.end()) continue;
        std::optional<std::uint64_t> current;
        if (branch == "public") current = depot->manifest_gid;
        else if (const auto found = depot->branches.find(branch); found != depot->branches.end()) current = found->second.gid;
        if (current && *current != 0 && *current != installed.gid) {
            LogNetworkLine("update app=" + std::to_string(app_id) + " depot=" + std::to_string(installed.id) +
                           " installed=" + std::to_string(installed.gid) + " current=" + std::to_string(*current));
            return true;
        }
    }
    return false;
}

bool MissingSelectedDepots(std::uint32_t app_id, const playden::steam::AppInfo &app)
{
    const auto record = playden::installs::ReadInstallRecord(
        GameInstallRoot(app_id) + "/" + playden::installs::kInstallRecordName);
    if (!record || record->depots.empty()) return false;
    // Ask the same selector used by installs which base depots belong to this
    // record's branch and language. Borrowed manifests live in the source
    // app's listing, so provide a placeholder solely for this comparison.
    auto selection_app = app;
    for (auto &depot : selection_app.depots) {
        if (!depot.depot_from_app) continue;
        if (record->branch == "public") depot.manifest_gid = 1;
        else depot.branches[record->branch] = {1, 0, 0, false};
    }
    playden::steam::SelectionResult selected;
    playden::steam::DepotSelection choice;
    choice.branch = record->branch;
    choice.language = record->language;
    choice.excluded_dlc.insert(record->excluded_dlc.begin(), record->excluded_dlc.end());
    if (!playden::steam::SelectDepots(selection_app, {}, {app_id}, std::nullopt, choice, &selected))
        return false;
    return std::any_of(selected.depots.begin(), selected.depots.end(), [&](const auto &depot) {
        return std::none_of(record->depots.begin(), record->depots.end(),
            [&](const auto &entry) { return entry.id == depot.id; });
    });
}

bool HasIncompleteInstall(std::uint32_t app_id)
{
    if (app_id == 0) return false;
    const auto root = GameInstallRoot(app_id);
    struct stat info {};
    if (lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    if (lstat((root + "/" + playden::steam::DepotInstaller::kJournalDir).c_str(), &info) == 0)
        return true;
    if (HasInstalledRecord(app_id)) return false;
    return lstat((root + "/" + playden::steam::DepotInstaller::kFileList).c_str(), &info) == 0 ||
           lstat((root + "/" + playden::steam::DepotInstaller::kFileTree).c_str(), &info) == 0;
}

// The install sheet's version and language for a first install, kept until that install finishes.
constexpr const char *kInstallChoiceDir = "/data/PLDN10001/install-choices";

std::string InstallChoicePath(std::uint32_t app_id)
{
    return std::string(kInstallChoiceDir) + "/" + std::to_string(app_id);
}

void SaveInstallChoice(std::uint32_t app_id, const playden::steam::InstallOptions &options)
{
    if (mkdir(kInstallChoiceDir, 0777) != 0 && errno != EEXIST) return;
    std::FILE *file = std::fopen(InstallChoicePath(app_id).c_str(), "w");
    if (!file) return;
    std::fprintf(file, "%s\n%s\n", options.branch.c_str(), options.language.c_str());
    std::fclose(file);
}

bool LoadInstallChoice(std::uint32_t app_id, playden::steam::InstallOptions *options)
{
    std::FILE *file = std::fopen(InstallChoicePath(app_id).c_str(), "r");
    if (!file) return false;
    char branch[128] = {}, language[128] = {};
    const bool read = std::fgets(branch, sizeof(branch), file) && std::fgets(language, sizeof(language), file);
    std::fclose(file);
    if (!read) return false;
    branch[std::strcspn(branch, "\r\n")] = '\0';
    language[std::strcspn(language, "\r\n")] = '\0';
    options->branch = branch;
    options->language = language;
    return true;
}

void ForgetInstallChoice(std::uint32_t app_id)
{
    std::remove(InstallChoicePath(app_id).c_str());
}

std::string InstalledMessage(std::uint32_t app_id)
{
    (void)app_id;
    return "Game files are installed. Press Cross to try launching.";
}

std::string InstalledDetailsMessage(std::uint32_t app_id, std::size_t windows_depots)
{
    std::string message = "Windows launch option found; " + std::to_string(windows_depots) +
                          " Windows depots listed. Game files are installed. ";
    message += app_id == 220 ? "Wine runtime ready; press Cross to play."
                            : "Press Cross to try launching with the Wine runtime.";
    return message;
}

bool CleanWindowsRelativePath(std::string &path)
{
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos)
        return false;
    for (char &ch : path) {
        if (ch == '/') ch = '\\';
        if (static_cast<unsigned char>(ch) < 0x20 || ch == '=' || ch == '\"') return false;
    }
    while (path.rfind(".\\", 0) == 0) path.erase(0, 2);
    if (path.empty()) return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find('\\', start);
        const std::string_view component(path.data() + start,
            (end == std::string::npos ? path.size() : end) - start);
        if (component.empty() || component == "." || component == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::string LaunchExecutableRelative(std::uint32_t app_id)
{
    if (app_id == 220) return "hl2.exe";
    char profile[256];
    std::snprintf(profile, sizeof(profile), "/data/PLDN10001/profiles/%u.profile", app_id);
    FILE *input = std::fopen(profile, "rb");
    if (!input) return {};
    std::string executable;
    char line[2048];
    static constexpr char key[] = "executable = ";
    while (std::fgets(line, sizeof(line), input)) {
        if (std::strncmp(line, key, sizeof(key) - 1) != 0) continue;
        executable.assign(line + sizeof(key) - 1);
        while (!executable.empty() && (executable.back() == '\r' || executable.back() == '\n'))
            executable.pop_back();
        break;
    }
    std::fclose(input);
    const std::string expected_root = "Z:\\data\\PLDN10001\\games\\" + std::to_string(app_id) + "\\";
    if (executable.size() <= expected_root.size()) return {};
    for (std::size_t i = 0; i < expected_root.size(); ++i) {
        char a = executable[i], b = expected_root[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return {};
    }
    std::string relative = executable.substr(expected_root.size());
    if (!CleanWindowsRelativePath(relative)) return {};
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

// The installed spelling of a game-relative path ('/' separated), from the installer's file list
// (.playden-files: "f<TAB>path" per file). The console's filesystem is case-sensitive and the title
// may not list folders (opendir fails with EPERM), so Steam's "Mafia/game.exe" is found only through
// the list as "Mafia/Game.exe". directory: match a folder that holds an installed file. Empty when
// nothing matches.
std::string InstalledSpelling(std::uint32_t app_id, const std::string &relative, bool directory = false)
{
    if (relative.empty()) return {};
    const auto lower = [](std::string text) {
        for (char &ch : text)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        return text;
    };
    const std::string wanted = lower(relative);
    FILE *list = std::fopen((GameInstallRoot(app_id) + "/" + playden::steam::DepotInstaller::kFileList).c_str(), "rb");
    if (!list) return {};
    std::string found;
    char line[4096];
    while (found.empty() && std::fgets(line, sizeof(line), list)) {
        if (line[0] != 'f' || line[1] != '\t') continue;
        std::string path(line + 2);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        if (path.size() < wanted.size()) continue;
        const std::string head = lower(path.substr(0, wanted.size()));
        if (head != wanted) continue;
        if (!directory && path.size() == wanted.size()) found = path;
        else if (directory && path.size() > wanted.size() && path[wanted.size()] == '/')
            found = path.substr(0, wanted.size());
    }
    std::fclose(list);
    return found;
}

std::string InstalledLaunchExecutable(std::uint32_t app_id, const std::string &requested = {})
{
    const std::string asked = requested.empty() ? LaunchExecutableRelative(app_id) : requested;
    if (asked.empty()) return {};
    // Steam's spelling may differ in case from the file on disk; use the installed one.
    const std::string spelled = InstalledSpelling(app_id, asked);
    const std::string relative = spelled.empty() ? asked : spelled;
    std::string path = GameInstallRoot(app_id);
    std::size_t start = 0;
    while (start < relative.size()) {
        const auto slash = relative.find('/', start);
        const std::string component = relative.substr(start, slash - start);
        std::string candidate = path + "/" + component;
        struct stat info {};
        if (lstat(candidate.c_str(), &info) != 0) {
            // Steam's launch spelling may differ only in case from a depot's
            // file name. Wine resolves that; the install check must as well.
            DIR *directory = opendir(path.c_str());
            if (!directory) return {};
            bool found = false;
            while (const auto *entry = readdir(directory)) {
                const std::string_view name(entry->d_name);
                if (name.size() != component.size()) continue;
                bool equal = true;
                for (std::size_t i = 0; i < name.size(); ++i) {
                    const auto lower = [](unsigned char ch) {
                        return ch >= 'A' && ch <= 'Z' ? static_cast<unsigned char>(ch - 'A' + 'a') : ch;
                    };
                    if (lower(name[i]) != lower(component[i])) { equal = false; break; }
                }
                if (!equal) continue;
                candidate = path + "/" + std::string(name);
                found = lstat(candidate.c_str(), &info) == 0;
                if (found) break;
            }
            closedir(directory);
            if (!found) return {};
        }
        if (slash == std::string::npos) return S_ISREG(info.st_mode) ? candidate : std::string();
        if (!S_ISDIR(info.st_mode)) return {};
        path = std::move(candidate);
        start = slash + 1;
    }
    return {};
}

bool HasInstalledLaunchExecutable(std::uint32_t app_id)
{
    return !InstalledLaunchExecutable(app_id).empty();
}

bool WriteSteamLaunchProfile(std::uint32_t app_id, const std::string &title,
                             const playden::steam::AppLaunch &launch)
{
    std::string executable = launch.executable;
    if (!CleanWindowsRelativePath(executable) || launch.arguments.find_first_of("\r\n") != std::string::npos ||
        launch.working_directory.find_first_of("\r\n") != std::string::npos)
        return false;
    std::string working = launch.working_directory;
    // Steam appinfo commonly spells a directory with a trailing separator
    // (Mafia uses "Mafia/"). A directory is not a path component after that
    // separator, so trim it before validating the relative path.
    if (!working.empty()) {
        while (!working.empty() && (working.back() == '/' || working.back() == '\\'))
            working.pop_back();
        if (working.empty() || !CleanWindowsRelativePath(working)) return false;
    }
    // Write the installed spelling: the console's filesystem is case-sensitive (Mafia's Steam option
    // says Mafia\game.exe for Mafia/Game.exe), and both the install check and the shared recipe
    // look the program up by exact name.
    const auto respell = [app_id](std::string *path, bool directory) {
        std::string slashed = *path;
        std::replace(slashed.begin(), slashed.end(), '\\', '/');
        std::string spelled = InstalledSpelling(app_id, slashed, directory);
        if (spelled.empty()) return;
        std::replace(spelled.begin(), spelled.end(), '/', '\\');
        *path = spelled;
    };
    respell(&executable, false);
    if (!working.empty()) respell(&working, true);
    char app_root[256], profile_path[256], temporary[272];
    std::snprintf(app_root, sizeof(app_root), "Z:\\data\\PLDN10001\\games\\%u", app_id);
    std::snprintf(profile_path, sizeof(profile_path), "/data/PLDN10001/profiles/%u.profile", app_id);
    std::snprintf(temporary, sizeof(temporary), "%s.tmp", profile_path);
    if (mkdir("/data/PLDN10001/profiles", 0777) != 0 && errno != EEXIST) return false;
    FILE *output = std::fopen(temporary, "wb");
    if (!output) return false;
    const std::string game_path = std::string(app_root) + "\\" + executable;
    const std::string working_path = working.empty() ? app_root : std::string(app_root) + "\\" + working;
    std::string safe_title = title;
    for (char &ch : safe_title)
        if (ch == '\r' || ch == '\n' || static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    if (safe_title.empty()) safe_title = "Steam app " + std::to_string(app_id);
    const std::string existing_prefix = ProfilePrefixName(app_id);
    const std::string prefix_name = existing_prefix.empty()
        ? "steam-" + std::to_string(app_id) : existing_prefix;
    // Prefer the shared settings model: the profile then carries the player's saved controls and
    // other choices, and a later visit or launch does not undo them.
    {
        std::string shared_reason;
        const playden::ps4::LaunchSpec spec{executable, working, launch.arguments};
        if (playden::ps4::WriteSharedLaunchProfile(app_id, safe_title, spec, prefix_name, &shared_reason)) {
            std::fclose(output);
            std::remove(temporary);
            LogNetworkLine("profile app=" + std::to_string(app_id) + " source=settings prefix=" + prefix_name);
            return true;
        }
        LogNetworkLine("profile app=" + std::to_string(app_id) + " source=plain reason=" + shared_reason);
        std::string slashed = executable;
        std::replace(slashed.begin(), slashed.end(), '\\', '/');
        LogNetworkLine("profile app=" + std::to_string(app_id) + " path-check" +
                       playden::ps4::DescribeGamePath(app_id, slashed));
    }
    // Prefer the installed PE header; Steam's osarch is only a hint before
    // download or when the executable has not yet been placed on disk.
    std::string relative = executable;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const std::string installed = InstalledLaunchExecutable(app_id, relative);
    const std::string architecture = playden::runtime::LaunchArchitecture(installed, launch.os_arch);
    const bool okay = std::fprintf(output,
        "[application]\n"
        "id = %u\n"
        "name = %s\n"
        "executable = %s\n"
        "working_directory = %s\n"
        "arguments = %s\n"
        "dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi=n\n"
        "prefix = %s\n"
        "runtime = wine-wow64\n"
        "architecture = %s\n"
        "graphics = dxvk\n\n"
        "[display]\n"
        "desktop = 1920x1080\n"
        "scaling = fit\n\n"
        "[input]\n"
        "mode = xinput\n",
        app_id, safe_title.c_str(), game_path.c_str(), working_path.c_str(),
        launch.arguments.c_str(), prefix_name.c_str(), architecture.c_str()) > 0;
    bool failed = !okay || std::fflush(output) != 0;
    if (std::fclose(output) != 0) failed = true;
    if (failed || std::rename(temporary, profile_path) != 0) {
        std::remove(temporary);
        return false;
    }
    return true;
}

std::mutex network_log_mutex;
bool network_log_warning_printed = false;
std::string library_detail_buffer;
bool EnsureSharedGamesDirectory()
{
    // Playden-owned Steam payloads remain isolated from Orbis-Win.
    constexpr const char *path = "/data/PLDN10001/games";
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        return false;
    // GoldHEN FTP and another title run outside this title's private UID context. Game content
    // is intentionally shareable; auth/session files remain private in the parent directory.
    return chmod(path, 0777) == 0;
}

void OrbisCompatLog(const char *format, va_list args)
{
    char detail[640]{};
    std::vsnprintf(detail, sizeof(detail), format, args);
    LogNetworkLine(std::string("sdk ") + detail);
}

void OrbisCompatFatalLog(const char *format, va_list args)
{
    // Keep the fatal channel independent of mutexes, C++ allocation and stdio: this can run from a
    // signal handler, including when the ordinary logger or its lock was the fault site. Preserve
    // the same record in the downloadable log; the normal logger's mutex is unsafe in this path.
    char line[768]{};
    std::vsnprintf(line, sizeof(line), format, args);
    char record[800]{};
    const int length = std::snprintf(record, sizeof(record), "PLDN10001 fatal: %s\n", line);
    if (length <= 0) return;
    sceKernelDebugOutText(0, record);
    const int fd = open("/data/PLDN10001-network.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    std::size_t offset = 0;
    const auto count = std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(record) - 1);
    while (offset < count) {
        const auto written = write(fd, record + offset, count - offset);
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    close(fd);
}

void OrbisCompatTerminate() noexcept
{
    char message[512] = "std::terminate with no active exception";
    if (const auto current = std::current_exception())
    {
        try { std::rethrow_exception(current); }
        catch (const std::exception &error) { std::snprintf(message, sizeof(message), "%s", error.what()); }
        catch (...) { std::snprintf(message, sizeof(message), "non-standard exception"); }
    }
    auto fatal = [](const char *format, ...) {
        va_list args;
        va_start(args, format);
        OrbisCompatFatalLog(format, args);
        va_end(args);
    };
    fatal("fatal: unhandled exception - %s", message);
    void *frames[32]{};
    const int count = backtrace(frames, 32);
    if (count <= 0) fatal("fatal: terminate backtrace returned %d", count);
    for (int i = 0; i < count; ++i)
        fatal("fatal: terminate frame[%02d]=%016llx", i,
              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i])));
    orbis_fatal_action("terminate");
    _exit(1);
}

void LogThreadCounters(const char *phase)
{
    unsigned long created = 0, raised = 0, create_failed = 0, attr_failed = 0;
    unsigned long alt_installed = 0, alt_failed = 0, alt_skipped = 0;
    orbis::threadCounts(&created, &raised);
    orbis::threadFailures(&create_failed, &attr_failed);
    orbis::threadAltStacks(&alt_installed, &alt_failed, &alt_skipped);
    LogNetworkLine(std::string("sdk thread state phase=") + phase +
        " default=" + std::to_string(orbis::threadDefaultStack()) +
        " floor=" + std::to_string(orbis::threadStackFloor()) +
        " created=" + std::to_string(created) + " raised=" + std::to_string(raised) +
        " create_failed=" + std::to_string(create_failed) + " attr_failed=" + std::to_string(attr_failed) +
        " altstack=" + std::to_string(alt_installed) + "/" + std::to_string(alt_failed) +
        " skipped=" + std::to_string(alt_skipped));
}

void LogNetworkLine(const std::string &line)
{
    const std::lock_guard<std::mutex> guard(network_log_mutex);
    std::string record = "PLDN10001 net: " + line + "\n";
    sceKernelDebugOutText(0, record.c_str());
    FILE *file = std::fopen("/data/PLDN10001-network.log", "a");
    if (file != nullptr)
    {
        std::fwrite(record.data(), 1, record.size(), file);
        std::fflush(file);
        std::fclose(file);
    }
    else if (!network_log_warning_printed)
    {
        // Keep klog as the fallback if app data isn't writable on this firmware.
        sceKernelDebugOutText(0, "PLDN10001 net: file log unavailable; klog remains active\n");
        network_log_warning_printed = true;
    }
}
}

namespace playden::ps4
{

class SteamQrAuth::SessionMemory final : public playden::SessionRepository
{
  public:
    SessionMemory() : storage_("/data/PLDN10001"), store_(storage_) { storage_.AllowTraversableRoot(); }

    playden::Result<void> EnsureRoot() const { return storage_.EnsureRootDirectory(); }

    playden::Result<std::optional<playden::StoredSession>> Load() const override
    {
        return store_.Load();
    }

    playden::Result<void> Save(const playden::StoredSession &session) const override
    {
        return store_.Save(session);
    }

    playden::Result<void> Clear() const override
    {
        return store_.Clear();
    }

  private:
    playden::Storage storage_;
    playden::SessionStore store_;
};

SteamQrAuth::SteamQrAuth()
{
    {
        playden::Storage storage("/data/PLDN10001");
        storage.AllowTraversableRoot();
        const auto saved = storage.Read("settings.txt", 256);
        if (saved.ok())
        {
            const std::string text(saved.value().begin(), saved.value().end());
            const auto language = text.find("language=");
            if (language != std::string::npos)
            {
                const auto start = language + 9;
                const auto end = text.find_first_of("\r\n", start);
                const std::string value = text.substr(start, end == std::string::npos ? end : end - start);
                if (playden::steam::IsContentLanguage(value)) content_language_ = value;
            }
            const auto receiver_ip = text.find("receiver_ip=");
            if (receiver_ip != std::string::npos)
            {
                const auto start = receiver_ip + 12;
                const auto end = text.find_first_of("\r\n", start);
                const std::string value = text.substr(start, end == std::string::npos ? end : end - start);
                if (IsIpv4Address(value)) log_receiver_ip_ = value;
            }
            const auto enabled = [&text](const char *key) {
                const std::string prefix = std::string(key) + "=";
                std::size_t cursor = 0;
                while (cursor < text.size()) {
                    const auto end = text.find_first_of("\r\n", cursor);
                    const auto length = end == std::string::npos ? text.size() - cursor : end - cursor;
                    if (text.compare(cursor, prefix.size(), prefix) == 0)
                        return text.compare(cursor + prefix.size(), length - prefix.size(), "1") == 0;
                    cursor = end == std::string::npos ? text.size() : end + 1;
                }
                return false;
            };
            wine_debug_level_ = enabled("wine_debug")
                ? "detailed" : "profile";
            wait_watchdog_enabled_ = enabled("wait_watchdog");
            gpu_hang_dump_enabled_ = enabled("gpu_hang_dump");
        }

        const auto runtime_saved = storage.Read("settings.conf", 1024);
        if (runtime_saved.ok())
        {
            const std::string text(runtime_saved.value().begin(), runtime_saved.value().end());
            const auto value_for = [&text](const char *key) -> std::string {
                const std::string prefix = std::string(key) + "=";
                std::size_t cursor = 0;
                while (cursor < text.size())
                {
                    const auto end = text.find_first_of("\r\n", cursor);
                    const auto length = end == std::string::npos ? text.size() - cursor : end - cursor;
                    if (text.compare(cursor, prefix.size(), prefix) == 0)
                        return text.substr(cursor + prefix.size(), length - prefix.size());
                    cursor = end == std::string::npos ? text.size() : end + 1;
                }
                return {};
            };
            const std::string logging = value_for("log");
            if (logging == "0" || logging == "1") logging_enabled_ = logging == "1";
            const std::string server = value_for("log_server");
            if (IsIpv4Address(server)) log_receiver_ip_ = server;
            const std::string port = value_for("log_port");
            if (!port.empty())
            {
                const long value = std::strtol(port.c_str(), nullptr, 10);
                if (value > 0 && value <= 65535) log_receiver_port_ = static_cast<int>(value);
            }
            std::string wine_log = value_for("wine_log");
            if (wine_log.empty()) wine_log = value_for("wine_debug");
            if (wine_log == "profile" || wine_log == "quiet" || wine_log == "detailed")
                wine_debug_level_ = wine_log;
            else if (wine_log == "off" || wine_log == "0")
                wine_debug_level_ = "profile";
            else if (wine_log == "err+all,+loaddll,+process,+seh" || wine_log == "1")
                wine_debug_level_ = "detailed";
            const std::string watchdog = value_for("wait_watchdog");
            if (watchdog == "0" || watchdog == "1") wait_watchdog_enabled_ = watchdog == "1";
            const std::string gpu = value_for("gpu_hang_report");
            if (gpu == "0" || gpu == "1") gpu_hang_dump_enabled_ = gpu == "1";
            // The old parallel-download preference is intentionally ignored until
            // the PS4 allocator passes a full multi-connection hardware run.
        }
        else
        {
            // Upgrade existing Playden installs without losing their receiver IP
            // or diagnostic toggles from the old private settings file.
            (void)persist_runtime_settings_locked();
        }

        const auto organization = storage.Read("library-organization.txt", 32768);
        if (organization.ok())
        {
            const std::string text(organization.value().begin(), organization.value().end());
            std::size_t cursor = 0;
            while (cursor < text.size())
            {
                const auto line_end = text.find_first_of("\r\n", cursor);
                const auto length = line_end == std::string::npos ? text.size() - cursor : line_end - cursor;
                const std::string line = text.substr(cursor, length);
                std::set<std::uint32_t> *destination = nullptr;
                std::size_t value_start = 0;
                if (line.compare(0, 10, "favorites=") == 0) {
                    destination = &favorite_games_;
                    value_start = 10;
                } else if (line.compare(0, 7, "hidden=") == 0) {
                    destination = &hidden_games_;
                    value_start = 7;
                }
                if (destination)
                {
                    std::size_t item_start = value_start;
                    while (item_start < line.size())
                    {
                        const auto item_end = line.find(',', item_start);
                        const auto item_length = (item_end == std::string::npos ? line.size() : item_end) - item_start;
                        if (item_length > 0 && item_length < 11)
                        {
                            const std::string item = line.substr(item_start, item_length);
                            char *end = nullptr;
                            const unsigned long value = std::strtoul(item.c_str(), &end, 10);
                            if (end && *end == '\0' && value > 0 && value <= UINT32_MAX)
                                destination->insert(static_cast<std::uint32_t>(value));
                        }
                        if (item_end == std::string::npos) break;
                        item_start = item_end + 1;
                    }
                }
                cursor = line_end == std::string::npos ? text.size() : line_end + 1;
                while (cursor < text.size() && (text[cursor] == '\r' || text[cursor] == '\n')) ++cursor;
            }
        }
        playden::OrganizationStore shared_organization(storage);
        auto saved_organization = shared_organization.Load(playden::kDeviceOrganization);
        if (!saved_organization.ok()) {
            // With no organization.v2 yet, Load lists /data/PLDN10001 for the PS5's per-account .v1
            // files, and the title may not list /data (opendir EPERM). The PS4 never wrote .v1 files,
            // so a missing organization.v2 is a first start: begin a new one from favorites and hidden.
            struct stat snapshot_info {};
            const bool first_start = ::lstat("/data/PLDN10001/organization.v2", &snapshot_info) != 0 &&
                                     errno == ENOENT;
            LogNetworkLine(std::string("library organization load failed: ") + saved_organization.message() +
                           (first_start ? " (no organization.v2: starting a new one)" : ""));
            if (first_start)
                saved_organization =
                    playden::Result<std::optional<playden::OrganizationSnapshot>>::Success(std::nullopt);
        }
        if (saved_organization.ok()) {
            if (saved_organization.value()) {
                organization_ = *saved_organization.value();
                favorite_games_.clear();
                hidden_games_.clear();
                for (const auto &id : organization_.favorites)
                    if (const auto steam = id.steam_app()) favorite_games_.insert(*steam);
                for (const auto &id : organization_.hidden)
                    if (const auto steam = id.steam_app()) hidden_games_.insert(*steam);
                organization_ready_ = true;
            } else {
                for (const auto id : favorite_games_)
                    organization_.favorites.insert(playden::GameId::Steam(id));
                for (const auto id : hidden_games_)
                    organization_.hidden.insert(playden::GameId::Steam(id));
                const auto saved = shared_organization.Save(playden::kDeviceOrganization, organization_);
                organization_ready_ = saved.ok();
                if (!saved.ok()) LogNetworkLine("library organization save failed: " + saved.message());
            }
        }
        LogNetworkLine(std::string("library organization ready=") + (organization_ready_ ? "yes" : "no") +
                       " collections=" + std::to_string(organization_.collections.size()));
    }
    orbis_set_log(OrbisCompatLog);
    orbis_set_log_fatal(OrbisCompatFatalLog);
    orbis::installCrashHandlers();
    std::set_terminate(OrbisCompatTerminate);
    orbis::threadStackProbe();
    LogThreadCounters("startup");
    ProbeHeapCapacity();
    playden::ps4::InstallControllerPresets();
    store_details_service_ = new playden::steam::StoreDetailsService(
        playden::homebrew::StoreFetch("/app0/assets/cacert.pem"));
}

SteamQrAuth::~SteamQrAuth()
{
    if (library_service_) library_service_->Shutdown();
    updates_cancel_.store(true);
    if (updates_worker_.joinable()) updates_worker_.join();
    details_cancel_.store(true);
    install_cancel_.store(true);
    saves_cancel_.store(true);
    if (details_worker_.joinable())
        details_worker_.join();
    if (install_worker_.joinable())
        install_worker_.join();
    if (saves_worker_.joinable())
        saves_worker_.join();
    if (uninstall_worker_.joinable())
        uninstall_worker_.join();
    delete store_details_service_;
    store_details_service_ = nullptr;
    library_service_.reset();
    entitled_library_.reset();
    library_cm_.reset();
    library_backend_.reset();
    library_cache_.reset();
    library_storage_.reset();
    delete service_;
    delete backend_;
    delete repository_;
}

void SteamQrAuth::start_library_fetch()
{
    LogNetworkLine("library startup step=begin");
    if (!library_service_) {
        library_detail_ = "Library service is unavailable.";
        state_ = 7;
        return;
    }
    auto loaded = repository_->Load();
    LogNetworkLine(std::string("library startup step=session result=") +
                   (loaded.ok() && loaded.value().has_value() ? "ready" : "unavailable"));
    if (!loaded.ok() || !loaded.value().has_value())
    {
        library_detail_ = "Sign-in session is unavailable";
        state_ = 7;
        return;
    }
    library_detail_ = "Loading your Steam library…";
    state_ = 5;
    library_request_ = library_service_->Refresh(loaded.value()->steam_id, 1);
    LogNetworkLine("library startup step=refresh request=" + std::to_string(library_request_));
    playden::steam::ZeroizeSecret(loaded.value()->refresh_token);
    playden::steam::ZeroizeSecret(loaded.value()->access_token);
    loaded.value()->account_name.clear();
    if (!library_request_) {
        library_detail_ = "Could not start the library refresh.";
        state_ = 7;
    }
}

std::size_t SteamQrAuth::library_count() const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return games_.size();
}

std::uint32_t SteamQrAuth::library_app_id(std::size_t index) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    if (index >= games_.size()) return 0;
    return games_[index].id.steam_app().value_or(0);
}

std::size_t SteamQrAuth::library_visible_count(int filter) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return visible_library_indices_locked(filter).size();
}

const char *SteamQrAuth::library_visible_title(int filter, std::size_t index) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    const auto indices = visible_library_indices_locked(filter);
    return index < indices.size() ? games_[indices[index]].title.c_str() : "";
}

std::uint32_t SteamQrAuth::library_visible_app_id(int filter, std::size_t index) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    const auto indices = visible_library_indices_locked(filter);
    return index < indices.size() ? games_[indices[index]].id.steam_app().value_or(0) : 0;
}

namespace {
// The Library tab's sorts, by the names the organization file keeps (OrganizationSnapshot::sort).
struct LibrarySortName { const char *id; const char *label; };
constexpr LibrarySortName kLibrarySorts[] = {
    {"name", "Name"}, {"recent-played", "Recently played"}, {"recent-added", "Recently added"},
    {"playtime", "Most played"}};
constexpr int kLibrarySortCount = static_cast<int>(sizeof(kLibrarySorts) / sizeof(kLibrarySorts[0]));
// Filter numbers: 0 is Home's installed list (no search, Steam's order). The Library tab passes
// kLibraryView + its row: 0 Installed, 1 All games, 2 Favorites, 3 Hidden, then one row per collection.
constexpr int kLibraryView = 1000;
} // namespace

std::vector<std::size_t> SteamQrAuth::visible_library_indices_locked(int filter) const
{
    std::vector<std::size_t> indices;
    const bool library_view = filter >= kLibraryView;
    const int row = library_view ? filter - kLibraryView : filter;
    const std::size_t collections = organization_.collections.size();
    if (row < 0 || static_cast<std::size_t>(row) >= 4 + (library_view ? collections : 0)) return indices;
    const playden::OrganizationCollection *collection =
        row >= 4 ? &organization_.collections[static_cast<std::size_t>(row - 4)] : nullptr;
    const std::string query = library_view ? playden::FoldLibraryText(organization_.query) : std::string();
    for (std::size_t i = 0; i < games_.size(); ++i)
    {
        const std::uint32_t app_id = games_[i].id.steam_app().value_or(0);
        if (app_id == 0) continue;
        const bool hidden = hidden_games_.contains(app_id);
        const bool favorite = favorite_games_.contains(app_id);
        const bool installed = std::find(installed_games_.begin(), installed_games_.end(), i) != installed_games_.end();
        bool shown = false;
        if (collection) shown = !hidden && collection->members.contains(games_[i].id);
        else if (row == 0) shown = installed && !hidden;
        else if (row == 1) shown = !hidden;
        else if (row == 2) shown = favorite && !hidden;
        else if (row == 3) shown = hidden;
        if (shown && !query.empty() &&
            playden::FoldLibraryText(games_[i].title).find(query) == std::string::npos)
            shown = false;
        if (shown) indices.push_back(i);
    }
    if (!library_view) return indices;
    // Sorted by the chosen order, then by name; games without a date or playtime go last.
    std::vector<std::string> names(games_.size());
    for (const auto i : indices) names[i] = playden::FoldLibraryText(games_[i].title);
    const std::string sort = organization_.sort;
    const auto key = [&](std::size_t i) -> std::uint64_t {
        const auto &game = games_[i];
        if (sort == "recent-played") return game.last_played_unix_seconds.value_or(0);
        if (sort == "recent-added") return game.added_at_unix_seconds.value_or(0);
        if (sort == "playtime") return game.playtime_minutes.value_or(0);
        return 0;
    };
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        const auto ka = key(a), kb = key(b);
        if (ka != kb) return ka > kb;
        if (names[a] != names[b]) return names[a] < names[b];
        return a < b;
    });
    return indices;
}

std::string SteamQrAuth::library_query() const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return organization_.query;
}

bool SteamQrAuth::library_set_query(const std::string &query)
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    const auto first = query.find_first_not_of(' ');
    const std::string trimmed = first == std::string::npos ? std::string()
        : query.substr(first, query.find_last_not_of(' ') - first + 1);
    if (trimmed.size() > 256) return false;
    organization_.query = trimmed;
    return persist_library_organization_locked();
}

int SteamQrAuth::library_sort() const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    for (int i = 0; i < kLibrarySortCount; ++i)
        if (organization_.sort == kLibrarySorts[i].id) return i;
    return 0;
}

bool SteamQrAuth::library_cycle_sort()
{
    const int next = (library_sort() + 1) % kLibrarySortCount;
    const std::lock_guard<std::mutex> guard(library_mutex_);
    organization_.sort = kLibrarySorts[next].id;
    return persist_library_organization_locked();
}

std::size_t SteamQrAuth::library_collection_count() const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return organization_.collections.size();
}

std::string SteamQrAuth::library_collection_name(std::size_t index) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return index < organization_.collections.size() ? organization_.collections[index].name : std::string();
}

std::size_t SteamQrAuth::library_collection_size(std::size_t index) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return visible_library_indices_locked(kLibraryView + 4 + static_cast<int>(index)).size();
}

int SteamQrAuth::library_create_collection(const std::string &name)
{
    const auto first = name.find_first_not_of(" \t");
    if (first == std::string::npos) return -1;
    const std::string trimmed = name.substr(first, name.find_last_not_of(" \t") - first + 1);
    if (trimmed.size() > 128) return -1;
    const std::lock_guard<std::mutex> guard(library_mutex_);
    if (!organization_ready_ || organization_.collections.size() >= 256) return -1;
    for (std::size_t i = 0; i < organization_.collections.size(); ++i)
        if (organization_.collections[i].name == trimmed) return static_cast<int>(i);
    // Ids are letters, digits, '-' and '_' (OrganizationSnapshotIsValid); "c-<n>", the first one free.
    std::string id;
    for (unsigned n = 1; id.empty(); ++n) {
        const std::string candidate = "c-" + std::to_string(n);
        if (std::none_of(organization_.collections.begin(), organization_.collections.end(),
                         [&](const auto &c) { return c.id == candidate; }))
            id = candidate;
    }
    playden::OrganizationCollection collection;
    collection.id = id;
    collection.name = trimmed;
    organization_.collections.push_back(collection);
    if (!persist_library_organization_locked()) {
        organization_.collections.pop_back();
        return -1;
    }
    return static_cast<int>(organization_.collections.size() - 1);
}

bool SteamQrAuth::library_delete_collection(std::size_t index)
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    if (index >= organization_.collections.size()) return false;
    const auto removed = organization_.collections[index];
    organization_.collections.erase(organization_.collections.begin() + static_cast<std::ptrdiff_t>(index));
    // A scope naming the collection would make the file invalid.
    const std::string scope = organization_.scope;
    if (scope == removed.id) organization_.scope = "all";
    if (persist_library_organization_locked()) return true;
    organization_.collections.insert(organization_.collections.begin() + static_cast<std::ptrdiff_t>(index), removed);
    organization_.scope = scope;
    return false;
}

bool SteamQrAuth::library_in_collection(std::uint32_t app_id, std::size_t index) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return index < organization_.collections.size() &&
           organization_.collections[index].members.contains(playden::GameId::Steam(app_id));
}

bool SteamQrAuth::library_toggle_collection(std::uint32_t app_id, std::size_t index)
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    if (index >= organization_.collections.size() || app_id == 0) return false;
    auto &members = organization_.collections[index].members;
    const auto id = playden::GameId::Steam(app_id);
    const bool added = members.insert(id).second;
    if (!added) members.erase(id);
    if (persist_library_organization_locked()) return true;
    if (added) members.erase(id);
    else members.insert(id);
    return false;
}

bool SteamQrAuth::library_is_family(std::uint32_t app_id) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    for (const auto &game : games_)
        if (game.id.steam_app().value_or(0) == app_id) return game.family_shared;
    return false;
}

bool SteamQrAuth::library_is_favorite(std::uint32_t app_id) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return favorite_games_.contains(app_id);
}

bool SteamQrAuth::library_is_hidden(std::uint32_t app_id) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return hidden_games_.contains(app_id);
}

bool SteamQrAuth::persist_library_organization_locked()
{
    playden::Storage storage("/data/PLDN10001");
    storage.AllowTraversableRoot();
    if (!storage.EnsureRootDirectory().ok()) return false;
    if (organization_ready_) {
        auto next = organization_;
        std::erase_if(next.favorites, [](const auto &id) { return id.steam_app().has_value(); });
        std::erase_if(next.hidden, [](const auto &id) { return id.steam_app().has_value(); });
        for (const auto id : favorite_games_) next.favorites.insert(playden::GameId::Steam(id));
        for (const auto id : hidden_games_) next.hidden.insert(playden::GameId::Steam(id));
        playden::OrganizationStore store(storage);
        if (!store.Save(playden::kDeviceOrganization, next).ok()) return false;
        organization_ = std::move(next);
        return true;
    }
    std::string text = "favorites=";
    for (const auto id : favorite_games_)
    {
        if (text.back() != '=') text += ',';
        text += std::to_string(id);
    }
    text += "\nhidden=";
    for (const auto id : hidden_games_)
    {
        if (text.back() != '=') text += ',';
        text += std::to_string(id);
    }
    text += '\n';
    return storage.WriteAtomicPrivate("library-organization.txt", playden::Bytes(text.begin(), text.end())).ok();
}

bool SteamQrAuth::library_toggle_favorite(std::uint32_t app_id)
{
    if (app_id == 0) return false;
    const std::lock_guard<std::mutex> guard(library_mutex_);
    const bool was_favorite = favorite_games_.contains(app_id);
    if (was_favorite) favorite_games_.erase(app_id);
    else favorite_games_.insert(app_id);
    if (persist_library_organization_locked()) return true;
    if (was_favorite) favorite_games_.insert(app_id);
    else favorite_games_.erase(app_id);
    return false;
}

bool SteamQrAuth::library_toggle_hidden(std::uint32_t app_id)
{
    if (app_id == 0) return false;
    const std::lock_guard<std::mutex> guard(library_mutex_);
    const bool was_hidden = hidden_games_.contains(app_id);
    if (was_hidden) hidden_games_.erase(app_id);
    else hidden_games_.insert(app_id);
    if (persist_library_organization_locked()) return true;
    if (was_hidden) hidden_games_.insert(app_id);
    else hidden_games_.erase(app_id);
    return false;
}

std::string SteamQrAuth::library_detail() const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return library_detail_;
}

int SteamQrAuth::game_details_state() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return details_state_;
}

bool SteamQrAuth::game_details_installed() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return details_installed_;
}

void SteamQrAuth::start_update_check(std::vector<std::uint32_t> installed)
{
    updates_started_ = true;
    if (installed.empty()) return;
    auto loaded = repository_ ? repository_->Load() :
        playden::Result<std::optional<playden::StoredSession>>::Failure(
            playden::StorageError::kNotFound, "No session");
    if (!loaded.ok() || !loaded.value().has_value()) return;
    playden::StoredSession session = std::move(*loaded.value());
    updates_cancel_.store(false);
    updates_worker_ = std::thread([this, installed = std::move(installed), session = std::move(session)]() mutable {
        playden::Storage storage("/data/PLDN10001");
        storage.AllowTraversableRoot();
        std::set<std::uint32_t> found;
        std::string outcome = "ok";
        if (!storage.EnsureRootDirectory().ok()) {
            outcome = "storage";
        } else {
            playden::steam::CmClient client("/app0/assets/cacert.pem", storage);
            client.SetOwner("updates");
            const auto buffers = client.FetchAppInfo(session, installed, updates_cancel_);
            if (buffers.error != playden::steam::CmError::kNone) {
                outcome = playden::steam::CmErrorName(buffers.error);
            } else {
                for (const auto id : installed) {
                    const auto buffer = buffers.buffers.find(id);
                    if (buffer == buffers.buffers.end()) continue;
                    if (const auto app = playden::steam::ParseAppInfo(id, buffer->second);
                        app && InstalledUpdateAvailable(id, *app))
                        found.insert(id);
                }
            }
        }
        playden::steam::ZeroizeSecret(session.refresh_token);
        playden::steam::ZeroizeSecret(session.access_token);
        std::string list;
        for (const auto id : found) list += (list.empty() ? "" : ",") + std::to_string(id);
        LogNetworkLine("updates checked=" + std::to_string(installed.size()) + " available=" +
                       std::to_string(found.size()) + (list.empty() ? "" : " apps=" + list) + " result=" + outcome);
        const std::lock_guard<std::mutex> guard(updates_mutex_);
        updates_.insert(found.begin(), found.end());
    });
}

bool SteamQrAuth::library_update_available(std::uint32_t app_id) const
{
    const std::lock_guard<std::mutex> guard(updates_mutex_);
    return updates_.count(app_id) != 0;
}

std::size_t SteamQrAuth::library_update_count() const
{
    const std::lock_guard<std::mutex> guard(updates_mutex_);
    return updates_.size();
}

// The sheet's sizes for its branch and language (details lock held), as OfferedBranches estimates them.
void SteamQrAuth::sheet_measure_locked()
{
    sheet_.download_bytes = sheet_.install_bytes = 0;
    sheet_.languages.clear();
    if (sheet_.offers.empty()) return;
    const auto &offer = sheet_.offers[sheet_.branch];
    sheet_.install_bytes = offer.install_bytes;
    sheet_.download_bytes = offer.download_bytes;
    for (const auto &language : offer.languages) sheet_.languages.push_back(language.language);
    if (sheet_.language >= sheet_.languages.size()) sheet_.language = 0;
    if (!offer.languages.empty()) {
        sheet_.install_bytes = offer.languages[sheet_.language].install_bytes;
        sheet_.download_bytes = offer.languages[sheet_.language].download_bytes;
    }
}

bool SteamQrAuth::install_sheet_open(std::uint32_t app_id)
{
    std::string language;
    {
        const std::lock_guard<std::mutex> guard(settings_mutex_);
        language = content_language_;
    }
    const std::lock_guard<std::mutex> guard(details_mutex_);
    if (!details_app_ || details_app_->app_id != app_id) return false;
    sheet_ = {};
    sheet_.app_id = app_id;
    // Public first, then each branch without a password that gives the game content of its own.
    sheet_.offers = playden::steam::OfferedBranches(*details_app_, {}, {app_id}, std::nullopt, language);
    if (sheet_.offers.empty()) sheet_.note = "Steam lists no Windows content for this game.";
    sheet_measure_locked();
    // The Settings language when the game has it; else English; else the first listed.
    const auto find = [this](const std::string &name) {
        for (std::size_t i = 0; i < sheet_.languages.size(); ++i)
            if (sheet_.languages[i] == name) return static_cast<long>(i);
        return -1L;
    };
    long chosen = find(language.empty() ? "english" : language);
    if (chosen < 0) chosen = find("english");
    sheet_.language = chosen < 0 ? 0 : static_cast<std::size_t>(chosen);
    sheet_measure_locked();
    return true;
}

void SteamQrAuth::install_sheet_step(int row, int direction)
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    const auto step = [direction](std::size_t &index, std::size_t count) {
        if (count < 2) return;
        const long n = static_cast<long>(count);
        index = static_cast<std::size_t>(((static_cast<long>(index) + direction) % n + n) % n);
    };
    if (row == 0) {
        // Keep the language across branches when the new branch has it.
        const std::string language = sheet_.languages.empty() ? std::string() : sheet_.languages[sheet_.language];
        step(sheet_.branch, sheet_.offers.size());
        sheet_.language = 0;
        sheet_measure_locked();
        for (std::size_t i = 0; i < sheet_.languages.size(); ++i)
            if (sheet_.languages[i] == language) sheet_.language = i;
    } else if (row == 1) {
        step(sheet_.language, sheet_.languages.size());
    }
    sheet_measure_locked();
}

std::string SteamQrAuth::install_sheet_value(int row) const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    const auto bytes = [](std::uint64_t value) {
        char text[32];
        if (value >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(value) / (1ull << 30));
        else std::snprintf(text, sizeof(text), "%llu MB", static_cast<unsigned long long>(value >> 20));
        return std::string(text);
    };
    switch (row) {
    case 0: {
        if (sheet_.offers.empty()) return "Public";
        const auto &offer = sheet_.offers[sheet_.branch];
        if (offer.name == "public") return "Public (current release)";
        return offer.description.empty() ? offer.name : offer.name + " - " + offer.description;
    }
    case 1: return sheet_.languages.empty() ? "As the game ships" : playden::steam::LanguageName(sheet_.languages[sheet_.language]);
    case 2: return sheet_.download_bytes ? "about " + bytes(sheet_.download_bytes) : "-";
    case 3: return sheet_.install_bytes ? "about " + bytes(sheet_.install_bytes) : "-";
    default: return {};
    }
}

std::string SteamQrAuth::install_sheet_note() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return sheet_.note;
}

bool SteamQrAuth::game_details_update() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return details_update_;
}

bool SteamQrAuth::game_details_incomplete() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return details_incomplete_;
}

bool SteamQrAuth::game_details_launch_supported() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return details_windows_launch_;
}

std::uint32_t SteamQrAuth::game_details_app_id() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return details_app_id_;
}

std::string SteamQrAuth::game_details_message() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return details_message_;
}

bool SteamQrAuth::game_store_details_ready() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_details_ready_;
}

bool SteamQrAuth::game_store_details_available() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_details_available_;
}

std::string SteamQrAuth::game_store_description() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_description_;
}

std::string SteamQrAuth::game_store_genres() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_genres_;
}

std::string SteamQrAuth::game_store_developers() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_developers_;
}

std::string SteamQrAuth::game_store_publishers() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_publishers_;
}

std::string SteamQrAuth::game_store_categories() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_categories_;
}

std::string SteamQrAuth::game_store_release_date() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_release_date_;
}

std::string SteamQrAuth::game_store_metacritic_score() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_metacritic_score_;
}

std::string SteamQrAuth::game_store_controller_support() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_controller_support_;
}

bool SteamQrAuth::game_store_windows_supported() const
{
    const std::lock_guard<std::mutex> guard(details_mutex_);
    return store_windows_supported_;
}

void SteamQrAuth::start_game_verify(std::uint32_t app_id)
{
    if (app_id == 0 || !HasInstalledRecord(app_id)) return;
    {
        const std::lock_guard<std::mutex> guard(details_mutex_);
        verify_requested_ = app_id;
    }
    start_game_install(app_id);
}

void SteamQrAuth::start_game_install(std::uint32_t app_id)
{
    if (app_id == 0) return;
    bool repair = false, verify = false;
    {
        const std::lock_guard<std::mutex> guard(details_mutex_);
        verify = verify_requested_ == app_id;
        verify_requested_ = 0;
        // An update downloads the changed files beside the game, then swaps them in (the shared UpdateApp).
        // Only a game with an install record can be updated or repaired: a first download cut short has
        // none yet, and resumes as an install (UpdateApp refuses it: "update without install").
        repair = verify || (details_app_id_ == app_id && (details_incomplete_ || details_update_) &&
                            HasInstalledRecord(app_id));
    }
    if (HasInstalledRecord(app_id) && !repair)
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        install_app_id_ = app_id;
        install_state_ = 2;
        install_message_ = InstalledMessage(app_id);
        ++install_version_;
        {
            const std::lock_guard<std::mutex> details_guard(details_mutex_);
            if (details_app_id_ == app_id && details_state_ == 2)
            {
                details_installed_ = true;
                details_message_ = InstalledDetailsMessage(app_id, details_windows_depots_);
            }
        }
        return;
    }
    const std::string install_root = GameInstallRoot(app_id);
    std::string unavailable_reason;
    {
        const std::lock_guard<std::mutex> guard(details_mutex_);
        if (details_app_id_ != app_id || details_state_ != 2)
            unavailable_reason = "Steam install details are still loading. Open the game again and retry.";
        else if (!details_windows_launch_ || details_windows_depots_ == 0)
            unavailable_reason = details_message_.empty()
                ? "No eligible Windows game files were found for this install."
                : details_message_;
    }
    if (!unavailable_reason.empty()) {
        LogNetworkLine("install app=" + std::to_string(app_id) +
                       " result=blocked reason=" + unavailable_reason);
        const std::lock_guard<std::mutex> guard(install_mutex_);
        install_app_id_ = app_id;
        install_state_ = 3;
        install_message_ = std::move(unavailable_reason);
        ++install_version_;
        return;
    }
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        if (install_state_ == 1 || (!repair && install_app_id_ == app_id && install_state_ == 2)) return;
    }
    if (install_worker_.joinable()) install_worker_.join();
    auto loaded = repository_ ? repository_->Load() :
        playden::Result<std::optional<playden::StoredSession>>::Failure(
            playden::StorageError::kNotFound, "No session");
    if (!loaded.ok() || !loaded.value().has_value())
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        install_app_id_ = app_id;
        install_state_ = 3;
        install_message_ = "Sign in to install this game.";
        ++install_version_;
        return;
    }
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        install_app_id_ = app_id;
        install_state_ = 1;
        install_done_ = false;
        install_progress_bytes_ = 0;
        install_total_bytes_ = 0;
        install_message_ = "Checking Steam licenses…";
        ++install_version_;
    }
    install_cancel_.store(false);
    playden::StoredSession session = std::move(*loaded.value());
    install_worker_ = std::thread([this, app_id, install_root, repair, verify,
                                   session = std::move(session)]() mutable {
        std::string message;
        bool installed = false;
        playden::Storage storage("/data/PLDN10001");
        storage.AllowTraversableRoot();
        if (!storage.EnsureRootDirectory().ok())
            message = "Playden storage is unavailable.";
        else if (!EnsureSharedGamesDirectory())
            message = "Playden storage is unavailable.";
        else if (app_id == 220 && playden_ps4_wine_prepare_hl2_install() != 0)
            message = playden_ps4_wine_status();
        else
        {
            constexpr const char *ca_bundle = "/app0/assets/cacert.pem";
            playden::steam::CmClient cm(ca_bundle, storage);
            cm.SetOwner("install");
            playden::steam::SetCmLog([](const std::string &line) { LogNetworkLine(line); });
            LogHeap("install-start");
            playden::steam::CdnClient cdn(ca_bundle, [](const std::string &line) {
                LogNetworkLine("cdn " + line);
                if (line.find(" complete ") != std::string::npos) LogHeap("cdn");
            });
            playden::steam::SteamInstallRunner runner(*repository_, cm, cdn);
            runner.SetTuning([this] {
                playden::steam::DownloadTuning tuning;
                tuning.slots = download_connections();
                return tuning;
            });
            runner.SetContentLanguage([this] {
                const std::lock_guard<std::mutex> guard(settings_mutex_);
                return content_language_;
            });
            runner.SetFreeDlcMemory(&storage);
            // The licenses the Library read, family-shared ones included, for depot selection.
            runner.SetLicensedDepots(&licensed_depots_);
            runner.SetLog([](std::uint32_t app, const char *step, const char *category,
                             const char *detail) {
                LogNetworkLine("install app=" + std::to_string(app) + " step=" + step +
                               " result=" + category + (detail && *detail ? " " + std::string(detail) : ""));
            });
            const auto progress =
                [this](std::uint64_t done, std::uint64_t total, std::uint64_t) {
                    const std::lock_guard<std::mutex> guard(install_mutex_);
                    install_progress_bytes_ = done;
                    install_total_bytes_ = total;
                    const auto mb = [](std::uint64_t bytes) { return bytes / (1024 * 1024); };
                    install_message_ = "Downloading " + std::to_string(mb(done)) + " / " +
                                       std::to_string(mb(total)) + " MB";
                    ++install_version_;
                };
            const auto stage = [this](playden::steam::InstallStep step) {
                const char *message =
                    step == playden::steam::InstallStep::kCheckingLicenses ? "Checking Steam licenses…"
                    : step == playden::steam::InstallStep::kCheckingFiles
                        ? "Checking every installed file against Steam. Large games take several minutes…"
                    : step == playden::steam::InstallStep::kApplying ? "Putting the new files in place…"
                    : "Preparing game download…";
                const std::lock_guard<std::mutex> guard(install_mutex_);
                install_message_ = message;
                ++install_version_;
            };
            // The install sheet's choice, when one was made for this game.
            playden::steam::InstallOptions options;
            bool from_sheet = false;
            {
                const std::lock_guard<std::mutex> sheet_guard(details_mutex_);
                if (!repair && sheet_.app_id == app_id) {
                    if (!sheet_.offers.empty() && sheet_.offers[sheet_.branch].name != "public")
                        options.branch = sheet_.offers[sheet_.branch].name;
                    if (!sheet_.languages.empty()) options.language = sheet_.languages[sheet_.language];
                    from_sheet = true;
                }
            }
            // A first install keeps the sheet's choice until it finishes, so a download resumed after a
            // restart (no sheet then) fetches the same depots.
            if (!repair) {
                if (from_sheet) SaveInstallChoice(app_id, options);
                else if (LoadInstallChoice(app_id, &options))
                    LogNetworkLine("install app=" + std::to_string(app_id) + " step=resume choice=saved");
            }
            // An update keeps what the install recorded (a beta stays a beta).
            if (repair) {
                if (const auto record = playden::installs::ReadInstallRecord(
                        install_root + "/" + playden::installs::kInstallRecordName)) {
                    if (record->branch != "public") options.branch = record->branch;
                    options.language = record->language;
                }
            }
            if (!options.branch.empty() || !options.language.empty())
                LogNetworkLine("install app=" + std::to_string(app_id) + " step=sheet branch=" + options.branch +
                               " language=" + options.language);
            // Check and repair: without its file list the update hashes every file (DepotInstaller::Run).
            // The list comes back when the check stops before staging anything; once an update has
            // started, the next update or check hashes again instead.
            const std::string tree = install_root + "/" + playden::steam::DepotInstaller::kFileTree;
            const std::string tree_aside = tree + ".before-check";
            if (verify) {
                std::remove(tree_aside.c_str());
                const bool aside = std::rename(tree.c_str(), tree_aside.c_str()) == 0;
                LogNetworkLine("install app=" + std::to_string(app_id) + " step=verify tree=" +
                               (aside ? "set-aside" : "none errno=" + std::to_string(errno)));
                const std::lock_guard<std::mutex> guard(install_mutex_);
                install_message_ = "Checking the game's files…";
                ++install_version_;
            }
            playden::installs::InstallRunner::Result outcome;
            try {
                outcome = repair ? runner.UpdateApp(
                    session, app_id, install_root, options, install_cancel_, progress, stage,
                    [] { return true; })
                    : runner.InstallApp(
                    session, app_id, install_root,
                    options, install_cancel_, progress, stage);
            } catch (const std::bad_alloc &) {
                LogNetworkLine("install app=" + std::to_string(app_id) + " result=out-of-memory");
                outcome.failure = "Install ran out of application memory. Progress is saved; try again to resume.";
            } catch (const std::exception &error) {
                LogNetworkLine("install app=" + std::to_string(app_id) + " result=exception " + error.what());
                outcome.failure = "Install stopped unexpectedly. Progress is saved; try again to resume.";
            }
            installed = outcome.ok;
            if (installed && !repair) ForgetInstallChoice(app_id);
            if (verify) {
                struct stat info {};
                const bool new_tree = ::lstat(tree.c_str(), &info) == 0;
                const bool update_started =
                    ::lstat((install_root + "/" + playden::steam::DepotInstaller::kUpdateDir).c_str(), &info) == 0;
                if (!new_tree && !update_started) std::rename(tree_aside.c_str(), tree.c_str());
                else std::remove(tree_aside.c_str());
                LogNetworkLine("install app=" + std::to_string(app_id) + " step=verify result=" +
                               (installed ? "ok" : "stopped") + " tree=" +
                               (new_tree ? "rewritten" : update_started ? "missing-until-next-update" : "restored"));
            }
            message = installed ? (verify ? std::string("Files checked: anything missing or changed was downloaded again.")
                                          : InstalledMessage(app_id))
                : outcome.canceled
                ? "Download canceled. Completed files are saved; Cross resumes the install."
                : (outcome.failure.empty() ? "Install failed. See the console network log." : outcome.failure);
            if (installed) {
                if (app_id != 220 && playden_ps4_wine_prepare_game_prefix(app_id) != 0)
                    message = std::string("Game files installed, but runtime setup failed: ") +
                              playden_ps4_wine_status();
                const auto steamworks_error = PrepareSteamworksForGame(
                    install_root, app_id, session.steam_id, session.account_name);
                if (!steamworks_error.empty())
                    message = "Game files installed, but Steamworks setup failed: " + steamworks_error;
            }
        }
        if (installed)
        {
            {
                std::lock_guard<std::mutex> guard(library_mutex_);
                for (std::size_t index = 0; index < games_.size(); ++index)
                {
                    if (games_[index].id.steam_app() != app_id) continue;
                    if (std::find(installed_games_.begin(), installed_games_.end(), index) == installed_games_.end())
                        installed_games_.push_back(index);
                    break;
                }
            }
            {
                const std::lock_guard<std::mutex> guard(details_mutex_);
                if (details_app_id_ == app_id && details_state_ == 2)
                {
                    details_installed_ = true;
                    details_incomplete_ = false;
                    details_update_ = false;
                    const std::lock_guard<std::mutex> updates_guard(updates_mutex_);
                    updates_.erase(app_id);
                    details_message_ = message == InstalledMessage(app_id)
                        ? InstalledDetailsMessage(app_id, details_windows_depots_) : message;
                }
            }
        }
        playden::steam::ZeroizeSecret(session.refresh_token);
        playden::steam::ZeroizeSecret(session.access_token);
        session.account_name.clear();
        {
            const std::lock_guard<std::mutex> guard(install_mutex_);
            install_state_ = installed ? 2 : 3;
            install_message_ = std::move(message);
            install_done_ = true;
            ++install_version_;
        }
        LogNetworkLine("install app=" + std::to_string(app_id) +
                       (installed ? " result=complete" : " result=failed"));
    });
}

bool SteamQrAuth::prepare_game_steamworks(std::uint32_t app_id)
{
    const auto root = InstalledGameRoot(app_id);
    if (root.empty()) return false;
    auto loaded = repository_ ? repository_->Load() :
        playden::Result<std::optional<playden::StoredSession>>::Failure(
            playden::StorageError::kNotFound, "No session");
    if (!loaded.ok() || !loaded.value().has_value()) {
        std::lock_guard<std::mutex> guard(details_mutex_);
        details_message_ = "Sign in to prepare this game's Steam features.";
        return false;
    }
    const auto &session = *loaded.value();
    const auto error = PrepareSteamworksForGame(root, app_id, session.steam_id, session.account_name);
    playden::steam::ZeroizeSecret(loaded.value()->refresh_token);
    playden::steam::ZeroizeSecret(loaded.value()->access_token);
    if (error.empty()) return true;
    {
        std::lock_guard<std::mutex> guard(details_mutex_);
        details_message_ = "Steamworks setup failed: " + error;
    }
    return false;
}

int SteamQrAuth::game_install_state() const
{
    const std::lock_guard<std::mutex> guard(install_mutex_);
    return install_state_;
}

std::string SteamQrAuth::game_install_message() const
{
    const std::lock_guard<std::mutex> guard(install_mutex_);
    if (install_message_.empty()) return "Press Cross to install this game.";
    return install_message_;
}

std::uint32_t SteamQrAuth::queue_app_id() const
{
    const std::lock_guard<std::mutex> guard(install_mutex_);
    return install_app_id_;
}

void SteamQrAuth::cancel_game_install()
{
    std::lock_guard<std::mutex> guard(install_mutex_);
    if (install_state_ != 1) return;
    install_cancel_.store(true);
    install_message_ = "Canceling download…";
    ++install_version_;
}

void SteamQrAuth::start_game_uninstall(std::uint32_t app_id)
{
    if (app_id == 0 || !HasInstalledRecord(app_id)) return;
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        if (install_state_ == 1 && install_app_id_ == app_id) {
            const std::lock_guard<std::mutex> uninstall_guard(uninstall_mutex_);
            uninstall_app_id_ = app_id;
            uninstall_state_ = 3;
            uninstall_message_ = "Wait for the active download to finish before deleting this game.";
            return;
        }
    }
    if (uninstall_worker_.joinable()) uninstall_worker_.join();
    {
        const std::lock_guard<std::mutex> guard(uninstall_mutex_);
        uninstall_app_id_ = app_id;
        uninstall_state_ = 1;
        uninstall_message_ = "Deleting Playden-managed game files…";
    }
    uninstall_worker_ = std::thread([this, app_id] {
        std::string failure;
        const auto root = InstalledGameRoot(app_id);
        const bool removed = !root.empty() && playden::steam::RemoveManagedInstall(root, &failure);
        if (removed) {
            std::lock_guard<std::mutex> guard(library_mutex_);
            installed_games_.erase(std::remove_if(installed_games_.begin(), installed_games_.end(),
                [this, app_id](std::size_t index) {
                    return index < games_.size() && games_[index].id.steam_app() == app_id;
                }), installed_games_.end());
        }
        {
            const std::lock_guard<std::mutex> guard(details_mutex_);
            if (details_app_id_ == app_id && removed) {
                details_installed_ = false;
                details_message_ = "Steam library entry loaded; game files were removed.";
            }
        }
        {
            const std::lock_guard<std::mutex> guard(uninstall_mutex_);
            uninstall_state_ = removed ? 2 : 3;
            uninstall_message_ = removed ? "Game files deleted. The Playden Wine prefix and saves were left in place."
                : (failure.empty() ? "Playden could not delete the managed game files." : failure);
        }
        LogNetworkLine("uninstall app=" + std::to_string(app_id) +
                       (removed ? " result=complete" : " result=failed"));
    });
}

int SteamQrAuth::game_uninstall_state() const
{
    const std::lock_guard<std::mutex> guard(uninstall_mutex_);
    return uninstall_state_;
}

std::uint32_t SteamQrAuth::game_uninstall_app_id() const
{
    const std::lock_guard<std::mutex> guard(uninstall_mutex_);
    return uninstall_app_id_;
}

std::string SteamQrAuth::game_uninstall_message() const
{
    const std::lock_guard<std::mutex> guard(uninstall_mutex_);
    return uninstall_message_;
}

int SteamQrAuth::saves_state() const
{
    const std::lock_guard<std::mutex> guard(saves_mutex_);
    return saves_state_;
}

std::uint32_t SteamQrAuth::saves_app_id() const
{
    const std::lock_guard<std::mutex> guard(saves_mutex_);
    return saves_app_id_;
}

std::uint64_t SteamQrAuth::saves_progress_done() const
{
    const std::lock_guard<std::mutex> guard(saves_mutex_);
    return saves_progress_done_;
}

std::uint64_t SteamQrAuth::saves_progress_total() const
{
    const std::lock_guard<std::mutex> guard(saves_mutex_);
    return saves_progress_total_;
}

std::string SteamQrAuth::saves_message() const
{
    const std::lock_guard<std::mutex> guard(saves_mutex_);
    return saves_message_;
}

std::string SteamQrAuth::content_language() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return playden::steam::LanguageName(content_language_);
}

std::string SteamQrAuth::log_receiver_ip() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return log_receiver_ip_;
}

std::string SteamQrAuth::log_receiver_status() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return log_receiver_status_;
}

bool SteamQrAuth::logging_enabled() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return logging_enabled_;
}

bool SteamQrAuth::wine_debug_enabled() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return wine_debug_level_ != "profile";
}

int SteamQrAuth::download_connections() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return download_connections_;
}

std::string SteamQrAuth::wine_debug_level() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return wine_debug_level_;
}

bool SteamQrAuth::wait_watchdog_enabled() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return wait_watchdog_enabled_;
}

bool SteamQrAuth::gpu_hang_dump_enabled() const
{
    const std::lock_guard<std::mutex> guard(settings_mutex_);
    return gpu_hang_dump_enabled_;
}

void SteamQrAuth::cycle_setting(int setting)
{
    std::lock_guard<std::mutex> guard(settings_mutex_);
    if (setting == 0)
    {
        const auto& languages = playden::steam::ContentLanguages();
        const auto found = std::find_if(languages.begin(), languages.end(), [this](const auto& language) {
            return language.code == content_language_;
        });
        content_language_ = found == languages.end() || std::next(found) == languages.end()
            ? std::string(languages.front().code) : std::string(std::next(found)->code);
    }
    else if (setting == 1) logging_enabled_ = !logging_enabled_;
    else if (setting == 2) wine_debug_level_ = wine_debug_level_ == "detailed"
        ? "profile" : "detailed";
    else if (setting == 3) wait_watchdog_enabled_ = !wait_watchdog_enabled_;
    else if (setting == 4) gpu_hang_dump_enabled_ = !gpu_hang_dump_enabled_;
    else return;

    if (setting == 0) (void)persist_settings_locked();
    else (void)persist_runtime_settings_locked();
}

bool SteamQrAuth::persist_settings_locked()
{
    playden::Storage storage("/data/PLDN10001");
    storage.AllowTraversableRoot();
    if (!storage.EnsureRootDirectory().ok()) return false;
    const std::string text = "language=" + content_language_ + "\n";
    return storage.WriteAtomicPrivate("settings.txt", playden::Bytes(text.begin(), text.end())).ok();
}

bool SteamQrAuth::persist_runtime_settings_locked()
{
    playden::Storage storage("/data/PLDN10001");
    storage.AllowTraversableRoot();
    if (!storage.EnsureRootDirectory().ok()) return false;
    const std::string text = "log=" + std::to_string(logging_enabled_ ? 1 : 0) +
        "\nlog_server=" + log_receiver_ip_ +
        "\nlog_port=" + std::to_string(log_receiver_port_) +
        "\nwine_log=" + (wine_debug_level_ == "detailed" || wine_debug_level_ == "quiet"
            ? wine_debug_level_ : "profile") +
        "\nwait_watchdog=" + std::to_string(wait_watchdog_enabled_ ? 1 : 0) +
        "\ngpu_hang_report=" + std::to_string(gpu_hang_dump_enabled_ ? 1 : 0) + "\n";
    return storage.WriteAtomicPrivate("settings.conf", playden::Bytes(text.begin(), text.end())).ok();
}

int SteamQrAuth::edit_log_receiver_ip(int user_id)
{
    const std::string current = log_receiver_ip();
    std::string edited;
    const int keyboard_result = EditReceiverIpWithSystemKeyboard(user_id, current, &edited);
    {
        const std::lock_guard<std::mutex> guard(settings_mutex_);
        if (keyboard_result == 0) {
            log_receiver_status_ = "No change; open the system keyboard and enter an IPv4 address.";
            return 0;
        }
        if (keyboard_result < 0) {
            log_receiver_status_ = "Invalid IPv4 address; use four dot-separated numbers.";
            return -1;
        }
        const std::string previous = log_receiver_ip_;
        log_receiver_ip_ = std::move(edited);
        if (!persist_runtime_settings_locked()) {
            log_receiver_ip_ = previous;
            log_receiver_status_ = "Could not save the receiver IP in Playden settings.";
            return -2;
        }
        log_receiver_status_ = "Saved; applied when the Wine runtime starts.";
    }
    return 1;
}

void SteamQrAuth::start_saves_sync(std::uint32_t app_id, int resolution)
{
    if (app_id == 0 || resolution < 0 || resolution > 2) return;
    {
        const std::lock_guard<std::mutex> guard(saves_mutex_);
        if (saves_state_ == 1) return;
        saves_state_ = 1;
        saves_app_id_ = app_id;
        saves_message_ = resolution == 0 ? "Checking Steam Cloud saves…" : "Resolving save conflict…";
        saves_progress_done_ = 0;
        saves_progress_total_ = 0;
        saves_progress_percent_ = -1;
        ++saves_version_;
    }
    if (saves_worker_.joinable()) saves_worker_.join();
    saves_cancel_.store(false);
    saves_worker_ = std::thread([this, app_id, resolution] {
        std::string message;
        int state = 4;
        auto loaded = repository_ ? repository_->Load() :
            playden::Result<std::optional<playden::StoredSession>>::Failure(
                playden::StorageError::kNotFound, "No session");
        if (!loaded.ok() || !loaded.value().has_value())
            message = "Sign in to check Steam Cloud saves.";
        else if (!HasInstalledRecord(app_id))
            message = "Install this game before checking its saves.";
        else
        {
            playden::Storage storage("/data/PLDN10001");
            storage.AllowTraversableRoot();
            if (!storage.EnsureRootDirectory().ok())
                message = "Playden storage is unavailable.";
            else
            {
                constexpr const char* ca_bundle = "/app0/assets/cacert.pem";
                playden::steam::CmClient cm(ca_bundle, storage);
                cm.SetOwner("cloud");
                playden::steam::HttpClient http(ca_bundle);
                playden::steam::SteamCloudRunner runner(cm, http);
                runner.SetProgress([this](std::uint64_t done, std::uint64_t total) {
                    const std::lock_guard<std::mutex> guard(saves_mutex_);
                    saves_progress_done_ = done;
                    saves_progress_total_ = total;
                    const int percent = total == 0 ? 0 : static_cast<int>(std::min<std::uint64_t>(100, done * 100 / total));
                    if (percent != saves_progress_percent_ || done == total)
                    {
                        saves_progress_percent_ = percent;
                        ++saves_version_;
                    }
                });
                runner.SetLog([](std::uint32_t app, const char* event, const std::string& detail) {
                    LogNetworkLine("cloud app=" + std::to_string(app) + " event=" + event + " " + detail);
                });
                const std::string prefix = ConfiguredWinePrefix(app_id);
                const bool has_prefix = !prefix.empty();
                playden::steam::CloudLocation location{
                    playden::GameId::Steam(app_id), GameInstallRoot(app_id),
                    "/data/PLDN10001/saves/steam-" + std::to_string(app_id),
                    prefix, false};
                auto report = runner.Check(*loaded.value(), location, saves_cancel_);
                if (saves_cancel_.load())
                    message = "Save check canceled.";
                else if (has_prefix && resolution != 0 && (report.state == playden::steam::CloudState::kConflict ||
                                             report.state == playden::steam::CloudState::kNeedsAccount))
                {
                    report = runner.Apply(*loaded.value(), location,
                        resolution == 1 ? playden::steam::CloudResolution::kUseLocal
                                         : playden::steam::CloudResolution::kUseCloud,
                        true, saves_cancel_);
                    message = report.message;
                    state = report.state == playden::steam::CloudState::kFailed ? 5 : 2;
                }
                else if (has_prefix && report.state == playden::steam::CloudState::kSyncing)
                {
                    {
                        const std::lock_guard<std::mutex> guard(saves_mutex_);
                        saves_message_ = "Syncing Steam Cloud saves…";
                        ++saves_version_;
                    }
                    report = runner.Apply(*loaded.value(), location,
                        playden::steam::CloudResolution::kAutomatic, false, saves_cancel_);
                    message = report.message;
                    state = report.state == playden::steam::CloudState::kFailed ? 5 : 2;
                }
                else if (!has_prefix && report.state == playden::steam::CloudState::kSyncing)
                {
                    message = "Steam Cloud has " + std::to_string(report.remote_files) +
                        " saves (" + std::to_string(report.remote_bytes / (1024 * 1024)) +
                        " MB). No Wine prefix is configured for this game, so this was a check only; no files changed.";
                    state = 4;
                }
                else if (!has_prefix && (report.state == playden::steam::CloudState::kConflict ||
                                         report.state == playden::steam::CloudState::kNeedsAccount))
                {
                    message = "Steam Cloud has " + std::to_string(report.remote_files) +
                        " saves, but this game has no configured Wine prefix. No files changed.";
                    state = 4;
                }
                else
                {
                    message = report.message;
                    state = report.state == playden::steam::CloudState::kConflict ||
                            report.state == playden::steam::CloudState::kNeedsAccount ? 3 :
                            report.state == playden::steam::CloudState::kUnsupported ? 4 :
                            report.state == playden::steam::CloudState::kFailed ? 5 : 2;
                }
            }
        }
        if (loaded.ok() && loaded.value())
        {
            playden::steam::ZeroizeSecret(loaded.value()->refresh_token);
            playden::steam::ZeroizeSecret(loaded.value()->access_token);
            loaded.value()->account_name.clear();
        }
        if (saves_cancel_.load()) { state = 0; message = "Save check canceled."; }
        {
            const std::lock_guard<std::mutex> guard(saves_mutex_);
            saves_state_ = state;
            saves_message_ = std::move(message);
            if (state == 2 && saves_progress_total_ != 0)
                saves_progress_done_ = saves_progress_total_;
            ++saves_version_;
        }
    });
}

bool SteamQrAuth::saves_poll()
{
    const std::lock_guard<std::mutex> guard(saves_mutex_);
    if (saves_seen_version_ == saves_version_) return false;
    saves_seen_version_ = saves_version_;
    return true;
}

void SteamQrAuth::request_game_details(std::uint32_t app_id)
{
    if (app_id == 0) return;
    // A file check that never finished (Playden closed or crashed) left the file list set aside; put
    // it back unless an install job is running for this game.
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        if (!(install_state_ == 1 && install_app_id_ == app_id)) {
            const std::string tree = GameInstallRoot(app_id) + "/" + playden::steam::DepotInstaller::kFileTree;
            struct stat info {};
            if (::lstat(tree.c_str(), &info) != 0 && ::lstat((tree + ".before-check").c_str(), &info) == 0 &&
                std::rename((tree + ".before-check").c_str(), tree.c_str()) == 0)
                LogNetworkLine("install app=" + std::to_string(app_id) + " step=verify tree=restored-after-stop");
        }
    }
    {
        const std::lock_guard<std::mutex> guard(details_mutex_);
        if (details_app_id_ == app_id && (details_state_ == 1 || details_state_ == 2)) return;
        if (details_state_ == 1) return;
        details_app_id_ = app_id;
        store_details_ready_ = false;
        store_details_available_ = false;
        store_description_.clear();
        store_genres_.clear();
        store_developers_.clear();
        store_publishers_.clear();
        store_categories_.clear();
        store_release_date_.clear();
        store_metacritic_score_.clear();
        store_controller_support_.clear();
        store_windows_supported_ = false;
    }
    if (store_details_service_ != nullptr)
        store_details_service_->Request(playden::GameId::Steam(app_id));
    if (details_worker_.joinable()) details_worker_.join();
    const bool already_installed = HasInstalledRecord(app_id);
    const bool already_incomplete = !already_installed && HasIncompleteInstall(app_id);
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        install_app_id_ = app_id;
        install_state_ = already_installed ? 2 : 0;
        install_message_ = already_installed
            ? InstalledMessage(app_id) : std::string();
    }
    auto loaded = repository_ ? repository_->Load() :
        playden::Result<std::optional<playden::StoredSession>>::Failure(
            playden::StorageError::kNotFound, "No session");
    if (!loaded.ok() || !loaded.value().has_value())
    {
        const std::lock_guard<std::mutex> guard(details_mutex_);
        details_app_id_ = app_id;
        details_state_ = 3;
        details_message_ = "Sign in to check Steam install details.";
        return;
    }
    {
        const std::lock_guard<std::mutex> guard(details_mutex_);
        details_app_id_ = app_id;
        details_state_ = 1;
        details_windows_launch_ = false;
        details_windows_depots_ = 0;
        details_installed_ = already_installed;
        details_incomplete_ = already_incomplete;
        details_update_ = false;
        details_message_ = "Checking Steam app details…";
        details_done_ = false;
    }
    details_cancel_.store(false);
    playden::StoredSession session = std::move(*loaded.value());
    const std::string app_title = library_title_by_app_id(app_id);
    details_worker_ = std::thread([this, app_id, already_incomplete, app_title,
                                   session = std::move(session)]() mutable {
        bool windows_launch = false;
        bool launch_ready = false;
        bool missing_depots = false;
        bool update_available = false;
        bool missing_launch = false;
        std::size_t windows_depots = 0;
        std::string message;
        playden::Storage storage("/data/PLDN10001");
        if (!storage.EnsureRootDirectory().ok())
            message = "Playden storage is unavailable.";
        else
        {
            playden::steam::CmClient client("/app0/assets/cacert.pem", storage);
            client.SetOwner("details");
            playden::steam::SetCmLog([](const std::string &line) { LogNetworkLine(line); });
            const auto buffers = client.FetchAppInfo(session, {app_id}, details_cancel_);
            if (buffers.error != playden::steam::CmError::kNone) {
                message = std::string("Steam app details unavailable: ") +
                          playden::steam::CmErrorName(buffers.error);
                const auto preflight = playden::steam::internal::NativeNetworkPreflightDetail();
                if (!preflight.empty()) LogNetworkLine("details network preflight failed: " + preflight);
            }
            else if (const auto found = buffers.buffers.find(app_id); found == buffers.buffers.end())
                message = "Steam returned no details for this game.";
            else if (const auto app = playden::steam::ParseAppInfo(app_id, found->second); !app)
                message = "Steam returned unreadable app details.";
            else
            {
                {
                    const std::lock_guard<std::mutex> app_guard(details_mutex_);
                    details_app_ = *app;
                }
                std::vector<playden::steam::AppLaunch> launches;
                std::string reason;
                windows_launch = playden::steam::EligibleLaunches(
                    *app, {app_id}, "public", &launches, &reason);
                launch_ready = windows_launch;
                if (launch_ready && app_id != 220) {
                    if (launches.empty() || !WriteSteamLaunchProfile(app_id, app_title, launches.front())) {
                        launch_ready = false;
                        reason = "Steam's Windows launch option cannot be represented by a Playden profile.";
                    }
                }
                windows_depots = static_cast<std::size_t>(std::count_if(
                    app->depots.begin(), app->depots.end(), [](const auto &depot) {
                        return !depot.shared_install && playden::steam::OsListMatchesWindows(depot.os_list) &&
                               depot.manifest_gid.has_value();
                    }));
                message = windows_launch ? "Windows launch option found; " : "No Windows launch option; ";
                message += std::to_string(windows_depots) + " Windows depots listed. ";
                missing_depots = HasInstalledRecord(app_id) && MissingSelectedDepots(app_id, *app);
                update_available = HasInstalledRecord(app_id) && !missing_depots &&
                                   InstalledUpdateAvailable(app_id, *app);
                missing_launch = HasInstalledRecord(app_id) && windows_launch &&
                    !HasInstalledLaunchExecutable(app_id);
                const bool now_installed = HasInstalledRecord(app_id) && !missing_depots && !missing_launch;
                if (launch_ready && now_installed && app_id != 220 &&
                    playden_ps4_wine_prepare_game_prefix(app_id) != 0) {
                    launch_ready = false;
                    reason = playden_ps4_wine_status();
                }
                if (!launch_ready && windows_launch)
                    message += reason.empty() ? "Wine runtime setup is unavailable." : reason;
                else if (now_installed)
                    message += app_id == 220 ? "Game files are installed; Wine runtime is ready. Press Cross to play."
                                             : "Game files are installed; press Cross to try launching.";
                else if (missing_depots || missing_launch)
                    message += "Required game files are missing. Press Cross to repair the install.";
                else if (already_incomplete || HasIncompleteInstall(app_id))
                    message += "An incomplete install remains. Press Cross to resume the download.";
                else
                    message += "Steam license and depot checks are required before install.";
            }
        }
        playden::steam::ZeroizeSecret(session.refresh_token);
        playden::steam::ZeroizeSecret(session.access_token);
        session.account_name.clear();
        {
            const std::lock_guard<std::mutex> guard(details_mutex_);
            LogNetworkLine("details app=" + std::to_string(app_id) +
                           " windows_launch=" + (windows_launch ? "yes" : "no") +
                           " launch_ready=" + (launch_ready ? "yes" : "no") +
                           " windows_depots=" + std::to_string(windows_depots));
            details_windows_launch_ = launch_ready;
            details_windows_depots_ = windows_depots;
            details_installed_ = HasInstalledRecord(app_id) && !missing_depots && !missing_launch;
            details_incomplete_ = missing_depots || missing_launch ||
                (!details_installed_ && HasIncompleteInstall(app_id));
            details_update_ = details_installed_ && update_available;
            {
                const std::lock_guard<std::mutex> updates_guard(updates_mutex_);
                if (details_update_) updates_.insert(app_id);
                else updates_.erase(app_id);
            }
            if (details_installed_ && windows_launch)
                message = InstalledDetailsMessage(app_id, windows_depots);
            if (details_update_)
                message = "An update is available on Steam. Press Cross to update; Square deletes the game.";
            details_message_ = std::move(message);
            details_done_ = true;
        }
    });
}

const char *SteamQrAuth::library_title(std::size_t index) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    return index < games_.size() ? games_[index].title.c_str() : "";
}

const char *SteamQrAuth::library_title_by_app_id(std::uint32_t app_id) const
{
    const std::lock_guard<std::mutex> guard(library_mutex_);
    for (const auto& game : games_)
        if (game.id.steam_app().value_or(0) == app_id) return game.title.c_str();
    return "";
}

bool SteamQrAuth::start()
{
    return start_auth(false);
}

bool SteamQrAuth::restore_cached_session()
{
    return start_auth(true);
}

bool SteamQrAuth::start_auth(bool cached_only)
{
    LogNetworkLine(cached_only ? "Steam cached-session restore requested"
                               : "Steam sign-in requested");
    // Match orbis-win's explicit SceNet startup and use the same bounded native
    // resolver path that Playden already uses on PS5.
    if (!playden::steam::internal::InitializeNativeNetworkForBootstrap())
    {
        failure_detail_ = "SceNet initialization failed";
        state_ = 4;
        return false;
    }
    if (service_ == nullptr)
    {
        repository_ = new SessionMemory();
        const auto root = repository_->EnsureRoot();
        if (!root.ok())
        {
            LogNetworkLine("session cache unavailable: " + root.message());
            failure_detail_ = "Playden session storage is unavailable.";
            state_ = 4;
            return false;
        }
        backend_ = new playden::steam::AuthClient("/app0/assets/cacert.pem");
        backend_->SetTransportTraceReport([](const std::string &line) {
            LogNetworkLine(line);
        });
        backend_->SetTransportFailureReport([this](const char *method,
                                                   const playden::steam::HttpResponse &response) {
            const std::lock_guard<std::mutex> guard(failure_mutex_);
            pending_failure_detail_ = std::string(method) + ": " +
                                      playden::steam::HttpFailureName(response) +
                                      " connect=" + std::to_string(response.timing.connect_ms) + "ms" +
                                      " tls=" + std::to_string(response.timing.tls_ms) + "ms" +
                                      " firstbyte=" + std::to_string(response.timing.first_byte_ms) + "ms" +
                                      " sent=" + std::to_string(response.timing.uploaded_bytes) + "B" +
                                      " status=" + std::to_string(response.status_code) +
                                      " total=" + std::to_string(response.timing.total_ms) + "ms";
        });
        backend_->SetCmRenewal([](const playden::StoredSession &session,
                                  const std::atomic<bool> &cancel) {
            playden::Storage storage("/data/PLDN10001");
            storage.AllowTraversableRoot();
            if (!storage.EnsureRootDirectory().ok())
                return playden::steam::AuthResult{playden::steam::AuthError::kPersistence, std::nullopt};
            playden::steam::SetCmLog([](const std::string &line) {
                LogNetworkLine(line);
            });
            playden::steam::CmClient cm("/app0/assets/cacert.pem", storage);
            cm.SetOwner("auth");
            return playden::steam::RenewThroughCm(cm, session, cancel);
        });
        service_ = new playden::steam::SteamService(
            *backend_, *repository_, [](const std::string &line) { LogNetworkLine(line); });
        library_backend_ = std::make_unique<playden::steam::LibraryClient>("/app0/assets/cacert.pem");
        library_storage_ = std::make_unique<playden::Storage>("/data/PLDN10001");
        library_storage_->AllowTraversableRoot();
        (void)library_storage_->EnsureRootDirectory();
        library_cm_ = std::make_unique<playden::steam::CmClient>("/app0/assets/cacert.pem", *library_storage_);
        library_cm_->SetOwner("library");
        entitled_library_ = std::make_unique<playden::steam::EntitledLibraryBackend>(*library_backend_, *library_cm_);
        entitled_library_->SetLicensedDepots(&licensed_depots_);
        entitled_library_->SetAppNameStorage(library_storage_.get());
        // The family service names the games a family member shares (and logs what it lists).
        entitled_library_->SetFamilyDiagnostics("/app0/assets/cacert.pem", [](const std::string &line) {
            LogNetworkLine(line);
        });
        library_service_ = std::make_unique<playden::LibraryService>(
            *entitled_library_, *repository_, nullptr,
            std::function<std::uint64_t()>{},
            [](const char *step) { LogNetworkLine(std::string("library worker step=") + step); });
    }
    {
        const std::lock_guard<std::mutex> guard(failure_mutex_);
        pending_failure_detail_.clear();
        failure_detail_.clear();
    }
    challenge_url_.clear();
    const auto saved = repository_->Load();
    if (saved.ok() && saved.value().has_value())
    {
        LogNetworkLine("session cache=hit; restoring saved Steam login");
        restoring_session_ = true;
        request_ = service_->RestoreSession();
    }
    else
    {
        if (cached_only)
        {
            LogNetworkLine(saved.ok() ? "session cache=miss; leaving Connect Steam available"
                                      : "session cache=unreadable; leaving Connect Steam available");
            restoring_session_ = false;
            request_ = 0;
            state_ = 0;
            return false;
        }
        LogNetworkLine(saved.ok() ? "session cache=miss; starting QR sign-in"
                                  : "session cache=unreadable; starting QR sign-in");
        restoring_session_ = false;
        request_ = service_->BeginQrSignIn();
    }
    state_ = request_ == 0 ? 4 : 1;
    return request_ != 0;
}

void SteamQrAuth::cancel()
{
    if (service_ != nullptr && request_ != 0)
        service_->Cancel(request_);
    request_ = 0;
    challenge_url_.clear();
    state_ = 0;
}

bool SteamQrAuth::start_password_sign_in(int user_id)
{
    std::string name, password;
    if (EditTextWithSystemKeyboard(user_id, "Steam account name", "Account name (not your email)", account_name_, 64,
                                   &name, kImeNoAutoCapitalization) != 1 || name.empty())
        return false;
    const int typed = EditTextWithSystemKeyboard(user_id, "Steam password", "Password", "", 128, &password,
                                                 kImeNoAutoCapitalization | kImePassword);
    if (typed != 1 || password.empty())
    {
        playden::steam::ZeroizeSecret(password);
        return false;
    }
    // The service is made by the first sign-in; a QR sign-in it starts is replaced right away.
    if (service_ == nullptr && !start_auth(false) && service_ == nullptr)
    {
        playden::steam::ZeroizeSecret(password);
        return false;
    }
    if (request_ != 0) service_->Cancel(request_);
    challenge_url_.clear();
    restoring_session_ = false;
    password_flow_ = true;
    guard_code_allowed_ = false;
    sign_in_prompt_ = "Signing in to Steam…";
    {
        const std::lock_guard<std::mutex> guard(failure_mutex_);
        pending_failure_detail_.clear();
        failure_detail_.clear();
    }
    LogNetworkLine("Steam password sign-in requested");
    request_ = service_->BeginCredentialSignIn(name, std::move(password));
    playden::steam::ZeroizeSecret(password);
    state_ = request_ == 0 ? 4 : 1;
    return request_ != 0;
}

bool SteamQrAuth::submit_guard_code(int user_id)
{
    if (service_ == nullptr || !guard_code_wanted()) return false;
    std::string code;
    if (EditTextWithSystemKeyboard(user_id, "Steam Guard code", "Code", "", 8, &code, kImeNoAutoCapitalization) != 1 ||
        code.empty())
        return false;
    // Steam Guard codes are upper case letters and digits; the keyboard may have typed lower case.
    for (auto &ch : code) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    sign_in_prompt_ = "Checking the code…";
    service_->SubmitGuardCode(std::move(code));
    return true;
}

void SteamQrAuth::sign_out()
{
    if (service_ == nullptr) return;
    if (request_ != 0) service_->Cancel(request_);
    signing_out_ = true;
    password_flow_ = false;
    sign_in_prompt_.clear();
    LogNetworkLine("Steam sign-out requested");
    service_->SignOut();
}

std::string SteamQrAuth::sign_in_prompt() const { return sign_in_prompt_; }
std::string SteamQrAuth::account_name() const { return account_name_; }

bool SteamQrAuth::poll()
{
    bool changed = false;
    if (store_details_service_ != nullptr)
    {
        for (auto &details : store_details_service_->Pump())
        {
            std::lock_guard<std::mutex> guard(details_mutex_);
            if (details.id.steam_app().value_or(0) != details_app_id_) continue;
            store_details_ready_ = true;
            store_details_available_ = details.available;
            store_description_ = std::move(details.description);
            store_genres_ = JoinStoreItems(details.genres);
            store_developers_ = JoinStoreItems(details.developers);
            store_publishers_ = JoinStoreItems(details.publishers);
            store_categories_ = JoinStoreItems(details.categories);
            store_release_date_ = std::move(details.release_date);
            store_metacritic_score_ = std::move(details.metacritic_score);
            store_controller_support_ = details.controller_support;
            store_windows_supported_ = details.windows_supported;
            changed = true;
        }
    }
    if (service_ == nullptr)
        return changed;
    for (const auto &event : service_->Pump(16))
    {
        // A sign-out is its own request (SignOut returns no id): its result is taken whatever request_ is.
        if (signing_out_ && (event.kind == playden::steam::SteamEventKind::kSignedOut ||
                             (event.kind == playden::steam::SteamEventKind::kFailure && event.request_id != request_)))
        {
            signing_out_ = false;
            if (event.kind == playden::steam::SteamEventKind::kSignedOut)
            {
                LogNetworkLine("Steam signed out");
                {
                    const std::lock_guard<std::mutex> guard(library_mutex_);
                    games_.clear();
                    installed_games_.clear();
                    library_detail_ = "Signed out of Steam.";
                }
                account_name_.clear();
                request_ = 0;
                library_request_ = 0;
                state_ = 0;
            }
            else
            {
                failure_detail_ = "Steam sign-out could not remove the saved sign-in.";
                LogNetworkLine("Steam sign-out failed");
            }
            changed = true;
            continue;
        }
        if (event.request_id != request_)
            continue;
        switch (event.kind)
        {
        case playden::steam::SteamEventKind::kChallenge:
            challenge_url_ = event.challenge_url;
            state_ = 2;
            changed = true;
            break;
        case playden::steam::SteamEventKind::kAwaitingApproval:
            if (password_flow_)
            {
                guard_code_allowed_ = event.code_allowed;
                sign_in_prompt_ = event.code_allowed
                    ? "Approve this sign-in in the Steam Mobile app, or press Triangle to type a code."
                    : "Approve this sign-in in the Steam Mobile app" +
                          (event.email_domain.empty() ? std::string(".") : " or from Steam's email to an address at " +
                                                                               event.email_domain + ".");
                state_ = 8;
            }
            else
            {
                state_ = 2;
            }
            changed = true;
            break;
        case playden::steam::SteamEventKind::kNeedsEmailCode:
            guard_code_allowed_ = true;
            sign_in_prompt_ = "Steam emailed a code to an address at " + event.email_domain +
                              ". Press Triangle to type it.";
            state_ = 8;
            changed = true;
            break;
        case playden::steam::SteamEventKind::kNeedsAuthenticatorCode:
            guard_code_allowed_ = true;
            sign_in_prompt_ = "Press Triangle and type the code from the Steam Mobile app.";
            state_ = 8;
            changed = true;
            break;
        case playden::steam::SteamEventKind::kCodeRejected:
            guard_code_allowed_ = true;
            sign_in_prompt_ = "Steam did not accept that code. Press Triangle to type it again.";
            state_ = 8;
            changed = true;
            break;
        case playden::steam::SteamEventKind::kAuthenticated:
            if (!event.account_name.empty()) account_name_ = event.account_name;
            password_flow_ = false;
            sign_in_prompt_.clear();
            restoring_session_ = false;
            challenge_url_.clear();
            LogNetworkLine("library startup step=authenticated");
            start_library_fetch();
            changed = true;
            break;
        case playden::steam::SteamEventKind::kFailure:
            challenge_url_.clear();
            if (restoring_session_ && event.error == playden::steam::AuthError::kCredentialsRevoked)
            {
                LogNetworkLine("session cache=expired; requesting a fresh QR sign-in");
                restoring_session_ = false;
                request_ = service_->BeginQrSignIn();
                state_ = request_ == 0 ? 4 : 1;
                changed = true;
                break;
            }
            restoring_session_ = false;
            {
                const std::lock_guard<std::mutex> guard(failure_mutex_);
                failure_detail_ = pending_failure_detail_;
                pending_failure_detail_.clear();
            }
            if (password_flow_)
            {
                // The password sign-in's own reasons, in words the player can act on.
                using E = playden::steam::AuthError;
                if (event.error == E::kInvalidPassword)
                    failure_detail_ = "Steam did not accept that account name and password.";
                else if (event.error == E::kRateLimited)
                    failure_detail_ = "Too many sign-in attempts. Wait a while, then try again.";
                else if (event.error == E::kSignInTimedOut)
                    failure_detail_ = "The sign-in timed out waiting for Steam Guard. Try again.";
                password_flow_ = false;
                sign_in_prompt_.clear();
            }
            if (failure_detail_.empty()) failure_detail_ = "Steam rejected the sign-in request";
            state_ = 4;
            changed = true;
            break;
        case playden::steam::SteamEventKind::kCanceled:
            challenge_url_.clear();
            state_ = 0;
            changed = true;
            break;
        default:
            break;
        }
    }
    if (library_service_) for (auto &event : library_service_->Pump(8)) {
        LogNetworkLine("library startup step=event kind=" +
                       std::to_string(static_cast<int>(event.kind)) +
                       " request=" + std::to_string(event.request_id));
        if (event.request_id != library_request_) continue;
        if (event.kind == playden::LibraryEventKind::kCached ||
            event.kind == playden::LibraryEventKind::kComplete ||
            event.kind == playden::LibraryEventKind::kUpdated) {
            std::vector<std::size_t> installed_indices;
            for (std::size_t i = 0; i < event.games.size(); ++i) {
                const auto app_id = event.games[i].id.steam_app();
                if (app_id && HasInstalledRecord(*app_id)) installed_indices.push_back(i);
            }
            if (event.kind == playden::LibraryEventKind::kComplete && !updates_started_) {
                std::vector<std::uint32_t> installed;
                for (const auto index : installed_indices)
                    if (const auto id = event.games[index].id.steam_app(); id && *id != 220) installed.push_back(*id);
                start_update_check(std::move(installed));
            }
            const std::lock_guard<std::mutex> guard(library_mutex_);
            games_ = std::move(event.games);
            installed_games_ = std::move(installed_indices);
            library_detail_ = std::to_string(games_.size()) +
                (event.kind == playden::LibraryEventKind::kCached
                    ? " cached games; refreshing…" : " games loaded");
            state_ = 6;
            changed = true;
            LogNetworkLine("library event=" + std::to_string(static_cast<int>(event.kind)) +
                           " games=" + std::to_string(games_.size()));
        } else if (event.kind == playden::LibraryEventKind::kFailure) {
            const std::lock_guard<std::mutex> guard(library_mutex_);
            if (!games_.empty() && event.cache_available) {
                library_detail_ = std::to_string(games_.size()) + " cached games; Steam refresh failed.";
                state_ = 6;
            } else {
                library_detail_ = event.error == playden::steam::LibraryError::kCredentialsRevoked
                    ? "Steam session expired. Sign in again."
                    : "Library request failed. Check the console network and try again.";
                state_ = 7;
            }
            changed = true;
        }
    }
    bool details_finished = false;
    {
        const std::lock_guard<std::mutex> guard(details_mutex_);
        details_finished = details_done_;
        details_done_ = false;
    }
    if (details_finished)
    {
        if (details_worker_.joinable()) details_worker_.join();
        const std::lock_guard<std::mutex> guard(details_mutex_);
        details_state_ = details_message_.find("unavailable") != std::string::npos ||
                         details_message_.find("returned no") != std::string::npos ||
                         details_message_.find("unreadable") != std::string::npos ||
                         details_message_.find("unavailable") != std::string::npos ? 3 : 2;
        changed = true;
    }
    bool install_finished = false;
    {
        const std::lock_guard<std::mutex> guard(install_mutex_);
        install_finished = install_done_;
        install_done_ = false;
        if (install_seen_version_ != install_version_)
        {
            install_seen_version_ = install_version_;
            changed = true;
        }
    }
    if (install_finished && install_worker_.joinable()) install_worker_.join();
    return changed;
}

} // namespace playden::ps4

namespace
{
playden::ps4::SteamQrAuth &auth()
{
    static playden::ps4::SteamQrAuth instance;
    return instance;
}
}

extern "C" int playden_ps4_auth_start() { return auth().start() ? 1 : 0; }
extern "C" int playden_ps4_auth_restore_cached() { return auth().restore_cached_session() ? 1 : 0; }
extern "C" void playden_ps4_auth_cancel() { auth().cancel(); }
extern "C" int playden_ps4_auth_poll() { return auth().poll() ? 1 : 0; }
extern "C" int playden_ps4_auth_state() { return auth().state(); }
extern "C" const char *playden_ps4_auth_challenge() { return auth().challenge_url().c_str(); }
extern "C" const char *playden_ps4_auth_failure() { return auth().failure_detail().c_str(); }
extern "C" std::size_t playden_ps4_library_count() { return auth().library_count(); }
extern "C" const char *playden_ps4_library_title(std::size_t index) { return auth().library_title(index); }
extern "C" const char *playden_ps4_library_title_by_app_id(std::uint32_t app_id)
{
    return auth().library_title_by_app_id(app_id);
}
extern "C" const char *playden_ps4_library_detail()
{
    library_detail_buffer = auth().library_detail();
    return library_detail_buffer.c_str();
}
extern "C" std::uint32_t playden_ps4_library_app_id(std::size_t index) { return auth().library_app_id(index); }
extern "C" std::size_t playden_ps4_library_visible_count(int filter) { return auth().library_visible_count(filter); }
extern "C" const char *playden_ps4_library_visible_title(int filter, std::size_t index)
{
    return auth().library_visible_title(filter, index);
}
extern "C" std::uint32_t playden_ps4_library_visible_app_id(int filter, std::size_t index)
{
    return auth().library_visible_app_id(filter, index);
}
extern "C" int playden_ps4_library_is_favorite(std::uint32_t app_id)
{
    return auth().library_is_favorite(app_id) ? 1 : 0;
}
extern "C" int playden_ps4_library_is_hidden(std::uint32_t app_id)
{
    return auth().library_is_hidden(app_id) ? 1 : 0;
}
extern "C" int playden_ps4_auth_password_start(int user_id) { return auth().start_password_sign_in(user_id) ? 1 : 0; }
extern "C" int playden_ps4_auth_guard_code(int user_id) { return auth().submit_guard_code(user_id) ? 1 : 0; }
extern "C" int playden_ps4_auth_guard_code_wanted() { return auth().guard_code_wanted() ? 1 : 0; }
extern "C" void playden_ps4_auth_sign_out() { auth().sign_out(); }
extern "C" const char *playden_ps4_auth_prompt()
{
    static std::string prompt;
    prompt = auth().sign_in_prompt();
    return prompt.c_str();
}
extern "C" const char *playden_ps4_auth_account_name()
{
    static std::string name;
    name = auth().account_name();
    return name.c_str();
}
extern "C" int playden_ps4_library_search_edit(int user_id)
{
    std::string edited;
    const int result = EditTextWithSystemKeyboard(user_id, "Search the library", "Part of a game's name",
                                                  auth().library_query(), 64, &edited);
    if (result != 1) return 0;
    return auth().library_set_query(edited) ? 1 : -1;
}
extern "C" void playden_ps4_library_search_clear() { auth().library_set_query(""); }
extern "C" const char *playden_ps4_library_search_text()
{
    static std::string text;
    text = auth().library_query();
    return text.c_str();
}
extern "C" int playden_ps4_library_sort_cycle() { return auth().library_cycle_sort() ? 1 : 0; }
extern "C" const char *playden_ps4_library_sort_label()
{
    return playden::ps4::kLibrarySorts[auth().library_sort()].label;
}
extern "C" std::size_t playden_ps4_library_collection_count() { return auth().library_collection_count(); }
extern "C" const char *playden_ps4_library_collection_name(std::size_t index)
{
    static std::string names[8];
    static unsigned next = 0;
    auto &name = names[next++ % 8];
    name = auth().library_collection_name(index);
    return name.c_str();
}
extern "C" std::size_t playden_ps4_library_collection_size(std::size_t index)
{
    return auth().library_collection_size(index);
}
extern "C" int playden_ps4_library_collection_create(int user_id)
{
    std::string name;
    if (EditTextWithSystemKeyboard(user_id, "New collection", "Collection name", "", 48, &name) != 1) return -1;
    const int made = auth().library_create_collection(name);
    if (made < 0) LogNetworkLine("library collection create failed (name length " + std::to_string(name.size()) + ")");
    return made;
}
extern "C" int playden_ps4_library_collection_delete(std::size_t index)
{
    return auth().library_delete_collection(index) ? 1 : 0;
}
extern "C" int playden_ps4_library_in_collection(std::uint32_t app_id, std::size_t index)
{
    return auth().library_in_collection(app_id, index) ? 1 : 0;
}
extern "C" int playden_ps4_library_collection_toggle(std::uint32_t app_id, std::size_t index)
{
    return auth().library_toggle_collection(app_id, index) ? 1 : 0;
}
extern "C" int playden_ps4_library_is_family(std::uint32_t app_id) { return auth().library_is_family(app_id) ? 1 : 0; }
extern "C" int playden_ps4_library_toggle_favorite(std::uint32_t app_id)
{
    return auth().library_toggle_favorite(app_id) ? 1 : 0;
}
extern "C" int playden_ps4_library_toggle_hidden(std::uint32_t app_id)
{
    return auth().library_toggle_hidden(app_id) ? 1 : 0;
}
extern "C" void playden_ps4_game_details_start(std::uint32_t app_id) { auth().request_game_details(app_id); }
extern "C" int playden_ps4_game_details_state() { return auth().game_details_state(); }
extern "C" int playden_ps4_game_details_installed() { return auth().game_details_installed() ? 1 : 0; }
extern "C" int playden_ps4_game_details_incomplete() { return auth().game_details_incomplete() ? 1 : 0; }
// One line in the network log, for Playden's C code (the update check).
extern "C" void playden_ps4_log_text(const char *line)
{
    if (line) LogNetworkLine(line);
}

extern "C" int playden_ps4_install_sheet_open(std::uint32_t app_id) { return auth().install_sheet_open(app_id) ? 1 : 0; }
extern "C" void playden_ps4_install_sheet_step(int row, int direction) { auth().install_sheet_step(row, direction); }
extern "C" const char *playden_ps4_install_sheet_value(int row)
{
    static std::string values[4];
    if (row < 0 || row > 3) return "";
    values[row] = auth().install_sheet_value(row);
    return values[row].c_str();
}
extern "C" const char *playden_ps4_install_sheet_note()
{
    static std::string note;
    note = auth().install_sheet_note();
    return note.c_str();
}

extern "C" int playden_ps4_game_details_update() { return auth().game_details_update() ? 1 : 0; }
extern "C" int playden_ps4_library_update_available(std::uint32_t app_id)
{
    return auth().library_update_available(app_id) ? 1 : 0;
}
extern "C" std::size_t playden_ps4_library_update_count() { return auth().library_update_count(); }
extern "C" int playden_ps4_game_details_launch_supported()
{
    return auth().game_details_launch_supported() ? 1 : 0;
}
extern "C" std::uint32_t playden_ps4_game_details_app_id() { return auth().game_details_app_id(); }
extern "C" const char *playden_ps4_game_details_message()
{
    static std::string detail;
    detail = auth().game_details_message();
    return detail.c_str();
}
extern "C" int playden_ps4_game_store_details_ready() { return auth().game_store_details_ready() ? 1 : 0; }
extern "C" int playden_ps4_game_store_details_available() { return auth().game_store_details_available() ? 1 : 0; }
extern "C" const char *playden_ps4_game_store_description()
{
    static std::string detail;
    detail = auth().game_store_description();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_store_genres()
{
    static std::string detail;
    detail = auth().game_store_genres();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_store_developers()
{
    static std::string detail;
    detail = auth().game_store_developers();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_store_publishers()
{
    static std::string detail;
    detail = auth().game_store_publishers();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_store_categories()
{
    static std::string detail;
    detail = auth().game_store_categories();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_store_release_date()
{
    static std::string detail;
    detail = auth().game_store_release_date();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_store_metacritic_score()
{
    static std::string detail;
    detail = auth().game_store_metacritic_score();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_store_controller_support()
{
    static std::string detail;
    detail = auth().game_store_controller_support();
    return detail.c_str();
}
extern "C" int playden_ps4_game_store_windows_supported()
{
    return auth().game_store_windows_supported() ? 1 : 0;
}
extern "C" void playden_ps4_game_install_start(std::uint32_t app_id) { auth().start_game_install(app_id); }
extern "C" void playden_ps4_game_verify_start(std::uint32_t app_id) { auth().start_game_verify(app_id); }
extern "C" int playden_ps4_game_prepare_steamworks(std::uint32_t app_id)
{
    return auth().prepare_game_steamworks(app_id) ? 1 : 0;
}
extern "C" int playden_ps4_game_install_state() { return auth().game_install_state(); }
extern "C" void playden_ps4_game_uninstall_start(std::uint32_t app_id) { auth().start_game_uninstall(app_id); }
extern "C" int playden_ps4_game_uninstall_state() { return auth().game_uninstall_state(); }
extern "C" std::uint32_t playden_ps4_game_uninstall_app_id() { return auth().game_uninstall_app_id(); }
extern "C" const char *playden_ps4_game_uninstall_message()
{
    static std::string detail;
    detail = auth().game_uninstall_message();
    return detail.c_str();
}
extern "C" const char *playden_ps4_game_install_message()
{
    static std::string detail;
    detail = auth().game_install_message();
    return detail.c_str();
}
extern "C" std::uint32_t playden_ps4_queue_app_id() { return auth().queue_app_id(); }
extern "C" int playden_ps4_queue_state() { return auth().game_install_state(); }
extern "C" void playden_ps4_queue_cancel() { auth().cancel_game_install(); }
extern "C" const char *playden_ps4_queue_message()
{
    static std::string detail;
    detail = auth().game_install_message();
    return detail.c_str();
}
extern "C" int playden_ps4_saves_state() { return auth().saves_state(); }
extern "C" std::uint32_t playden_ps4_saves_app_id() { return auth().saves_app_id(); }
extern "C" std::uint64_t playden_ps4_saves_progress_done() { return auth().saves_progress_done(); }
extern "C" std::uint64_t playden_ps4_saves_progress_total() { return auth().saves_progress_total(); }
extern "C" const char *playden_ps4_saves_message()
{
    static std::string detail;
    detail = auth().saves_message();
    return detail.c_str();
}
extern "C" void playden_ps4_saves_start(std::uint32_t app_id, int resolution) { auth().start_saves_sync(app_id, resolution); }
extern "C" int playden_ps4_saves_poll() { return auth().saves_poll() ? 1 : 0; }
extern "C" const char *playden_ps4_log_receiver_ip()
{
    static std::string address;
    address = auth().log_receiver_ip();
    return address.c_str();
}
extern "C" const char *playden_ps4_log_receiver_status()
{
    static std::string status;
    status = auth().log_receiver_status();
    return status.c_str();
}
extern "C" int playden_ps4_edit_text(int user_id, const char *title, const char *placeholder,
                                     const char *initial, char *out, std::size_t out_size)
{
    LogNetworkLine("keyboard open user=" + std::to_string(user_id) + " title=" + (title ? title : ""));
    std::string edited;
    const int result = EditTextWithSystemKeyboard(user_id, title, placeholder, initial ? initial : "",
                                                  out_size > 1 ? out_size - 1 : 0, &edited);
    if (result == 1 && out && out_size > edited.size()) std::memcpy(out, edited.c_str(), edited.size() + 1);
    return result == 1 && (!out || out_size <= edited.size()) ? -1 : result;
}

extern "C" int playden_ps4_edit_log_receiver_ip(int user_id)
{
    return auth().edit_log_receiver_ip(user_id);
}
extern "C" int playden_ps4_logging_enabled() { return auth().logging_enabled() ? 1 : 0; }
extern "C" int playden_ps4_wine_debug_enabled() { return auth().wine_debug_enabled() ? 1 : 0; }
extern "C" const char *playden_ps4_wine_debug_level()
{
    static std::string level;
    level = auth().wine_debug_level();
    return level.c_str();
}
extern "C" int playden_ps4_wait_watchdog_enabled() { return auth().wait_watchdog_enabled() ? 1 : 0; }
extern "C" int playden_ps4_gpu_hang_dump_enabled() { return auth().gpu_hang_dump_enabled() ? 1 : 0; }
extern "C" int playden_ps4_download_connections() { return auth().download_connections(); }
extern "C" const char *playden_ps4_content_language()
{
    static std::string language;
    language = auth().content_language();
    return language.c_str();
}
extern "C" void playden_ps4_cycle_setting(int setting) { auth().cycle_setting(setting); }
