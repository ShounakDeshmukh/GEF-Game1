#include "world.hpp"

#include "level.hpp"

#include <cmath>
#include <cstddef>

namespace world {

namespace {

constexpr glm::vec2 horizontalSweep{4.f * tileSize, 0.f};
constexpr glm::vec2 verticalSweep{0.f, 2.f * tileSize};
constexpr float horizontalRate = 0.8f;
constexpr float verticalRate = 1.2f;

// The bee flies a figure 8: y oscillates twice per x sweep.
constexpr glm::vec2 beeSweep{5.f * tileSize, 0.4f * tileSize};
constexpr float beeRate = 1.1f;
constexpr float beeHarmonic = 2.f;

bool isMover(char cell) {
  return cell == level::horizontalMover || cell == level::verticalMover;
}

} // namespace

glm::vec2 cellPosition(int column, int row) {
  return {static_cast<float>(column) * tileSize,
          static_cast<float>(row) * tileSize};
}

glm::vec2 positionAt(const Motion &motion, float seconds) {
  return motion.origin +
         glm::vec2{std::sin(seconds * motion.rate) * motion.extent.x,
                   std::sin(seconds * motion.rate * motion.yHarmonic) *
                       motion.extent.y};
}

std::vector<Motion> motions() {
  std::vector<Motion> result;
  std::optional<Motion> bee;
  for (int row = 0; row < level::rows; ++row) {
    const char *line = level::map[static_cast<std::size_t>(row)];
    for (int column = 0; column < level::columns; ++column) {
      const char cell = line[column];
      if (cell == level::beeSpawn) {
        bee = Motion{Kind::bee,
                     cellPosition(column, row),
                     {beeSize, beeSize},
                     beeSweep,
                     beeRate,
                     beeHarmonic};
      }
      // Only the first cell of a run starts a platform.
      if (!isMover(cell) || (column > 0 && line[column - 1] == cell)) {
        continue;
      }
      int length = 1;
      while (column + length < level::columns &&
             line[column + length] == cell) {
        ++length;
      }
      const bool horizontal = cell == level::horizontalMover;
      result.push_back({Kind::platform,
                        cellPosition(column, row),
                        {static_cast<float>(length) * tileSize, tileSize},
                        horizontal ? horizontalSweep : verticalSweep,
                        horizontal ? horizontalRate : verticalRate});
    }
  }
  if (bee) {
    result.push_back(*bee);
  }
  return result;
}

std::vector<engine::EntityId> spawn(engine::Scene &scene,
                                    const std::vector<Motion> &motions,
                                    std::optional<Art> art) {
  std::vector<engine::EntityId> entities;
  entities.reserve(motions.size());
  for (const Motion &motion : motions) {
    const engine::EntityId entity = scene.createEntity();
    scene.transform(entity).position = motion.origin;
    // The server needs the Shape too: a snapshot without one removes the
    // client's copy. tiled is replicated, the texture is not.
    engine::Shape shape{.size = motion.size,
                        .tiled = motion.kind == Kind::platform};
    if (art && motion.kind == Kind::platform) {
      shape.texture = art->platform;
    }
    scene.addShape(entity, shape);
    scene.addCollider(entity, {.size = motion.size});
    if (art && motion.kind == Kind::bee) {
      scene.addSpriteAnimation(
          entity, engine::SpriteAnimation::uniform(art->bee, {0, 1}, 0.08f));
    }
    entities.push_back(entity);
  }
  return entities;
}

} // namespace world
