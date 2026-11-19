# OVRAS IPC (protocol v1)

Other programs can read and change the **playspace offset** (the position and rotation that Space Drag and Space Turn produce, shown in the Offsets tab) while OpenVR Advanced Settings is running.

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
| `hello` | – | – | `{name:"OVRAS", protocol:1, version}` |
| `get` | `/space/offset` or `/space/offset/{x,y,z,rotation}` | – | full offset or a single number |
| `set` | same | object with any of `x,y,z,rotation` (whole path) or a number (field path) | full offset after the change |
| `add` | same | same, but relative | full offset after the change |
| `subscribe` / `unsubscribe` | `/space/offset` | – | snapshot / `null` |

- **`id`** is optional and must be an integer. If you send one, it is echoed back in the reply. The server sends exactly one reply per request, in the order the requests arrived.
- **Errors** have the form `{"ok":false,"error":{"code","message"}}`. `code` is one of `bad_json`, `bad_op`, `bad_path`, `bad_value` or `internal`. A request that fails validation changes nothing, even if only one of its fields was bad.
- **Events** have the form `{"event":"changed","path":"/space/offset","value":{...}}`. They are only sent to subscribed clients and are sent for changes from any source: the UI, drag, turn, profiles or other clients. At most one event is sent per ~16 ms, carrying the latest state, and nothing is sent while the offset isn't changing.

## Semantics

- **Units:** meters for `x/y/z`, degrees for `rotation`. Rotation is stored internally in 0.01° steps. Writes are limited to ±10 000 m and ±1 000 000°.
- **Rotation pivot:** like the UI, changing `rotation` turns the playspace **around the headset**, which also shifts `x`/`z`. The exception is when *Universe-Centered Rotation* is enabled in Settings. In a combined `set`, rotation is applied first, so the `x`/`z` values you send are what you end up with.
- **Axis locks:** the X/Y/Z axis locks set in OVRAS also block IPC writes to the locked axis. The reply shows the actual resulting offset.
- **Concurrency:** IPC writes and the user's Space Drag both add to the same offset, so both take effect. The latest write in each frame wins.

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
