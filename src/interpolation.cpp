#include "interpolation.hpp"

#include "world.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <iterator>

namespace {

// Measured arrival jitter stays under one tick, so two ticks still leaves room
// for a late snapshot.
// LAT_DELAY overrides it, to compare delays.
const double delayTicks =
    std::getenv("LAT_DELAY") ? std::atof(std::getenv("LAT_DELAY")) : 2.0;
// Past this the playback time jumps instead of easing back, e.g. after a pause.
constexpr double maxDriftTicks = 8.0;
constexpr double driftCorrection = 0.1;
// Farther apart than this, two samples are a respawn: jump instead of sliding.
constexpr float teleportDistance = 240.f;
constexpr std::size_t maxSamples = 16;

} // namespace

void Interpolator::record(engine::EntityId entity, engine::ClientId owner,
                          std::int64_t stamp, glm::vec2 position) {
  Track &track = tracks_.try_emplace(entity, Track{owner, {}}).first->second;
  if (!track.samples.empty() && stamp <= track.samples.back().tick) {
    return;
  }
  track.samples.push_back({stamp, position});
  if (track.samples.size() > maxSamples) {
    track.samples.pop_front();
  }
  Playback &playback =
      owners_
          .try_emplace(owner, Playback{static_cast<double>(stamp) - delayTicks,
                                       stamp, std::nullopt})
          .first->second;
  playback.newest = std::max(playback.newest, stamp);
}

void Interpolator::advance(Clock::time_point now) {
  for (auto &[owner, playback] : owners_) {
    if (playback.lastAdvance) {
      const std::chrono::duration<double> elapsed = now - *playback.lastAdvance;
      playback.tick += elapsed.count() * world::ticksPerSecond;
    }
    playback.lastAdvance = now;
    const double drift =
        static_cast<double>(playback.newest) - delayTicks - playback.tick;
    playback.tick +=
        std::abs(drift) > maxDriftTicks ? drift : drift * driftCorrection;
  }
}

std::optional<glm::vec2>
Interpolator::positionOf(engine::EntityId entity) const {
  const auto found = tracks_.find(entity);
  if (found == tracks_.end()) {
    return std::nullopt;
  }
  const std::deque<Sample> &samples = found->second.samples;
  const double tick = owners_.at(found->second.owner).tick;
  const auto after =
      std::ranges::upper_bound(samples, tick, {}, [](const Sample &sample) {
        return static_cast<double>(sample.tick);
      });
  if (after == samples.begin()) {
    return samples.front().position;
  }
  if (after == samples.end()) {
    return samples.back().position;
  }
  const Sample &before = *std::prev(after);
  if (glm::distance(before.position, after->position) > teleportDistance) {
    return after->position;
  }
  const double t = (tick - static_cast<double>(before.tick)) /
                   static_cast<double>(after->tick - before.tick);
  return glm::mix(before.position, after->position, static_cast<float>(t));
}

void Interpolator::forgetMissing(const engine::Scene &scene) {
  std::erase_if(tracks_, [&scene](const auto &entry) {
    return !scene.hasEntity(entry.first);
  });
  std::erase_if(owners_, [this](const auto &entry) {
    return std::ranges::none_of(tracks_, [&entry](const auto &track) {
      return track.second.owner == entry.first;
    });
  });
}
