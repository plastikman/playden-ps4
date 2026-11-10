// Playden PS4 port - Steam QR sign-in adapter.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <cstdint>
#include <memory>
#include "library/model.h"
#include "library/organization.h"
#include "steam/backend/app_info.h"
#include <optional>

namespace playden { class LibraryCache; class LibraryService; class Storage; }
namespace playden::steam { class AuthClient; class SteamService; class StoreDetailsService; class LibraryClient; struct StoreDetails; }

namespace playden::ps4
{

class SteamQrAuth
{
  public:
    SteamQrAuth();
    ~SteamQrAuth();
    SteamQrAuth(const SteamQrAuth &) = delete;
    SteamQrAuth &operator=(const SteamQrAuth &) = delete;

    bool start();
    bool restore_cached_session();
    void cancel();
    // Pumps completed worker events. Returns true when the UI should redraw.
    bool poll();
    int state() const { return state_; }
    const std::string &challenge_url() const { return challenge_url_; }
    const std::string &failure_detail() const { return failure_detail_; }
    std::size_t library_count() const;
    const char *library_title(std::size_t index) const;
    const char *library_title_by_app_id(std::uint32_t app_id) const;
    std::uint32_t library_app_id(std::size_t index) const;
    std::size_t library_visible_count(int filter) const;
    const char *library_visible_title(int filter, std::size_t index) const;
    std::uint32_t library_visible_app_id(int filter, std::size_t index) const;
    bool library_is_favorite(std::uint32_t app_id) const;
    bool library_is_hidden(std::uint32_t app_id) const;
    bool library_toggle_favorite(std::uint32_t app_id);
    bool library_toggle_hidden(std::uint32_t app_id);
    std::string library_detail() const;
    void request_game_details(std::uint32_t app_id);
    int game_details_state() const;
    bool game_details_installed() const;
    bool game_details_incomplete() const;
    bool game_details_update() const;
    bool game_details_launch_supported() const;
    std::uint32_t game_details_app_id() const;
    std::string game_details_message() const;
    bool game_store_details_ready() const;
    bool game_store_details_available() const;
    std::string game_store_description() const;
    std::string game_store_genres() const;
    std::string game_store_developers() const;
    std::string game_store_publishers() const;
    std::string game_store_categories() const;
    std::string game_store_release_date() const;
    std::string game_store_metacritic_score() const;
    std::string game_store_controller_support() const;
    bool game_store_windows_supported() const;
    void start_game_install(std::uint32_t app_id);
    bool prepare_game_steamworks(std::uint32_t app_id);
    int game_install_state() const;
    std::string game_install_message() const;
    std::uint32_t queue_app_id() const;
    void cancel_game_install();
    int saves_state() const;
    std::uint32_t saves_app_id() const;
    std::string saves_message() const;
    bool saves_poll();
    std::uint64_t saves_progress_done() const;
    std::uint64_t saves_progress_total() const;
    void start_saves_sync(std::uint32_t app_id, int resolution);
    std::string content_language() const;
    int download_connections() const;
    std::string log_receiver_ip() const;
    std::string log_receiver_status() const;
    bool logging_enabled() const;
    void cycle_setting(int setting);
    int edit_log_receiver_ip(int user_id);
    bool wine_debug_enabled() const;
    std::string wine_debug_level() const;
    bool wait_watchdog_enabled() const;
    bool gpu_hang_dump_enabled() const;
    void start_game_uninstall(std::uint32_t app_id);
    int game_uninstall_state() const;
    std::uint32_t game_uninstall_app_id() const;
    std::string game_uninstall_message() const;

  private:
    bool start_auth(bool cached_only);
    class SessionMemory;
    SessionMemory *repository_ = nullptr;
    playden::steam::AuthClient *backend_ = nullptr;
    playden::steam::SteamService *service_ = nullptr;
    std::uint64_t request_ = 0;
    int state_ = 0; // idle, waiting, showing QR, authenticated, failed
    bool restoring_session_ = false;
    std::string challenge_url_;
    mutable std::mutex failure_mutex_;
    std::string pending_failure_detail_;
    std::string failure_detail_;
    mutable std::mutex library_mutex_;
    std::vector<playden::GameSummary> games_;
    std::vector<std::size_t> installed_games_;
    std::set<std::uint32_t> favorite_games_;
    std::set<std::uint32_t> hidden_games_;
    playden::OrganizationSnapshot organization_;
    bool organization_ready_ = false;
    std::vector<std::size_t> visible_library_indices_locked(int filter) const;
    bool persist_library_organization_locked();
    std::unique_ptr<playden::Storage> library_storage_;
    std::unique_ptr<playden::LibraryCache> library_cache_;
    std::unique_ptr<playden::steam::LibraryClient> library_backend_;
    std::unique_ptr<playden::LibraryService> library_service_;
    std::uint64_t library_request_ = 0;
    std::string library_detail_;
    void start_library_fetch();
    std::thread details_worker_;
    // Installed games Steam has a newer version of: one check after the first fresh library load, kept
    // current by each details visit and finished update.
    std::thread updates_worker_;
    std::atomic<bool> updates_cancel_{false};
    mutable std::mutex updates_mutex_;
    std::set<std::uint32_t> updates_;
    bool updates_started_ = false;
    void start_update_check(std::vector<std::uint32_t> installed);
  public:
    bool library_update_available(std::uint32_t app_id) const;
    std::size_t library_update_count() const;
  private:
    std::atomic<bool> details_cancel_{false};
    mutable std::mutex details_mutex_;
    bool details_done_ = false;
    int details_state_ = 0;
    std::uint32_t details_app_id_ = 0;
    bool details_windows_launch_ = false;
    bool details_installed_ = false;
    bool details_incomplete_ = false;
    bool details_update_ = false;  // installed, and Steam has a newer version
    // The install sheet: the branch and language an install takes, chosen before it starts, and the sizes
    // for that choice (base game depots, as the app's listing gives them).
    std::optional<playden::steam::AppInfo> details_app_;
    struct InstallSheet {
        std::uint32_t app_id = 0;
        std::vector<playden::steam::BranchOffer> offers;
        std::vector<std::string> languages;  // the chosen branch's
        std::size_t branch = 0, language = 0;
        std::uint64_t download_bytes = 0, install_bytes = 0;
        std::string note;
    };
    InstallSheet sheet_;
    void sheet_measure_locked();
  public:
    bool install_sheet_open(std::uint32_t app_id);
    void install_sheet_step(int row, int direction);
    std::string install_sheet_value(int row) const;
    std::string install_sheet_note() const;
  private:
    std::size_t details_windows_depots_ = 0;
    std::string details_message_;
    playden::steam::StoreDetailsService *store_details_service_ = nullptr;
    bool store_details_ready_ = false;
    bool store_details_available_ = false;
    std::string store_description_;
    std::string store_genres_;
    std::string store_developers_;
    std::string store_publishers_;
    std::string store_categories_;
    std::string store_release_date_;
    std::string store_metacritic_score_;
    std::string store_controller_support_;
    bool store_windows_supported_ = false;
    std::thread install_worker_;
    std::atomic<bool> install_cancel_{false};
    mutable std::mutex install_mutex_;
    bool install_done_ = false;
    int install_state_ = 0;
    std::uint32_t install_app_id_ = 0;
    std::string install_message_;
    std::uint64_t install_progress_bytes_ = 0;
    std::uint64_t install_total_bytes_ = 0;
    std::uint64_t install_version_ = 0;
    std::uint64_t install_seen_version_ = 0;
    std::uint64_t saves_version_ = 0;
    std::uint64_t saves_seen_version_ = 0;
    mutable std::mutex settings_mutex_;
    std::string content_language_ = "english";
    // PS5's default. PS4 was held at 1 while the UI used libc's fixed 13 MiB heap; it now
    // has its own heap (ps4/playden_heap.c).
    int download_connections_ = 4;
    std::string log_receiver_ip_;
    std::string log_receiver_status_ = "Press Cross to edit; applies on next Wine launch.";
    int log_receiver_port_ = 9300;
    bool logging_enabled_ = false;
    std::string wine_debug_level_ = "profile";
    bool wait_watchdog_enabled_ = false;
    bool gpu_hang_dump_enabled_ = false;
    bool persist_runtime_settings_locked();
    std::thread uninstall_worker_;
    mutable std::mutex uninstall_mutex_;
    int uninstall_state_ = 0; // idle, removing, removed, failed
    std::uint32_t uninstall_app_id_ = 0;
    std::string uninstall_message_;
    std::atomic<bool> saves_cancel_{false};
    std::thread saves_worker_;
    mutable std::mutex saves_mutex_;
    int saves_state_ = 0;
    std::uint32_t saves_app_id_ = 0;
    std::string saves_message_ = "Check Half-Life 2 Steam Cloud saves.";
    std::uint64_t saves_progress_done_ = 0;
    std::uint64_t saves_progress_total_ = 0;
    int saves_progress_percent_ = -1;

    bool persist_settings_locked();
};

} // namespace playden::ps4
