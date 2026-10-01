#include "game/multiplayer/platform/session/connection_input.h"

#include <random>

namespace multiplayer::platform {

std::string generate_room_code() {
  static constexpr std::string_view kRoomCodeAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static std::mt19937 generator(std::random_device{}());
  std::uniform_int_distribution<size_t> distribution(0, kRoomCodeAlphabet.size() - 1);
  std::string result;
  result.resize(kMultiplayerRoomCodeLength);
  for (char& character : result) {
    character = kRoomCodeAlphabet[distribution(generator)];
  }
  return result;
}

}  // namespace multiplayer::platform
