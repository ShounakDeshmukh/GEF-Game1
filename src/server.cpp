#include "modes.hpp"
#include "pitfallEvents.hpp"
#include "raceEvents.hpp"
#include "world.hpp"

#include <engine/engine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace net = engine::networking;
namespace ev = engine::events;

// For beeBurstLength of every beeBurstEvery seconds, the bee flies its path
// beeBurstSpeed times faster.
constexpr float beeBurstEvery = 5.f;
constexpr float beeBurstLength = 1.f;
constexpr float beeBurstSpeed = 3.f;

// How far a client's last replicated hitbox may be from the flag when it claims
// it, since that position lags the claim by a tick or two.
constexpr float goalClaimSlack = world::tileSize;

std::atomic<bool> stopRequested{false};

void requestStop(int) { stopRequested = true; }

void logRoster(net::sessionServer &server) {
  for (const net::rosterEvent &event : server.drainRosterEvents()) {
    if (event.change == net::RosterChange::Left) {
      engine::log::info("P{} left", event.client.id);
    } else if (event.client.peerEndpoint.empty()) {
      engine::log::info("P{} joined as a client", event.client.id);
    } else {
      engine::log::info("P{} joined as a peer, publishing directly on {}",
                        event.client.id, event.client.peerEndpoint);
    }
  }
}

// updates/s follows each client's own Timeline speed. playerStates/s is 0 for
// peers, whose player data never reaches the server.
void logRates(
    const net::sessionServer &server,
    std::unordered_map<engine::ClientId, net::clientStats> &previous) {
  std::unordered_map<engine::ClientId, net::clientStats> current;
  for (const net::clientStats &stats : server.stats()) {
    const auto found = previous.find(stats.id);
    const net::clientStats before =
        found != previous.end() ? found->second : net::clientStats{};
    engine::log::info("P{}: {} updates/s, {} playerStates/s", stats.id,
                      stats.updates - before.updates,
                      stats.stateUpdates - before.stateUpdates);
    current.emplace(stats.id, stats);
  }
  previous = std::move(current);
}

glm::vec2 goalPosition(const level::Map &map) {
  for (int row = 0; row < static_cast<int>(map.size()); ++row) {
    const std::size_t column = map[static_cast<std::size_t>(row)].find(level::goal);
    if (column != std::string::npos) {
      return world::cellPosition(static_cast<int>(column), row);
    }
  }
  throw std::runtime_error("the level has no flag");
}

// Peers publish their players straight to each other, so the server has no
// position to check and takes their word for it.
bool claimPlausible(net::sessionServer &server, const engine::Scene &scene,
                    engine::ClientId claimant, glm::vec2 goal) {
  const std::vector<engine::networking::clientInfo> roster = server.roster();
  const auto found = std::ranges::find(roster, claimant,
                                       &engine::networking::clientInfo::id);
  if (found != roster.end() && !found->peerEndpoint.empty()) {
    return true;
  }
  // A client's player is the only entity it tracks, so it has local index 0.
  const std::optional<engine::EntityId> player =
      server.replicator().entityOf(engine::makeNetId(claimant, 0));
  const engine::Collider *hitbox =
      player ? scene.getCollider(*player) : nullptr;
  if (hitbox == nullptr) {
    return false;
  }
  const glm::vec2 low = scene.transform(*player).position + hitbox->offset;
  const glm::vec2 high = low + hitbox->size;
  return low.x <= goal.x + world::goalSize + goalClaimSlack &&
         high.x >= goal.x - goalClaimSlack &&
         low.y <= goal.y + world::goalSize + goalClaimSlack &&
         high.y >= goal.y - goalClaimSlack;
}

} // namespace

int runServer(int port) {
  const std::string endpoint = "tcp://*:" + std::to_string(port);
  net::sessionServer server(endpoint);

  const level::Map map = level::load(level::path);
  const std::vector<world::Motion> motions = world::motions(map);
  const glm::vec2 goal = goalPosition(map);
  engine::Scene scene;
  const std::vector<engine::EntityId> objects =
      world::spawn(scene, motions, std::nullopt);
  std::vector<engine::NetId> netIds;
  netIds.reserve(objects.size());
  for (const engine::EntityId object : objects) {
    netIds.push_back(server.replicator().track(object));
  }

  engine::Timeline realTime;
  engine::Timeline gameTime(realTime, world::ticksPerSecond);

  // world subsystem -> sim: only the newest positions matter. tick is the one
  // they were computed for, which a sim tick that picks them up may not match.
  struct WorldUpdate {
    std::int64_t tick;
    std::vector<glm::vec2> positions;
  };
  engine::threading::LatestValue<WorldUpdate> worldSlot;

  // Clients report their own player's deaths; the server relays them to
  // everyone, so each window's kill feed agrees.
  ev::eventManager events;
  events.subscribe<ev::deathEvent>(
      [&](const ev::deathEvent &e, engine::Scene &) {
        // from is set by the session, so a client can't forge another's death.
        if (engine::ownerOf(e.entity) != e.from) {
          engine::log::warn("P{} reported a death for P{}; dropped", e.from,
                            engine::ownerOf(e.entity));
          return;
        }
        server.broadcast(ev::deathEvent::kType, e.encode(), events.now());
      },
      /*fromNetwork=*/true);

  // The first goalReached of a round wins it; later ones are ignored until the
  // countdown reopens the round.
  bool roundOpen = true;
  events.subscribe<race::goalReached>(
      [&](const race::goalReached &e, engine::Scene &scene) {
        if (!roundOpen) {
          return;
        }
        if (!claimPlausible(server, scene, e.from, goal)) {
          engine::log::warn("P{} claimed the flag from too far away; dropped",
                            e.from);
          return;
        }
        roundOpen = false;
        engine::log::info("P{} won the round", e.from);
        server.broadcast(race::roundWon::kType,
                         race::roundWon{{}, e.from}.encode(), events.now());
        for (int left = race::resetDelaySeconds - 1; left >= 0; --left) {
          events.raise(race::countdown{{}, static_cast<std::uint8_t>(left)},
                       events.now() + (race::resetDelaySeconds - left) *
                                          world::ticksPerSecond);
        }
      },
      /*fromNetwork=*/true);
  events.subscribe<race::countdown>(
      [&](const race::countdown &e, engine::Scene &) {
        server.broadcast(race::countdown::kType, e.encode(), events.now());
        roundOpen = e.seconds == 0;
      });

  // dropping[i] covers a pitfall's whole cycle, from the first touch until it
  // is back, so touches meanwhile are ignored.
  std::vector<bool> dropping(static_cast<std::size_t>(
      std::ranges::count(map | std::views::join, level::pitfall)));
  events.subscribe<pitfalls::touched>(
      [&](const pitfalls::touched &e, engine::Scene &) {
        if (e.index >= dropping.size() || dropping[e.index]) {
          return;
        }
        dropping[e.index] = true;
        events.raise(pitfalls::changed{{}, e.index, true},
                     events.now() + pitfalls::dropDelayTicks);
      },
      /*fromNetwork=*/true);
  // Raised here on the server's own clock, then broadcast as it happens.
  events.subscribe<pitfalls::changed>(
      [&](const pitfalls::changed &e, engine::Scene &) {
        server.broadcast(pitfalls::changed::kType, e.encode(), events.now());
        if (e.down) {
          events.raise(pitfalls::changed{{}, e.index, false},
                       events.now() + pitfalls::returnTicks);
        } else {
          dropping[e.index] = false;
        }
      });

  // sim thread: owns the Scene and every replicator call.
  engine::SimulationThread sim(
      std::move(scene), gameTime, [&](const engine::TickContext &ctx) {
        server.applyClientStates(ctx.scene);
        events.receive(server.drainMessages(), ctx.tick);
        events.run(ctx.scene, ctx.tick);
        if (const auto update = worldSlot.take()) {
          for (std::size_t i = 0; i < objects.size(); ++i) {
            ctx.scene.transform(objects[i]).position = update->positions[i];
            server.replicator().setExtra(netIds[i],
                                         world::encodeStamp(update->tick));
          }
        }
        server.publishScene(ctx.scene, ctx.tick);
      });

  // world thread: moves the platforms and the bee without touching the Scene.
  sim.addSubsystemThread(
      "world", [&worldSlot, &motions, seconds = 0.f, beeSeconds = 0.f](
                   const engine::SubsystemContext &ctx) mutable {
        seconds += ctx.dt;
        // The bee keeps its own clock, so a burst speeds it along its path
        // instead of making it jump.
        const bool bursting = std::fmod(seconds, beeBurstEvery) < beeBurstLength;
        beeSeconds += ctx.dt * (bursting ? beeBurstSpeed : 1.f);
        std::vector<glm::vec2> positions;
        positions.reserve(motions.size());
        for (const world::Motion &motion : motions) {
          positions.push_back(world::positionAt(
              motion, motion.kind == world::Kind::bee ? beeSeconds : seconds));
        }
        worldSlot.publish({ctx.tick, std::move(positions)});
      });

  std::signal(SIGINT, requestStop);
  std::signal(SIGTERM, requestStop);

  server.start();
  sim.start();
  engine::log::info("headless server on {}: no window, simulating {} moving "
                    "objects at {} ticks/s; Ctrl+C to stop",
                    endpoint, objects.size(), world::ticksPerSecond);

  // main thread: roster and rate logging only.
  std::unordered_map<engine::ClientId, net::clientStats> previous;
  while (!stopRequested && !sim.failure()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    logRoster(server);
    logRates(server, previous);
  }

  sim.stop();
  server.stop();
  if (sim.failure()) {
    engine::log::error("server simulation failed");
    return 1;
  }
  return 0;
}
