#include "level.hpp"

#include <fmt/format.h>

#include <fstream>
#include <stdexcept>

namespace level {

Map load(const std::string &path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error(fmt::format("can't open the level {}", path));
  }
  Map map;
  for (std::string line; std::getline(file, line);) {
    if (!map.empty() && line.size() != map.front().size()) {
      throw std::runtime_error(
          fmt::format("{}: row {} is {} wide, the first row is {}", path,
                      map.size(), line.size(), map.front().size()));
    }
    map.push_back(std::move(line));
  }
  if (map.empty() || map.front().empty()) {
    throw std::runtime_error(fmt::format("{}: the level is empty", path));
  }
  return map;
}

} // namespace level
