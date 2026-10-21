#pragma once

#include <engine/engine.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

// Plays remote entities back a few ticks behind their owner's newest stamp,
// blending between received positions so uneven snapshot arrival never shows
// as stutter.
class Interpolator {
public:
  using Clock = std::chrono::steady_clock;

  // Keeps position as a sample if stamp is newer than the entity's last one.
  void record(engine::EntityId entity, engine::ClientId owner,
              std::int64_t stamp, glm::vec2 position);
  // Moves every owner's playback time forward to now.
  void advance(Clock::time_point now);
  // The entity's position at its owner's playback time; nullopt before its
  // first sample.
  std::optional<glm::vec2> positionOf(engine::EntityId entity) const;
  // Drops entities no longer in scene, and owners with none left.
  void forgetMissing(const engine::Scene &scene);
  std::optional<double> playbackTick(engine::ClientId owner) const {
    const auto it = owners_.find(owner);
    return it == owners_.end() ? std::nullopt : std::optional<double>{it->second.tick};
  }

private:
  struct Sample {
    std::int64_t tick;
    glm::vec2 position;
  };
  struct Track {
    engine::ClientId owner;
    std::deque<Sample> samples;
  };
  // Each owner's ticks count from its own launch, so each gets its own clock.
  struct Playback {
    double tick;
    std::int64_t newest;
    std::optional<Clock::time_point> lastAdvance;
  };

  std::unordered_map<engine::EntityId, Track> tracks_;
  std::unordered_map<engine::ClientId, Playback> owners_;
};
