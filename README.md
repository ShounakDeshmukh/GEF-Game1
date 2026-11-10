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

- Ghost blocks look like grass but can't be stood on. Holes are at least 2 tiles wide, since the
  player's hitbox is wider than one tile. Two fake floating platforms bait the jump.
- A staircase from the start and the vertical moving platform lead up to a sky bridge across the
  top: a shortcut with a ghost gap, a saw, a pitfall and a hidden spike. Falling off isn't fatal,
  it just drops you back to the lower route.
- Pitfalls are real grass that drops away 0.3 s after anyone lands on it, and comes back 3 s later.
  The server times them, so every window loses the same floor at the same moment.
- Spikes are hidden until you step on them, then spring up and kill you. They hide again when you
  respawn, so the next run has to remember where they are.
- Saws are always visible. Step into the hidden zone in front of one and it rushes across the zone
  at you, waits a second, then rolls home slowly enough to jump over. One guards the ground route
  past the third spawn point, one the high route along the wooden ledge.
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
| Grass | `#` | Transform, Shape (tiled texture), Collider; one entity per run | yes |
| Wooden ledge | `=` | Transform, Shape (tiled texture), Collider | yes |
| Stone block | `S` | Transform, Shape (tiled texture), Collider | yes |
| Moving platform | `H`, `V` | Transform, Shape, Collider; server-replicated | yes |
| Bee | `B` | Transform, Shape, Collider, SpriteAnimation; server-replicated | yes |
| Spawn point | `P` | Transform, `SpawnPoint` | no |
| Death zone | `X` | Transform, Collider, `DeathZone` | no |
| Ghost block | `g` | Transform, Shape (tiled texture); one entity per run | yes |
| Pitfall | `f` | Transform, Shape (texture), Collider, `Pitfall` | until it drops |
| Spikes | `^` | Transform, Collider, `Spikes`; Shape once sprung | once sprung |
| Saw | `s` | Transform, Shape, Collider, SpriteAnimation, `Saw` | yes |
| Saw trigger | `t` | Transform, Collider, `SawTrigger` (works the nearest saw in its row) | no |
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
| `collisionEvent` | client, for its player against the bee, death zones, spikes, saws, saw triggers, the flag, and pitfalls it lands on | client: death (springing hidden spikes), a saw rush, a `goalReached` claim, or a `pitfalls::touched` claim | local |
| `sawReturn` | client, a second after a saw finishes its rush | client: sends the saw rolling home | local |
| `pitfalls::touched` | client collision handler | server: starts that pitfall's drop unless it is already dropping | client -> server |
| `pitfalls::changed` | server, 0.3 s after the first touch, then 3 s later | server: broadcasts it; client: hides the tile, then restores it | server -> all clients |
| `deathEvent` | client collision handler, or a fall out of the level | client: death cam; server: relays it | client -> server -> all clients |
| `spawnEvent` | client, after the death cam or a round reset | client: places the player, hides sprung spikes, resumes recording | local |
| `goalReached` | client collision handler | server: the first claim of a round wins | client -> server |
| `roundWon` | server | client: scoreboard, shows the restart countdown | server -> all clients |
| `race::countdown` | server, each second after a win | server: broadcasts it, reopens the round at 0; client: updates the countdown, resets to the start at 0 | server -> all clients |
| `replayRecordStart`, `replayPlayStart`, `replayPlayStop` | client death and spawn handlers | engine `replayController` | local |

The server accepts a `deathEvent` only for the sender's own player, and takes the `goalReached`
claimant from the session rather than the payload, so a client can't kill or crown another player.
In client-server mode it also drops a flag claim unless the claimant's last replicated hitbox is
within a tile of the flag. Peers' positions never reach the server, so their claims are trusted.
Pause, speed and scaling keys stay on the main thread: a paused sim doesn't tick, so it couldn't
handle the event that unpauses it.
