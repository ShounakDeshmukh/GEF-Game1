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
./build/debug/game --client [endpoint]    # client-server: player state goes through the server
./build/debug/game --p2p [endpoint]       # hybrid: player state goes directly between peers
```

`port` defaults to `5555` and `endpoint` to `tcp://localhost:5555`.
The server logs joins and leaves, and each client's updates/s once a second.

## Controls

- Move: `A`/`D` or the left/right arrow keys
- Jump: `Space`, `W`, or the up arrow key
- Pause / unpause: `P`
- Speed 0.5x / 1x / 2x: `1` / `2` / `3`
- Toggle scaling mode: `F`
