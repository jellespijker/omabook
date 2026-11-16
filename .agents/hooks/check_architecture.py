#!/usr/bin/env python3
"""The layers of src/ (.agents/architecture.toml): no include may point up or sideways.

Checks, for the code as a change leaves it:
  - every module in src/ is placed in a layer;
  - a module includes only its own layer and the layers in its `uses`;
  - Qt Quick and QML headers are included only where the window lives.

Includes that broke the rules before this check existed are listed in
.agents/architecture-baseline.json. They may disappear, never be added. When
one disappears the check asks for the baseline to be updated (`--update`), so
the removed include can't come back unnoticed.

  --report   print the layer of every module and every violation, baselined or not
  --update   rewrite the baseline to what is in the tree now (only ever shrinks it)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

from harness import CPP_SUFFIXES, ROOT, Scope, report

CONFIG = ROOT / ".agents/architecture.toml"
BASELINE = ROOT / ".agents/architecture-baseline.json"
SOURCES = "src"
LOCAL_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)\.h"', re.MULTILINE)
SYSTEM_INCLUDE = re.compile(r"^\s*#\s*include\s+<(\w+)", re.MULTILINE)


@dataclass(frozen=True)
class Layers:
    layer_of: dict[str, str]
    uses: dict[str, frozenset[str]]
    window_only: tuple[str, ...]
    window_layers: frozenset[str]

    @classmethod
    def load(cls) -> Layers:
        config = tomllib.loads(CONFIG.read_text(encoding="utf-8"))
        layer_of: dict[str, str] = {}
        for name, layer in config["layers"].items():
            for module in layer["modules"]:
                if module in layer_of:
                    raise SystemExit(f"check_architecture: {module} is in both {layer_of[module]} and {name}")
                layer_of[module] = name
        uses = {name: frozenset([name, *layer["uses"]]) for name, layer in config["layers"].items()}
        qt = config.get("qt", {})
        return cls(layer_of, uses, tuple(qt.get("window_only", ())), frozenset(qt.get("allowed_in", ())))

    def violations(self, path: str, text: str) -> list[str]:
        module = Path(path).stem
        layer = self.layer_of.get(module)
        if layer is None:
            return [f"{path}: unplaced -> add `{module}` to a layer in .agents/architecture.toml"]
        found = []
        for target in sorted(set(LOCAL_INCLUDE.findall(text)) - {module}):
            target_layer = self.layer_of.get(Path(target).name)
            if target_layer and target_layer not in self.uses[layer]:
                found.append(f"{path}: {layer} includes {target}.h ({target_layer})")
        if layer not in self.window_layers:
            for header in sorted(set(SYSTEM_INCLUDE.findall(text))):
                if header.startswith(self.window_only):
                    found.append(f"{path}: {layer} includes <{header}>, which only the window may")
        return found


def sources() -> list[str]:
    return sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / SOURCES).iterdir() if p.suffix in CPP_SUFFIXES)


def current(layers: Layers, scope: Scope | None) -> set[str]:
    changed = set(scope.files(CPP_SUFFIXES)) if scope else set()
    found: set[str] = set()
    for path in sources():
        text = scope.after(path) if path in changed else (ROOT / path).read_text(encoding="utf-8", errors="replace")
        found.update(layers.violations(path, text))
    if scope:  # files the change adds to src/ that are not on disk yet (staged only)
        for path in changed - set(sources()):
            if path.startswith(f"{SOURCES}/") and path.count("/") == 1:
                found.update(layers.violations(path, scope.after(path)))
    return found


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--report", action="store_true")
    mode.add_argument("--update", action="store_true")
    known, rest = parser.parse_known_args(argv)
    layers = Layers.load()
    baseline = set(json.loads(BASELINE.read_text(encoding="utf-8"))["violations"]) if BASELINE.exists() else set()

    if known.report or known.update:
        found = current(layers, None)
        if known.update:
            kept = sorted(found & baseline) if BASELINE.exists() else sorted(found)
            BASELINE.write_text(json.dumps({"comment": BASELINE_COMMENT, "violations": kept}, indent=2) + "\n")
            removed = len(baseline) - len(kept) if baseline else 0
            print(f"check_architecture: baseline has {len(kept)} entries ({removed} removed)")
            return 0
        for module, layer in sorted(layers.layer_of.items(), key=lambda item: (item[1], item[0])):
            print(f"{layer:12} {module}")
        for violation in sorted(found):
            print(("baselined  " if violation in baseline else "NEW        ") + violation)
        return 0

    scope_parser = argparse.ArgumentParser()
    Scope.add_arguments(scope_parser)
    scope = Scope.from_arguments(scope_parser.parse_args(rest))
    found = current(layers, scope)
    failures = sorted(found - baseline)
    gone = sorted(baseline - found)
    status = report("architecture", failures, "see .agents/rules/10-architecture.md: move the code down a layer, or invert the dependency")
    if gone and scope.mode == "changed":
        print(f"architecture: {len(gone)} baselined include(s) fixed; run --update before committing.", file=sys.stderr)
    elif gone:
        status |= report(
            "architecture (baseline is stale)",
            [f"no longer true: {entry}" for entry in gone],
            "good: run `python3 .agents/hooks/check_architecture.py --update` and commit the smaller baseline",
        )
    return status


BASELINE_COMMENT = (
    "Includes that broke the layers in .agents/architecture.toml when the check was introduced. "
    "Remove entries by fixing them (then run --update); never add one by hand."
)

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
