#pragma once

#include "world.hpp"

#include <engine/events/simpleEvent.hpp>

#include <cstdint>
#include <cstring>
#include <optional>

// The flag race: the first player to touch the flag wins the round, then every
// player goes back to the start. The server decides the winner.
namespace race {

// Every client resets to the start this long after a win; the server reopens
// the round at the same time.
constexpr std::int64_t resetDelayTicks = 2 * world::ticksPerSecond;

// client -> server: this client's player touched the flag. Carries nothing:
// the server takes the claimant from the session, not the payload.
struct goalReached : engine::events::simpleEvent {
  static constexpr std::uint16_t kType = 1;

  engine::networking::Bytes encode() const { return {}; }
  static std::optional<goalReached> decode(engine::networking::ByteView in) {
    return in.empty() ? std::optional<goalReached>{{}} : std::nullopt;
  }
};

// server -> clients: winner reached the flag first this round.
struct roundWon : engine::events::simpleEvent {
  static constexpr std::uint16_t kType = 2;
  engine::ClientId winner = 0;

  engine::networking::Bytes encode() const {
    engine::networking::Bytes out(sizeof winner);
    std::memcpy(out.data(), &winner, sizeof winner);
    return out;
  }
  static std::optional<roundWon> decode(engine::networking::ByteView in) {
    roundWon e;
    if (in.size() != sizeof e.winner) {
      return std::nullopt;
    }
    std::memcpy(&e.winner, in.data(), sizeof e.winner);
    return e;
  }
};

} // namespace race
