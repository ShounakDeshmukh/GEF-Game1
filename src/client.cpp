#include "components.hpp"
#include "interpolation.hpp"
#include "level.hpp"
#include "modes.hpp"
#include "raceEvents.hpp"
#include "world.hpp"

#include <engine/engine.hpp>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace net = engine::networking;
namespace ev = engine::events;
namespace SC = engine::SC;
using world::goalSize;
using world::playerSize;
using world::tileSize;

constexpr int windowWidth = 1920;
constexpr int windowHeight = 1080;

constexpr float gravity = 2600.f;
constexpr float moveSpeed = 380.f;
constexpr float jumpSpeed = 1020.f;
// Shift with a move key.
constexpr float sprintSpeed = 1.6f * moveSpeed;

// On death, the last moments replay in slow motion before the player respawns:
// deathCamTicks of game time show deathCamSpeed times as much recording.
constexpr std::int64_t deathCamTicks = 4 * world::ticksPerSecond;
constexpr float deathCamSpeed = 0.5f;
constexpr std::int64_t deathCamFootageTicks =
    static_cast<std::int64_t>(deathCamTicks * deathCamSpeed);
// The death holds on the live view this long first, so the player sees what hit
// it and the recording catches it.
constexpr std::int64_t deathPauseTicks = world::ticksPerSecond / 3;
// The replay buffer is trimmed a keyframe at a time, so it keeps between 5 and
// 6 s: always enough for the death cam.
constexpr std::int64_t replayKeyframeTicks = world::ticksPerSecond;
constexpr std::size_t replayKeyframes = 6;
constexpr std::size_t cameraHistoryLength =
    replayKeyframes * replayKeyframeTicks;
// deathEvent::cause for a fall; a bee death names the bee's NetId instead.
constexpr std::uint32_t fellCause = 0;

// A pitfall gives way this long after the player lands on it, and is back in
// time for the next attempt.
constexpr std::int64_t pitfallDelayTicks = world::ticksPerSecond * 3 / 10;
constexpr std::int64_t pitfallReturnTicks = 3 * world::ticksPerSecond;

// The cropped spike sprite's height; spikes sit on the floor of their cell.
constexpr float spikeHeight = 34.f;
// deathEvent::cause for spikes: not a NetId any entity can have.
constexpr std::uint32_t spikesCause = engine::kNetIdLocalMask;

constexpr std::array moveKeys{SC::SDL_SCANCODE_A, SC::SDL_SCANCODE_D,
                              SC::SDL_SCANCODE_LEFT, SC::SDL_SCANCODE_RIGHT};

constexpr int joinAttempts = 5;

// The camera only scrolls once the player leaves the middle 720px of the view.
constexpr float viewWidth = static_cast<float>(windowWidth);
constexpr float scrollMargin = 600.f;
// Boundaries reach far past the level, so a respawn anywhere still touches one.
constexpr float boundaryReach = 8000.f;

constexpr float ledgeHeight = 20.f;

// Player N is drawn with sheet (N - 1) % 4, so every window agrees on colors.
constexpr std::array<const char *, 4> playerSheetFiles{
    "sheet_player.png", "sheet_player_blue.png", "sheet_player_pink.png",
    "sheet_player_orange.png"};

// Drawn by main outside the Scene, so they need text-cache ids no entity uses.
constexpr engine::EntityId hudTextId =
    std::numeric_limits<engine::EntityId>::max();
constexpr engine::EntityId disconnectedTextId = hudTextId - 1;
constexpr engine::EntityId replayBannerTextId = disconnectedTextId - 1;
constexpr engine::EntityId countdownTextId = replayBannerTextId - 1;
constexpr engine::EntityId firstFeedTextId = countdownTextId - 1;
constexpr std::size_t feedLength = 4;
// A feed line disappears this long after it was added.
constexpr std::int64_t feedLineTicks = 4 * world::ticksPerSecond;
constexpr glm::vec2 hudPosition{24.f, 24.f};
constexpr float hudFontSize = 40.f;
// "REPLAY" across the middle of the view. Its size was measured offline for
// SuperBouncer at this font size, since the renderer can't measure text.
constexpr float bannerFontSize = 160.f;
constexpr glm::vec2 replayBannerSize{487.f, 179.f};
constexpr glm::vec2 replayBannerPosition{(windowWidth - replayBannerSize.x) / 2.f,
                                         (windowHeight - replayBannerSize.y) / 2.f};
// "P1 WON - RESTARTING IN 3" (or "YOU WON ...") is about 900px wide at this
// size; it sits above the replay banner so the two never overlap.
constexpr float countdownFontSize = 80.f;
constexpr glm::vec2 countdownPosition{(windowWidth - 900.f) / 2.f, 260.f};
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

// The rightmost spawn at or left of x, else the leftmost. spawns is sorted by
// x.
glm::vec2 respawnFor(const std::vector<glm::vec2> &spawns, float x) {
  const auto past = std::ranges::upper_bound(spawns, x, {}, &glm::vec2::x);
  return past == spawns.begin() ? spawns.front() : *std::prev(past);
}

// Puts the two hidden scroll boundaries just inside the view's edges.
void placeBoundaries(engine::Scene &scene, engine::EntityId left,
                     engine::EntityId right, float cameraX) {
  scene.transform(left).position = {cameraX + scrollMargin - boundaryReach,
                                    -boundaryReach};
  scene.transform(right).position = {cameraX + viewWidth - scrollMargin,
                                     -boundaryReach};
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
  const engine::TextureId ledgeTexture =
      renderer.loadTexture(assetDir + "png/bridge.png");
  const engine::TextureId stoneTexture =
      renderer.loadTexture(assetDir + "png/terrain_stone_block.png");
  const engine::Shape spikesShape{
      .size = {tileSize, spikeHeight},
      .texture = renderer.loadTexture(assetDir + "png/spikes.png")};
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
  const engine::FontId bannerFont =
      renderer.loadFont(assetDir + "fonts/SuperBouncer.ttf", bannerFontSize);
  const engine::FontId countdownFont =
      renderer.loadFont(assetDir + "fonts/SuperBouncer.ttf", countdownFontSize);

  engine::Scene scene;

  // Static level: built identically by every client and never replicated.
  const level::Map map = level::load(level::path);
  const float levelWidth = world::widthOf(map);
  const float levelHeight = world::heightOf(map);
  const engine::Shape grassShape{.size = {tileSize, tileSize},
                                 .texture = terrainTexture};
  std::vector<engine::EntityId> terrain;
  engine::EntityId goal = 0;
  for (int row = 0; row < static_cast<int>(map.size()); ++row) {
    const std::string &line = map[static_cast<std::size_t>(row)];
    for (int column = 0; column < static_cast<int>(line.size()); ++column) {
      switch (const char cell = line[column]) {
      case level::solid:
      case level::ghost:
      case level::pitfall: {
        const engine::EntityId tile = scene.createEntity();
        scene.transform(tile).position = world::cellPosition(column, row);
        scene.addShape(tile, grassShape);
        // A ghost is only drawn: nothing to stand on.
        if (cell == level::ghost) {
          break;
        }
        scene.addCollider(tile, {.size = {tileSize, tileSize}});
        if (cell == level::pitfall) {
          scene.add<Pitfall>(tile);
        }
        terrain.push_back(tile);
        break;
      }
      case level::ledge:
      case level::stone: {
        const int run = world::runAt(line, column);
        if (run == 0) {
          break;
        }
        const bool isLedge = cell == level::ledge;
        const glm::vec2 size{static_cast<float>(run) * tileSize,
                             isLedge ? ledgeHeight : tileSize};
        const engine::EntityId block = scene.createEntity();
        scene.transform(block).position = world::cellPosition(column, row);
        scene.addShape(block,
                       {.size = size,
                        .texture = isLedge ? ledgeTexture : stoneTexture,
                        .tiled = true});
        scene.addCollider(block, {.size = size});
        terrain.push_back(block);
        break;
      }
      case level::deathZone: {
        const int run = world::runAt(line, column);
        if (run == 0) {
          break;
        }
        const engine::EntityId zone = scene.createEntity();
        scene.transform(zone).position = world::cellPosition(column, row);
        scene.addCollider(
            zone, {.size = {static_cast<float>(run) * tileSize, tileSize}});
        scene.add<DeathZone>(zone);
        break;
      }
      case level::spikes: {
        // One per cell, so each cell springs on its own.
        const engine::EntityId trap = scene.createEntity();
        scene.transform(trap).position =
            world::cellPosition(column, row) +
            glm::vec2{0.f, tileSize - spikeHeight};
        scene.addCollider(trap, {.size = spikesShape.size});
        scene.add<Spikes>(trap);
        break;
      }
      case level::playerSpawn: {
        // Bottom-align the taller player sprite to the cell it is standing in.
        const engine::EntityId spawn = scene.createEntity();
        scene.transform(spawn).position =
            world::cellPosition(column, row + 1) - glm::vec2{0.f, playerSize};
        scene.add<SpawnPoint>(spawn);
        break;
      }
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

  // Spawn points never move, so the sim thread can take a copy.
  std::vector<glm::vec2> spawns;
  scene.each<SpawnPoint>([&](engine::EntityId id, SpawnPoint &) {
    spawns.push_back(scene.transform(id).position);
  });
  std::ranges::sort(spawns, {}, &glm::vec2::x);
  if (spawns.size() < 2) {
    engine::log::error("the level needs at least two spawn points");
    return 1;
  }
  // Players alternate between the first two, so they don't start stacked.
  const glm::vec2 startSpawn = spawns[(self - 1) % 2];

  // Server-driven objects: pre-built with textures, then bound to the server's
  // NetIds so its snapshots move them instead of spawning untextured copies.
  const std::vector<world::Motion> motions = world::motions(map);
  const std::vector<engine::EntityId> objects = world::spawn(
      scene, motions, world::Art{.platform = terrainTexture, .bee = beeSheet});
  std::vector<engine::EntityId> platforms;
  engine::EntityId bee = 0;
  engine::NetId beeNetId = 0;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const engine::NetId netId =
        engine::makeNetId(engine::kServerId, static_cast<std::uint32_t>(i));
    link.session.replicator().bind(netId, objects[i]);
    if (motions[i].kind == world::Kind::bee) {
      bee = objects[i];
      beeNetId = netId;
    } else {
      platforms.push_back(objects[i]);
    }
  }

  // The only entity this process owns; every other RigidBody in the Scene
  // belongs to a remote player.
  const engine::EntityId player = scene.createEntity();
  scene.transform(player).position = startSpawn;
  scene.addShape(player, {.size = {playerSize, playerSize}});
  scene.addSpriteAnimation(player,
                           animationFor(PlayerState::idle, playerSheet));
  scene.addCollider(player, {.size = {playerSize, playerSize}});
  scene.addRigidBody(player);
  const engine::NetId playerNetId = link.session.replicator().track(player);

  // Local view state: never replicated, every client scrolls on its own.
  const engine::EntityId cameraEntity = scene.createEntity();
  scene.add<Camera>(cameraEntity);
  const auto addBoundary = [&scene, levelHeight](ScrollBoundary::Side side) {
    const engine::EntityId boundary = scene.createEntity();
    scene.addCollider(
        boundary,
        {.size = {boundaryReach, levelHeight + 2 * boundaryReach}});
    scene.add<ScrollBoundary>(boundary, {side});
    return boundary;
  };
  const engine::EntityId leftBoundary = addBoundary(ScrollBoundary::Side::left);
  const engine::EntityId rightBoundary =
      addBoundary(ScrollBoundary::Side::right);
  placeBoundaries(scene, leftBoundary, rightBoundary, 0.f);

  engine::Timeline realTime;
  engine::Timeline gameTime(realTime, world::ticksPerSecond);

  // Everything below is captured by onTick and belongs to the sim thread once
  // it starts.
  PlayerState playerState = PlayerState::idle;
  bool alive = true;
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
  Interpolator interpolator;
  std::vector<engine::EntityId> interpolated; // reused every tick

  ev::eventManager events;
  ev::inputMapper keys;
  // Any key order completes a chord, so pressing Shift mid-run starts a sprint.
  for (const SC::SDL_Scancode moveKey : moveKeys) {
    keys.addChord({SC::SDL_SCANCODE_LSHIFT, moveKey});
  }
  // Held move keys per direction: A and Left both count as left.
  int leftHeld = 0;
  int rightHeld = 0;
  int sprintChords = 0; // Shift+move chords currently held
  bool jumpPressed = false; // this tick
  struct Score {
    int wins = 0;
    int deaths = 0;
  };
  std::map<engine::ClientId, Score> scores;
  struct FeedLine {
    std::int64_t expiresAt;
    std::string text;
  };
  std::deque<FeedLine> feed;
  // sim -> main: the scoreboard then the feed, newest last.
  engine::threading::LatestValue<std::vector<std::string>> hudSlot;

  // Built once sim exists, before it starts.
  std::optional<engine::replay::replayController> replays;
  // The replay Scene has no Camera, so the death cam looks up the view each
  // replayed tick had here, by tick.
  std::deque<std::pair<std::int64_t, float>> cameraHistory;
  std::atomic<float> deathCamX{0.f}; // sim -> main
  struct deathCamStart : ev::simpleEvent {};
  // Seconds until the round restarts; 0 clears the countdown.
  struct restartCountdown : ev::simpleEvent {
    int seconds = 0;
  };
  // sim -> main: the countdown on screen, and whose win it follows.
  std::atomic<int> restartIn{0};
  std::atomic<engine::ClientId> roundWinner{0};
  struct deathCamOver : ev::simpleEvent {};
  struct feedLineExpired : ev::simpleEvent {};
  struct pitfallDrop : ev::simpleEvent {
    engine::EntityId tile = 0;
  };
  struct pitfallReturn : ev::simpleEvent {
    engine::EntityId tile = 0;
  };

  events.subscribe<ev::inputEvent>([&](const ev::inputEvent &e, engine::Scene &) {
    const int change = e.pressed ? 1 : -1;
    switch (e.key) {
    case SC::SDL_SCANCODE_A:
    case SC::SDL_SCANCODE_LEFT:
      leftHeld += change;
      break;
    case SC::SDL_SCANCODE_D:
    case SC::SDL_SCANCODE_RIGHT:
      rightHeld += change;
      break;
    case SC::SDL_SCANCODE_SPACE:
    case SC::SDL_SCANCODE_W:
    case SC::SDL_SCANCODE_UP:
      jumpPressed = jumpPressed || e.pressed;
      break;
    default:
      break;
    }
  });
  events.subscribe<ev::inputChordEvent>(
      [&](const ev::inputChordEvent &e, engine::Scene &) {
        sprintChords += e.pressed ? 1 : -1;
      });

  // Only the local player's contacts with the bee, death zones, spikes, the
  // flag and pitfalls it lands on are raised; terrain push-out stays in onTick.
  events.subscribe<ev::collisionEvent>(
      [&](const ev::collisionEvent &e, engine::Scene &scene) {
        if (Pitfall *pit = scene.get<Pitfall>(e.b)) {
          if (!pit->dropping) {
            pit->dropping = true;
            events.raise(pitfallDrop{{}, e.b}, events.now() + pitfallDelayTicks);
          }
          return;
        }
        // The server decides who won, so touching the flag is only a claim.
        if (e.b == goal) {
          link.session.send(race::goalReached::kType,
                            race::goalReached{}.encode(), events.now());
          return;
        }
        // Hidden spikes spring up under the player, killing it.
        if (Spikes *spikes = scene.get<Spikes>(e.b); spikes && !spikes->up) {
          spikes->up = true;
          scene.addShape(e.b, spikesShape);
        }
        // Death events carry NetIds, which every process agrees on.
        const std::uint32_t cause = e.b == bee ? beeNetId
                                    : scene.has<Spikes>(e.b) ? spikesCause
                                                             : fellCause;
        events.raise(ev::deathEvent{{}, playerNetId, cause}, events.now());
      });
  // A dropped pitfall is drawn and collided with again once it returns.
  events.subscribe<pitfallDrop>(
      [&](const pitfallDrop &e, engine::Scene &scene) {
        scene.removeShape(e.tile);
        scene.removeCollider(e.tile);
        events.raise(pitfallReturn{{}, e.tile},
                     events.now() + pitfallReturnTicks);
      });
  events.subscribe<pitfallReturn>(
      [&](const pitfallReturn &e, engine::Scene &scene) {
        scene.addShape(e.tile, grassShape);
        scene.addCollider(e.tile, {.size = {tileSize, tileSize}});
        scene.get<Pitfall>(e.tile)->dropping = false;
      });
  const auto publishHud = [&] {
    std::vector<std::string> lines;
    lines.reserve(feed.size() + scores.size());
    for (const auto &[id, score] : scores) {
      lines.push_back(fmt::format("P{}   {} wins   {} deaths", id, score.wins,
                                  score.deaths));
    }
    for (const FeedLine &line : feed) {
      lines.push_back(line.text);
    }
    hudSlot.publish(std::move(lines));
  };
  const auto addFeedLine = [&](std::string text) {
    const std::int64_t expiresAt = events.now() + feedLineTicks;
    feed.push_back({expiresAt, std::move(text)});
    if (feed.size() > feedLength) {
      feed.pop_front();
    }
    publishHud();
    events.raise(feedLineExpired{}, expiresAt);
  };
  events.subscribe<feedLineExpired>(
      [&](const feedLineExpired &, engine::Scene &) {
        std::erase_if(feed, [&](const FeedLine &line) {
          return line.expiresAt <= events.now();
        });
        publishHud();
      });
  const auto recordDeath = [&](engine::ClientId victim, std::uint32_t cause) {
    ++scores[victim].deaths;
    addFeedLine(fmt::format("P{} {}", victim,
                            cause == beeNetId     ? "was stung by the bee"
                            : cause == spikesCause ? "got spiked"
                                                   : "fell into a pit"));
  };
  // Every player, winner included, goes back to the start once the win has
  // been shown for a moment.
  events.subscribe<race::roundWon>(
      [&](const race::roundWon &e, engine::Scene &) {
        ++scores[e.winner].wins;
        addFeedLine(e.winner == self ? "you won the race!"
                                     : fmt::format("P{} won the race!", e.winner));
        events.raise(ev::spawnEvent{{}, playerNetId, self, startSpawn},
                     events.now() + race::resetDelayTicks);
        roundWinner = e.winner;
        for (int left = race::resetDelaySeconds; left >= 0; --left) {
          events.raise(restartCountdown{{}, left},
                       events.now() + (race::resetDelaySeconds - left) *
                                          world::ticksPerSecond);
        }
      },
      /*fromNetwork=*/true);
  events.subscribe<restartCountdown>(
      [&](const restartCountdown &e, engine::Scene &) {
        restartIn = e.seconds;
      });
  // Local deaths are raised by the collision handler and sent to the server;
  // remote ones are the server relaying other players' deaths.
  events.subscribe<ev::deathEvent>(
      [&](const ev::deathEvent &e, engine::Scene &scene) {
        const engine::ClientId victim = engine::ownerOf(e.entity);
        if (e.remote) {
          // The relay includes our own deaths, already counted when raised.
          if (victim != self) {
            recordDeath(victim, e.cause);
          }
          return;
        }
        if (!alive) {
          return;
        }
        alive = false;
        recordDeath(self, e.cause);
        link.session.send(ev::deathEvent::kType, e.encode(), events.now());
        scene.getRigidBody(player)->velocity = {0.f, 0.f};
        *scene.getSpriteAnimation(player) =
            engine::SpriteAnimation::uniform(playerSheet, {frameHit}, 1.f);
        events.raise(deathCamStart{}, events.now() + deathPauseTicks);
        events.raise(deathCamOver{},
                     events.now() + deathPauseTicks + deathCamTicks);
      },
      /*fromNetwork=*/true);
  // A round reset may have respawned the player during the pause.
  events.subscribe<deathCamStart>(
      [&](const deathCamStart &, engine::Scene &) {
        if (!alive) {
          events.raise(
              ev::replayPlayStart{{}, events.now() - deathCamFootageTicks},
              events.now());
        }
      });
  // A round reset may have respawned the player during the death cam.
  events.subscribe<deathCamOver>([&](const deathCamOver &, engine::Scene &scene) {
    if (!alive) {
      events.raise(
          ev::spawnEvent{{}, playerNetId, self,
                         respawnFor(spawns, scene.transform(player).position.x)},
          events.now());
    }
  });
  events.subscribe<ev::spawnEvent>(
      [&](const ev::spawnEvent &e, engine::Scene &scene) {
        if (replays->playing()) {
          events.raise(ev::replayPlayStop{}, events.now());
        }
        if (!replays->recording()) {
          events.raise(ev::replayRecordStart{}, events.now());
        }
        scene.transform(player).position = e.position;
        scene.getRigidBody(player)->velocity = {0.f, 0.f};
        playerState = PlayerState::idle;
        *scene.getSpriteAnimation(player) =
            animationFor(playerState, playerSheet);
        alive = true;
        grounded = false;
        riding.reset();
        // Sprung spikes hide again, so the next run has to remember them.
        scene.each<Spikes>([&scene](engine::EntityId trap, Spikes &spikes) {
          spikes.up = false;
          scene.removeShape(trap);
        });
      });

  auto onTick = [&](const engine::TickContext &ctx) {
    engine::Scene &scene = ctx.scene;
    link.pull(scene);
    events.receive(link.session.drainMessages(), ctx.tick);

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

    // Snapshots land unevenly, so remote entities are drawn a few ticks behind
    // from their stamped positions instead of jumping to the newest one.
    const engine::networking::sceneReplicator &replicator =
        link.session.replicator();
    interpolated.assign(objects.begin(), objects.end());
    for (const auto &[remote, state] : remoteStates) {
      interpolated.push_back(remote);
    }
    for (const engine::EntityId entity : interpolated) {
      const engine::NetId netId = replicator.netIdOf(entity).value();
      if (const auto stamp = world::decodeStamp(replicator.extraOf(netId))) {
        interpolator.record(entity, engine::ownerOf(netId), *stamp,
                            scene.transform(entity).position);
      }
    }
    interpolator.forgetMissing(scene);
    interpolator.advance(Interpolator::Clock::now());
    for (const engine::EntityId entity : interpolated) {
      if (const auto at = interpolator.positionOf(entity)) {
        scene.transform(entity).position = *at;
      }
    }

    // Carry the player along with the platform it stood on last tick.
    glm::vec2 &position = scene.transform(player).position;
    for (std::size_t i = 0; i < platforms.size(); ++i) {
      const glm::vec2 now = scene.transform(platforms[i]).position;
      if (riding == i) {
        position += now - platformLast[i];
      }
      platformLast[i] = now;
    }

    keys.raise(events, ctx.keyboard, ctx.previousKeyboard, ctx.tick);
    events.run(scene, ctx.tick);

    // A dead player stays frozen where it died until its spawnEvent.
    if (alive) {
      engine::RigidBody &body = *scene.getRigidBody(player);
      const float speed = sprintChords > 0 ? sprintSpeed : moveSpeed;
      body.velocity.x =
          (rightHeld > 0 ? speed : 0.f) - (leftHeld > 0 ? speed : 0.f);
      if (jumpPressed && grounded) {
        body.velocity.y = -jumpSpeed;
      }

      physics.step(scene, ctx.dt);

      // Raised for the next tick, so its run() handles them before that tick's
      // input.
      const std::int64_t next = ctx.tick + 1;

      grounded = false;
      riding.reset();
      for (const engine::EntityId tile : terrain) {
        if (!resolveCollision(scene, physics, player, tile)) {
          continue;
        }
        grounded = true;
        if (scene.has<Pitfall>(tile)) {
          events.raise(ev::collisionEvent{{}, player, tile}, next);
        }
      }
      for (std::size_t i = 0; i < platforms.size(); ++i) {
        if (resolveCollision(scene, physics, player, platforms[i])) {
          grounded = true;
          riding = i;
        }
      }

      const auto touch = [&](engine::EntityId other) {
        if (physics.isCollision(scene, player, other)) {
          events.raise(ev::collisionEvent{{}, player, other}, next);
        }
      };
      touch(bee);
      scene.each<DeathZone>(
          [&](engine::EntityId zone, DeathZone &) { touch(zone); });
      scene.each<Spikes>(
          [&](engine::EntityId trap, const Spikes &) { touch(trap); });
      // Only on first contact, so standing on the flag doesn't repeat it.
      const bool onGoal = physics.isCollision(scene, player, goal);
      if (onGoal && !touchingGoal) {
        touch(goal);
      }
      touchingGoal = onGoal;
      // A fast fall could tunnel through a zone in one tick.
      if (position.y > levelHeight + playerSize) {
        events.raise(ev::deathEvent{{}, playerNetId, fellCause}, next);
      }

      const PlayerState desired = stateFor(body.velocity, grounded);
      if (desired != playerState) {
        playerState = desired;
        *scene.getSpriteAnimation(player) = animationFor(desired, playerSheet);
      }
    }
    jumpPressed = false;

    // Touching a boundary moves the camera just enough to put the player back
    // on its edge; then the boundaries follow the view.
    Camera &camera = *scene.get<Camera>(cameraEntity);
    float cameraX = camera.offset.x;
    scene.each<ScrollBoundary>(
        [&](engine::EntityId boundary, const ScrollBoundary &edge) {
          if (!physics.isCollision(scene, player, boundary)) {
            return;
          }
          cameraX = edge.side == ScrollBoundary::Side::right
                        ? position.x + playerSize - (viewWidth - scrollMargin)
                        : position.x - scrollMargin;
        });
    cameraX = std::clamp(cameraX, 0.f, levelWidth - viewWidth);
    if (cameraX != camera.offset.x) {
      camera.offset.x = cameraX;
      placeBoundaries(scene, leftBoundary, rightBoundary, cameraX);
    }

    engine::advanceAnimations(scene, ctx.dt);

    // The live Scene keeps simulating under the death cam: remote players,
    // platforms and the network all carry on.
    replays->update(scene, ctx.tick);
    cameraHistory.emplace_back(ctx.tick, camera.offset.x);
    if (cameraHistory.size() > cameraHistoryLength) {
      cameraHistory.pop_front();
    }
    if (replays->playing()) {
      const std::int64_t shown = replays->recorder().playbackTick();
      const auto at = std::ranges::lower_bound(
          cameraHistory, shown, {}, &std::pair<std::int64_t, float>::first);
      deathCamX = at != cameraHistory.end() ? at->second : camera.offset.x;
    }

    // Once per tick, so the send rate follows this client's own Timeline.
    link.session.replicator().setExtra(playerNetId,
                                       world::encodeStamp(ctx.tick));
    link.push(scene, ctx.tick);
  };

  engine::SimulationThread sim(std::move(scene), gameTime, onTick);
  replays.emplace(events, sim, world::ticksPerSecond, replayKeyframeTicks,
                  replayKeyframes);
  events.raise(ev::replayRecordStart{}, 0);
  events.raise(ev::replaySpeed{{}, deathCamSpeed}, 0);
  sim.start();
  engine::log::info("P{} joined in {} mode. A/D or arrows to move, Space/W/Up "
                    "to jump, P pause, 1/2/3 speed 0.5x/1x/2x, F scaling mode",
                    self, Link::mode);

  // main thread: input capture, time control requests and rendering.
  std::optional<engine::RenderFrame> frame;
  std::vector<std::string> hudLines;
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
    if (auto next = hudSlot.take()) {
      hudLines = std::move(*next);
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
      // A death cam frame is the replay Scene, which has no Camera.
      const Camera *live = frame->scene.get<Camera>(cameraEntity);
      const bool replaying = live == nullptr;
      const glm::vec2 camera =
          replaying ? glm::vec2{deathCamX.load(), 0.f} : live->offset;
      renderer.setCamera(camera);
      renderer.drawEntities(frame->scene);
      const engine::SimStatus &status = frame->status;
      const std::string hud =
          status.paused ? fmt::format("P{}  |  {}  |  PAUSED", self, Link::mode)
          : replaying
              ? fmt::format("P{}  |  {}  |  DEATH CAM", self, Link::mode)
              : fmt::format("P{}  |  {}  |  {:.1f}x  |  cam {:.0f}", self,
                            Link::mode, status.speed, camera.x);
      renderer.drawText(
          hudTextId,
          {.val = hud,
           .font = hudFont,
           .color = status.paused || replaying ? hudAlertColor : hudColor},
          hudPosition);
      if (const int seconds = restartIn.load(); seconds > 0) {
        const engine::ClientId winner = roundWinner.load();
        renderer.drawText(
            countdownTextId,
            {.val = winner == self
                        ? fmt::format("YOU WON - RESTARTING IN {}", seconds)
                        : fmt::format("P{} WON - RESTARTING IN {}", winner,
                                      seconds),
             .font = countdownFont,
             .color = hudAlertColor},
            countdownPosition);
      }
      if (replaying) {
        renderer.drawText(replayBannerTextId,
                          {.val = "REPLAY",
                           .font = bannerFont,
                           .color = hudAlertColor},
                          replayBannerPosition);
      }
    }
    if (!online) {
      renderer.drawText(disconnectedTextId,
                        {.val = "DISCONNECTED FROM SERVER",
                         .font = hudFont,
                         .color = hudAlertColor},
                        disconnectedPosition);
    }
    for (std::size_t i = 0; i < hudLines.size(); ++i) {
      renderer.drawText(
          firstFeedTextId - static_cast<engine::EntityId>(i),
          {.val = hudLines[i], .font = hudFont, .color = hudColor},
          {hudPosition.x, disconnectedPosition.y +
                              static_cast<float>(i + 1) * 1.5f * hudFontSize});
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
