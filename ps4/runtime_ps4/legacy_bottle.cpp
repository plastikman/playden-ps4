#include "runtime_ps4/legacy_bottle.hpp"
#include "wine_runtime.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::ps4 {
namespace fs = std::filesystem;
namespace {

runtime::PackageActionResult Fail(const std::string &reason)
{
    return {runtime::PackageActionState::kFailed, "legacy-bottle-migration", reason};
}

bool SyncFile(const fs::path &path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool CopyFileReplacing(const fs::path &source, const fs::path &destination, std::string *error)
{
    const int input = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0) { *error = "cannot open legacy save: " + std::string(strerror(errno)); return false; }
    struct stat info{};
    if (::fstat(input, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(input); *error = "legacy save is not a regular file"; return false;
    }
    const int output = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) {
        *error = "cannot open save destination: " + std::string(strerror(errno));
        ::close(input); return false;
    }
    char bytes[64 * 1024];
    bool ok = true;
    for (;;) {
        const ssize_t count = ::read(input, bytes, sizeof(bytes));
        if (count == 0) break;
        if (count < 0) { ok = false; break; }
        ssize_t written = 0;
        while (written < count) {
            const ssize_t part = ::write(output, bytes + written, static_cast<size_t>(count - written));
            if (part <= 0) { ok = false; break; }
            written += part;
        }
        if (!ok) break;
    }
    if (ok) ok = ::fsync(output) == 0;
    const int failure = errno;
    ::close(output);
    ::close(input);
    if (!ok) *error = "cannot copy legacy save: " + std::string(strerror(failure));
    return ok;
}

bool SyncDirectory(const fs::path &path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool SetPermissionsIfSupported(const fs::path &path, fs::perms mode, std::error_code *error)
{
    fs::permissions(path, mode, fs::perm_options::replace, *error);
    // The PS4 /data filesystem may report ENOSYS for chmod even though the
    // app's private data tree is writable. In that case keep the inherited
    // mode and continue the save migration; other permission failures matter.
    if (*error == std::errc::function_not_supported) {
        error->clear();
        return true;
    }
    return !*error;
}

bool CopyMutableTree(const fs::path &source, const fs::path &destination, std::string *error)
{
    std::error_code ec;
    if (!fs::exists(source, ec)) return !ec;
    if (!fs::is_directory(fs::symlink_status(source, ec)) || ec) {
        *error = "legacy save folder is not a directory";
        return false;
    }
    fs::create_directories(destination, ec);
    if (ec) { *error = "cannot create save folder: " + ec.message(); return false; }
    if (!SetPermissionsIfSupported(destination, fs::perms::owner_all, &ec)) {
        *error = "cannot set save folder mode: " + ec.message(); return false;
    }
    std::vector<fs::path> directories{destination};
    for (fs::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        const auto relative = it->path().lexically_relative(source);
        const auto target = destination / relative;
        const auto type = it->symlink_status(ec);
        if (ec || fs::is_symlink(type)) { *error = "legacy saves contain an unsafe link"; return false; }
        if (fs::is_directory(type)) {
            const auto existing = fs::symlink_status(target, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) break;
            ec.clear();
            if (fs::exists(existing) && !fs::is_directory(existing)) {
                *error = "save destination is not a directory";
                return false;
            }
            fs::create_directories(target, ec);
            if (ec) break;
            SetPermissionsIfSupported(target, fs::perms::owner_all, &ec);
            directories.push_back(target);
        } else if (fs::is_regular_file(type)) {
            const auto existing = fs::symlink_status(target, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) break;
            ec.clear();
            if (fs::exists(existing) && !fs::is_regular_file(existing)) {
                *error = "save destination is not a regular file";
                return false;
            }
            if (!CopyFileReplacing(it->path(), target, error)) return false;
            SetPermissionsIfSupported(target, fs::perms::owner_read | fs::perms::owner_write, &ec);
            if (ec || !SyncFile(target)) { *error = "cannot persist a migrated save"; return false; }
        } else {
            *error = "legacy saves contain a non-file entry";
            return false;
        }
        if (ec) break;
    }
    if (ec) { *error = "cannot copy legacy saves: " + ec.message(); return false; }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
        if (!SyncDirectory(*it)) { *error = "cannot persist a migrated save folder"; return false; }
    return true;
}

bool CopyRegistry(const fs::path &source, const fs::path &destination, std::string *error)
{
    std::error_code ec;
    if (!fs::exists(source, ec)) return !ec;
    if (!fs::is_regular_file(fs::symlink_status(source, ec)) || ec ||
        fs::is_symlink(fs::symlink_status(destination, ec))) {
        *error = "legacy registry path is unsafe";
        return false;
    }
    ec.clear();
    if (!CopyFileReplacing(source, destination, error)) return false;
    SetPermissionsIfSupported(destination, fs::perms::owner_read | fs::perms::owner_write, &ec);
    return !ec && SyncFile(destination);
}
}  // namespace

runtime::PackageActionResult MigrateLegacyBottle(const runtime::RuntimeInstall &record,
                                                   const fs::path &bottle)
{
    const runtime::PackageActionResult complete{runtime::PackageActionState::kComplete, "", ""};
    if (!record.runtime_number || record.install_id != "steam-" + std::to_string(record.runtime_number))
        return complete;
    char active_prefix[512];
    const bool named = playden_ps4_wine_prefix_path(record.runtime_number, active_prefix,
                                                     sizeof(active_prefix)) == 0;
    fs::path old = named ? fs::path(active_prefix) : fs::path("/data/PLDN10001/prefixes") /
        (record.runtime_number == 220 ? "half-life-2" : "steam-" + std::to_string(record.runtime_number));
    std::error_code ec;
    if (!fs::exists(old, ec)) return ec ? Fail("cannot inspect the old Wine prefix") : complete;
    if (!fs::is_directory(fs::symlink_status(old, ec)) || ec)
        return Fail("the old Wine prefix is not a regular folder");
    const fs::path marker = bottle / ".playden-legacy-saves-v1";
    if (fs::is_regular_file(marker, ec) && !ec) return complete;
    if (ec && ec != std::errc::no_such_file_or_directory)
        return Fail("cannot inspect the save migration marker");
    std::string error;
    if (!CopyMutableTree(old / "drive_c/users", bottle / "drive_c/users", &error))
        return Fail(error);
    for (const char *name : {"user.reg", "userdef.reg"})
        if (!CopyRegistry(old / name, bottle / name, &error)) return Fail(error);
    const fs::path temporary = bottle / ".playden-legacy-saves-v1.tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return Fail("cannot write the save migration marker");
    static const char content[] = "legacy-saves-copied-v1\n";
    const bool durable = ::write(fd, content, sizeof(content) - 1) == sizeof(content) - 1 &&
                         ::fsync(fd) == 0;
    ::close(fd);
    if (!durable) return Fail("cannot persist the save migration marker");
    fs::rename(temporary, marker, ec);
    if (ec || !SyncDirectory(bottle)) return Fail("cannot publish the save migration marker");
    return complete;
}
}  // namespace playden::ps4
