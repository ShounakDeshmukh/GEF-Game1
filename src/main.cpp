#include <engine/engine.hpp>

int main() {
  engine::log::init();
  engine::Window window("mygame", 1280, 720);
  engine::Renderer renderer(window);

  while (!window.shouldClose()) {
    window.pollEvents();
    renderer.clear({30, 30, 40, 255});
    renderer.present();
  }
  return 0;
}