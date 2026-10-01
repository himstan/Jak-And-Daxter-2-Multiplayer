#pragma once

#include <string>

#include "game/multiplayer/platform/transport/network_simulation.h"

class NetworkSimulationPanel {
 public:
  void draw(bool* open);

 private:
  void apply();

  multiplayer::platform::NetworkSimulationSettings settings_;
  std::string status_;
};
