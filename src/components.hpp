#pragma once

#include <engine/ids.hpp>
#include <glm/vec2.hpp>

#include <cstdint>

// Game-defined components. The engine stores them through add<T>/get<T>/each<T>
// without knowing their types. Entities with these but no Shape are never
// drawn: they are the level's hidden objects.

// A start and respawn location: the player's top-left on spawn.
struct SpawnPoint {};

// Touching one sends the player back to a spawn point.
struct DeathZone {};

// A floor tile that drops away shortly after a player lands on it, then comes
// back. The server times it; index names it on the network.
struct Pitfall {
  std::uint32_t index = 0;
  bool dropping = false; // from our touch (or the server's drop) until it's back
};

// Hidden until the player steps on them, then sprung (and drawn) until it
// respawns.
struct Spikes {
  bool up = false;
};

// Always visible and deadly. Rests at home until a player enters its trigger,
// rushes across the trigger, holds, then rolls back home.
struct Saw {
  enum class State { resting, rushing, holding, returning };
  float homeX = 0.f;
  float targetX = 0.f;
  State state = State::resting;
};

// Hidden zone near a saw; entering it sends the saw to targetX, the zone's far
// edge, so the player has to back away.
struct SawTrigger {
  engine::EntityId saw = 0;
  float targetX = 0.f;
};

// Touching one scrolls the camera. side is the view edge it guards.
struct ScrollBoundary {
  enum class Side { left, right } side;
};

// The view's top-left in world coordinates. One per client; main reads it from
// the RenderFrame and hands it to Renderer::setCamera.
struct Camera {
  glm::vec2 offset{0.f, 0.f};
};
