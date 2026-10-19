Art: [Kenney Platformer Pack](https://kenney.nl/assets/new-platformer-pack)(CC0).

```sh
cmake --preset debug        # or: release
cmake --build --preset debug
```

## Launch modes

Start the headless server first, then any number of clients or peers, each in its own terminal.
They can join at any time.

```sh
./build/debug/game --server [port]        # headless, drives the moving platforms and the bee
./build/debug/game --client [host:port]   # client-server: player state goes through the server
./build/debug/game --p2p [host:port]      # hybrid: player state goes directly between peers
```

`port` defaults to `5555` and `host:port` to `localhost:5555`, e.g. `--client 192.168.1.20:5555`.
The server logs joins and leaves, and each client's updates/s once a second.
Each window's top-left corner shows its player id, mode, speed or `PAUSED`, and the camera's x,
plus `DISCONNECTED FROM SERVER` if the server stops replying.

## Controls

- Move: `A`/`D` or the left/right arrow keys
- Jump: `Space`, `W`, or the up arrow key
- Pause / unpause: `P`
- Speed 0.5x / 1x / 2x: `1` / `2` / `3`
- Toggle scaling mode: `F`

## Level objects

The level is two screens wide (`src/level.hpp`). The camera scrolls when the player touches one of
the hidden scroll boundaries near the edges of the view. Falling into a pit respawns the player at
the last spawn point passed.

| Object | Map char | Components | Visible |
|---|---|---|---|
| Grass tile | `#` | Transform, Shape (texture), Collider | yes |
| Wooden ledge | `=` | Transform, Shape (color), Collider | yes |
| Stone block | `S` | Transform, Shape (color), Collider | yes |
| Moving platform | `H`, `V` | Transform, Shape, Collider; server-replicated | yes |
| Bee | `B` | Transform, Shape, Collider, SpriteAnimation; server-replicated | yes |
| Spawn point | `P` | Transform, `SpawnPoint` | no |
| Death zone | `X` | Transform, Collider, `DeathZone` | no |
| Scroll boundary | none, 2 per client | Transform, Collider, `ScrollBoundary` | no |
| Camera | none, 1 per client | Transform, `Camera` | no |

Hidden objects are entities with no Shape, so the renderer never draws them. `SpawnPoint`,
`DeathZone`, `ScrollBoundary` and `Camera` are game-defined components in `src/components.hpp`;
the engine stores them without knowing their types. None of them are replicated: every client
builds the same level locally and scrolls its own view.
