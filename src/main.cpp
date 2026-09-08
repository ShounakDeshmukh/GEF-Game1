#include <engine/engine.hpp>

#include "level.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int windowWidth = 1920;
constexpr int windowHeight = 1080;
constexpr float tileSize = 60.f;
constexpr float playerSize = 120.f;
constexpr float beeSize = 60.f;
constexpr float goalSize = 60.f;

constexpr float gravity = 2600.f;
constexpr float moveSpeed = 380.f;
constexpr float jumpSpeed = 1020.f;

constexpr float beePatrol =
    5.f * tileSize; // horizontal half-span of the flight path
constexpr float beeBob = 0.4f * tileSize;
constexpr float beeRate = 1.1f; // radians per second

// Frame order baked into sheet_player.png.
enum PlayerFrame : std::uint32_t {
  frameIdle = 0,
  frameJump,
  frameWalkA,
  frameWalkB,
  frameHit
};

enum class PlayerState { idle, walking, airborne };

engine::SpriteAnimation animationFor(PlayerState state,
                                     engine::SpriteSheetId sheet) {
  switch (state) {
  case PlayerState::walking:
    return engine::SpriteAnimation::uniform(sheet, {frameWalkA, frameWalkB},
                                            0.12f);
  case PlayerState::airborne:
    return engine::SpriteAnimation::uniform(sheet, {frameJump}, 1.f);
  case PlayerState::idle:
    break;
  }
  return engine::SpriteAnimation::uniform(sheet, {frameIdle}, 1.f);
}

glm::vec2 cellPosition(int column, int row) {
  return {static_cast<float>(column) * tileSize,
          static_cast<float>(row) * tileSize};
}

} // namespace

int main() {
  engine::log::init();

  engine::Window window("Parkour", windowWidth, windowHeight);
  engine::Renderer renderer(window);
  engine::InputHandler input;
  engine::PhysicsSystem physics(gravity);

  const std::string assetDir = GAME_ASSET_DIR "game/png/";
  const engine::TextureId background =
      renderer.loadTexture(assetDir + "background_clouds.png");

  const engine::TextureId terrainTexture =
      renderer.loadTexture(assetDir + "terrain_grass_block.png");

  const engine::SpriteSheetId playerSheet = renderer.createSpriteSheet(
      renderer.loadTexture(assetDir + "sheet_player.png"),
      engine::SpriteSheetLayout::grid({playerSize, playerSize}, 5));
  const engine::SpriteSheetId beeSheet = renderer.createSpriteSheet(
      renderer.loadTexture(assetDir + "sheet_bee.png"),
      engine::SpriteSheetLayout::grid({beeSize, beeSize}, 2));
  const engine::SpriteSheetId goalSheet = renderer.createSpriteSheet(
      renderer.loadTexture(assetDir + "sheet_flag.png"),
      engine::SpriteSheetLayout::grid({goalSize, goalSize}, 2));

  engine::Scene scene;

  // Static objects: every solid cell becomes one non-moving, gravity-free
  // entity.
  std::vector<engine::EntityId> terrain;
  glm::vec2 playerSpawn{0.f, 0.f};
  glm::vec2 beeOrigin{0.f, 0.f};
  engine::EntityId goal = 0;

  for (int row = 0; row < level::rows; ++row) {
    for (int column = 0; column < level::columns; ++column) {
      switch (level::map[static_cast<std::size_t>(row)][column]) {
      case level::solid: {
        const engine::EntityId tile = scene.createEntity();
        scene.transform(tile).position = cellPosition(column, row);
        scene.addShape(
            tile, {.size = {tileSize, tileSize}, .texture = terrainTexture});
        scene.addCollider(tile, {.size = {tileSize, tileSize}});
        terrain.push_back(tile);
        break;
      }
      case level::playerSpawn:
        // Bottom-align the taller player sprite to the cell it is standing in.
        playerSpawn =
            cellPosition(column, row + 1) - glm::vec2{0.f, playerSize};
        break;
      case level::beeSpawn:
        beeOrigin = cellPosition(column, row);
        break;
      case level::goal: {
        goal = scene.createEntity();
        scene.transform(goal).position = cellPosition(column, row);
        scene.addShape(goal, {.size = {goalSize, goalSize}});
        scene.addSpriteAnimation(
            goal, engine::SpriteAnimation::uniform(goalSheet, {0, 1}, 0.35f));
        scene.addCollider(goal, {.size = {goalSize, goalSize}});
        break;
      }
      default:
        break;
      }
    }
  }

  // main Char: the only entity with a RigidBody, so the only one
  // gravity moves.
  const engine::EntityId player = scene.createEntity();
  scene.transform(player).position = playerSpawn;
  scene.addShape(player, {.size = {playerSize, playerSize}});
  scene.addSpriteAnimation(player,
                           animationFor(PlayerState::idle, playerSheet));
  scene.addCollider(player, {.size = {playerSize, playerSize}});
  engine::RigidBody &playerBody = scene.addRigidBody(player);

  // Automoving bee: no RigidBody, so it is not affected by gravity, but it does
  // have a Collider so it can hit the player.
  const engine::EntityId bee = scene.createEntity();
  scene.transform(bee).position = beeOrigin;
  scene.addShape(bee, {.size = {beeSize, beeSize}});
  scene.addSpriteAnimation(
      bee, engine::SpriteAnimation::uniform(beeSheet, {0, 1}, 0.08f));
  scene.addCollider(bee, {.size = {beeSize, beeSize}});

  engine::Timeline realTime;
  engine::Timeline gameTime(realTime, 60);
  engine::Stepper sim(gameTime);

  PlayerState playerState = PlayerState::idle;
  bool grounded = false;
  bool jumpHeld = false;
  bool jumpQueued = false;
  bool scalingHeld = false;
  bool touchingGoal = false;
  float elapsed = 0.f;

  engine::log::info(
      "A/D or arrows to move, Space/W/Up to jump, P toggles scaling mode");

  // GAME LOOP
  while (!window.shouldClose()) {
    window.pollEvents();

    const bool moveLeft = input.isKeyPressed(engine::SC::SDL_SCANCODE_A) ||
                          input.isKeyPressed(engine::SC::SDL_SCANCODE_LEFT);
    const bool moveRight = input.isKeyPressed(engine::SC::SDL_SCANCODE_D) ||
                           input.isKeyPressed(engine::SC::SDL_SCANCODE_RIGHT);
    const bool jumpKey = input.isKeyPressed(engine::SC::SDL_SCANCODE_SPACE) ||
                         input.isKeyPressed(engine::SC::SDL_SCANCODE_W) ||
                         input.isKeyPressed(engine::SC::SDL_SCANCODE_UP);

    jumpQueued = jumpQueued || (jumpKey && !jumpHeld);
    jumpHeld = jumpKey;

    const bool scalingKey = input.isKeyPressed(engine::SC::SDL_SCANCODE_P);
    if (scalingKey && !scalingHeld) {
      renderer.toggleScalingMode();
    }
    scalingHeld = scalingKey;

    sim.beginFrame();
    while (sim.step()) {
      const float stepSeconds = gameTime.tickSeconds();
      elapsed += stepSeconds;

      playerBody.velocity.x =
          (moveRight ? moveSpeed : 0.f) - (moveLeft ? moveSpeed : 0.f);
      if (jumpQueued) {
        if (grounded) {
          playerBody.velocity.y = -jumpSpeed;
          grounded = false;
        }
        jumpQueued = false;
      }

      physics.step(scene, stepSeconds);

      scene.transform(bee).position =
          beeOrigin + glm::vec2{std::sin(elapsed * beeRate) * beePatrol,
                                std::sin(elapsed * beeRate * 2.f) * beeBob};

      // Terrain collision response: push the player out of any solid tiles it
      // is overlapping, and zero the velocity in that direction.
      grounded = false;
      for (const engine::EntityId tile : terrain) {
        if (!physics.isCollision(scene, player, tile)) {
          continue;
        }
        const engine::Rect overlap =
            physics.GetCollisionOverlap(scene, player, tile);
        if (overlap.size.x <= 0.f || overlap.size.y <= 0.f) {
          continue;
        }
        engine::Transform &transform = scene.transform(player);
        const float playerMid = transform.position.y + playerSize * 0.5f;
        const float tileMid =
            scene.transform(tile).position.y + tileSize * 0.5f;
        if (overlap.size.x < overlap.size.y) {
          const float playerMidX = transform.position.x + playerSize * 0.5f;
          const float tileMidX =
              scene.transform(tile).position.x + tileSize * 0.5f;
          transform.position.x +=
              playerMidX < tileMidX ? -overlap.size.x : overlap.size.x;
          playerBody.velocity.x = 0.f;
        } else {
          if (playerMid < tileMid) {
            transform.position.y -= overlap.size.y;
            grounded = true;
          } else {
            transform.position.y += overlap.size.y;
          }
          playerBody.velocity.y = 0.f;
        }
      }

      const auto respawn = [&]() {
        scene.transform(player).position = playerSpawn;
        playerBody.velocity = {0.f, 0.f};
        grounded = false;
      };

      // Bee collision response: if the player hits the bee, it is sent back to
      // the start.
      if (physics.isCollision(scene, player, bee)) {
        engine::log::info("hit the bee, back to the start");
        respawn();
      }

      if (scene.transform(player).position.y >
          static_cast<float>(windowHeight)) {
        engine::log::info("fell into a pit, back to the start");
        respawn();
      }

      // Goal collision response: if the player touches the goal, it is logged
      const bool onGoal = physics.isCollision(scene, player, goal);
      if (onGoal && !touchingGoal) {
        engine::log::info("flag reached, level complete, now play again :)");
        respawn();
      }
      touchingGoal = onGoal;

      const PlayerState desired =
          !grounded ? PlayerState::airborne
                    : (playerBody.velocity.x != 0.f ? PlayerState::walking
                                                    : PlayerState::idle);
      if (desired != playerState) {
        playerState = desired;
        *scene.getSpriteAnimation(player) = animationFor(desired, playerSheet);
      }

      engine::advanceAnimations(scene, stepSeconds);
    }

    renderer.clear({0, 0, 0, 255});
    renderer.drawTexture(
        background, {0.f, 0.f},
        {static_cast<float>(windowWidth), static_cast<float>(windowHeight)},
        true);
    renderer.drawEntities(scene);
    renderer.present();
  }

  return 0;
}
