"""
Conformance test for the OVRAS IPC server (protocol v1).

Every reply and event is validated against docs/ipc/schemas, the same JSON
Schemas referenced by docs/ipc/asyncapi.yaml.

Requirements: OVRAS running with SteamVR, IPC enabled in the Settings tab.
    pip install -r test/ipc/requirements.txt
    python test/ipc/conformance.py [--port 8998]

The test nudges the playspace by a few centimeters / degrees and restores the
original offset at the end (also on failure).
"""
import argparse
import json
import sys
import time
from pathlib import Path

from jsonschema import Draft7Validator
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT7
from websockets.exceptions import InvalidHandshake
from websockets.sync.client import connect

SCHEMA_DIR = Path(__file__).resolve().parents[2] / "docs" / "ipc" / "schemas"
TOLERANCE = 1e-3


def load_registry() -> Registry:
    resources = []
    for path in SCHEMA_DIR.glob("*.schema.json"):
        contents = json.loads(path.read_text(encoding="utf-8"))
        resources.append((contents["$id"], Resource.from_contents(contents, default_specification=DRAFT7)))
    return Registry().with_resources(resources)


REGISTRY = load_registry()


def validator(ref: str) -> Draft7Validator:
    return Draft7Validator({"$ref": ref}, registry=REGISTRY)


SCHEMAS = {
    "response": validator("response.schema.json"),
    "hello": validator("response.schema.json#/definitions/helloResponse"),
    "offset": validator("response.schema.json#/definitions/offsetResponse"),
    "number": validator("response.schema.json#/definitions/numberResponse"),
    "null": validator("response.schema.json#/definitions/nullResponse"),
    "stacks": validator("response.schema.json#/definitions/stackListResponse"),
    "error": validator("response.schema.json#/definitions/errorResponse"),
    "event": validator("event.schema.json"),
    "request": validator("request.schema.json"),
}

failures = []
passed = 0


def check(name: str, condition: bool, detail: str = "") -> None:
    global passed
    if condition:
        passed += 1
        print(f"  ok   {name}")
    else:
        failures.append(name)
        print(f"  FAIL {name} {detail}")


def conforms(kind: str, message: dict) -> bool:
    errors = list(SCHEMAS[kind].iter_errors(message))
    if errors:
        print(f"       schema '{kind}' violations: {[e.message for e in errors]}")
    return not errors


class Client:
    def __init__(self, url: str):
        # Enter the context manager explicitly; newer websockets warns otherwise.
        self._connection = connect(url, open_timeout=5)
        self.ws = self._connection.__enter__()
        self.pending_events = []

    def close(self) -> None:
        self._connection.__exit__(None, None, None)

    def recv_json(self, timeout: float = 2.0) -> dict:
        return json.loads(self.ws.recv(timeout=timeout))

    def request(self, payload, raw: bool = False) -> dict:
        """Send a request and return the next reply, stashing events."""
        self.ws.send(payload if raw else json.dumps(payload))
        while True:
            message = self.recv_json()
            if "event" in message:
                self.pending_events.append(message)
                continue
            return message

    def wait_event(self, timeout: float = 1.0):
        if self.pending_events:
            return self.pending_events.pop(0)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = self.recv_json(timeout=max(0.01, deadline - time.monotonic()))
            except TimeoutError:
                return None
            if "event" in message:
                return message
        return None


def close(a: float, b: float) -> bool:
    return abs(a - b) <= TOLERANCE


def run(url: str) -> None:
    client = Client(url)
    original = None
    try:
        print("hello / get")
        reply = client.request({"id": 1, "op": "hello"})
        check("hello conforms", conforms("hello", reply))
        check("hello echoes id", reply.get("id") == 1)
        features = reply["value"].get("features", [])
        check("hello advertises stacks, stackGravity, hmdFrame",
              {"stacks", "stackGravity", "hmdFrame"} <= set(features), str(features))

        reply = client.request({"id": 2, "op": "get", "path": "/space/offset"})
        check("get offset conforms", conforms("offset", reply))
        original = reply["value"]

        reply = client.request({"op": "get", "path": "/space/offset/rotation"})
        check("get field conforms", conforms("number", reply))
        check("reply without id has no id", "id" not in reply)
        check("field matches whole", close(reply["value"], original["rotation"]))

        print("set / add")
        target_x = original["x"] + 0.05
        reply = client.request({"id": 3, "op": "set", "path": "/space/offset", "value": {"x": target_x}})
        check("set whole conforms", conforms("offset", reply))
        check("set x applied (unless X axis locked)", close(reply["value"]["x"], target_x), str(reply))

        reply = client.request({"id": 4, "op": "add", "path": "/space/offset/y", "value": 0.02})
        check("add field conforms", conforms("offset", reply))
        check("add y applied (unless Y axis locked)", close(reply["value"]["y"], original["y"] + 0.02), str(reply))

        reply = client.request({"id": 5, "op": "add", "path": "/space/offset", "value": {"rotation": 5}})
        check("add rotation conforms", conforms("offset", reply))
        check("rotation applied", close(reply["value"]["rotation"], original["rotation"] + 5), str(reply))

        print("subscribe")
        reply = client.request({"id": 6, "op": "subscribe", "path": "/space/offset"})
        check("subscribe conforms", conforms("offset", reply))
        client.request({"id": 7, "op": "add", "path": "/space/offset", "value": {"rotation": 1}})
        event = client.wait_event()
        check("changed event received", event is not None)
        if event is not None:
            check("event conforms", conforms("event", event))
        idle = client.wait_event(timeout=0.3)
        check("no event while idle", idle is None, str(idle))

        reply = client.request({"id": 8, "op": "unsubscribe", "path": "/space/offset"})
        check("unsubscribe conforms", conforms("null", reply))
        client.request({"op": "add", "path": "/space/offset", "value": {"rotation": -1}})
        check("no event after unsubscribe", client.wait_event(timeout=0.3) is None)

        print("errors")
        error_cases = [
            ("not json", "{nope", True, "bad_json", None),
            ("non-object", "[1]", True, "bad_json", None),
            ("missing op", {"id": 10}, False, "bad_op", 10),
            ("unknown op", {"id": 11, "op": "explode"}, False, "bad_op", 11),
            ("unknown path", {"id": 12, "op": "get", "path": "/space/nope"}, False, "bad_path", 12),
            ("missing path", {"id": 13, "op": "get"}, False, "bad_path", 13),
            ("unexpected field", {"id": 14, "op": "get", "path": "/space/offset", "value": 1}, False, "bad_value", 14),
            ("string value", {"id": 15, "op": "set", "path": "/space/offset/x", "value": "1"}, False, "bad_value", 15),
            ("empty object", {"id": 16, "op": "set", "path": "/space/offset", "value": {}}, False, "bad_value", 16),
            ("unknown field", {"id": 17, "op": "set", "path": "/space/offset", "value": {"w": 1}}, False, "bad_value", 17),
            ("out of range", {"id": 18, "op": "set", "path": "/space/offset", "value": {"x": 1e9}}, False, "bad_value", 18),
            ("object on field path", {"id": 19, "op": "set", "path": "/space/offset/x", "value": {"x": 1}}, False, "bad_value", 19),
            ("subscribe field path", {"id": 20, "op": "subscribe", "path": "/space/offset/x"}, False, "bad_path", 20),
            ("non-integer id", {"id": 1.5, "op": "hello"}, False, "bad_value", None),
        ]
        for name, payload, raw, code, expected_id in error_cases:
            if not raw:
                check(f"{name}: request is invalid per schema", not SCHEMAS["request"].is_valid(payload))
            reply = client.request(payload, raw=raw)
            check(f"{name}: conforms", conforms("error", reply))
            check(f"{name}: code {code}", reply.get("error", {}).get("code") == code, str(reply))
            check(f"{name}: id", reply.get("id") == expected_id if expected_id is not None else "id" not in reply, str(reply))

        before = client.request({"op": "get", "path": "/space/offset"})["value"]
        client.request({"op": "set", "path": "/space/offset", "value": {"x": before["x"] + 1, "y": "bad"}})
        after = client.request({"op": "get", "path": "/space/offset"})["value"]
        check("invalid write is atomic (nothing applied)", after == before, f"{before} -> {after}")

        client.ws.send(b"\x00\x01")
        reply = client.recv_json()
        check("binary frame: bad_json", conforms("error", reply) and reply["error"]["code"] == "bad_json")
    finally:
        if original is not None:
            reply = client.request({"op": "set", "path": "/space/offset", "value": original})
            print("restore")
            check("original offset restored", all(close(reply["value"][k], original[k]) for k in original), str(reply))
        client.close()

    print("security")
    try:
        with connect(url, open_timeout=5, additional_headers={"Origin": "http://evil.example"}):
            pass
        check("browser Origin refused", False, "connection was accepted")
    except InvalidHandshake:
        check("browser Origin refused", True)


PREFIX = "conformance-"  # all test stacks use this prefix and are removed at the end


def offsets_close(a: dict, b: dict) -> bool:
    return all(close(a[k], b[k]) for k in ("x", "y", "z", "rotation"))


def stack_ids(client: Client) -> list:
    reply = client.request({"op": "get", "path": "/space/stacks"})
    return [s["id"] for s in reply["value"]["stacks"]]


def run_stacks(url: str) -> None:
    client = Client(url)
    user_before = None
    try:
        print("stacks: basics")
        user_before = client.request({"op": "get", "path": "/space/offset"})["value"]
        total_before = client.request({"op": "get", "path": "/space/total"})["value"]

        sid = PREFIX + "a"
        reply = client.request({"id": 30, "op": "set", "path": "/space/offset/y", "value": 0.5, "stack": sid})
        check("stack set conforms", conforms("offset", reply))
        check("stack set returns stack offset", offsets_close(reply["value"], {"x": 0, "y": 0.5, "z": 0, "rotation": 0}), str(reply))

        user_now = client.request({"op": "get", "path": "/space/offset"})["value"]
        check("user offset untouched by stack write", offsets_close(user_now, user_before), f"{user_before} -> {user_now}")

        total = client.request({"op": "get", "path": "/space/total"})
        check("total conforms", conforms("offset", total))
        check("total = previous total + stack", close(total["value"]["y"], total_before["y"] + 0.5), str(total))

        reply = client.request({"op": "get", "path": "/space/offset/y", "stack": sid})
        check("get stack field", conforms("number", reply) and close(reply["value"], 0.5), str(reply))

        reply = client.request({"op": "get", "path": "/space/total/y"})
        check("get total field", conforms("number", reply) and close(reply["value"], total_before["y"] + 0.5), str(reply))

        reply = client.request({"op": "get", "path": "/space/offset", "stack": PREFIX + "missing"})
        check("missing stack reads as zero", conforms("offset", reply) and offsets_close(reply["value"], {"x": 0, "y": 0, "z": 0, "rotation": 0}), str(reply))

        reply = client.request({"op": "add", "path": "/space/offset", "value": {"rotation": 30, "x": 0.1}, "stack": sid, "pivot": "origin"})
        check("stack add with origin pivot", offsets_close(reply["value"], {"x": 0.1, "y": 0.5, "z": 0, "rotation": 30}), str(reply))

        reply = client.request({"op": "add", "path": "/space/offset/rotation", "value": 10, "stack": sid, "pivot": "hmd"})
        check("stack hmd-pivot rotation applied", close(reply["value"]["rotation"], 40), str(reply))

        reply = client.request({"id": 31, "op": "get", "path": "/space/stacks"})
        check("stack list conforms", conforms("stacks", reply))
        entry = next((s for s in reply["value"]["stacks"] if s["id"] == sid), None)
        check("stack listed as non-persistent", entry is not None and entry["persist"] is False, str(entry))
        check("stack list max is 32", reply["value"]["max"] == 32)

        print("stacks: subscriptions")
        reply = client.request({"op": "subscribe", "path": "/space/stacks"})
        check("subscribe stacks conforms", conforms("stacks", reply))
        reply = client.request({"op": "subscribe", "path": "/space/total"})
        check("subscribe total conforms", conforms("offset", reply))
        client.request({"op": "add", "path": "/space/offset/z", "value": 0.05, "stack": sid})
        events = []
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline and len({e["path"] for e in events}) < 2:
            event = client.wait_event(timeout=max(0.05, deadline - time.monotonic()))
            if event is None:
                break
            events.append(event)
        paths = {e["path"] for e in events}
        check("stacks + total events received", paths == {"/space/stacks", "/space/total"}, str(paths))
        check("events conform", all(conforms("event", e) for e in events))
        client.request({"op": "unsubscribe", "path": "/space/stacks"})
        client.request({"op": "unsubscribe", "path": "/space/total"})
        while client.wait_event(timeout=0.1) is not None:
            pass

        print("stacks: user reset keeps stacks")
        reply = client.request({"id": 32, "op": "reset", "path": "/space/offset"})
        check("user reset conforms", conforms("null", reply))
        user_after_reset = client.request({"op": "get", "path": "/space/offset"})["value"]
        check("user reset zeroed user offset", offsets_close(user_after_reset, {"x": 0, "y": 0, "z": 0, "rotation": 0}), str(user_after_reset))
        check("user reset kept stack", sid in stack_ids(client))

        print("stacks: reset")
        reply = client.request({"id": 33, "op": "reset", "path": "/space/offset", "stack": sid})
        check("stack reset conforms", conforms("null", reply))
        check("stack removed", sid not in stack_ids(client))
        reply = client.request({"op": "reset", "path": "/space/offset", "stack": PREFIX + "missing"})
        check("resetting a missing stack is ok", conforms("null", reply))

        print("stacks: lifetime")
        persistent, transient = PREFIX + "persist", PREFIX + "transient"
        other = Client(url)
        other.request({"op": "set", "path": "/space/offset/x", "value": 0.01, "stack": persistent, "persist": True})
        other.request({"op": "set", "path": "/space/offset/x", "value": 0.01, "stack": transient})
        check("both stacks exist", {persistent, transient} <= set(stack_ids(client)))
        other.close()
        time.sleep(0.3)
        ids = stack_ids(client)
        check("non-persistent stack removed on disconnect", transient not in ids, str(ids))
        check("persistent stack survives disconnect", persistent in ids, str(ids))
        reply = client.request({"op": "set", "path": "/space/offset/x", "value": 0.02, "stack": persistent})
        entry = next(s for s in client.request({"op": "get", "path": "/space/stacks"})["value"]["stacks"] if s["id"] == persistent)
        check("omitting persist keeps the setting", entry["persist"] is True, str(entry))

        print("stacks: limit")
        existing = len(stack_ids(client))
        created = []
        for i in range(32 - existing):
            name = f"{PREFIX}fill{i}"
            reply = client.request({"op": "set", "path": "/space/offset/x", "value": 0, "stack": name})
            if reply.get("ok"):
                created.append(name)
        check("filled up to 32 stacks", len(stack_ids(client)) == 32)
        reply = client.request({"id": 34, "op": "set", "path": "/space/offset/x", "value": 0, "stack": PREFIX + "overflow"})
        check("33rd stack: conforms", conforms("error", reply))
        check("33rd stack: stack_limit", reply.get("error", {}).get("code") == "stack_limit", str(reply))
        reply = client.request({"op": "set", "path": "/space/offset/x", "value": 0.03, "stack": created[0] if created else persistent})
        check("existing stack still writable at limit", reply.get("ok") is True, str(reply))
        for name in created:
            client.request({"op": "reset", "path": "/space/offset", "stack": name})

        print("stacks: errors")
        error_cases = [
            ("bad stack id", {"id": 40, "op": "set", "path": "/space/offset/x", "value": 0, "stack": "has space"}, "bad_value"),
            ("empty stack id", {"id": 41, "op": "get", "path": "/space/offset", "stack": ""}, "bad_value"),
            ("persist without stack", {"id": 42, "op": "set", "path": "/space/offset/x", "value": 0, "persist": True}, "bad_value"),
            ("persist not bool", {"id": 43, "op": "set", "path": "/space/offset/x", "value": 0, "stack": PREFIX + "a", "persist": "yes"}, "bad_value"),
            ("bad pivot", {"id": 44, "op": "set", "path": "/space/offset/rotation", "value": 0, "stack": PREFIX + "a", "pivot": "hand"}, "bad_value"),
            ("write to total", {"id": 45, "op": "set", "path": "/space/total/x", "value": 0}, "bad_path"),
            ("write to stacks", {"id": 46, "op": "add", "path": "/space/stacks", "value": 0}, "bad_path"),
            ("reset field path", {"id": 47, "op": "reset", "path": "/space/offset/x"}, "bad_path"),
            ("stack on subscribe", {"id": 48, "op": "subscribe", "path": "/space/offset", "stack": PREFIX + "a"}, "bad_value"),
            ("stack on total get", {"id": 49, "op": "get", "path": "/space/total", "stack": PREFIX + "a"}, "bad_value"),
            ("gravity without stack", {"id": 50, "op": "add", "path": "/space/offset/y", "value": 0, "gravity": True}, "bad_value"),
            ("floor without stack", {"id": 51, "op": "add", "path": "/space/offset/y", "value": 0, "floor": 0}, "bad_value"),
            ("gravity not bool", {"id": 52, "op": "add", "path": "/space/offset/y", "value": 0, "stack": PREFIX + "a", "gravity": 1}, "bad_value"),
            ("floor out of range", {"id": 53, "op": "add", "path": "/space/offset/y", "value": 0, "stack": PREFIX + "a", "floor": 1e9}, "bad_value"),
            ("bad frame", {"id": 54, "op": "add", "path": "/space/offset/x", "value": 0, "stack": PREFIX + "a", "frame": "hand"}, "bad_value"),
        ]
        for name, payload, code in error_cases:
            check(f"{name}: request is invalid per schema", not SCHEMAS["request"].is_valid(payload))
            reply = client.request(payload)
            check(f"{name}: conforms", conforms("error", reply))
            check(f"{name}: code {code}", reply.get("error", {}).get("code") == code, str(reply))
        reply = client.request({"id": 55, "op": "set", "path": "/space/offset/x", "value": 0, "stack": PREFIX + "a", "frame": "hmd"})
        check("frame on set: bad_value", conforms("error", reply) and reply["error"]["code"] == "bad_value", str(reply))
        check("failed writes created no stack", PREFIX + "a" not in stack_ids(client))

        print("stacks: gravity")
        gid = PREFIX + "gravity"
        reply = client.request({"op": "set", "path": "/space/offset", "value": {"x": 0.3, "y": -1.0, "z": 0.2}, "stack": gid})
        entry = next(s for s in client.request({"op": "get", "path": "/space/stacks"})["value"]["stacks"] if s["id"] == gid)
        check("new stack: gravity off, floor 0", entry["gravity"] is False and entry["floor"] == 0, str(entry))
        time.sleep(0.3)
        held = client.request({"op": "get", "path": "/space/offset", "stack": gid})["value"]
        check("no gravity: stack stays lifted", close(held["y"], -1.0), str(held))

        client.request({"op": "add", "path": "/space/offset/y", "value": 0, "stack": gid, "gravity": True})
        time.sleep(0.15)
        mid = client.request({"op": "get", "path": "/space/offset", "stack": gid})["value"]
        check("gravity: falling (y rising toward floor)", -1.0 < mid["y"] < 0, str(mid))
        time.sleep(0.6)  # 1 m at 9.8 m/s^2 takes ~0.45 s
        landed = client.request({"op": "get", "path": "/space/offset", "stack": gid})["value"]
        check("gravity: landed exactly on floor 0", landed["y"] == 0, str(landed))
        check("gravity: x/z unchanged", close(landed["x"], 0.3) and close(landed["z"], 0.2), str(landed))
        entry = next(s for s in client.request({"op": "get", "path": "/space/stacks"})["value"]["stacks"] if s["id"] == gid)
        check("stack list shows gravity on", entry["gravity"] is True, str(entry))

        client.request({"op": "set", "path": "/space/offset/y", "value": -0.5, "stack": gid, "floor": -0.2})
        time.sleep(0.6)
        landed = client.request({"op": "get", "path": "/space/offset", "stack": gid})["value"]
        check("custom floor -0.2: lands on it", close(landed["y"], -0.2), str(landed))

        client.request({"op": "set", "path": "/space/offset/y", "value": 0.4, "stack": gid})
        time.sleep(0.2)
        below = client.request({"op": "get", "path": "/space/offset", "stack": gid})["value"]
        check("below floor: gravity never raises the user", close(below["y"], 0.4), str(below))

        client.request({"op": "set", "path": "/space/offset/y", "value": -1.0, "stack": gid, "gravity": False})
        time.sleep(0.3)
        held = client.request({"op": "get", "path": "/space/offset", "stack": gid})["value"]
        check("gravity off again: holds", close(held["y"], -1.0), str(held))

        user_y = client.request({"op": "get", "path": "/space/offset/y"})["value"]
        other = PREFIX + "nogravity"
        client.request({"op": "set", "path": "/space/offset/y", "value": -0.7, "stack": other})
        client.request({"op": "add", "path": "/space/offset/y", "value": 0, "stack": gid, "gravity": True})
        time.sleep(0.8)
        other_y = client.request({"op": "get", "path": "/space/offset/y", "stack": other})["value"]
        check("gravity only affects stacks that enable it", close(other_y, -0.7), str(other_y))
        check("gravity doesn't touch the user offset",
              close(client.request({"op": "get", "path": "/space/offset/y"})["value"], user_y))

        print("stacks: hmd frame")
        fid = PREFIX + "frame"
        reply = client.request({"op": "add", "path": "/space/offset", "value": {"x": 0.3, "y": -0.1, "z": 0.4}, "stack": fid, "frame": "hmd"})
        v = reply["value"]
        check("hmd frame: add conforms", conforms("offset", reply))
        check("hmd frame: length preserved", close((v["x"] ** 2 + v["z"] ** 2) ** 0.5, 0.5), str(v))
        check("hmd frame: y unchanged by heading", close(v["y"], -0.1), str(v))
        reply = client.request({"op": "add", "path": "/space/offset/x", "value": 0.0, "stack": fid, "frame": "base"})
        check("frame base accepted", reply.get("ok") is True, str(reply))
    finally:
        for name in stack_ids(client):
            if name.startswith(PREFIX):
                client.request({"op": "reset", "path": "/space/offset", "stack": name})
        if user_before is not None:
            client.request({"op": "set", "path": "/space/offset", "value": user_before})
        print("stacks: cleanup")
        check("test stacks removed", not any(n.startswith(PREFIX) for n in stack_ids(client)))
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8998)
    args = parser.parse_args()
    url = f"ws://127.0.0.1:{args.port}/"
    print(f"Testing {url}")
    try:
        run(url)
        run_stacks(url)
    except (OSError, InvalidHandshake) as error:
        print(f"Could not talk to OVRAS at {url}: {error}")
        print("Is OVRAS running with the IPC server enabled in Settings?")
        return 2
    print(f"\n{passed} passed, {len(failures)} failed")
    for name in failures:
        print(f"  - {name}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
