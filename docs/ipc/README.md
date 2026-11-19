# OVRAS IPC (protocol v1, spec 1.2)

Other programs can read and change the **playspace offset** (the position and rotation that Space Drag and Space Turn produce, shown in the Offsets tab) while OpenVR Advanced Settings is running. Tools that move the user (leashes, grab/throw) should use **[offset stacks](#offset-stacks)**, so they don't fight the user's own Space Drag.

- **Contract:** [`asyncapi.yaml`](asyncapi.yaml) (AsyncAPI 3.0) with payload schemas in [`schemas/`](schemas). If this README and the spec disagree, the spec wins.
- **Transport:** WebSocket at `ws://127.0.0.1:8998/`, with JSON in text frames.
- **Enable it:** OVRAS → Settings → **IPC Server**. The server is off by default. The port can be changed there, and the status text shows errors such as the port already being in use.

## Quick start (Python)

```python
import json
from websockets.sync.client import connect

with connect("ws://127.0.0.1:8998/") as ws:
    ws.send(json.dumps({"id": 1, "op": "get", "path": "/space/offset"}))
    print(ws.recv())  # {"id":1,"ok":true,"value":{"x":0,"y":0,"z":0,"rotation":0}}

    ws.send(json.dumps({"id": 2, "op": "add", "path": "/space/offset", "value": {"y": 0.1}}))
    print(ws.recv())  # moves the playspace offset 10 cm along Y; reply has the new offset

    ws.send(json.dumps({"id": 3, "op": "subscribe", "path": "/space/offset"}))
    while True:
        print(ws.recv())  # reply with a snapshot, then {"event":"changed",...} while you drag
```

## Operations

| op | path | value | reply `value` |
|---|---|---|---|
| `hello` | – | – | `{name:"OVRAS", protocol:1, version, features:["stacks","stackGravity","hmdFrame"]}` |
| `get` | `/space/offset[/x\|y\|z\|rotation]` (optional `stack`), `/space/total[/field]`, `/space/stacks` | – | full offset, a single number, or the stack list |
| `set` | `/space/offset` or `/space/offset/{field}` (optional `stack`, `persist`, `gravity`, `floor`, `pivot`) | object with any of `x,y,z,rotation` (whole path) or a number (field path) | full offset of the target after the change |
| `add` | same, plus optional `frame` | same, but relative | full offset of the target after the change |
| `reset` | `/space/offset` (optional `stack`) | – | `null` |
| `subscribe` / `unsubscribe` | `/space/offset`, `/space/total` or `/space/stacks` | – | snapshot / `null` |

- **`id`** is optional and must be an integer. If you send one, it is echoed back in the reply. The server sends exactly one reply per request, in the order the requests arrived.
- **Errors** have the form `{"ok":false,"error":{"code","message"}}`. `code` is one of `bad_json`, `bad_op`, `bad_path`, `bad_value`, `stack_limit` or `internal`. A request that fails validation changes nothing, even if only one of its fields was bad.
- **Events** have the form `{"event":"changed","path":"...","value":{...}}`. They are only sent to clients subscribed to that path, for changes from any source: the UI, drag, turn, profiles or other clients. At most one event is sent per path per ~16 ms, carrying the latest state, and nothing is sent while the value isn't changing.

## Offset stacks

If your tool writes the user's own offset, it fights the user: their Space Drag and your writes change the same numbers, and the user's Reset undoes your work. Instead, give your tool its own **stack** by adding `"stack": "<name>"` to a request:

```jsonc
{"op":"set","path":"/space/offset/y","value":-1.0,"stack":"vrcleash"}
```

OVRAS places the playspace at:

```
total = user offset + Σ all stacks
```

Translations and rotations are simply added together, so the order of stacks doesn't matter and several tools can run at once. The Offsets tab still shows only the user's own part. `/space/total` gives what is actually applied.

**Rules**

- **Names** are your choice: 1–64 characters out of `A–Z a–z 0–9 _ . -`. Any client may read or write any stack, so prefix the name with your tool's name. A stack is created by its first `set`/`add`. Reading a stack that doesn't exist gives zeros.
- **Limit:** at most **32** stacks exist at once. A write that would create a 33rd fails with `stack_limit`, but existing stacks can still be written.
- **Lifetime:** by default a stack belongs to the connection that last wrote it and is **removed when that connection closes**, so if your tool crashes the user snaps back instead of staying stuck. Send `"persist": true` on any write to keep it until it's reset. Leaving `persist` out keeps the stack's current setting. `persist` without `stack` is an error.
- **Resets:**

  | What happens | User offset | Stacks |
  |---|---|---|
  | User presses Reset / the Reset Offsets binding | cleared | **kept** |
  | `{"op":"reset","path":"/space/offset"}` | cleared | **kept** |
  | `{"op":"reset","path":"/space/offset","stack":"vrcleash"}` | kept | that stack removed |
  | "Reset" / "Reset All Stacks" on the Offset Stacks page | kept | that stack / all removed |
  | OVRAS safety resets (out of bounds, room setup, leaving seated mode) | cleared | **all removed** |
  | Connection closes | kept | its non-persistent stacks removed |
  | OVRAS restarts | – | all gone (stacks aren't saved to disk) |

- **Axis locks** in the Offsets tab only limit the user's own offset. Stacks can move on any axis.
- **Rotation pivot:** add `"pivot": "hmd"` to turn around the headset (the stack's `x`/`z` are adjusted so the headset stays put) or `"pivot": "origin"` to turn around the playspace origin (only the rotation changes). Without `pivot`, the rotation follows the user's *Universe-Centered Rotation* setting: headset when it's off, origin when it's on. `pivot` works the same on the user offset.
- **Gravity:** send `"gravity": true` to let a stack fall. OVRAS then raises the stack's `y` (remember: positive `y` = lower) at the gravity strength from the OVRAS Motion settings (9.8 m/s² by default) until it reaches the stack's **floor**, lands exactly on it and stops. `"floor"` defaults to `0`, the height the user was at before the stack lifted them. OVRAS can't see the VRChat world, so it never finds real ground by itself. Gravity only pulls down: a stack already at or below its floor stays put.
  - It only affects stacks that turn it on. The user's own offset and the OVRAS gravity toggle are separate.
  - Any write to a stack resets its fall speed, so every drop starts from rest.
  - Turn it **off while your tool controls the height** (while carrying) and **on to drop**. Leaving `gravity`/`floor` out keeps the current values; new stacks start with gravity off and floor 0.
  - It runs every OVRAS frame, also while the dashboard is open.
- **Head-relative moves:** `add` accepts `"frame": "hmd"`. The `x`/`y`/`z` you send are then relative to the headset's current heading (yaw only): `x` = to the headset's right, `y` = up, `z` = backwards. OVRAS turns them into playspace offsets. The sign convention stays the same (you move the world), so to move the user forward, send `z` = `+d`. `frame` only works with `add`; `"frame": "base"` is the default.
- **Interaction with Space Drag:** OVRAS works out the user's drag and turn from the total, so your tool moving a stack is not read as hand movement, and the user can keep dragging while being moved.

**UI:** Space Offsets → **Offset Stacks (N)** lists every stack with its values, lifetime and gravity state (Off / On / Falling), and has a Reset button per stack plus *Reset All Stacks*.

### Example: carry and drop

```python
import json, time
from websockets.sync.client import connect

STACK = "mytool.carry"

with connect("ws://127.0.0.1:8998/") as ws:
    def call(req):
        ws.send(json.dumps(req))
        return json.loads(ws.recv())

    features = call({"op": "hello"})["value"]["features"]
    assert {"stacks", "stackGravity", "hmdFrame"} <= set(features)

    # Picked up: gravity off while we control the height.
    call({"op": "add", "path": "/space/offset/y", "value": 0,
          "stack": STACK, "gravity": False})

    # Carried: each frame, move toward the grabbing hand. Here: 1 m up and
    # 0.5 m forward over half a second, relative to where the user is facing.
    # Remember the convention: you move the world, so "user up" is y < 0
    # and "user forward" is z > 0 in the hmd frame.
    for _ in range(45):
        call({"op": "add", "path": "/space/offset", "stack": STACK,
              "frame": "hmd", "value": {"x": 0, "y": -1 / 45, "z": 0.5 / 45}})
        time.sleep(1 / 90)

    # Dropped: OVRAS lets the user fall back to the floor (y = 0) and they
    # stay where they were carried horizontally.
    call({"op": "add", "path": "/space/offset/y", "value": 0,
          "stack": STACK, "gravity": True})
    time.sleep(1)

    # Keep the connection open as long as the user should stay displaced:
    # closing it removes the non-persistent stack (the user snaps back).
```

To watch everything that's applied, subscribe to `/space/total`. To watch which tools are active, subscribe to `/space/stacks`, whose value is `{"max":32,"stacks":[{"id","persist","gravity","floor","offset":{x,y,z,rotation}}]}`.

## Semantics

- **Sign convention:** offsets move the **world** relative to the user, exactly like the OVRAS Offsets tab. `+x` moves the user toward `-x`, and so on. In particular, **a negative `y` lifts the user up**.
- **Units:** meters for `x/y/z`, degrees for `rotation`. Rotation is stored internally in 0.01° steps. Writes are limited to ±10 000 m and ±1 000 000°.
- **Rotation pivot:** like the UI, changing `rotation` turns the playspace **around the headset**, which also shifts `x`/`z`. The exception is when *Universe-Centered Rotation* is enabled in Settings. In a combined `set`, rotation is applied first, so the `x`/`z` values you send are what you end up with.
- **Axis locks:** the X/Y/Z axis locks set in OVRAS also block IPC writes to the locked axis of the *user* offset (not to stacks). The reply shows the actual resulting offset.
- **Concurrency:** IPC writes to the user offset and the user's Space Drag both add to the same offset, so both take effect. The latest write in each frame wins. To avoid that, use a [stack](#offset-stacks).
- **Timing:** writes change OVRAS's state immediately and are applied to SteamVR on the next frame, the same as edits in the UI.

## Security

- The server listens only on `127.0.0.1` and can't be reached from other machines.
- Handshakes that include an `Origin` header are refused. Browsers always send one, so web pages can't move your playspace. Native clients (Python, C#, Node and so on) don't send it.
- There's no authentication beyond that: any local program can connect while IPC is enabled.

## Testing

```powershell
python -m pip install -r test/ipc/requirements.txt
python test/ipc/check_spec.py        # offline: spec examples match schemas
python test/ipc/conformance.py       # live: needs OVRAS + SteamVR with IPC enabled
```

To validate the AsyncAPI document itself: `npx @asyncapi/cli validate docs/ipc/asyncapi.yaml`.
