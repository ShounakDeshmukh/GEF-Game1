#include "modes.hpp"

#include <engine/log.hpp>

#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "usage: game --server [port] | --client [host:port] | --p2p [host:port]\n"
    "  port defaults to 5555, host:port to localhost:5555";

} // namespace

int main(int argc, char *argv[]) {
  engine::log::init();
  if (argc < 2 || argc > 3) {
    engine::log::error("{}", usage);
    return 1;
  }
  const std::string_view mode = argv[1];
  // ZeroMQ needs a transport prefix; accept a bare host:port too.
  const std::string address =
      argc == 3 ? argv[2] : "localhost:" + std::to_string(defaultPort);
  const std::string endpoint =
      address.find("://") == std::string::npos ? "tcp://" + address : address;
  try {
    if (mode == "--server") {
      return runServer(argc == 3 ? std::stoi(argv[2]) : defaultPort);
    }
    if (mode == "--client") {
      return runClient(endpoint);
    }
    if (mode == "--p2p") {
      return runPeer(endpoint);
    }
  } catch (const std::exception &error) {
    engine::log::error("{}", error.what());
    return 1;
  }
  engine::log::error("{}", usage);
  return 1;
}
