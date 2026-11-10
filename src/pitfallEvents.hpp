#pragma once

#include "world.hpp"

#include <engine/events/simpleEvent.hpp>

#include <cstdint>
#include <cstring>
#include <optional>

// Pitfalls are shared: the server times every drop, so all windows agree on
// which floor is gone. A pitfall is named by its index in map scan order.
namespace pitfalls {

// A pitfall gives way this long after a player lands on it, and is back in
// time for the next attempt. Server ticks, so no client's speed changes them.
constexpr std::int64_t dropDelayTicks = world::ticksPerSecond * 3 / 10;
constexpr std::int64_t returnTicks = 3 * world::ticksPerSecond;

// client -> server: this client's player landed on pitfall index.
struct touched : engine::events::simpleEvent {
  static constexpr std::uint16_t kType = 3;
  std::uint32_t index = 0;

  engine::networking::Bytes encode() const {
    engine::networking::Bytes out(sizeof index);
    std::memcpy(out.data(), &index, sizeof index);
    return out;
  }
  static std::optional<touched> decode(engine::networking::ByteView in) {
    touched e;
    if (in.size() != sizeof e.index) {
      return std::nullopt;
    }
    std::memcpy(&e.index, in.data(), sizeof e.index);
    return e;
  }
};

// server -> clients: pitfall index dropped away (down) or came back.
struct changed : engine::events::simpleEvent {
  static constexpr std::uint16_t kType = 4;
  std::uint32_t index = 0;
  bool down = false;

  engine::networking::Bytes encode() const {
    engine::networking::Bytes out(sizeof index + 1);
    std::memcpy(out.data(), &index, sizeof index);
    out.back() = std::byte{down};
    return out;
  }
  static std::optional<changed> decode(engine::networking::ByteView in) {
    changed e;
    if (in.size() != sizeof e.index + 1) {
      return std::nullopt;
    }
    std::memcpy(&e.index, in.data(), sizeof e.index);
    e.down = in.back() != std::byte{0};
    return e;
  }
};

} // namespace pitfalls
