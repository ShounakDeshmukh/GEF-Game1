# GEF Game1

A 2D platformer built on the [GEF/Deadlock engine](https://github.com/ShounakDeshmukh/GEF_engine)
(C++20 + SDL3), pinned at tag `v0.1.0` via CMake `FetchContent`.

Art: Kenney Pixel Platformer pack in `assets/kenney_pixel-platformer/` (CC0, see its License.txt).

## Requirements

CMake >= 3.28, Ninja, ccache, a C++20 GCC or Clang, plus SDL3's build-time
dependencies (X11/Wayland, ALSA/PulseAudio headers) since SDL3 is compiled from source.

## Build and run

```sh
cmake --preset debug        # or: release
cmake --build --preset debug
./build/debug/game
```

The first configure downloads and builds SDL3, SDL3_image, SDL3_ttf, spdlog, glm and fmt.

## Controls

- A / D or Left / Right: move
- Space: jump

## Layout

- `src/main.cpp` - game entry point and main loop
- `assets/` - art and tilesets, exposed to the build as `GAME_ASSET_DIR`
- `build/` - build trees, one per preset (gitignored)
