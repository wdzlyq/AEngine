"""
agent_planner.py — LLM-driven planning loop for the Piccolo AI-native engine.

The planner uses Claude to generate a sequence of engine tool calls that
satisfy a natural-language game goal.  An Executor sends the commands to the
running engine via PiccoloClient, with snapshot/restore on failure.  A
Verifier confirms that the resulting game is actually playable.

Quickstart (requires ANTHROPIC_API_KEY and a running PiccoloEditor)::

    python3 scripts/agent_planner.py

Or as a library::

    from scripts.agent_planner import run_demo
    run_demo("create a minimal third-person game with a player, floor and two walls")
"""

from __future__ import annotations

import json
import os
import sys
import textwrap
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import anthropic

from scripts.piccolo_client import PiccoloClient, InputCommand
from scripts.verifier import VerificationSuite

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
_REPO_ROOT    = Path(__file__).resolve().parent.parent
_CATALOG_PATH = _REPO_ROOT / "docs" / "ai-native-research" / "asset_catalog.json"

# ---------------------------------------------------------------------------
# LLM model
# ---------------------------------------------------------------------------
_MODEL = "claude-sonnet-5-5"

# ---------------------------------------------------------------------------
# Tool definitions exposed to the LLM
# (These mirror what the C++ MCP server supports, plus save_level.)
# ---------------------------------------------------------------------------
_TOOLS = [
    {
        "name": "get_state",
        "description": "Return all GObjects in the current scene with their world positions.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "create_object",
        "description": (
            "Create a new GObject from an asset definition file. "
            "Use the asset catalog to find the correct definition path. "
            "If position is omitted the object is placed at the origin."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "name":       {"type": "string", "description": "Unique display name"},
                "definition": {"type": "string",
                               "description": "Asset path, e.g. asset/objects/character/player/player.object.json"},
                "position":   {
                    "type": "object",
                    "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"}}
                },
            },
            "required": ["name", "definition"],
        },
    },
    {
        "name": "set_field",
        "description": "Set a component field on an existing GObject.",
        "input_schema": {
            "type": "object",
            "properties": {
                "object_name":    {"type": "string"},
                "component_type": {"type": "string", "description": "e.g. MotorComponent"},
                "field_name":     {"type": "string"},
                "value":          {},
            },
            "required": ["object_name", "component_type", "field_name", "value"],
        },
    },
    {
        "name": "snapshot",
        "description": "Save (action='save') or restore (action='restore') a named scene snapshot.",
        "input_schema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["save", "restore"]},
                "tag":    {"type": "string"},
            },
            "required": ["action"],
        },
    },
    {
        "name": "save_level",
        "description": "Persist the current in-memory scene to disk so it survives engine restarts.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "set_input",
        "description": (
            "Inject a game input bitmask. "
            "Bits: 1=FORWARD 2=BACKWARD 4=LEFT 8=RIGHT 16=JUMP 64=SPRINT. "
            "duration_frames=-1 means hold indefinitely."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "command_mask":    {"type": "integer"},
                "duration_frames": {"type": "integer"},
            },
            "required": ["command_mask"],
        },
    },
    {
        "name": "step",
        "description": "Advance the engine exactly n_frames logic ticks and return the new state.",
        "input_schema": {
            "type": "object",
            "properties": {"n_frames": {"type": "integer"}},
        },
    },
]


# ---------------------------------------------------------------------------
# Executor — sends commands to the engine
# ---------------------------------------------------------------------------

@dataclass
class ExecutionLog:
    tool_name: str
    arguments: dict
    success: bool
    payload: Any
    error: str = ""

    def __str__(self) -> str:
        status = "ok" if self.success else "ERR"
        return f"[{status}] {self.tool_name}({json.dumps(self.arguments)[:80]})"


class Executor:
    """
    Sends LLM-generated tool calls to the Piccolo engine.
    Before each destructive sequence it saves a snapshot; on failure it
    rolls back automatically.
    """

    def __init__(self, client: PiccoloClient):
        self.client = client
        self.log: list[ExecutionLog] = []

    def execute(self, tool_name: str, arguments: dict) -> tuple[bool, Any, str]:
        """
        Run one tool call.  Returns (success, payload, error_message).
        """
        client = self.client
        result = None

        try:
            if tool_name == "get_state":
                result = client.get_state()
            elif tool_name == "create_object":
                result = client.create_object(
                    name=arguments["name"],
                    definition=arguments["definition"],
                    position=arguments.get("position"),
                )
            elif tool_name == "set_field":
                result = client.set_field(
                    object_name=arguments["object_name"],
                    component_type=arguments["component_type"],
                    field_name=arguments["field_name"],
                    value=arguments["value"],
                )
            elif tool_name == "snapshot":
                result = client.snapshot(
                    action=arguments["action"],
                    tag=arguments.get("tag", "default"),
                )
            elif tool_name == "save_level":
                result = client.save_level()
            elif tool_name == "set_input":
                result = client.set_input(
                    command_mask=arguments["command_mask"],
                    duration_frames=arguments.get("duration_frames", -1),
                )
            elif tool_name == "step":
                result = client.step(n_frames=arguments.get("n_frames", 1))
            else:
                entry = ExecutionLog(tool_name, arguments, False, None,
                                     f"Unknown tool: {tool_name}")
                self.log.append(entry)
                return False, None, entry.error

        except Exception as exc:
            entry = ExecutionLog(tool_name, arguments, False, None, str(exc))
            self.log.append(entry)
            return False, None, str(exc)

        entry = ExecutionLog(
            tool_name, arguments,
            result.success, result.payload, result.error
        )
        self.log.append(entry)
        return result.success, result.payload, result.error

    def print_log(self) -> None:
        for entry in self.log:
            print(f"  {entry}")


# ---------------------------------------------------------------------------
# Planner — drives the LLM conversation
# ---------------------------------------------------------------------------

class Planner:
    """
    Uses Claude to generate a plan for a game-creation goal, then drives the
    Executor to carry it out.

    The LLM is given:
      - The asset catalog (all available prefabs and their fields)
      - The current scene state
      - The list of available tools

    It iterates until it declares the task done or a maximum turn limit is
    reached.
    """

    MAX_TURNS = 20

    def __init__(self, client: PiccoloClient, api_key: str | None = None):
        self.client   = client
        self.executor = Executor(client)
        self.anthropic = anthropic.Anthropic(api_key=api_key or os.environ.get("ANTHROPIC_API_KEY"))
        self.catalog  = self._load_catalog()

    @staticmethod
    def _load_catalog() -> dict:
        if _CATALOG_PATH.exists():
            return json.loads(_CATALOG_PATH.read_text())
        return {}

    def _system_prompt(self) -> str:
        catalog_str = json.dumps(self.catalog, indent=2, ensure_ascii=False)
        return textwrap.dedent(f"""\
            You are an AI game-creation agent operating the Piccolo engine.
            Your job is to build a minimal but playable third-person game by
            issuing tool calls.

            ## Asset Catalog
            All available prefabs and their fields are listed below.  Only use
            definition paths that appear in this catalog — do not invent new ones.

            {catalog_str}

            ## Rules
            1. Always call get_state first to understand what is already in the scene.
            2. Before creating or deleting objects, save a snapshot (action='save').
               If a tool call fails, restore the snapshot and try a different approach.
            3. The scene needs at minimum:
               - One player (definition: asset/objects/character/player/player.object.json)
               - One floor  (definition: asset/objects/environment/floor/floor.object.json)
               - At least two walls for collision testing
            4. When you are satisfied the scene is ready, call save_level.
            5. Do not loop forever: if you cannot achieve the goal in {self.MAX_TURNS} turns,
               stop and report what is missing.
            6. Reply with ONLY tool calls until the scene is ready.  When done, reply
               with a plain text summary starting with "DONE:" describing what you built.
        """)

    def run(self, goal: str, verbose: bool = True) -> tuple[bool, str]:
        """
        Drive the LLM until it completes the goal or hits MAX_TURNS.

        Returns (success, summary_message).
        """
        messages: list[dict] = [{"role": "user", "content": goal}]

        if verbose:
            print(f"\n[Planner] Goal: {goal}")
            print("[Planner] Starting planning loop…\n")

        for turn in range(self.MAX_TURNS):
            if verbose:
                print(f"[Planner] Turn {turn + 1}/{self.MAX_TURNS}")

            response = self.anthropic.messages.create(
                model=_MODEL,
                max_tokens=4096,
                system=self._system_prompt(),
                tools=_TOOLS,
                messages=messages,
            )

            # Accumulate the assistant turn
            assistant_content = response.content
            messages.append({"role": "assistant", "content": assistant_content})

            # Check stop reason
            if response.stop_reason == "end_turn":
                # Extract text reply
                for block in assistant_content:
                    if hasattr(block, "text"):
                        text = block.text.strip()
                        if verbose:
                            print(f"[Planner] LLM reply: {text[:200]}")
                        if text.upper().startswith("DONE:"):
                            return True, text
                        # LLM finished without DONE: treat as completion
                        return True, text
                return True, "Planning completed"

            if response.stop_reason != "tool_use":
                return False, f"Unexpected stop_reason: {response.stop_reason}"

            # Execute tool calls
            tool_results = []
            for block in assistant_content:
                if block.type != "tool_use":
                    continue

                tool_name  = block.name
                arguments  = block.input or {}
                tool_id    = block.id

                if verbose:
                    print(f"  → {tool_name}({json.dumps(arguments)[:100]})")

                success, payload, error = self.executor.execute(tool_name, arguments)

                if success:
                    result_text = json.dumps(payload) if payload is not None else "ok"
                else:
                    result_text = f"ERROR: {error}"
                    if verbose:
                        print(f"    ✗ {error}")

                tool_results.append({
                    "type":        "tool_result",
                    "tool_use_id": tool_id,
                    "content":     result_text,
                })

            messages.append({"role": "user", "content": tool_results})

        return False, f"Reached {self.MAX_TURNS}-turn limit without completing goal"


# ---------------------------------------------------------------------------
# End-to-end demo
# ---------------------------------------------------------------------------

def run_demo(
    goal: str = (
        "Create a minimal playable third-person game: "
        "a player character, a large floor, and at least two wall segments "
        "to serve as obstacles.  The player must be able to move and jump."
    ),
    skip_verification: bool = False,
    verbose: bool = True,
) -> bool:
    """
    Full end-to-end demo:
      1. Wait for the engine to become reachable.
      2. Run the LLM planner to build the scene.
      3. Run the verification suite to confirm gameplay.

    Returns True if the scene was built and all verifications passed.
    """
    # --- connect ---
    client = PiccoloClient()
    if verbose:
        print("[demo] Waiting for engine on localhost:7777 …")
    if not client.wait_for_engine(retries=20, delay=1.5):
        print("[demo] Engine not reachable — make sure PiccoloEditor is running.")
        return False
    if verbose:
        print("[demo] Engine connected.\n")

    # --- plan & build ---
    planner = Planner(client)
    success, summary = planner.run(goal, verbose=verbose)

    if verbose:
        print(f"\n[demo] Planning {'succeeded' if success else 'failed'}:")
        print(f"       {summary}\n")
        print("[demo] Executor log:")
        planner.executor.print_log()

    if not success:
        return False

    # --- verify ---
    if skip_verification:
        return True

    if verbose:
        print("\n[demo] Running verification suite …")

    suite  = VerificationSuite(client)
    report = suite.run_all(verbose=verbose)
    VerificationSuite.print_report(report)

    return report.all_passed


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Piccolo AI-native agent: build and verify a third-person game."
    )
    parser.add_argument(
        "--goal", default=None,
        help="Natural-language game-creation goal (uses built-in default if omitted)."
    )
    parser.add_argument(
        "--skip-verify", action="store_true",
        help="Skip the verification suite (useful for quick iteration)."
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="Suppress verbose output."
    )
    args = parser.parse_args()

    ok = run_demo(
        goal=args.goal or run_demo.__defaults__[0],
        skip_verification=args.skip_verify,
        verbose=not args.quiet,
    )
    sys.exit(0 if ok else 1)
