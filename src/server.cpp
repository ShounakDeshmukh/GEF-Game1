#include "modes.hpp"
#include "world.hpp"

#include <engine/engine.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace net = engine::networking;

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

} // namespace

int runServer(int port) {
  const std::string endpoint = "tcp://*:" + std::to_string(port);
  net::sessionServer server(endpoint);

  const std::vector<world::Motion> motions = world::motions();
  engine::Scene scene;
  const std::vector<engine::EntityId> objects =
      world::spawn(scene, motions, std::nullopt);
  for (const engine::EntityId object : objects) {
    server.replicator().track(object);
  }

  engine::Timeline realTime;
  engine::Timeline gameTime(realTime, world::ticksPerSecond);

  // world subsystem -> sim: only the newest positions matter.
  engine::threading::LatestValue<std::vector<glm::vec2>> worldSlot;

  // sim thread: owns the Scene and every replicator call.
  engine::SimulationThread sim(
      std::move(scene), gameTime, [&](const engine::TickContext &ctx) {
        server.applyClientStates(ctx.scene);
        if (const auto positions = worldSlot.take()) {
          for (std::size_t i = 0; i < objects.size(); ++i) {
            ctx.scene.transform(objects[i]).position = (*positions)[i];
          }
        }
        server.publishScene(ctx.scene, ctx.tick);
      });

  // world thread: moves the platforms and the bee without touching the Scene.
  sim.addSubsystemThread(
      "world", [&worldSlot, &motions,
                seconds = 0.f](const engine::SubsystemContext &ctx) mutable {
        seconds += ctx.dt;
        std::vector<glm::vec2> positions;
        positions.reserve(motions.size());
        for (const world::Motion &motion : motions) {
          positions.push_back(world::positionAt(motion, seconds));
        }
        worldSlot.publish(std::move(positions));
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
