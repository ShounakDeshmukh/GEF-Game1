#pragma once

#include "level.hpp"

#include <engine/engine.hpp>

#include <cstdint>
#include <optional>
#include <vector>

// The server-driven part of the level, shared by the server and every client.
namespace world {

constexpr float tileSize = 60.f;
constexpr float playerSize = 120.f;
constexpr float beeSize = 60.f;
constexpr float goalSize = 60.f;
constexpr std::int64_t ticksPerSecond = 60;
constexpr float levelWidth = level::columns * tileSize;
constexpr float levelHeight = level::rows * tileSize;

enum class Kind { platform, bee };

// position = origin + (sin(rate * t) * extent.x, sin(rate * yHarmonic * t) *
// extent.y)
struct Motion {
  Kind kind;
  glm::vec2 origin;
  glm::vec2 size;
  glm::vec2 extent;
  float rate; // radians per second
  float yHarmonic = 1.f;
};

// Client-side textures; the headless server has none.
struct Art {
  engine::TextureId platform;
  engine::SpriteSheetId bee;
};

glm::vec2 cellPosition(int column, int row);

// Length of the run of equal cells starting at column, or 0 if column is not
// the run's first cell.
int runAt(const char *line, int column);

glm::vec2 positionAt(const Motion &motion, float seconds);

// The owner tick an entity's replicated position belongs to, carried in its
// extra bytes so receivers can interpolate on the owner's clock.
engine::networking::Bytes encodeStamp(std::int64_t tick);
std::optional<std::int64_t> decodeStamp(const engine::networking::Bytes *extra);

// Every moving object in level::map, in a fixed order (platforms in scan
// order, then the bee). Index i is replicated as makeNetId(kServerId, i).
std::vector<Motion> motions();

// One entity per motion, in the same order.
std::vector<engine::EntityId> spawn(engine::Scene &scene,
                                    const std::vector<Motion> &motions,
                                    std::optional<Art> art);

} // namespace world
