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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8998)
    args = parser.parse_args()
    url = f"ws://127.0.0.1:{args.port}/"
    print(f"Testing {url}")
    try:
        run(url)
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
