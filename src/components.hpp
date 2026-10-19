#pragma once

#include <glm/vec2.hpp>

// Game-defined components. The engine stores them through add<T>/get<T>/each<T>
// without knowing their types. Entities with these but no Shape are never
// drawn: they are the level's hidden objects.

// A start and respawn location: the player's top-left on spawn.
struct SpawnPoint {};

// Touching one sends the player back to a spawn point.
struct DeathZone {};

// Touching one scrolls the camera. side is the view edge it guards.
struct ScrollBoundary {
  enum class Side { left, right } side;
};

// The view's top-left in world coordinates. One per client; main reads it from
// the RenderFrame and hands it to Renderer::setCamera.
struct Camera {
  glm::vec2 offset{0.f, 0.f};
};
