// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_ps4/game_maintenance.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <orbis/libkernel.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
int playden_ps4_wine_prefix_path(std::uint32_t app_id, char *out, std::size_t capacity);
int playden_ps4_wine_prepare_game_prefix(std::uint32_t app_id);
void playden_ps4_log_text(const char *line);
const char *playden_ps4_game_program_relative(std::uint32_t app_id);
}

// Everything here is function-local or a plain constant: the UI module's static initializers do not run.
namespace playden::ps4 {
namespace {

constexpr const char *kBackupRoot = "/data/PLDN10001/user/save-backups";
constexpr int kKeptBackups = 3;
constexpr std::size_t kDirectoryBlock = 64 * 1024;  // /data's directory block (Orbis-Win, patch 0908)

void Log(const std::string &line) { playden_ps4_log_text(("maintenance " + line).c_str()); }

// FreeBSD dirent records: d_fileno (4), d_reclen (2), d_type (1), d_namlen (1), then the name.
bool ParseDirents(const char *buffer, std::size_t length, std::vector<DirectoryEntry> *out)
{
    std::size_t offset = 0;
    while (offset < length) {
        if (length - offset < 8) return false;
        std::uint16_t reclen = 0;
        std::memcpy(&reclen, buffer + offset + 4, 2);
        const auto type = static_cast<unsigned char>(buffer[offset + 6]);
        const auto namlen = static_cast<unsigned char>(buffer[offset + 7]);
        if (reclen < 8 || reclen > length - offset || static_cast<std::size_t>(namlen) + 8 > reclen) return false;
        const std::string name(buffer + offset + 8, namlen);
        if (!name.empty() && name != "." && name != "..")
            out->push_back({name, type == 4 /* DT_DIR */});
        offset += reclen;
    }
    return true;
}

bool IsDirectory(const std::string &path)
{
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool EnsureDirectories(const std::string &path)
{
    for (std::size_t slash = path.find('/', 1);; slash = path.find('/', slash + 1)) {
        const std::string part = slash == std::string::npos ? path : path.substr(0, slash);
        if (::mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) return false;
        if (slash == std::string::npos) return IsDirectory(path);
    }
}

struct TreeStats {
    std::uint64_t files = 0, bytes = 0;
    std::string error;
};

bool CopyFile(const std::string &from, const std::string &to, TreeStats *stats)
{
    const int in = ::open(from.c_str(), O_RDONLY);
    if (in < 0) { stats->error = "cannot read " + from + " errno=" + std::to_string(errno); return false; }
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        stats->error = "cannot write " + to + " errno=" + std::to_string(errno);
        ::close(in);
        return false;
    }
    std::vector<char> buffer(256 * 1024);
    bool ok = true;
    for (;;) {
        const ssize_t got = ::read(in, buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { stats->error = "read failed " + from; ok = false; break; }
        if (got == 0) break;
        for (ssize_t done = 0; done < got;) {
            const ssize_t put = ::write(out, buffer.data() + done, static_cast<std::size_t>(got - done));
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) { stats->error = "write failed " + to + " errno=" + std::to_string(errno); ok = false; break; }
            done += put;
        }
        if (!ok) break;
        stats->bytes += static_cast<std::uint64_t>(got);
    }
    ::close(in);
    if (::close(out) != 0 && ok) { stats->error = "close failed " + to; ok = false; }
    if (ok) ++stats->files;
    return ok;
}

bool CopyTree(const std::string &from, const std::string &to, TreeStats *stats)
{
    if (!EnsureDirectories(to)) { stats->error = "cannot create " + to + " errno=" + std::to_string(errno); return false; }
    std::vector<DirectoryEntry> entries;
    std::string how;
    if (!ListDirectory(from, &entries, &how)) { stats->error = "cannot list " + from + " (" + how + ")"; return false; }
    for (const auto &entry : entries) {
        const std::string source = from + "/" + entry.name, target = to + "/" + entry.name;
        struct stat info {};
        if (::lstat(source.c_str(), &info) != 0 || S_ISLNK(info.st_mode)) continue;  // links stay out
        if (S_ISDIR(info.st_mode) ? !CopyTree(source, target, stats)
                                  : S_ISREG(info.st_mode) && !CopyFile(source, target, stats))
            return false;
    }
    return true;
}

// Deletes path and everything under it, except the folders `keep` names (relative to `top`).
bool RemoveTree(const std::string &top, const std::string &relative, const std::vector<std::string> &keep,
                TreeStats *stats, bool *kept_something)
{
    const std::string path = relative.empty() ? top : top + "/" + relative;
    if (std::find(keep.begin(), keep.end(), relative) != keep.end()) {
        *kept_something = true;
        return true;
    }
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) {
        if (::unlink(path.c_str()) == 0) { ++stats->files; return true; }
        stats->error = "cannot delete " + path + " errno=" + std::to_string(errno);
        return false;
    }
    std::vector<DirectoryEntry> entries;
    std::string how;
    if (!ListDirectory(path, &entries, &how)) { stats->error = "cannot list " + path + " (" + how + ")"; return false; }
    bool inner_kept = false;
    for (const auto &entry : entries)
        if (!RemoveTree(top, relative.empty() ? entry.name : relative + "/" + entry.name, keep, stats, &inner_kept))
            return false;
    if (inner_kept) { *kept_something = true; return true; }
    if (relative.empty() && !keep.empty()) return true;  // the top folder itself stays when something is kept
    if (::rmdir(path.c_str()) == 0) return true;
    stats->error = "cannot delete folder " + path + " errno=" + std::to_string(errno);
    return false;
}

std::string AppFolder(std::uint32_t app_id) { return std::string(kBackupRoot) + "/" + std::to_string(app_id); }

// backups.txt: "<stamp>\t<files>\t<bytes>" per backup, oldest first (kept as a list: folders can't be relied on
// to list here).
std::vector<std::string> ReadIndex(std::uint32_t app_id)
{
    std::vector<std::string> lines;
    FILE *file = std::fopen((AppFolder(app_id) + "/backups.txt").c_str(), "rb");
    if (!file) return lines;
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        std::string text = line;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (!text.empty()) lines.push_back(text);
    }
    std::fclose(file);
    return lines;
}

bool WriteIndex(std::uint32_t app_id, const std::vector<std::string> &lines)
{
    const std::string path = AppFolder(app_id) + "/backups.txt", part = path + ".part";
    FILE *file = std::fopen(part.c_str(), "wb");
    if (!file) return false;
    for (const auto &line : lines) std::fprintf(file, "%s\n", line.c_str());
    const bool ok = std::fflush(file) == 0 && std::fclose(file) == 0;
    return ok && std::rename(part.c_str(), path.c_str()) == 0;
}

std::string Field(const std::string &line, int index)
{
    std::size_t start = 0;
    for (int i = 0; i < index; ++i) {
        start = line.find('\t', start);
        if (start == std::string::npos) return {};
        ++start;
    }
    return line.substr(start, line.find('\t', start) - start);
}

std::string Megabytes(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

struct Job {
    std::mutex mutex;
    std::atomic<int> state{0};
    std::string message;
};

Job &CurrentJob()
{
    static Job job;
    return job;
}

void Finish(bool ok, const std::string &message)
{
    auto &job = CurrentJob();
    {
        const std::lock_guard<std::mutex> guard(job.mutex);
        job.message = message;
    }
    job.state.store(ok ? 2 : 3);
    Log(std::string(ok ? "done: " : "failed: ") + message);
}

bool BackUp(std::uint32_t app_id, const std::string &prefix, std::string *message)
{
    const std::string users = prefix + "/drive_c/users";
    if (!IsDirectory(users)) { *message = "This game has no saves in its Wine prefix yet."; return false; }
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc {};
    gmtime_r(&now, &utc);
    // Not strftime: the UI PRX's libc reads its locale from thread-local storage that is never set up
    // here (02.09 crashed on the first backup: SIGSEGV at address 0x10 inside strftime).
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    const std::string folder = AppFolder(app_id) + "/" + stamp;
    TreeStats stats;
    if (!CopyTree(users, folder + "/users", &stats)) {
        Log("backup app=" + std::to_string(app_id) + " " + stats.error);
        TreeStats cleanup;
        bool kept = false;
        RemoveTree(folder, "", {}, &cleanup, &kept);
        *message = "Backup failed: " + stats.error;
        return false;
    }
    auto index = ReadIndex(app_id);
    index.push_back(std::string(stamp) + "\t" + std::to_string(stats.files) + "\t" + std::to_string(stats.bytes));
    while (index.size() > static_cast<std::size_t>(kKeptBackups)) {
        TreeStats removed;
        bool kept = false;
        RemoveTree(AppFolder(app_id) + "/" + Field(index.front(), 0), "", {}, &removed, &kept);
        index.erase(index.begin());
    }
    if (!WriteIndex(app_id, index)) { *message = "Backup copied, but its list could not be saved."; return false; }
    *message = "Saves backed up: " + std::to_string(stats.files) + " files, " + Megabytes(stats.bytes) + ".";
    Log("backup app=" + std::to_string(app_id) + " stamp=" + stamp + " files=" + std::to_string(stats.files) +
        " bytes=" + std::to_string(stats.bytes));
    return true;
}

void Run(std::uint32_t app_id, int action)
{
    char prefix_text[512];
    if (playden_ps4_wine_prefix_path(app_id, prefix_text, sizeof(prefix_text)) != 0) {
        Finish(false, "This game has no Wine prefix yet: play it once first.");
        return;
    }
    const std::string prefix = prefix_text;
    std::string message;
    if (action == 1) {
        Finish(BackUp(app_id, prefix, &message), message);
        return;
    }
    if (action == 2) {
        const auto index = ReadIndex(app_id);
        if (index.empty()) { Finish(false, "This game has no save backup."); return; }
        if (!IsDirectory(prefix + "/drive_c") && playden_ps4_wine_prepare_game_prefix(app_id) != 0) {
            Finish(false, "The Wine prefix could not be made to restore into.");
            return;
        }
        TreeStats stats;
        const std::string stamp = Field(index.back(), 0);
        if (!CopyTree(AppFolder(app_id) + "/" + stamp + "/users", prefix + "/drive_c/users", &stats)) {
            Finish(false, "Restore stopped: " + stats.error);
            return;
        }
        Finish(true, "Restored the backup from " + stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2) +
                   (stamp.size() >= 13 ? " " + stamp.substr(9, 2) + ":" + stamp.substr(11, 2) + " UTC" : std::string()) + ": " + std::to_string(stats.files) + " files.");
        return;
    }
    if (action == 3) {
        if (IsDirectory(prefix + "/drive_c/users") && !BackUp(app_id, prefix, &message)) {
            Finish(false, "Reset stopped, nothing was deleted. " + message);
            return;
        }
        // Everything but the saves goes; the completion marker with it, so the next launch copies the template.
        TreeStats stats;
        bool kept = false;
        if (!RemoveTree(prefix, "", {"drive_c/users"}, &stats, &kept)) {
            Finish(false, "Reset stopped partway: " + stats.error + ". Play the game to repair the prefix.");
            return;
        }
        Finish(true, "Wine prefix reset (" + std::to_string(stats.files) +
                         " files removed). The next launch sets it up again; saves were kept.");
        return;
    }
    if (action == 4) {
        // Before an uninstall that removes the prefix: back the saves up, then delete the whole prefix.
        if (IsDirectory(prefix + "/drive_c/users") && !BackUp(app_id, prefix, &message)) {
            Finish(false, "Nothing was deleted. " + message);
            return;
        }
        TreeStats stats;
        bool kept = false;
        if (!RemoveTree(prefix, "", {}, &stats, &kept)) {
            Finish(false, "The Wine prefix could not be removed: " + stats.error);
            return;
        }
        Finish(true, "Saves backed up and the Wine prefix removed.");
        return;
    }
    if (action == 5) {
        // The game's DXVK pipeline cache beside its program, and the RADV shader cache all games share.
        int removed = 0;
        const char *program = playden_ps4_game_program_relative(app_id);
        if (program && program[0]) {
            std::string base = "/data/PLDN10001/games/" + std::to_string(app_id) + "/" + program;
            const auto dot = base.rfind('.');
            if (dot != std::string::npos && base.find('/', dot) == std::string::npos) base.erase(dot);
            if (std::remove((base + ".dxvk-cache").c_str()) == 0) ++removed;
        }
        TreeStats stats;
        bool kept = false;
        const std::string radv = "/data/PLDN10001/cache/radv";
        if (IsDirectory(radv) && !RemoveTree(radv, "", {}, &stats, &kept)) {
            Finish(false, "Shader cache clear stopped partway: " + stats.error);
            return;
        }
        Finish(true, "Shader caches cleared (" + std::to_string(stats.files + removed) +
                         " files). Games build them again as they play; the first minutes may stutter.");
        return;
    }
    Finish(false, "Unknown maintenance action.");
}

}  // namespace

bool ListDirectory(const std::string &path, std::vector<DirectoryEntry> *out, std::string *how)
{
    out->clear();
    std::string notes;
    if (DIR *directory = ::opendir(path.c_str())) {
        errno = 0;
        while (const dirent *entry = ::readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            out->push_back({name, IsDirectory(path + "/" + name)});
        }
        const int error = errno;
        ::closedir(directory);
        if (error == 0) { if (how) *how = "readdir"; return true; }
        notes = "readdir errno=" + std::to_string(error);
        out->clear();
    } else {
        notes = "opendir errno=" + std::to_string(errno);
    }
    std::vector<char> buffer(kDirectoryBlock);
    const auto drain = [&](auto read_block, int fd, const char *name) {
        int got = 0;
        bool parsed = true;
        while ((got = read_block(fd, buffer.data(), static_cast<int>(buffer.size()))) > 0)
            if (!(parsed = ParseDirents(buffer.data(), static_cast<std::size_t>(got), out))) break;
        if (got == 0 && parsed) { if (how) *how = name; return true; }
        notes += std::string(" ") + name + (parsed ? " errno=" + std::to_string(errno) : " malformed");
        out->clear();
        return false;
    };
    constexpr int kSceDirectory = 0x00020000;  // FreeBSD O_DIRECTORY
    const int kernel_fd = sceKernelOpen(path.c_str(), kSceDirectory, 0);
    if (kernel_fd >= 0) {
        const bool ok = drain([](int f, char *b, int n) { return sceKernelGetdents(f, b, n); }, kernel_fd,
                              "sceKernelGetdents");
        sceKernelClose(kernel_fd);
        if (ok) { for (auto &e : *out) e.directory = IsDirectory(path + "/" + e.name); return true; }
    } else {
        notes += " sceKernelOpen=" + std::to_string(kernel_fd);
    }
    if (how) *how = notes;
    return false;
}

}  // namespace playden::ps4

extern "C" int playden_ps4_maintenance_start(std::uint32_t app_id, int action)
{
    auto &job = playden::ps4::CurrentJob();
    if (app_id == 0 || action < 1 || action > 5) return -1;
    int state = job.state.load();
    do {
        if (state == 1) return -1;
    } while (!job.state.compare_exchange_weak(state, 1));
    {
        const std::lock_guard<std::mutex> guard(job.mutex);
        job.message = action == 1 ? "Backing up saves…" : action == 2 ? "Restoring saves…" :
                      action == 3 ? "Resetting the Wine prefix…" : action == 5 ? "Clearing shader caches…" :
                      "Backing up saves, then removing the Wine prefix…";
    }
    playden::ps4::Log("start app=" + std::to_string(app_id) + " action=" + std::to_string(action));
    std::thread([app_id, action] { playden::ps4::Run(app_id, action); }).detach();
    return 0;
}

extern "C" int playden_ps4_maintenance_state(void) { return playden::ps4::CurrentJob().state.load(); }

extern "C" const char *playden_ps4_maintenance_message(void)
{
    static std::string message;
    auto &job = playden::ps4::CurrentJob();
    const std::lock_guard<std::mutex> guard(job.mutex);
    message = job.message;
    return message.c_str();
}

extern "C" const char *playden_ps4_maintenance_backup_label(std::uint32_t app_id)
{
    static std::string label;
    const auto index = playden::ps4::ReadIndex(app_id);
    if (index.empty()) { label.clear(); return label.c_str(); }
    const std::string stamp = playden::ps4::Field(index.back(), 0);
    const std::string bytes = playden::ps4::Field(index.back(), 2);
    label = "Last backup " + stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2) + " " +
            stamp.substr(9, 2) + ":" + stamp.substr(11, 2) + " UTC, " +
            playden::ps4::Megabytes(std::strtoull(bytes.c_str(), nullptr, 10)) + " (" +
            std::to_string(index.size()) + " kept)";
    return label.c_str();
}
