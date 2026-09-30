#include "level.hpp"
#include "modes.hpp"
#include "world.hpp"

#include <engine/engine.hpp>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace net = engine::networking;
namespace SC = engine::SC;
using world::goalSize;
using world::playerSize;
using world::tileSize;

constexpr int windowWidth = 1920;
constexpr int windowHeight = 1080;

constexpr float gravity = 2600.f;
constexpr float moveSpeed = 380.f;
constexpr float jumpSpeed = 1020.f;

constexpr int joinAttempts = 5;

// Player N is drawn with sheet (N - 1) % 4, so every window agrees on colors.
constexpr std::array<const char *, 4> playerSheetFiles{
    "sheet_player.png", "sheet_player_blue.png", "sheet_player_pink.png",
    "sheet_player_orange.png"};

// Drawn by main outside the Scene, so they need text-cache ids no entity uses.
constexpr engine::EntityId hudTextId =
    std::numeric_limits<engine::EntityId>::max();
constexpr engine::EntityId disconnectedTextId = hudTextId - 1;
constexpr glm::vec2 hudPosition{24.f, 24.f};
constexpr float hudFontSize = 40.f;
constexpr glm::vec2 disconnectedPosition{hudPosition.x,
                                         hudPosition.y + 1.5f * hudFontSize};
constexpr engine::Color hudColor{20, 30, 60, 255};
constexpr engine::Color hudAlertColor{200, 30, 30, 255};

constexpr std::array<std::pair<SC::SDL_Scancode, float>, 3> speedKeys{{
    {SC::SDL_SCANCODE_1, 0.5f},
    {SC::SDL_SCANCODE_2, 1.f},
    {SC::SDL_SCANCODE_3, 2.f},
}};

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

PlayerState stateFor(glm::vec2 velocity, bool grounded) {
  if (!grounded) {
    return PlayerState::airborne;
  }
  return velocity.x != 0.f ? PlayerState::walking : PlayerState::idle;
}

// Client-server: player state goes to the server, which relays it.
struct ServerLink {
  static constexpr std::string_view mode = "client-server";

  explicit ServerLink(const std::string &endpoint) : session(endpoint) {}

  void pull(engine::Scene &scene) { session.applySnapshot(scene); }
  void push(const engine::Scene &scene, std::int64_t tick) {
    session.submitScene(scene, tick);
  }

  net::sessionClient session;
};

// Hybrid P2P: player state goes straight to each peer; the server only sends
// platforms.
struct PeerLink {
  static constexpr std::string_view mode = "p2p";

  explicit PeerLink(const std::string &endpoint) : session(endpoint) {}

  void pull(engine::Scene &scene) { session.applyUpdates(scene); }
  void push(const engine::Scene &scene, std::int64_t tick) {
    session.publishScene(scene, tick);
    if (tick % world::ticksPerSecond == 0) {
      engine::log::info("P{} publishing player state directly to peers [{}]",
                        session.id(), fmt::join(session.peers(), ", "));
    }
  }

  net::peerSession session;
};

template <class Link> bool joinServer(Link &link) {
  for (int attempt = 1; attempt <= joinAttempts; ++attempt) {
    if (link.session.join()) {
      return true;
    }
    engine::log::warn("no reply from the server (attempt {}/{})", attempt,
                      joinAttempts);
  }
  return false;
}

// Pushes player out of other along the shallower overlap axis. True if it
// landed on top.
bool resolveCollision(engine::Scene &scene,
                      const engine::PhysicsSystem &physics,
                      engine::EntityId player, engine::EntityId other) {
  if (!physics.isCollision(scene, player, other)) {
    return false;
  }
  const engine::Rect overlap =
      physics.GetCollisionOverlap(scene, player, other);
  if (overlap.size.x <= 0.f || overlap.size.y <= 0.f) {
    return false;
  }
  glm::vec2 &position = scene.transform(player).position;
  glm::vec2 &velocity = scene.getRigidBody(player)->velocity;
  const glm::vec2 playerMid = position + glm::vec2{playerSize * 0.5f};
  const glm::vec2 otherMid =
      scene.transform(other).position + scene.getCollider(other)->size * 0.5f;
  if (overlap.size.x < overlap.size.y) {
    position.x += playerMid.x < otherMid.x ? -overlap.size.x : overlap.size.x;
    velocity.x = 0.f;
    return false;
  }
  velocity.y = 0.f;
  if (playerMid.y < otherMid.y) {
    position.y -= overlap.size.y;
    return true;
  }
  position.y += overlap.size.y;
  return false;
}

template <class Link> int play(Link &link) {
  if (!joinServer(link)) {
    engine::log::error("could not join the server");
    return 1;
  }
  // Heartbeats start now, so the server does not time us out while loading.
  link.session.start();
  const engine::ClientId self = link.session.id();

  engine::Window window(fmt::format("Parkour - P{} ({})", self, Link::mode),
                        windowWidth, windowHeight);
  engine::Renderer renderer(window);
  engine::InputHandler input;
  engine::PhysicsSystem physics(gravity);

  const std::string assetDir = GAME_ASSET_DIR "game/";
  const engine::TextureId background =
      renderer.loadTexture(assetDir + "png/background_clouds.png");
  const engine::TextureId terrainTexture =
      renderer.loadTexture(assetDir + "png/terrain_grass_block.png");
  std::array<engine::SpriteSheetId, playerSheetFiles.size()> playerSheets{};
  std::ranges::transform(
      playerSheetFiles, playerSheets.begin(), [&](const char *file) {
        return renderer.createSpriteSheet(
            renderer.loadTexture(assetDir + "png/" + file),
            engine::SpriteSheetLayout::grid({playerSize, playerSize}, 5));
      });
  const auto sheetFor = [&playerSheets](engine::ClientId id) {
    return playerSheets[(id - 1) % playerSheets.size()];
  };
  const engine::SpriteSheetId playerSheet = sheetFor(self);
  const engine::SpriteSheetId beeSheet = renderer.createSpriteSheet(
      renderer.loadTexture(assetDir + "png/sheet_bee.png"),
      engine::SpriteSheetLayout::grid({world::beeSize, world::beeSize}, 2));
  const engine::SpriteSheetId goalSheet = renderer.createSpriteSheet(
      renderer.loadTexture(assetDir + "png/sheet_flag.png"),
      engine::SpriteSheetLayout::grid({goalSize, goalSize}, 2));
  const engine::FontId hudFont =
      renderer.loadFont(assetDir + "fonts/SuperBouncer.ttf", hudFontSize);

  engine::Scene scene;

  // Static level: built identically by every client and never replicated.
  std::vector<engine::EntityId> terrain;
  glm::vec2 playerSpawn{0.f, 0.f};
  engine::EntityId goal = 0;
  for (int row = 0; row < level::rows; ++row) {
    for (int column = 0; column < level::columns; ++column) {
      switch (level::map[static_cast<std::size_t>(row)][column]) {
      case level::solid: {
        const engine::EntityId tile = scene.createEntity();
        scene.transform(tile).position = world::cellPosition(column, row);
        scene.addShape(
            tile, {.size = {tileSize, tileSize}, .texture = terrainTexture});
        scene.addCollider(tile, {.size = {tileSize, tileSize}});
        terrain.push_back(tile);
        break;
      }
      case level::playerSpawn:
        // Bottom-align the taller player sprite to the cell it is standing in.
        playerSpawn =
            world::cellPosition(column, row + 1) - glm::vec2{0.f, playerSize};
        break;
      case level::goal: {
        goal = scene.createEntity();
        scene.transform(goal).position = world::cellPosition(column, row);
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

  // Server-driven objects: pre-built with textures, then bound to the server's
  // NetIds so its snapshots move them instead of spawning untextured copies.
  const std::vector<world::Motion> motions = world::motions();
  const std::vector<engine::EntityId> objects = world::spawn(
      scene, motions, world::Art{.platform = terrainTexture, .bee = beeSheet});
  std::vector<engine::EntityId> platforms;
  engine::EntityId bee = 0;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    link.session.replicator().bind(
        engine::makeNetId(engine::kServerId, static_cast<std::uint32_t>(i)),
        objects[i]);
    if (motions[i].kind == world::Kind::bee) {
      bee = objects[i];
    } else {
      platforms.push_back(objects[i]);
    }
  }

  // The only entity this process owns; every other RigidBody in the Scene
  // belongs to a remote player.
  const engine::EntityId player = scene.createEntity();
  scene.transform(player).position = playerSpawn;
  scene.addShape(player, {.size = {playerSize, playerSize}});
  scene.addSpriteAnimation(player,
                           animationFor(PlayerState::idle, playerSheet));
  scene.addCollider(player, {.size = {playerSize, playerSize}});
  scene.addRigidBody(player);
  link.session.replicator().track(player);

  engine::Timeline realTime;
  engine::Timeline gameTime(realTime, world::ticksPerSecond);

  // Everything below is captured by onTick and belongs to the sim thread once
  // it starts.
  PlayerState playerState = PlayerState::idle;
  bool grounded = false;
  bool touchingGoal = false;
  std::optional<std::size_t> riding; // index into platforms
  std::vector<glm::vec2> platformLast;
  platformLast.reserve(platforms.size());
  for (const engine::EntityId platform : platforms) {
    platformLast.push_back(scene.transform(platform).position);
  }
  std::unordered_map<engine::EntityId, PlayerState> remoteStates;
  std::vector<engine::EntityId> remotes;

  auto onTick = [&](const engine::TickContext &ctx) {
    engine::Scene &scene = ctx.scene;
    link.pull(scene);

    // Remote players arrive with a RigidBody; animate them from its velocity,
    // then strip it so physics.step never drifts them between snapshots.
    remotes.clear();
    for (const auto &[id, body] : scene.rigidBodies()) {
      if (id != player) {
        remotes.push_back(id);
      }
    }
    for (const engine::EntityId remote : remotes) {
      const glm::vec2 velocity = scene.getRigidBody(remote)->velocity;
      const PlayerState state = stateFor(velocity, velocity.y == 0.f);
      const auto [entry, added] = remoteStates.try_emplace(remote, state);
      if (added || entry->second != state) {
        entry->second = state;
        // Every remote RigidBody came from the replicator, so it has a NetId.
        const engine::NetId netId =
            link.session.replicator().netIdOf(remote).value();
        scene.addSpriteAnimation(
            remote, animationFor(state, sheetFor(engine::ownerOf(netId))));
      }
      scene.removeRigidBody(remote);
    }
    std::erase_if(remoteStates, [&scene](const auto &entry) {
      return !scene.hasEntity(entry.first);
    });

    // Carry the player along with the platform it stood on last tick.
    glm::vec2 &position = scene.transform(player).position;
    for (std::size_t i = 0; i < platforms.size(); ++i) {
      const glm::vec2 now = scene.transform(platforms[i]).position;
      if (riding == i) {
        position += now - platformLast[i];
      }
      platformLast[i] = now;
    }

    engine::RigidBody &body = *scene.getRigidBody(player);
    const bool moveLeft = ctx.keyboard.isKeyPressed(SC::SDL_SCANCODE_A) ||
                          ctx.keyboard.isKeyPressed(SC::SDL_SCANCODE_LEFT);
    const bool moveRight = ctx.keyboard.isKeyPressed(SC::SDL_SCANCODE_D) ||
                           ctx.keyboard.isKeyPressed(SC::SDL_SCANCODE_RIGHT);
    const bool jump = std::ranges::any_of(
        std::array{SC::SDL_SCANCODE_SPACE, SC::SDL_SCANCODE_W,
                   SC::SDL_SCANCODE_UP},
        [&ctx](SC::SDL_Scancode key) {
          return ctx.keyboard.justPressed(key, ctx.previousKeyboard);
        });
    body.velocity.x =
        (moveRight ? moveSpeed : 0.f) - (moveLeft ? moveSpeed : 0.f);
    if (jump && grounded) {
      body.velocity.y = -jumpSpeed;
    }

    physics.step(scene, ctx.dt);

    grounded = false;
    riding.reset();
    for (const engine::EntityId tile : terrain) {
      grounded = resolveCollision(scene, physics, player, tile) || grounded;
    }
    for (std::size_t i = 0; i < platforms.size(); ++i) {
      if (resolveCollision(scene, physics, player, platforms[i])) {
        grounded = true;
        riding = i;
      }
    }

    const auto respawn = [&]() {
      position = playerSpawn;
      body.velocity = {0.f, 0.f};
      grounded = false;
      riding.reset();
    };

    if (physics.isCollision(scene, player, bee)) {
      engine::log::info("hit the bee, back to the start");
      respawn();
    }
    if (position.y > static_cast<float>(windowHeight)) {
      engine::log::info("fell into a pit, back to the start");
      respawn();
    }
    const bool onGoal = physics.isCollision(scene, player, goal);
    if (onGoal && !touchingGoal) {
      engine::log::info("flag reached, level complete, now play again :)");
      respawn();
    }
    touchingGoal = onGoal;

    const PlayerState desired = stateFor(body.velocity, grounded);
    if (desired != playerState) {
      playerState = desired;
      *scene.getSpriteAnimation(player) = animationFor(desired, playerSheet);
    }
    engine::advanceAnimations(scene, ctx.dt);

    // Once per tick, so the send rate follows this client's own Timeline.
    link.push(scene, ctx.tick);
  };

  engine::SimulationThread sim(std::move(scene), gameTime, onTick);
  sim.start();
  engine::log::info("P{} joined in {} mode. A/D or arrows to move, Space/W/Up "
                    "to jump, P pause, 1/2/3 speed 0.5x/1x/2x, F scaling mode",
                    self, Link::mode);

  // main thread: input capture, time control requests and rendering.
  std::optional<engine::RenderFrame> frame;
  engine::KeyboardState previous;
  bool online = true;
  while (!window.shouldClose()) {
    window.pollEvents();
    const engine::KeyboardState keyboard =
        engine::KeyboardState::capture(input);
    sim.submitKeyboard(keyboard);

    if (keyboard.justPressed(SC::SDL_SCANCODE_P, previous)) {
      sim.togglePause();
    }
    for (const auto &[key, speed] : speedKeys) {
      if (keyboard.justPressed(key, previous)) {
        sim.setSpeed(speed);
      }
    }
    if (keyboard.justPressed(SC::SDL_SCANCODE_F, previous)) {
      renderer.toggleScalingMode();
    }
    previous = keyboard;

    if (auto next = sim.takeRenderFrame()) {
      frame = std::move(next);
    }
    if (sim.failure()) {
      engine::log::error("simulation failed; exiting");
      break;
    }
    // Goes false after kDefaultClientTimeoutMs without a server reply.
    if (link.session.connected() != online) {
      online = !online;
      if (online) {
        engine::log::info("reconnected to the server");
      } else {
        engine::log::warn("lost the server: platforms and the bee are frozen");
      }
    }

    renderer.clear({0, 0, 0, 255});
    renderer.drawTexture(
        background, {0.f, 0.f},
        {static_cast<float>(windowWidth), static_cast<float>(windowHeight)},
        true);
    if (frame) {
      renderer.drawEntities(frame->scene);
      const engine::SimStatus &status = frame->status;
      const std::string hud =
          status.paused ? fmt::format("P{}  |  {}  |  PAUSED", self, Link::mode)
                        : fmt::format("P{}  |  {}  |  {:.1f}x", self,
                                      Link::mode, status.speed);
      renderer.drawText(hudTextId,
                        {.val = hud,
                         .font = hudFont,
                         .color = status.paused ? hudAlertColor : hudColor},
                        hudPosition);
    }
    if (!online) {
      renderer.drawText(disconnectedTextId,
                        {.val = "DISCONNECTED FROM SERVER",
                         .font = hudFont,
                         .color = hudAlertColor},
                        disconnectedPosition);
    }
    renderer.present();
  }

  sim.stop();
  return sim.failure() ? 1 : 0;
}

} // namespace

int runClient(const std::string &serverEndpoint) {
  ServerLink link(serverEndpoint);
  return play(link);
}

int runPeer(const std::string &serverEndpoint) {
  PeerLink link(serverEndpoint);
  return play(link);
}
