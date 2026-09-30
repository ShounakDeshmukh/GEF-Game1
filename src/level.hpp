#pragma once

#include <array>

namespace level {

constexpr int columns = 32;
constexpr int rows = 18;

constexpr char empty = '.';
constexpr char solid = '#';
constexpr char playerSpawn = 'P';
constexpr char beeSpawn = 'B';
constexpr char goal = 'F';
// A run of these is one server-driven moving platform.
constexpr char horizontalMover = 'H';
constexpr char verticalMover = 'V';

// One char per 60px cell, so the map is the 1920x1080 frame at 1:1.
// Ground has two pits; the platforms above it are an optional high route, and
// the H platform sweeps back and forth over both pits.
constexpr std::array<const char *, rows> map = {
    "................................", //  0
    "................................", //  1
    "................................", //  2
    "................................", //  3
    "................................", //  4
    "................................", //  5
    "..........###...................", //  6
    "..................VVV...........", //  7
    "................................", //  8
    "...###.................###......", //  9
    ".........###....................", // 10
    ".......................B........", // 11
    "................................", // 12
    "..............HHH...............", // 13
    "....###.................###.....", // 14
    "..P..........................F..", // 15
    "#########...######...###########", // 16
    "#########...######...###########", // 17
};

} // namespace level
