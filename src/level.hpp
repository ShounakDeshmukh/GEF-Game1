#pragma once

#include <array>
#include <string>

namespace level {

constexpr int columns = 64;
constexpr int rows = 18;

constexpr char empty = '.';
constexpr char solid = '#';
constexpr char playerSpawn = 'P';
constexpr char beeSpawn = 'B';
constexpr char goal = 'F';
constexpr char ledge = '='; // thin wooden ledge, one entity per run
constexpr char stone = 'S'; // full-height stone block, one entity per run
constexpr char deathZone = 'X'; // hidden, one entity per run
// A run of these is one server-driven moving platform.
constexpr char horizontalMover = 'H';
constexpr char verticalMover = 'V';

// One char per 60px cell. The level is two 1920px screens wide; the camera
// scrolls between them.
constexpr std::array<const char *, rows> map = {
    "................................................................", //  0
    "................................................................", //  1
    "................................................................", //  2
    "................................................................", //  3
    "................................................................", //  4
    "................................................................", //  5
    "..........###...................................................", //  6
    "..................VVV...........................................", //  7
    "....................................................VVV.........", //  8
    "...###.................###....................====..............", //  9
    ".........###....................................................", // 10
    ".......................B............SSS.........................", // 11
    "........................................................SSSS....", // 12
    "..............HHH.............======....HHH.....................", // 13
    "....###.................###......................===............", // 14
    "..P....P..........................P..........................F..", // 15
    "#########...######...###################....###########...######", // 16
    "#########XXX######XXX###################XXXX###########XXX######", // 17
};

static_assert(
    [] {
      for (const char *line : map) {
        if (std::char_traits<char>::length(line) != columns) {
          return false;
        }
      }
      return true;
    }(),
    "every map row must be `columns` wide");

} // namespace level
