#pragma once

#include <string>

constexpr int defaultPort = 5555;

// Headless authority over the moving platforms and the bee; relays players in
// client-server mode.
int runServer(int port);

// Player state goes through the server.
int runClient(const std::string &serverEndpoint);

// Player state goes directly to the other peers; the server still drives the
// platforms.
int runPeer(const std::string &serverEndpoint);
