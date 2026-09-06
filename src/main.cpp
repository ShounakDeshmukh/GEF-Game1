#include <engine/engine.hpp>

#include <string>
#include <vector>

namespace {

constexpr int windowWidth = 1280;
constexpr int windowHeight = 720;

constexpr float tileSource = 18.f;   // pixel size of one tile in tilemap_packed.png
constexpr float charSource = 24.f;   // pixel size of one character frame
constexpr float scale = 3.f;
constexpr float tileSize = tileSource * scale;
constexpr float playerSize = charSource * scale;

constexpr std::uint32_t grassTile = 1;

constexpr float gravity = 1800.f;
constexpr float moveSpeed = 260.f;
constexpr float jumpSpeed = 700.f;

} // namespace

int main() {
    engine::log::init();

    engine::Window window("GEF Game1", windowWidth, windowHeight);
    engine::Renderer renderer(window);
    engine::InputHandler input;
    engine::PhysicsSystem physics(gravity);

    const std::string assetDir = GAME_ASSET_DIR "kenney_pixel-platformer/Tilemap/";
    const engine::TextureId tileTexture = renderer.loadTexture(assetDir + "tilemap_packed.png");
    const engine::TextureId charTexture =
        renderer.loadTexture(assetDir + "tilemap-characters_packed.png");

    const engine::SpriteSheetId tileSheet = renderer.createSpriteSheet(
        tileTexture, engine::SpriteSheetLayout::grid({tileSource, tileSource}, 20, 9));
    const engine::SpriteSheetId charSheet = renderer.createSpriteSheet(
        charTexture, engine::SpriteSheetLayout::grid({charSource, charSource}, 9, 3));

    engine::Scene scene;

    const float groundY = windowHeight - tileSize;
    std::vector<engine::EntityId> ground;
    for (float x = 0.f; x < windowWidth; x += tileSize) {
        const engine::EntityId tile = scene.createEntity();
        scene.transform(tile).position = {x, groundY};
        scene.addShape(tile, {.size = {tileSize, tileSize}});
        scene.addSpriteAnimation(
            tile, engine::SpriteAnimation::uniform(tileSheet, {grassTile}, 1.f, false));
        scene.addCollider(tile, {.size = {tileSize, tileSize}});
        ground.push_back(tile);
    }

    const engine::EntityId player = scene.createEntity();
    scene.transform(player).position = {windowWidth / 2.f, 200.f};
    scene.addShape(player, {.size = {playerSize, playerSize}});
    scene.addSpriteAnimation(player, engine::SpriteAnimation::uniform(charSheet, {0, 1}, 0.15f));
    scene.addCollider(player, {.size = {playerSize, playerSize}});
    engine::RigidBody& playerBody = scene.addRigidBody(player);

    engine::Timeline realTime;
    engine::Timeline gameTime(realTime, 60);
    engine::Stepper sim(gameTime);

    bool grounded = false;

    while (!window.shouldClose()) {
        window.pollEvents();

        const bool left = input.isKeyPressed(engine::SC::SDL_SCANCODE_A) ||
                          input.isKeyPressed(engine::SC::SDL_SCANCODE_LEFT);
        const bool right = input.isKeyPressed(engine::SC::SDL_SCANCODE_D) ||
                           input.isKeyPressed(engine::SC::SDL_SCANCODE_RIGHT);
        const bool jump = input.isKeyPressed(engine::SC::SDL_SCANCODE_SPACE);

        sim.beginFrame();
        while (sim.step()) {
            const float stepSeconds = gameTime.tickSeconds();

            playerBody.velocity.x = (right ? moveSpeed : 0.f) - (left ? moveSpeed : 0.f);
            if (jump && grounded) {
                playerBody.velocity.y = -jumpSpeed;
                grounded = false;
            }

            physics.step(scene, stepSeconds);

            grounded = false;
            for (const engine::EntityId tile : ground) {
                if (!physics.isCollision(scene, player, tile)) {
                    continue;
                }
                const engine::Rect overlap = physics.GetCollisionOverlap(scene, player, tile);
                if (overlap.size.y > overlap.size.x) {
                    continue; // resolve the shallower axis only
                }
                engine::Transform& playerTransform = scene.transform(player);
                if (playerTransform.position.y < scene.transform(tile).position.y) {
                    playerTransform.position.y -= overlap.size.y;
                    grounded = true;
                } else {
                    playerTransform.position.y += overlap.size.y;
                }
                playerBody.velocity.y = 0.f;
            }

            engine::advanceAnimations(scene, stepSeconds);
        }

        renderer.clear({92, 148, 252, 255});
        renderer.drawEntities(scene);
        renderer.present();
    }

    return 0;
}
