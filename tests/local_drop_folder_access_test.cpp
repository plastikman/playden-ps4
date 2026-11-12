// The drop-folder walk: a game FTP copied in (root-owned 0755 folders, 0644 files) is opened to
// 0777/0666 with x kept, never past a link, resumably. The host can't chown, so "root-owned" is
// simulated: the walk is told the app user is someone else, and a fake as_root runs the work.
#include "stores/local/drop_folder_access.h"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace playden::stores::local;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
#if defined(PLAYDEN_TEST_DUP_DIR_EPERM)
extern "C" int __real_dup(int fd);
extern "C" int __wrap_dup(int fd) {
  struct stat info{};
  if (::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) { errno = EPERM; return -1; }
  return __real_dup(fd);
}
#endif
namespace {
// The console's walk (by path) can't read a link, so every link fails it there.
#if defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5
constexpr bool kConsole = true;
#else
constexpr bool kConsole = false;
#endif
unsigned Mode(const fs::path& path) {
  struct stat info{};
  CHECK(lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}
void Write(const fs::path& path, unsigned mode) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << "data";
  CHECK(chmod(path.c_str(), mode) == 0);
}
void Folder(const fs::path& path, unsigned mode) {
  fs::create_directories(path);
  CHECK(chmod(path.c_str(), mode) == 0);
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}  // namespace

int main() {
  const char* scratch = std::getenv("SCRATCH_ROOT");
  const std::string pattern = ((scratch ? fs::path(scratch) : fs::path("/tmp")) / "playden-drop-folder-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back('\0');
  CHECK(mkdtemp(name.data()));
  // /tmp is a link on macOS; the walk never follows one.
  const fs::path root = fs::canonical(name.data());
  const fs::path drop = root / "data/playden-import";
  fs::create_directories(drop);
  CHECK(chmod(drop.c_str(), 0777) == 0);

  std::vector<std::string> lines;
  unsigned raises = 0;
  int refuse_after = -1;  // as_root refuses from this call on (an interrupted walk)
  DropFolderAccessOptions options;
  options.namespace_root = root;
  options.log = [&lines](const std::string& line) { lines.push_back(line); };
  options.as_root = [&](const std::function<bool()>& work) {
    if (refuse_after >= 0 && static_cast<int>(raises) >= refuse_after) { ++raises; return false; }
    ++raises;
    return work();
  };
  // Every entry is someone else's (root's, as FTP writes it): changes go through as_root.
  options.app_uid = ::geteuid() + 1;

  // Only a folder directly inside /data/playden-import is walked.
  for (const char* outside : {"/data/playden-import", "/data/playden-import/", "/data/playden-import/a/b",
                              "/data/playden-import/../playden/x", "/data/playden-import/.", "/data/playden-import/..",
                              "/data/playden/x", "/mnt/usb0/playden/import/x", "data/playden-import/x",
                              "/data/playden-import/x/", "/data/playden-import//x", ""}) {
    const auto refused = OpenDropFolderEntry(outside, options);
    CHECK(!refused.ok && Has(refused.message, "directly inside /data/playden-import") && refused.raises == 0);
  }
  CHECK(raises == 0);
  CHECK(IsDropFolderEntry("/data/playden-import/Night Sky") && !IsDropFolderEntry("/data/playden-import/a/b"));
  // A missing folder fails, naming it.
  {
    const auto missing = OpenDropFolderEntry("/data/playden-import/Missing", options);
    CHECK(!missing.ok && Has(missing.message, "/data/playden-import/Missing"));
  }

  // A root-owned tree as FTP leaves it: folders 0755, files 0644, the programs 0755/0744.
  const auto game = drop / "Game";
  Folder(game / "Bin", 0755);
  Folder(game / "Deep/a/b", 0755);
  Write(game / "Bin/Game.exe", 0755);
  Write(game / "Bin/run.sh", 0744);
  Write(game / "settings.ini", 0644);
  Write(game / "Deep/a/b/save.dat", 0640);
  CHECK(chmod(game.c_str(), 0755) == 0);
  // A file with a second name is left alone; so is a link that stays inside.
  Write(game / "shared.dat", 0644);
  CHECK(link((game / "shared.dat").c_str(), (game / "Deep/shared-again.dat").c_str()) == 0);
  if (kConsole) {
    // On the console a link fails the walk even inside the folder, named; removed, it goes on.
    CHECK(symlink("Bin", (game / "Programs").c_str()) == 0);
    const auto refused = OpenDropFolderEntry("/data/playden-import/Game", options);
    CHECK(!refused.ok && Has(refused.message, "/data/playden-import/Game/Programs"));
    CHECK(unlink((game / "Programs").c_str()) == 0);
    for (const auto& folder : {game, game / "Bin", game / "Deep", game / "Deep/a", game / "Deep/a/b"}) CHECK(chmod(folder.c_str(), 0755) == 0);
    for (const auto& file : {"settings.ini", "shared.dat"}) CHECK(chmod((game / file).c_str(), 0644) == 0);
    CHECK(chmod((game / "Bin/Game.exe").c_str(), 0755) == 0 && chmod((game / "Bin/run.sh").c_str(), 0744) == 0);
    CHECK(chmod((game / "Deep/a/b/save.dat").c_str(), 0640) == 0);
    raises = 0;
  } else {
    CHECK(symlink("../settings.ini", (game / "Bin/settings-link.ini").c_str()) == 0);
    CHECK(symlink("Bin", (game / "Programs").c_str()) == 0);
  }
  lines.clear();
  auto opened = OpenDropFolderEntry("/data/playden-import/Game", options);
  CHECK(opened.ok && opened.message.empty());
  for (const auto& folder : {game, game / "Bin", game / "Deep", game / "Deep/a", game / "Deep/a/b"}) CHECK(Mode(folder) == 0777);
  CHECK(Mode(game / "settings.ini") == 0666 && Mode(game / "Deep/a/b/save.dat") == 0666);
  CHECK(Mode(game / "Bin/Game.exe") == 0777 && Mode(game / "Bin/run.sh") == 0766);
  CHECK(Mode(game / "shared.dat") == 0644 && Mode(game / "Deep/shared-again.dat") == 0644 && opened.hard_linked == 2);
  CHECK(kConsole ? opened.links == 0
                 : fs::is_symlink(game / "Bin/settings-link.ini") && fs::is_symlink(game / "Programs") && opened.links == 2);
  CHECK(opened.folders == 5 && opened.files == 6 && opened.changed == 9);
  // Root in short raises: batches of root-owned entries, never one for the whole tree.
  CHECK(opened.raises >= 1 && opened.raises == raises);
  CHECK(Has(lines.back(), "drop folder opened entry=/data/playden-import/Game"));
  bool logged_link = false;
  for (const auto& line : lines) logged_link = logged_link || Has(line, "skips /data/playden-import/Game/shared.dat");
  CHECK(logged_link);

  // Again on the open tree: nothing changes and root is never raised.
  raises = 0;
  opened = OpenDropFolderEntry("/data/playden-import/Game", options);
  CHECK(opened.ok && opened.changed == 0 && opened.raises == 0 && raises == 0 && Mode(game / "settings.ini") == 0666);

  // FTP added a file since: the next walk opens just that one.
  Write(game / "Deep/a/new.cfg", 0644);
  opened = OpenDropFolderEntry("/data/playden-import/Game", options);
  CHECK(opened.ok && opened.changed == 1 && opened.raises == 1 && Mode(game / "Deep/a/new.cfg") == 0666);

  // Entries the app user owns are changed without root.
  {
    auto own = options;
    own.app_uid.reset();
    raises = 0;
    CHECK(chmod((game / "settings.ini").c_str(), 0644) == 0 && chmod((game / "Bin").c_str(), 0755) == 0);
    opened = OpenDropFolderEntry("/data/playden-import/Game", own);
    CHECK(opened.ok && opened.changed == 2 && raises == 0 && Mode(game / "settings.ini") == 0666 && Mode(game / "Bin") == 0777);
  }

  // Without root, root-owned entries can't be opened: the walk fails naming the path.
  {
    auto rootless = options;
    rootless.as_root = nullptr;
    CHECK(chmod((game / "settings.ini").c_str(), 0644) == 0);
    opened = OpenDropFolderEntry("/data/playden-import/Game", rootless);
    CHECK(!opened.ok && Has(opened.message, "/data/playden-import/Game/settings.ini") && Mode(game / "settings.ini") == 0644);
    CHECK(OpenDropFolderEntry("/data/playden-import/Game", options).ok && Mode(game / "settings.ini") == 0666);
  }

  // A link that leads outside the game's folder fails the walk, and what it points at is untouched.
  {
    const auto escaping = drop / "Escaping";
    const auto outside = root / "outside";
    Folder(outside, 0755);
    Write(outside / "secret.txt", 0644);
    Write(escaping / "Game.exe", 0644);
    Folder(escaping / "Sub", 0755);
    CHECK(symlink("../../../outside", (escaping / "Sub/out").c_str()) == 0);
    const auto failed = OpenDropFolderEntry("/data/playden-import/Escaping", options);
    CHECK(!failed.ok && Has(failed.message, "/data/playden-import/Escaping/Sub/out") &&
          Has(failed.message, kConsole ? "can't be checked" : "outside"));
    CHECK(Mode(outside) == 0755 && Mode(outside / "secret.txt") == 0644);
    // Absolute targets too, unless they name a place inside the folder.
    CHECK(unlink((escaping / "Sub/out").c_str()) == 0);
    CHECK(symlink(outside.c_str(), (escaping / "Sub/out").c_str()) == 0);
    CHECK(!OpenDropFolderEntry("/data/playden-import/Escaping", options).ok && Mode(outside) == 0755);
    CHECK(unlink((escaping / "Sub/out").c_str()) == 0);
    CHECK(symlink("/data/playden-import/Escaping/Game.exe", (escaping / "Sub/out").c_str()) == 0);
    CHECK(symlink("/data/playden-import/EscapingNot/x", (escaping / "near").c_str()) == 0);
    const auto near = OpenDropFolderEntry("/data/playden-import/Escaping", options);
    CHECK(!near.ok && Has(near.message, kConsole ? "/data/playden-import/Escaping/" : "/data/playden-import/Escaping/near"));
    CHECK(unlink((escaping / "near").c_str()) == 0);
    if (kConsole) CHECK(unlink((escaping / "Sub/out").c_str()) == 0);
    CHECK(OpenDropFolderEntry("/data/playden-import/Escaping", options).ok && Mode(escaping / "Game.exe") == 0666);
    CHECK(Mode(outside) == 0755 && Mode(outside / "secret.txt") == 0644);
    // The game's folder itself a link: refused.
    CHECK(symlink(outside.c_str(), (drop / "Linked").c_str()) == 0);
    const auto linked = OpenDropFolderEntry("/data/playden-import/Linked", options);
    CHECK(!linked.ok && Has(linked.message, "link") && Mode(outside) == 0755);
  }

  // Interrupted part way (root refused after the first raise), then run again: the whole tree opens.
  {
    const auto big = drop / "Big";
    for (int folder = 0; folder < 4; ++folder) {
      Folder(big / ("f" + std::to_string(folder)), 0755);
      for (int file = 0; file < 5; ++file) Write(big / ("f" + std::to_string(folder)) / ("x" + std::to_string(file)), 0644);
    }
    CHECK(chmod(big.c_str(), 0755) == 0);
    auto small = options;
    small.batch = 3;
    raises = 0;
    refuse_after = 1;
    const auto stopped = OpenDropFolderEntry("/data/playden-import/Big", small);
    CHECK(!stopped.ok && Has(stopped.message, "/data/playden-import/Big") && raises == 2);
    unsigned open_files = 0;
    for (int folder = 0; folder < 4; ++folder)
      for (int file = 0; file < 5; ++file)
        open_files += Mode(big / ("f" + std::to_string(folder)) / ("x" + std::to_string(file))) == 0666;
    CHECK(open_files < 20);
    refuse_after = -1;
    const auto redone = OpenDropFolderEntry("/data/playden-import/Big", small);
    CHECK(redone.ok && redone.changed > 0);
    for (int folder = 0; folder < 4; ++folder) {
      CHECK(Mode(big / ("f" + std::to_string(folder))) == 0777);
      for (int file = 0; file < 5; ++file) CHECK(Mode(big / ("f" + std::to_string(folder)) / ("x" + std::to_string(file))) == 0666);
    }
    CHECK(Mode(big) == 0777);
    CHECK(OpenDropFolderEntry("/data/playden-import/Big", small).changed == 0);
  }

  // A file nobody may read, not even its owner: named, nothing else hidden.
  if (::geteuid() != 0) {
    const auto locked = drop / "Locked";
    Write(locked / "Game.exe", 0644);
    Write(locked / "sealed.bin", 0000);
    const auto failed = OpenDropFolderEntry("/data/playden-import/Locked", options);
    CHECK(!failed.ok && Has(failed.message, "/data/playden-import/Locked/sealed.bin"));
    CHECK(chmod((locked / "sealed.bin").c_str(), 0644) == 0);
  }

  // The callback form: empty on success, the sentence otherwise.
  const auto opener = MakeDropFolderOpener(options);
  CHECK(opener("/data/playden-import/Game").empty() && !opener("/data/playden/Game").empty());

  // A copied entry can be pruned even when directory descriptors cannot be duplicated.
  // Files outside the copy plan and links stay in the drop folder.
  {
    const auto copied = drop / "Copied";
    Write(copied / "owned.txt", 0644);
    Write(copied / "Sub/owned.bin", 0644);
    Write(copied / "keep.txt", 0644);
    CHECK(symlink("keep.txt", (copied / "link").c_str()) == 0);
    const DropFolderPlanned planned = [](const std::string& relative, const struct stat&) {
      return relative == "owned.txt" || relative == "Sub/owned.bin";
    };
    const auto partial = RemoveDropFolderEntry(drop.string(), "Copied", planned, options.as_root);
    CHECK(!partial.ok && partial.files == 2 && partial.kept >= 2);
    CHECK(!fs::exists(copied / "owned.txt") && !fs::exists(copied / "Sub/owned.bin"));
    CHECK(fs::exists(copied / "keep.txt") && fs::is_symlink(copied / "link"));
    CHECK(unlink((copied / "link").c_str()) == 0 && unlink((copied / "keep.txt").c_str()) == 0);
    const auto finished = RemoveDropFolderEntry(drop.string(), "Copied", planned, options.as_root);
    CHECK(finished.ok && !fs::exists(copied));
  }

  std::error_code ignored;
  fs::remove_all(root, ignored);
  std::puts("drop folder access tests passed");
  return 0;
}
