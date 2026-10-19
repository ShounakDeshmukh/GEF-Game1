#pragma once

// Game-defined components. The engine stores them through add<T>/get<T>/each<T>
// without knowing their types. Entities with these but no Shape are never
// drawn: they are the level's hidden objects.

// A start and respawn location: the player's top-left on spawn.
struct SpawnPoint {};

// Touching one sends the player back to a spawn point.
struct DeathZone {};
