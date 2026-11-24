"""
Piccolo Agent Harness - Phase 0
Provides Python control over a running Piccolo engine via file-based IPC.
"""

import json
import os
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
_REPO_ROOT   = Path(__file__).resolve().parent.parent
_AGENT_DIR   = _REPO_ROOT / "engine" / "asset" / "agent"

RELOAD_SIGNAL_PATH  = _AGENT_DIR / "reload_signal"
INPUT_COMMAND_PATH  = _AGENT_DIR / "input_command.json"
STATE_PATH          = _AGENT_DIR / "state.json"

# ---------------------------------------------------------------------------
# GameCommand bit masks (must stay in sync with input_system.h)
# ---------------------------------------------------------------------------
class GameCommand:
    FORWARD     = 1 << 0   # W
    BACKWARD    = 1 << 1   # S
    LEFT        = 1 << 2   # A
    RIGHT       = 1 << 3   # D
    JUMP        = 1 << 4   # SPACE
    SQUAT       = 1 << 5
    SPRINT      = 1 << 6   # LEFT SHIFT
    FIRE        = 1 << 7
    FREE_CAMERA = 1 << 8   # F
    INVALID     = 1 << 31


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def reload_level() -> None:
    """Write the sentinel file that tells the engine to reload the current level."""
    _AGENT_DIR.mkdir(parents=True, exist_ok=True)
    RELOAD_SIGNAL_PATH.touch()


def set_input(command_mask: int, duration_frames: int = -1) -> None:
    """
    Inject a game-command bitmask into the engine.

    Parameters
    ----------
    command_mask : int
        OR-combination of GameCommand bit values.
    duration_frames : int
        -1  -> hold until the next set_input() call
        > 0 -> auto-clear after N engine ticks
    """
    _AGENT_DIR.mkdir(parents=True, exist_ok=True)
    payload = {"command_mask": command_mask, "duration_frames": duration_frames}
    tmp = INPUT_COMMAND_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(payload))
    tmp.rename(INPUT_COMMAND_PATH)   # atomic on most platforms


def get_state() -> dict:
    """
    Read the latest state snapshot written by the engine.

    Returns an empty dict if no state file exists yet.
    Example return value::

        {
            "tick": 1234,
            "objects": [
                {"name": "Hero", "position": {"x": 1.0, "y": 0.0, "z": 2.5}}
            ]
        }
    """
    if not STATE_PATH.exists():
        return {}
    try:
        return json.loads(STATE_PATH.read_text())
    except (json.JSONDecodeError, OSError):
        return {}


# ---------------------------------------------------------------------------
# Demo
# ---------------------------------------------------------------------------

def demo_move_forward(n_frames: int = 60, frame_duration: float = 1 / 60) -> None:
    """
    Send a 'move forward' command for *n_frames* ticks, then stop.

    With duration_frames the engine auto-clears the command, so we just need
    to send it once and wait for it to complete.

    Parameters
    ----------
    n_frames : int
        How many engine ticks the character should move forward.
    frame_duration : float
        Approximate real-time duration of one engine tick (seconds).
        Used only for the progress-print delay; does not need to be exact.
    """
    print(f"[harness] Sending FORWARD command for {n_frames} frames …")
    before = get_state()
    if before:
        objects = before.get("objects", [])
        for obj in objects:
            pos = obj.get("position", {})
            print(f"  Before: {obj['name']} @ {pos}")

    set_input(GameCommand.FORWARD, duration_frames=n_frames)

    # Wait for the command to complete, then read state
    wait_secs = n_frames * frame_duration + 0.5   # extra 0.5 s margin
    print(f"[harness] Waiting {wait_secs:.1f} s …")
    time.sleep(wait_secs)

    after = get_state()
    if after:
        objects = after.get("objects", [])
        for obj in objects:
            pos = obj.get("position", {})
            print(f"  After:  {obj['name']} @ {pos}")
    else:
        print("[harness] No state file found — is the engine running?")

    print("[harness] Done.")


if __name__ == "__main__":
    demo_move_forward()
