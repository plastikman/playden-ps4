// Playden - PS4 live services for the homebrew UI (step 3a of running Playden's own app on the PS4).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The service graph of the desktop host's live mode (src/homebrew/host.cpp), built with
// PS4 paths and the bundled Orbis-Win kit. The PS4 game handoff is still being wired.

#include "homebrew_live.hpp"

#include "app/diagnostic_log.h"
#include "app/experimental_features.h"
#include "community/endpoints.h"
#include "community/recipe_service.h"
#include "diagnostics/redaction.h"
#include "homebrew/artwork_backend.hpp"
#include "homebrew/controller.hpp"
#include "homebrew/program_icon.hpp"
#include "homebrew/runtime_host.hpp"
#include "homebrew/sharing_service.hpp"
#include "homebrew/store_fetch.hpp"
#include "installs/game_move.h"
#include "installs/install_record.h"
#include "library/cache.h"
#include "runtime/game_probe.h"
#include "runtime/artifact_hash.h"
#include "runtime/private_directories.h"
#include "runtime/settings_storage.h"
#include "runtime/steamworks.h"
#include "runtime_ps4/kit_ps4.hpp"
#include "runtime_ps4/legacy_bottle.hpp"
#include "wine_runtime.h"
#include "steam/backend/auth_client.h"
#include "steam/backend/cm_app_details.h"
#include "steam/backend/download_server_source.h"
#include "steam/backend/entitled_library.h"
#include "steam/backend/library_client.h"
#include "steam/backend/steam_cloud_runner.h"
#include "steam/backend/steam_install_runner.h"
#include "steam/install_volumes.h"
#include "steam/local_session_repository.h"
#include "stores/local/copy_runner.h"
#include "stores/local/library_source.h"
#include "update/version.h"

#include <atomic>
#include <cstring>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#ifndef PLAYDEN_DEV_BUILD
#define PLAYDEN_DEV_BUILD 0
#endif

extern "C" void playden_ps4_log_text(const char *line);

namespace playden::ps4 {
namespace {

constexpr const char *kCaBundle = "/app0/assets/cacert.pem";
constexpr const char *kDataDirectory = "/data/PLDN10001";

std::mutex content_language_lock;
std::string content_language{playden::steam::kDefaultContentLanguage};
std::string selected_content_language()
{
    const std::lock_guard<std::mutex> guard(content_language_lock);
    return content_language;
}

void log_line(const std::string &line)
{
    playden_ps4_log_text(("PLDN10001 live: " + line).c_str());
}

// The PS4 launcher (ps4/platform_ps4) installs Steam games with the same runner into games/<appid>,
// with the same .playden-install.json, but never wrote the install service's registry
// (games/.playden-games). Each start, a finished install the registry doesn't name is added as a
// legacy line ("<appid>": the primary volume), which InstallService::Scan reads as installed.
void adopt_launcher_installs(const std::filesystem::path &games)
{
    const auto registry = games / ".playden-games";
    std::set<std::string> named;
    if (FILE *file = std::fopen(registry.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            const std::string text(line, std::strcspn(line, "\t\r\n"));
            if (!text.empty())
                named.insert(text);
        }
        std::fclose(file);
    }
    std::vector<std::string> adopt;
    std::error_code error;
    for (std::filesystem::directory_iterator it(games, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name.size() > 10 ||
            name.find_first_not_of("0123456789") != std::string::npos || named.count(name))
            continue;
        std::error_code exists_error;
        if (std::filesystem::is_regular_file(it->path() / playden::installs::kInstallRecordName, exists_error))
            adopt.push_back(name);
        // An unfinished download (its .playden-download journal, no record yet): a paused job in Downloads,
        // which Resume continues with the same runner and journal.
        else if (std::filesystem::is_directory(it->path() / ".playden-download", exists_error))
            adopt.push_back(name + "\t" + playden::steam::InstallService::kPrimaryVolumeId + "\t0\tpaused");
    }
    if (error) {
        log_line("games folder unreadable: " + error.message());
        return;
    }
    if (adopt.empty())
        return;
    FILE *file = std::fopen(registry.c_str(), "a");
    if (!file) {
        log_line("cannot add launcher installs to " + registry.string());
        return;
    }
    std::string names;
    for (const auto &name : adopt) {
        std::fprintf(file, "%s\n", name.c_str());
        names += (names.empty() ? "" : " ") + name.substr(0, name.find('\t')) +
                 (name.find('\t') == std::string::npos ? "" : " (paused download)");
    }
    const bool ok = std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    std::fclose(file);
    log_line(std::string(ok ? "adopted" : "could not save adopted") + " launcher installs: " + names);
}

// ManagedDirectory (src/platform/managed_directory.cpp) opens each folder from / down and refuses
// with ESTALE when lstat(path) and fstat(the opened folder) disagree; private folders must be 0700
// and the app's. On the PS4 the settings storage failed that way (02.37): log what each side says.
void probe_directory_identity()
{
    const uid_t uid = ::geteuid();
    log_line("identity probe: euid=" + std::to_string(uid) + " egid=" + std::to_string(::getegid()));
    for (const char *path : {"/", "/data", "/data/PLDN10001", "/data/PLDN10001/games", "/data/PLDN10001/logs"}) {
        struct stat named{}, opened{};
        const int lstat_rc = ::lstat(path, &named);
        const int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        const int open_errno = fd < 0 ? errno : 0;
        const int fstat_rc = fd >= 0 ? ::fstat(fd, &opened) : -1;
        if (fd >= 0)
            ::close(fd);
        char line[400];
        std::snprintf(line, sizeof(line),
                      "identity %s: lstat=%d dev=%llu ino=%llu uid=%u gid=%u mode=%o | open=%d fstat=%d dev=%llu "
                      "ino=%llu uid=%u gid=%u mode=%o",
                      path, lstat_rc, (unsigned long long)named.st_dev, (unsigned long long)named.st_ino,
                      (unsigned)named.st_uid, (unsigned)named.st_gid, (unsigned)named.st_mode, fd >= 0 ? 0 : open_errno,
                      fstat_rc, (unsigned long long)opened.st_dev, (unsigned long long)opened.st_ino,
                      (unsigned)opened.st_uid, (unsigned)opened.st_gid, (unsigned)opened.st_mode);
        log_line(line);
    }
    // Does a private (0700) folder stay 0700 here?
    const char *probe = "/data/PLDN10001/.mode-probe";
    ::rmdir(probe);
    const int made = ::mkdir(probe, 0700);
    struct stat after_mkdir{}, after_chmod{};
    ::lstat(probe, &after_mkdir);
    const int chmod_rc = ::chmod(probe, 0700);
    ::lstat(probe, &after_chmod);
    char line[200];
    std::snprintf(line, sizeof(line), "identity mode probe: mkdir=%d mode=%o uid=%u, chmod=%d mode=%o", made,
                  (unsigned)after_mkdir.st_mode, (unsigned)after_mkdir.st_uid, chmod_rc, (unsigned)after_chmod.st_mode);
    log_line(line);
    ::rmdir(probe);
}

}  // namespace

struct LiveServices::State {
    // The install worker reads this snapshot when a download starts.
    struct DownloadTuning {
        std::atomic<bool> downloads_slow_network{false};
        std::mutex mutex;
        std::string download_server;
    } download_tuning;
    std::filesystem::path data_directory{kDataDirectory};
    std::string log_directory;
    playden::app::ExperimentalFeatures features;

    // Declaration order is destruction order in reverse, as in host.cpp.
    std::unique_ptr<playden::Storage> auth_storage;
    std::shared_ptr<playden::steam::CmSharedSession> steam_cm;
    std::unique_ptr<playden::steam::CmClient> auth_cm;
    std::unique_ptr<playden::steam::AuthClient> auth;
    std::unique_ptr<playden::SessionVault> session_vault;
    std::unique_ptr<playden::LocalSessionRepository> sessions;
    std::unique_ptr<playden::steam::LibraryClient> owned_library;
    std::unique_ptr<playden::LocalSessionRepository> library_sessions;
    std::unique_ptr<playden::Storage> library_storage;
    std::unique_ptr<playden::steam::CmClient> cm_client;
    std::unique_ptr<playden::steam::EntitledLibraryBackend> library_backend;
    playden::steam::LicensedDepots licensed_depots;
    std::unique_ptr<playden::LibraryCache> library_cache;
    std::unique_ptr<playden::LibraryService> library_service;
    std::unique_ptr<playden::PreferencesStore> preferences;
    std::unique_ptr<playden::OrganizationStore> organization;
    std::unique_ptr<playden::UpdateCheckStore> update_checks;
    std::unique_ptr<playden::app::PersistenceService> persistence;
    std::unique_ptr<playden::steam::CmClient> details_cm;
    std::unique_ptr<playden::steam::CmClient> sizes_cm;
    std::unique_ptr<playden::steam::CdnClient> details_cdn;
    std::unique_ptr<playden::steam::DepotInstaller> details_measurer;
    std::unique_ptr<playden::steam::CmAppDetailsSource> details_source;
    std::unique_ptr<playden::LocalSessionRepository> details_sessions;
    std::unique_ptr<playden::steam::AppDetailsService> app_details;
    std::unique_ptr<playden::steam::CmClient> install_cm;
    std::unique_ptr<playden::steam::CdnClient> install_cdn;
    std::unique_ptr<playden::steam::SteamInstallRunner> install_runner;
    std::unique_ptr<playden::LocalSessionRepository> install_sessions;
    std::unique_ptr<playden::Storage> import_storage;
    std::unique_ptr<playden::stores::local::ImportStore> imports;
    std::unique_ptr<playden::stores::local::LocalCopyRunner> copy_runner;
    std::unique_ptr<playden::installs::GameMover> game_mover;
    std::unique_ptr<playden::steam::InstallService> installs;
    std::unique_ptr<playden::steam::CmClient> servers_cm;
    std::unique_ptr<playden::steam::CdnClient> servers_cdn;
    std::unique_ptr<playden::LocalSessionRepository> servers_sessions;
    std::unique_ptr<playden::steam::CdnDownloadServerSource> servers_source;
    std::unique_ptr<playden::steam::DownloadServerService> servers;
    std::unique_ptr<playden::steam::CmClient> cloud_cm;
    std::unique_ptr<playden::steam::HttpClient> cloud_http;
    std::unique_ptr<playden::steam::SteamCloudRunner> cloud_runner;
    std::unique_ptr<playden::LocalSessionRepository> cloud_sessions;
    std::unique_ptr<playden::steam::CloudService> cloud;
    std::unique_ptr<playden::steam::StoreDetailsService> store_details;
    std::unique_ptr<playden::stores::Registry> stores;
    std::unique_ptr<playden::steam::HttpClient> community_http;
    std::unique_ptr<playden::community::CommunityRecipeService> community;
    std::unique_ptr<playden::steam::HttpClient> sharing_http;
    std::unique_ptr<playden::homebrew::SharingService> sharing;
    std::unique_ptr<playden::homebrew::RuntimeHost> runtime_host;
    std::unique_ptr<playden::homebrew::Controller> services;
    std::unique_ptr<playden::homebrew::HttpArtworkBackend> artwork_backend;
    std::unique_ptr<playden::homebrew::ProgramIconArtworkBackend> program_icons;
    std::unique_ptr<playden::homebrew::ArtworkService> artwork;
};

LiveServices::LiveServices() : state_(std::make_unique<State>()) {}
LiveServices::~LiveServices() { Shutdown(); }

playden::homebrew::ArtworkService *LiveServices::artwork() const { return state_->artwork.get(); }

bool LiveServices::Start(playden::homebrew::Simulation &simulation)
{
    State &s = *state_;
    const auto &data = s.data_directory;
    s.log_directory = (data / "logs").string();
    const std::string log_directory = s.log_directory;

    s.features = playden::app::ReadExperimentalFeatures(
        (data / playden::app::kExperimentalFeaturesFileName).string(),
        playden::app::DefaultExperimentalFeatures(playden::update::CurrentVersion().nightly() || PLAYDEN_DEV_BUILD),
        nullptr);
    simulation.SetCommunityRecipesEnabled(s.features.community_recipes);

    probe_directory_identity();
    std::string settings_reason;
    const auto settings_file = playden::runtime::PrepareGameSettingsStorage(data, &settings_reason);
    if (!settings_file)
        log_line("private game settings unavailable: " + settings_reason);
    simulation.SetGameSettingsFile(settings_file.value_or(std::filesystem::path{}));
    if (settings_file)
        for (const auto &line : playden::runtime::MyPresetsLoadLog(
                 simulation.SetMyPresetsFile(playden::runtime::MyPresetsPath(*settings_file))))
            log_line(line);

    s.steam_cm = playden::steam::MakeCmSharedSession();
    s.auth_storage = std::make_unique<playden::Storage>(data.string());
    s.auth_cm = std::make_unique<playden::steam::CmClient>(kCaBundle, *s.auth_storage);
    s.auth_cm->SetOwner("auth");
    s.auth_cm->SetSharedSession(s.steam_cm);
    s.auth = std::make_unique<playden::steam::AuthClient>(kCaBundle);
    s.auth->SetMachineId([storage = s.auth_storage.get()] {
        return playden::steam::MakeDeviceIdentity(playden::steam::LoadOrCreateDeviceSeed(*storage)).machine_id;
    });
    s.auth->SetCmRenewal([cm = s.auth_cm.get()](const playden::StoredSession &stored, const std::atomic<bool> &cancel) {
        return playden::steam::RenewThroughCm(*cm, stored, cancel);
    });
    s.auth->SetFailureReport([log_directory](const char *method, long http_status, int eresult) {
        char line[160];
        std::snprintf(line, sizeof(line), "auth_failure method=%s http=%ld eresult=%d", method, http_status, eresult);
        playden::app::AppendLogLine(log_directory, "auth.log", line);
    });
    s.auth->SetSignInReport([log_directory](long long persistence, std::uint64_t lifetime_seconds) {
        playden::app::AppendLogLine(log_directory, "auth.log", "signed_in persistence=" + std::to_string(persistence) +
                                                                    " lifetime_days=" + std::to_string(lifetime_seconds / 86400));
    });
    const bool stay_signed_in = playden::PreferencesStore(*s.auth_storage).Load().preferences.steam_stay_signed_in;
    s.session_vault = std::make_unique<playden::SessionVault>(stay_signed_in);
    s.sessions = std::make_unique<playden::LocalSessionRepository>(
        data.string(),
        [log_directory](const std::string &diagnostic) {
            playden::app::AppendLogLine(log_directory, "storage.log", diagnostic);
        },
        s.session_vault.get());
    if (!stay_signed_in)
        (void)s.sessions->Clear();
    const auto session_repository = [&] {
        return std::make_unique<playden::LocalSessionRepository>(
            data.string(), playden::LocalSessionRepository::DiagnosticReport{}, s.session_vault.get());
    };
    s.owned_library = std::make_unique<playden::steam::LibraryClient>(kCaBundle);
    s.library_sessions = session_repository();
    s.library_storage = std::make_unique<playden::Storage>(data.string());
    s.cm_client = std::make_unique<playden::steam::CmClient>(kCaBundle, *s.library_storage);
    s.cm_client->SetOwner("library");
    s.cm_client->SetSharedSession(s.steam_cm);
    s.library_backend = std::make_unique<playden::steam::EntitledLibraryBackend>(*s.owned_library, *s.cm_client);
    s.library_backend->SetLicensedDepots(&s.licensed_depots);
    s.library_backend->SetAppNameStorage(s.library_storage.get());
    s.library_backend->SetFamilyDiagnostics(kCaBundle, [log_directory](const std::string &line) {
        playden::app::AppendLogLine(log_directory, "steam-cm.log", line);
    });
    s.library_cache = std::make_unique<playden::LibraryCache>(*s.library_storage);
    s.library_service = std::make_unique<playden::LibraryService>(*s.library_backend, *s.library_sessions,
                                                                  s.library_cache.get());
    s.preferences = std::make_unique<playden::PreferencesStore>(*s.library_storage);
    {
        const auto saved = s.preferences->Load().preferences;
        s.download_tuning.downloads_slow_network.store(saved.downloads_slow_network);
        {
            std::lock_guard lock(s.download_tuning.mutex);
            s.download_tuning.download_server = saved.download_server;
        }
        const std::lock_guard<std::mutex> guard(content_language_lock);
        content_language = saved.content_language;
    }
    s.organization = std::make_unique<playden::OrganizationStore>(*s.library_storage);
    s.update_checks = std::make_unique<playden::UpdateCheckStore>(*s.library_storage);
    s.persistence = std::make_unique<playden::app::PersistenceService>(*s.preferences, *s.organization,
                                                                       s.update_checks.get());
    s.details_cm = std::make_unique<playden::steam::CmClient>(kCaBundle, *s.library_storage);
    s.details_cm->SetOwner("details");
    s.details_cm->SetSharedSession(s.steam_cm);
    s.sizes_cm = std::make_unique<playden::steam::CmClient>(kCaBundle, *s.library_storage);
    s.sizes_cm->SetOwner("sizes");
    s.sizes_cm->SetSharedSession(s.steam_cm);
    s.details_cdn = std::make_unique<playden::steam::CdnClient>(kCaBundle);
    s.details_measurer = std::make_unique<playden::steam::DepotInstaller>(*s.sizes_cm, *s.details_cdn);
    s.details_source = std::make_unique<playden::steam::CmAppDetailsSource>(*s.details_cm);
    s.details_source->SetMeasurer(s.details_measurer.get(), s.sizes_cm.get());
    s.details_source->SetLog([log_directory](std::uint32_t app, const char *step, const char *result,
                                             const std::string &detail) {
        playden::app::AppendLogLine(log_directory, "install.log",
                                    "app=" + std::to_string(app) + " step=" + step + " result=" + result +
                                        " detail=" + detail);
    });
    s.details_source->SetLicensedDepots(&s.licensed_depots);
    s.details_source->SetFreeDlcMemory(s.library_storage.get());
    s.details_source->SetContentLanguage([] { return selected_content_language(); });
    auto *installs_slot = &s.installs;
    s.details_source->SetInstalled([installs_slot](std::uint32_t app) -> std::optional<playden::installs::InstalledRecord> {
        if (!*installs_slot)
            return std::nullopt;
        const auto snapshot = (*installs_slot)->Snapshot();
        for (const auto &game : snapshot.installed)
            if (game.id == playden::GameId::Steam(app) && game.available)
                return game.record;
        return std::nullopt;
    });
    s.details_source->SetInstallRoot([installs_slot](std::uint32_t app) -> std::optional<std::string> {
        if (!*installs_slot)
            return std::nullopt;
        const auto snapshot = (*installs_slot)->Snapshot();
        for (const auto &game : snapshot.installed)
            if (game.id == playden::GameId::Steam(app) && game.available)
                return game.root;
        return std::nullopt;
    });
    s.details_sessions = session_repository();
    s.app_details = std::make_unique<playden::steam::AppDetailsService>(*s.details_source, *s.details_sessions);

    s.install_cm = std::make_unique<playden::steam::CmClient>(kCaBundle, *s.library_storage);
    s.install_cm->SetOwner("install");
    s.install_cm->SetSharedSession(s.steam_cm);
    s.install_cdn = std::make_unique<playden::steam::CdnClient>(kCaBundle);
    s.install_sessions = session_repository();
    s.install_runner = std::make_unique<playden::steam::SteamInstallRunner>(*s.install_sessions, *s.install_cm,
                                                                           *s.install_cdn);
    auto *tuning = &s.download_tuning;
    s.install_runner->SetTuning([data, tuning] {
        const auto file = data / "dev" / "download-tuning.txt";
        std::error_code error;
        if (std::filesystem::exists(file, error))
            return playden::steam::ReadDownloadTuning(file.string());
        playden::steam::DownloadTuning result;
        if (tuning->downloads_slow_network.load())
            result.slots = 8;
        {
            std::lock_guard lock(tuning->mutex);
            result.fastest = tuning->download_server == playden::steam::kFastestDownloadServer;
            if (!result.fastest)
                result.preferred_server = tuning->download_server;
        }
        return result;
    });
    s.install_runner->SetContentLanguage([] { return selected_content_language(); });
    s.install_runner->SetLicensedDepots(&s.licensed_depots);
    s.install_runner->SetFreeDlcMemory(s.library_storage.get());
    s.install_runner->SetLog([log_directory](std::uint32_t app, const char *step, const char *category,
                                             const char *detail) {
        char line[640];
        if (std::string(step) == "install-removal")
            std::snprintf(line, sizeof(line), "install-removal done app=%u %s", app, detail);
        else
            std::snprintf(line, sizeof(line), "app=%u step=%s result=%s detail=%s", app, step, category, detail);
        playden::app::AppendLogLine(log_directory, "install.log", line);
    });
    adopt_launcher_installs(data / "games");
    std::vector<playden::steam::VolumeCandidate> candidates = {
        {playden::steam::InstallService::kPrimaryVolumeId, "PS4 storage", data.string(), (data / "games").string()}};
    s.import_storage = std::make_unique<playden::Storage>(data.string());
    s.imports = std::make_unique<playden::stores::local::ImportStore>(*s.import_storage);
    playden::stores::local::LocalCopyOptions copy_options;
    copy_options.log = [log_directory](const std::string &line) {
        playden::app::AppendLogLine(log_directory, "install.log", line);
    };
    s.copy_runner = std::make_unique<playden::stores::local::LocalCopyRunner>(*s.imports, std::move(copy_options));
    s.installs = std::make_unique<playden::steam::InstallService>(
        playden::installs::InstallService::Runners{{playden::Store::kSteam, s.install_runner.get()},
                                                   {playden::Store::kLocal, s.copy_runner.get()}},
        playden::steam::DiscoverInstallVolumes(candidates));
    playden::installs::GameMoveOptions move_options;
    move_options.log = [log_directory](const std::string &line) {
        playden::app::AppendLogLine(log_directory, "install.log", line);
    };
    s.game_mover = std::make_unique<playden::installs::GameMover>(std::move(move_options));
    s.installs->SetMoveRunner(s.game_mover.get());

    s.servers_cm = std::make_unique<playden::steam::CmClient>(kCaBundle, *s.library_storage);
    s.servers_cm->SetOwner("servers");
    s.servers_cm->SetSharedSession(s.steam_cm);
    s.servers_cdn = std::make_unique<playden::steam::CdnClient>(kCaBundle);
    s.servers_sessions = session_repository();
    s.servers_source = std::make_unique<playden::steam::CdnDownloadServerSource>(
        *s.servers_cm, *s.servers_cdn, [service = s.installs.get()] {
            const auto snapshot = service->Snapshot();
            const playden::steam::InstalledGame *newest = nullptr;
            for (const auto &game : snapshot.installed)
                if (game.available && game.id.steam_app() && (!newest || game.finished_at > newest->finished_at))
                    newest = &game;
            return newest ? *newest->id.steam_app() : 0u;
        });
    s.servers = std::make_unique<playden::steam::DownloadServerService>(*s.servers_source, *s.servers_sessions);
    s.servers->SetLog([log_directory](std::uint32_t app, const char *step, const char *category, const char *detail) {
        char line[640];
        std::snprintf(line, sizeof(line), "app=%u step=%s result=%s detail=%s", app, step, category, detail);
        playden::app::AppendLogLine(log_directory, "install.log", line);
    });

    s.cloud_cm = std::make_unique<playden::steam::CmClient>(kCaBundle, *s.library_storage);
    s.cloud_cm->SetOwner("cloud");
    s.cloud_cm->SetSharedSession(s.steam_cm);
    s.cloud_http = std::make_unique<playden::steam::HttpClient>(kCaBundle);
    s.cloud_runner = std::make_unique<playden::steam::SteamCloudRunner>(*s.cloud_cm, *s.cloud_http);
    s.cloud_runner->SetLog([log_directory](std::uint32_t app, const char *event, const std::string &message) {
        playden::app::AppendLogLine(log_directory, "cloud.log",
                                    "app=" + std::to_string(app) + " event=" + event + " " + message);
    });
    s.cloud_sessions = session_repository();
    s.cloud = std::make_unique<playden::steam::CloudService>(*s.cloud_runner, *s.cloud_sessions);
    simulation.SetLogDirectory(data / "logs");
    simulation.SetRecipeExportTarget(data / "logs", "");

    const auto auth_log = [log_directory](const std::string &line) {
        playden::app::AppendLogLine(log_directory, "auth.log", "sign-in " + line);
    };
    s.stores = std::make_unique<playden::stores::Registry>();
    s.stores->Add(std::make_unique<playden::stores::SteamAccount>(*s.auth, *s.sessions, auth_log));
    s.stores->Add(std::make_unique<playden::stores::SteamLibrarySource>(*s.library_service));
    s.stores->Add(std::make_unique<playden::stores::SteamDetailsSource>(*s.app_details));
    playden::stores::local::LocalLibraryConfig import_config;
    import_config.volumes = {{playden::steam::InstallService::kPrimaryVolumeId, data.string(), "playden-import", false}};
    import_config.log = [log_directory](const std::string &line) {
        playden::app::AppendLogLine(log_directory, "install.log", line);
    };
    import_config.copy_into_playden = [service = s.installs.get(), runner = s.copy_runner.get()](
                                          const playden::stores::local::ImportEntry &entry, const std::string &source,
                                          const std::string &volume_id, std::uint32_t runtime_number) {
        return runner->Enqueue(*service, entry, source, volume_id, runtime_number);
    };
    import_config.move_into_playden = [service = s.installs.get(), runner = s.copy_runner.get()](
                                          const playden::stores::local::ImportEntry &entry, const std::string &source,
                                          std::uint32_t runtime_number) {
        return runner->Move(*service, entry, source, runtime_number);
    };
    import_config.remove_copy_folder = [runner = s.copy_runner.get()](const std::string &root) {
        return runner->Remove(root).empty();
    };
    import_config.game_program = [](const std::string &folder, const std::string &program) {
        return playden::runtime::UnrealGameProgram(folder, program);
    };
    auto local_library = std::make_unique<playden::stores::local::LocalLibrarySource>(*s.imports, std::move(import_config));
    auto *local = local_library.get();
    s.copy_runner->SetChanged([local] { local->Reload(); });
    s.stores->Add(std::move(local_library));

    const auto community_root = data / "community-recipes";
    auto community_objects = std::make_shared<playden::community::RecipeStore>(community_root);
    // The PS4 package describes its bundled Orbis-Win in the same verified view the PS5
    // install service consumes. Game files may stay at their existing install paths; each
    // game's private bottle and title descriptor use Playden's shared /data/playden layout.
    playden::homebrew::RuntimeHostConfig runtime_config;
    runtime_config.boundary = "/data";
    runtime_config.state_root = "/data/playden/runtime-state";
    runtime_config.homebrew_root = "/data/PLDN10001";
    runtime_config.package_paths = {"/data", "/data"};
    runtime_config.admit_kit = playden::ps4::AdmitPs4Kit;
    runtime_config.package_operations.hash_file = playden::runtime::HashArtifactFile;
    runtime_config.package_operations.hash_bytes = playden::runtime::HashArtifactBytes;
    runtime_config.package_operations.migrate_legacy_bottle = playden::ps4::MigrateLegacyBottle;
    runtime_config.package_operations.free_space_unverifiable = true;
    runtime_config.package_operations.validate_bottle = [](const std::filesystem::path &bottle) {
        std::error_code error;
        for (const char *name : {"system.reg", "user.reg", "userdef.reg"})
            if (!std::filesystem::is_regular_file(bottle / name, error))
                return false;
        return true;
    };
    runtime_config.runtime_dir = "/app0/homebrew/assets/runtime";
    runtime_config.steamworks_dir = "/app0/runtime/steamworks";
    runtime_config.community_recipe_bytes =
        playden::homebrew::CommunityRecipeBytes(community_objects, s.features.community_recipes);
    runtime_config.log = [log_directory](const std::string &line) {
        if (line.rfind("startup ", 0) == 0 || line.find("unavailable") != std::string::npos ||
            line.find("directory") != std::string::npos || line.find("kit ") != std::string::npos)
            log_line("runtime: " + line);
        playden::app::AppendLogLine(log_directory, "runtime.log", line);
    };
    runtime_config.settings_record = [settings_file](const playden::GameId &game)
        -> std::optional<playden::runtime::GameSettingsRecord> {
        std::map<playden::GameId, playden::runtime::GameSettingsRecord> records;
        std::string reason;
        if (!settings_file || !playden::runtime::LoadGameSettingsFile(settings_file->string(), &records, &reason))
            return std::nullopt;
        const auto found = records.find(game);
        return found == records.end() ? std::nullopt
                                      : std::optional<playden::runtime::GameSettingsRecord>(found->second);
    };
    runtime_config.my_presets = [settings_file] {
        std::vector<playden::runtime::MyPreset> presets;
        std::string reason;
        if (settings_file)
            (void)playden::runtime::ReadMyPresetsFile(
                playden::runtime::MyPresetsPath(*settings_file).string(), &presets, &reason);
        return presets;
    };
    runtime_config.owned_dlc = [backend = s.library_backend.get(), grants = &s.licensed_depots](
                                   std::uint64_t steam_id, std::uint32_t app_id) {
        std::vector<std::uint32_t> owned;
        const auto grant = grants->Get(steam_id);
        if (!grant)
            return owned;
        for (const auto dlc : backend->DlcOf(app_id))
            if (grant->apps.count(dlc))
                owned.push_back(dlc);
        return owned;
    };
    runtime_config.owned_sdk_apps = [grants = &s.licensed_depots](std::uint64_t steam_id)
        -> std::optional<std::vector<std::uint32_t>> {
        const auto grant = grants->Get(steam_id);
        if (!grant)
            return std::nullopt;
        std::vector<std::uint32_t> owned;
        for (const auto app : playden::runtime::kSourceSdkBaseApps)
            if (grant->apps.count(app))
                owned.push_back(app);
        return owned;
    };
    runtime_config.cloud_saves_root = (data / "games" / ".playden-saves").string();
    runtime_config.launch_title = [log_directory](const std::string &title,
                                                   const std::vector<std::string> &arguments) {
        playden::ps5::TitleResult result;
        result.stage = "ps4-wine-handoff";
        if (arguments.size() != 1 || arguments.front().rfind("session=", 0) != 0) {
            result.code = EINVAL;
            return result;
        }
        const int code = playden_ps4_wine_launch_descriptor(title.c_str(), arguments.front().c_str() + 8);
        result.code = static_cast<std::uint32_t>(code);
        playden::app::AppendLogLine(log_directory, "runtime.log",
                                    "launch_title title=" + title + " stage=" + result.stage +
                                    " returned=" + std::to_string(code));
        return result;
    };
    // PS4 games are profile slots inside PLDN10001, not separately registered
    // console titles. RuntimeHost still requires a remover callback before it
    // retires an interrupted slot; its inventory check must remain free.
    runtime_config.unregister_title = [](const std::string &, const std::string &) {
        return playden::homebrew::TitleRemoverResult{true, 0, "PS4 profile slot"};
    };
    runtime_config.write_title_allow_list = [](const std::string &contents, std::string *error) {
        constexpr const char *root = "/data/playden/title-rule";
        if (!playden::runtime::EnsurePrivateDirectories("/data", root, root, error))
            return false;
        const auto written = playden::Storage(root).WriteDurable(
            "allow-list", playden::Bytes(contents.begin(), contents.end()));
        if (!written.ok() && error)
            *error = written.message();
        return written.ok();
    };
    s.runtime_host = std::make_unique<playden::homebrew::RuntimeHost>(std::move(runtime_config));
    if (!s.runtime_host->Start()) {
        log_line("runtime host could not start");
        s.runtime_host.reset();
    }
    s.services = std::make_unique<playden::homebrew::Controller>(simulation, *s.stores, s.persistence.get(),
                                                                 s.installs.get(), s.cloud.get(), s.runtime_host.get(), auth_log);
    s.services->SetLocalLibrary(local);
    s.services->SetDownloadServers(s.servers.get());
    s.services->SetStaySignedIn([repository = s.sessions.get(), log_directory](bool stay) {
        const auto moved = repository->SetPersist(stay);
        playden::app::AppendLogLine(log_directory, "auth.log",
                                    std::string("stay signed in ") + (stay ? "on" : "off") +
                                        " moved=" + (moved.ok() ? "1" : "0"));
    });
    s.store_details = std::make_unique<playden::steam::StoreDetailsService>(playden::homebrew::StoreFetch(kCaBundle));
    s.services->SetStoreDetails(s.store_details.get());

    if (!s.features.community_recipes) {
        s.services->SetCommunityRecipeCache(community_objects);
    } else {
        playden::community::RecipeServiceConfig config;
        config.store_root = community_root.string();
        std::string endpoints_line;
        config.endpoints = playden::community::LoadEndpoints(
            (data / playden::community::kDevEndpointsFileName).string(), &endpoints_line);
        const auto community_log = [log_directory](const std::string &line) {
            playden::app::AppendLogLine(log_directory, "community.log", playden::diagnostics::RedactSecrets(line));
        };
        if (!endpoints_line.empty())
            community_log(endpoints_line);
        s.community_http = std::make_unique<playden::steam::HttpClient>(kCaBundle);
        config.fetch = [client = s.community_http.get()](const playden::steam::HttpRequest &request,
                                                         const std::atomic<bool> &cancel) {
            return client->Perform(request, cancel);
        };
        config.log = community_log;
        auto keep = std::make_shared<playden::homebrew::CommunityKeepSet>();
        config.referenced = playden::homebrew::CommunityKeepCallback(keep);
        const auto endpoints = config.endpoints;
        s.community = std::make_unique<playden::community::CommunityRecipeService>(std::move(config));
        s.community->Start();
        s.services->SetCommunityRecipes(s.community.get(), keep, /*no_runtime_installs=*/false);
        playden::homebrew::SharingServiceConfig sharing_config;
        sharing_config.store_root = community_root.string();
        sharing_config.endpoints = endpoints;
        s.sharing_http = std::make_unique<playden::steam::HttpClient>(kCaBundle);
        sharing_config.fetch = [client = s.sharing_http.get()](const playden::steam::HttpRequest &request,
                                                               const std::atomic<bool> &cancel) {
            return client->Perform(request, cancel);
        };
        sharing_config.log = community_log;
        sharing_config.logs = playden::web::LogsPaths{log_directory, (data / "homebrew").string(),
                                                      (data / "kits").string(), data.string()};
        s.sharing = std::make_unique<playden::homebrew::SharingService>(std::move(sharing_config));
        s.sharing->Start();
        s.services->SetSharing(s.sharing.get());
        simulation.SetSharingContext("", playden::update::BuildVersionText());
    }

    const std::string art_cache_dir = (data / "art").string();
    s.artwork_backend = std::make_unique<playden::homebrew::HttpArtworkBackend>(kCaBundle, art_cache_dir);
    s.program_icons = std::make_unique<playden::homebrew::ProgramIconArtworkBackend>(*s.artwork_backend);
    s.artwork = std::make_unique<playden::homebrew::ArtworkService>(*s.program_icons);

    const auto snapshot = s.installs->Snapshot();
    log_line("services up: data " + data.string() + ", installed games " + std::to_string(snapshot.installed.size()) +
             ", community recipes " + (s.features.community_recipes ? "on" : "off") + ", stay signed in " +
             (stay_signed_in ? "on" : "off"));
    return true;
}

void LiveServices::Tick(playden::homebrew::Simulation &simulation)
{
    State &s = *state_;
    if (s.services)
        s.services->Tick();
    if (simulation.PreferencesLoaded()) {
        s.download_tuning.downloads_slow_network.store(simulation.state().downloads_slow_network);
        {
            std::lock_guard lock(s.download_tuning.mutex);
            s.download_tuning.download_server = simulation.state().download_server;
        }
        const std::lock_guard<std::mutex> guard(content_language_lock);
        content_language = simulation.state().content_language;
    }
}

void LiveServices::Shutdown()
{
    if (!state_)
        return;
    State &s = *state_;
    if (s.services)
        s.services->Shutdown();
    if (s.runtime_host)
        s.runtime_host->Shutdown();
    if (s.community)
        s.community->Shutdown();
    if (s.sharing)
        s.sharing->Shutdown();
    if (s.servers)
        s.servers->Shutdown();
    if (s.installs)
        s.installs->Shutdown();
    if (s.copy_runner)
        s.copy_runner->SetChanged({});
    if (s.artwork)
        s.artwork->Shutdown();
    state_.reset();
}

}  // namespace playden::ps4
