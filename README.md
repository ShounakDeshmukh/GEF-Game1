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
plus `DISCONNECTED FROM SERVER` if the server stops replying. Below it are the scoreboard (wins
and deaths per player) and a feed of the latest deaths and race wins; each feed line disappears
after 4 s.

## The race

The first player to touch the flag wins the round. Every window shows a 3-second countdown, then
every player goes back to the start. Dying (the bee, spikes, or falling into a pit) replays the last 2 s in slow motion under a
`REPLAY` banner, then respawns the player at the last spawn point passed.

The course is built to troll first-time runners, and every trap repeats the same way each run, so
players learn the patterns by dying:

- Ghost blocks look like grass but can't be stood on.
- Pitfalls are real grass that drops away 0.3 s after you land on it, and comes back 3 s later.
- Spikes are hidden until you step on them, then spring up and kill you. They hide again when you
  respawn, so the next run has to remember where they are.
- The bee dashes along its path at 3x speed for 1 s out of every 5 (driven by the server).

## Controls

- Move: `A`/`D` or the left/right arrow keys
- Jump: `Space`, `W`, or the up arrow key
- Sprint (chord): left `Shift` with a move key, in either order
- Pause / unpause: `P`
- Speed 0.5x / 1x / 2x: `1` / `2` / `3`
- Toggle scaling mode: `F`

## Level objects

The level is two screens wide (`assets/game/level.txt`, one char per 60px cell; the chars are in `src/level.hpp`). The camera scrolls when the player touches one of
the hidden scroll boundaries near the edges of the view.

| Object | Map char | Components | Visible |
|---|---|---|---|
| Grass tile | `#` | Transform, Shape (texture), Collider | yes |
| Wooden ledge | `=` | Transform, Shape (tiled texture), Collider | yes |
| Stone block | `S` | Transform, Shape (tiled texture), Collider | yes |
| Moving platform | `H`, `V` | Transform, Shape, Collider; server-replicated | yes |
| Bee | `B` | Transform, Shape, Collider, SpriteAnimation; server-replicated | yes |
| Spawn point | `P` | Transform, `SpawnPoint` | no |
| Death zone | `X` | Transform, Collider, `DeathZone` | no |
| Ghost block | `g` | Transform, Shape (texture) | yes |
| Pitfall | `f` | Transform, Shape (texture), Collider, `Pitfall` | until it drops |
| Spikes | `^` | Transform, Collider, `Spikes`; Shape once sprung | once sprung |
| Scroll boundary | none, 2 per client | Transform, Collider, `ScrollBoundary` | no |
| Camera | none, 1 per client | Transform, `Camera` | no |

Hidden objects are entities with no Shape, so the renderer never draws them. `SpawnPoint`,
`DeathZone`, `ScrollBoundary` and `Camera` are game-defined components in `src/components.hpp`;
the engine stores them without knowing their types. None of them are replicated: every client
builds the same level locally and scrolls its own view.

## Events

Gameplay runs on the engine's `eventManager` (engine v0.4.0), on the sim thread.

| Event | Raised by | Handled by | Network |
|---|---|---|---|
| `inputEvent`, `inputChordEvent` | `inputMapper`, from each tick's keyboard | client: move, jump, sprint | local |
| `collisionEvent` | client, for its player against the bee, death zones, spikes, the flag, and pitfalls it lands on | client: death (springing hidden spikes), a `goalReached` claim, or a `pitfallDrop` | local |
| `pitfallDrop`, `pitfallReturn` | client, after landing on a pitfall | client: hides the tile, then restores it | local |
| `deathEvent` | client collision handler, or a fall out of the level | client: death cam; server: relays it | client -> server -> all clients |
| `spawnEvent` | client, after the death cam or a round reset | client: places the player, hides sprung spikes, resumes recording | local |
| `goalReached` | client collision handler | server: the first claim of a round wins | client -> server |
| `roundWon` | server | client: scoreboard, restart countdown, reset to the start | server -> all clients |
| `replayRecordStart`, `replayPlayStart`, `replayPlayStop` | client death and spawn handlers | engine `replayController` | local |

The server accepts a `deathEvent` only for the sender's own player, and takes the `goalReached`
claimant from the session rather than the payload, so a client can't kill or crown another player.
Pause, speed and scaling keys stay on the main thread: a paused sim doesn't tick, so it couldn't
handle the event that unpauses it.
