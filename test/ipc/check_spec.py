"""
Offline consistency check for docs/ipc: every message example in asyncapi.yaml
must validate against the JSON Schema its payload references, and every
payload $ref must resolve. Does not need OVRAS or SteamVR.

    python test/ipc/check_spec.py
"""
import json
import sys
from pathlib import Path

import yaml
from jsonschema import Draft7Validator
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT7

DOCS = Path(__file__).resolve().parents[2] / "docs" / "ipc"


def main() -> int:
    resources = []
    for path in (DOCS / "schemas").glob("*.schema.json"):
        contents = json.loads(path.read_text(encoding="utf-8"))
        Draft7Validator.check_schema(contents)
        resources.append((contents["$id"], Resource.from_contents(contents, default_specification=DRAFT7)))
    registry = Registry().with_resources(resources)

    spec = yaml.safe_load((DOCS / "asyncapi.yaml").read_text(encoding="utf-8"))
    failures = 0
    checked = 0
    for name, message in spec["components"]["messages"].items():
        ref = message["payload"]["$ref"].removeprefix("./schemas/")
        validator = Draft7Validator({"$ref": ref}, registry=registry)
        examples = message.get("examples", [])
        if not examples:
            print(f"FAIL {name}: no examples")
            failures += 1
        for example in examples:
            checked += 1
            errors = [e.message for e in validator.iter_errors(example["payload"])]
            if errors:
                failures += 1
                print(f"FAIL {name}: {example['payload']} -> {errors}")
            else:
                print(f"ok   {name}: {json.dumps(example['payload'])}")

    # Every message listed on the channel must exist in components.
    for name, entry in spec["channels"]["root"]["messages"].items():
        target = entry["$ref"].split("/")[-1]
        if target not in spec["components"]["messages"]:
            failures += 1
            print(f"FAIL channel message {name} -> missing component {target}")

    print(f"\n{checked} examples checked, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
