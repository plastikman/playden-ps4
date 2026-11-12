// Playden - PS4 live services for the homebrew UI: the desktop host's live service graph with PS4 paths.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "homebrew/simulation.hpp"

#include <memory>

namespace playden::homebrew {
class ArtworkService;
}

namespace playden::ps4 {

class LiveServices {
 public:
  LiveServices();
  ~LiveServices();
  LiveServices(const LiveServices &) = delete;
  LiveServices &operator=(const LiveServices &) = delete;

  // Builds every service and the Controller over `simulation` (ModelMode::Live).
  bool Start(playden::homebrew::Simulation &simulation);
  // Once per frame, before drawing.
  void Tick(playden::homebrew::Simulation &simulation);
  void Shutdown();
  playden::homebrew::ArtworkService *artwork() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace playden::ps4
