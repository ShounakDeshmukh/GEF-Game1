#pragma once

#include <string>
#include <vector>

namespace level {

constexpr char empty = '.';
constexpr char solid = '#';
constexpr char playerSpawn = 'P';
constexpr char beeSpawn = 'B';
constexpr char goal = 'F';
constexpr char ledge = '='; // thin wooden ledge, one entity per run
constexpr char stone = 'S'; // full-height stone block, one entity per run
constexpr char deathZone = 'X'; // hidden, one entity per run
constexpr char ghost = 'g';     // looks like grass, has no collider
constexpr char pitfall = 'f';   // grass that drops away when stood on
constexpr char spikes = '^';    // rise and sink on a rhythm, one entity per run
// A run of these is one server-driven moving platform.
constexpr char horizontalMover = 'H';
constexpr char verticalMover = 'V';

// One char per 60px cell, one string per row; every row has the same width.
using Map = std::vector<std::string>;

// Every process loads the same file, so their maps (and the order of the moving
// objects in them) agree.
inline const std::string path = GAME_ASSET_DIR "game/level.txt";

// Throws std::runtime_error if the file can't be read, is empty, or has rows of
// different widths.
Map load(const std::string &path);

} // namespace level
