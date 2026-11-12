// Playden PS4 port - the console side of Playden's runtime for the PS4.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the PS5 each game is its own installed title: Playden asks the console whether it runs,
// whether its folder and app.db entry are free, and registers it (ShadowMount). On the PS4 every
// game runs inside Playden's own title (PLDN10001), which restarts into the bundled Orbis-Win, so
// a game's "title" is a name for its profile: never running while Playden's UI is up, always free,
// and registered as soon as its profile is published.
#pragma once

#include "platform/ps5/native_coordinator.h"
#include "runtime/game_install.h"
#include "runtime/install_service.h"

namespace playden::ps4 {

class Ps4NativeCoordinator final : public playden::runtime::NativeCoordinator {
 public:
  explicit Ps4NativeCoordinator(playden::runtime::PackageInstaller &package) : package_(package) {}

  // A game runs only after Playden's own title has restarted into it, so never while asked.
  playden::runtime::NativeActivity Activity(const std::string &) override {
    return playden::runtime::NativeActivity::kStopped;
  }

  playden::runtime::NativeStatus Query(const playden::runtime::RuntimeInstall &record) override {
    return Status(package_.Inspect(record));
  }
  playden::runtime::NativeStatus QueryOwnership(const playden::runtime::RuntimeInstall &record) override {
    return Status(package_.InspectOwnership(record));
  }

  // The PS5 coordinator's own questions, which RuntimeHost asks of its title slots: every PS4 slot
  // is a profile name, so free, unlisted, and not running.
  playden::runtime::NativeTitleAvailability Availability(const std::string &) {
    return {playden::runtime::TitleAvailability::kFree, playden::runtime::TitleAvailability::kFree};
  }
  playden::runtime::TitleAvailability Inventory(const std::string &) { return playden::runtime::TitleAvailability::kFree; }
  using AvailabilityReport = playden::ps5::Ps5NativeCoordinator::AvailabilityReport;
  AvailabilityReport ReportAvailability(const std::string &) {
    AvailabilityReport report;
    report.database = playden::runtime::TitleAvailability::kFree;
    report.folder = playden::runtime::TitleAvailability::kFree;
    report.shadowmount.state = playden::ps5::RegistrationState::kAbsent;
    report.activity = playden::runtime::NativeActivity::kStopped;
    return report;
  }

  // Nothing to register with the console: the published profile is what Play starts.
  playden::runtime::RegistrationResult Register(const playden::runtime::RuntimeInstall &) override {
    return {playden::runtime::RegistrationResultState::kAccepted, {}, {}};
  }

 private:
  static playden::runtime::NativeStatus Status(const playden::runtime::PackageStatus &package) {
    playden::runtime::NativeStatus status;
    status.available = true;
    status.source_available = package.known;
    status.installed = package.known && package.title_staged;
    status.owned_manifest_matches = package.known && package.manifest_matches;
    return status;
  }

  playden::runtime::PackageInstaller &package_;
};

}  // namespace playden::ps4
