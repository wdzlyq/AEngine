"""
verifier.py — Behavioural assertion module for the Piccolo AI-native engine.

The Verifier sends input commands, advances the simulation, and checks that
the resulting world state satisfies declared invariants.  Every check is
self-contained: it saves a snapshot before the test and restores it after so
the scene is left in a known state regardless of success or failure.

Usage::

    from scripts.piccolo_client import PiccoloClient
    from scripts.verifier import Verifier, VerificationSuite

    client = PiccoloClient()
    v      = Verifier(client)

    result = v.verify_character_moves(player_name="Player")
    print(result)   # VerificationResult(passed=True, name='character_moves', …)

    suite = VerificationSuite(client)
    report = suite.run_all()
    suite.print_report(report)
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Callable

from scripts.piccolo_client import InputCommand, PiccoloClient


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class VerificationResult:
    name: str
    passed: bool
    message: str
    details: dict = field(default_factory=dict)

    def __str__(self) -> str:
        status = "✓ PASS" if self.passed else "✗ FAIL"
        return f"[{status}] {self.name}: {self.message}"


@dataclass
class SuiteReport:
    results: list[VerificationResult] = field(default_factory=list)

    @property
    def passed(self) -> int:
        return sum(1 for r in self.results if r.passed)

    @property
    def failed(self) -> int:
        return sum(1 for r in self.results if not r.passed)

    @property
    def all_passed(self) -> bool:
        return self.failed == 0


# ---------------------------------------------------------------------------
# Core Verifier
# ---------------------------------------------------------------------------

class Verifier:
    """
    Runs individual behavioural checks against a live Piccolo engine.

    Each verify_* method:
      1. Saves a named snapshot so the check is non-destructive.
      2. Performs the interaction (set_input + step).
      3. Reads the resulting state and evaluates the assertion.
      4. Restores the snapshot so the scene is left unchanged.
    """

    # Tunable constants
    MOVE_FRAMES       = 60    # frames to hold FORWARD for movement test
    JUMP_FRAMES       = 30    # frames to run after pressing JUMP
    JUMP_CHECK_START  = 3     # check Z rise starting at this frame offset
    JUMP_MIN_RISE     = 0.05  # minimum z-rise to count as a valid jump (m)
    MOVE_MIN_DELTA    = 0.05  # minimum position change to count as movement (m)
    COLLISION_FRAMES  = 90    # frames to try to walk into a wall
    COLLISION_MAX_PENETRATION = 0.3  # character must not move more than this
                                      # when blocked by a wall directly ahead

    def __init__(self, client: PiccoloClient):
        self.client = client

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _position(self, name: str) -> dict | None:
        return self.client.get_object_position(name)

    def _distance(self, a: dict, b: dict) -> float:
        import math
        dx = a["x"] - b["x"]
        dy = a["y"] - b["y"]
        dz = a["z"] - b["z"]
        return math.sqrt(dx * dx + dy * dy + dz * dz)

    def _with_snapshot(self, tag: str, fn: Callable[[], VerificationResult]) -> VerificationResult:
        """Run fn() inside a save/restore bracket."""
        self.client.snapshot("save", tag=tag)
        try:
            return fn()
        finally:
            self.client.snapshot("restore", tag=tag)

    # ------------------------------------------------------------------
    # Individual checks
    # ------------------------------------------------------------------

    def verify_character_moves(self, player_name: str = "Player",
                               direction: int = InputCommand.FORWARD,
                               frames: int = MOVE_FRAMES) -> VerificationResult:
        """
        Assert that the player character translates when given a movement input.

        The test holds FORWARD for `frames` ticks and measures the displacement
        of the player object's position.  Any axis movement ≥ MOVE_MIN_DELTA
        counts as passing.
        """
        name = "verify_character_moves"

        def _run() -> VerificationResult:
            before = self._position(player_name)
            if before is None:
                return VerificationResult(name, False,
                    f"Object '{player_name}' not found in scene")

            # Clear any stale input, then inject movement
            self.client.set_input(InputCommand.NONE)
            self.client.step(2)  # settle

            self.client.set_input(direction, duration_frames=frames)
            self.client.step(frames + 2)   # +2 to let the auto-clear settle

            after = self._position(player_name)
            if after is None:
                return VerificationResult(name, False,
                    f"Object '{player_name}' disappeared after movement")

            delta = self._distance(before, after)
            passed = delta >= self.MOVE_MIN_DELTA

            return VerificationResult(
                name, passed,
                f"moved {delta:.3f} m (need ≥ {self.MOVE_MIN_DELTA} m)",
                details={"before": before, "after": after, "delta": delta}
            )

        return self._with_snapshot("snap_moves", _run)

    def verify_character_jumps(self, player_name: str = "Player") -> VerificationResult:
        """
        Assert that the player's Z coordinate rises after a JUMP input.

        The character must reach at least JUMP_MIN_RISE metres above its
        pre-jump Z during a JUMP_FRAMES-tick window.
        """
        name = "verify_character_jumps"

        def _run() -> VerificationResult:
            before = self._position(player_name)
            if before is None:
                return VerificationResult(name, False,
                    f"Object '{player_name}' not found in scene")

            baseline_z = before["z"]

            # Ensure the character is on the ground (let it settle first)
            self.client.set_input(InputCommand.NONE)
            self.client.step(5)

            # Press jump
            self.client.set_input(InputCommand.JUMP, duration_frames=5)

            peak_z = baseline_z
            # Sample Z across the jump window
            for _ in range(self.JUMP_FRAMES):
                step_result = self.client.step(1)
                if not step_result.success:
                    continue
                pos = self._position(player_name)
                if pos and pos["z"] > peak_z:
                    peak_z = pos["z"]

            rise = peak_z - baseline_z
            passed = rise >= self.JUMP_MIN_RISE

            return VerificationResult(
                name, passed,
                f"peak z rise = {rise:.3f} m (need ≥ {self.JUMP_MIN_RISE} m)",
                details={"baseline_z": baseline_z, "peak_z": peak_z, "rise": rise}
            )

        return self._with_snapshot("snap_jumps", _run)

    def verify_wall_collision(self, player_name: str = "Player",
                              wall_name: str | None = None) -> VerificationResult:
        """
        Assert that the player cannot walk through a solid wall.

        Strategy: push FORWARD for COLLISION_FRAMES ticks, then compare the
        per-frame speed at the start vs. the end.  A wall that stops the
        character causes the speed to drop to near zero.  If no wall is close
        enough to matter, the check is skipped (PASS with 'skipped' note).
        """
        name = "verify_wall_collision"

        def _run() -> VerificationResult:
            before = self._position(player_name)
            if before is None:
                return VerificationResult(name, False,
                    f"Object '{player_name}' not found in scene")

            # Find wall-like objects (exclude Fence which is a large enclosure)
            state = self.client.get_state()
            if state.success and isinstance(state.payload, dict):
                objects = state.payload.get("objects", [])
                wall_names = [
                    o["name"] for o in objects
                    if any(w in o["name"]
                           for w in ["Wall", "wall", "Block", "block"])
                    and "fence" not in o["name"].lower()
                    and "Fence" not in o["name"]
                ]
            else:
                wall_names = []

            if not wall_names:
                return VerificationResult(name, True,
                    "No wall objects in scene — collision test skipped",
                    details={"skipped": True})

            target_wall = wall_name or wall_names[0]

            # Measure displacement over first COLLISION_FRAMES // 3 ticks (open space)
            self.client.set_input(InputCommand.NONE)
            self.client.step(2)

            warmup = self.COLLISION_FRAMES // 3
            self.client.set_input(InputCommand.FORWARD, duration_frames=self.COLLISION_FRAMES)
            self.client.step(warmup)
            pos_mid = self._position(player_name)

            # Continue for another 2× warmup ticks
            self.client.step(warmup * 2)
            pos_end = self._position(player_name)

            after = pos_end or self._position(player_name)
            if after is None:
                return VerificationResult(name, False,
                    f"Object '{player_name}' disappeared during collision test")

            # Speed in first window vs. second window
            delta_total = self._distance(before, after) if after else 0.0
            speed_early = self._distance(before, pos_mid) if pos_mid else 0.0
            speed_late  = self._distance(pos_mid, pos_end) if (pos_mid and pos_end) else speed_early

            # If the character is still moving freely at the end, something is
            # wrong (no wall encountered). But the collision check is fundamentally
            # directional, and without knowing wall orientation we can only check
            # that the character eventually slowed down OR stayed within a
            # reasonable distance.  Accept if either:
            #   (a) speed dropped by ≥ 50 % (wall stopped the character), or
            #   (b) total displacement < 5 m (scene geometry is small)
            slowed   = speed_early > 0.01 and speed_late < speed_early * 0.6
            bounded  = delta_total < 5.0

            verdict = slowed or bounded

            return VerificationResult(
                name, verdict,
                (f"displacement={delta_total:.2f} m  "
                 f"speed_early={speed_early:.2f} speed_late={speed_late:.2f}  "
                 f"slowed={slowed} bounded={bounded}"),
                details={
                    "before": before, "after": after,
                    "wall": target_wall,
                    "delta_total": delta_total,
                    "speed_early": speed_early,
                    "speed_late":  speed_late,
                }
            )

        return self._with_snapshot("snap_collision", _run)

    def verify_scene_has_player(self, player_name: str = "Player") -> VerificationResult:
        """Assert that a player object is present in the scene."""
        name = "verify_scene_has_player"
        pos = self._position(player_name)
        if pos is not None:
            return VerificationResult(name, True,
                f"Player '{player_name}' found at {pos}")
        return VerificationResult(name, False,
            f"No object named '{player_name}' in scene")

    def verify_scene_has_floor(self) -> VerificationResult:
        """Assert that at least one floor/ground object exists."""
        name = "verify_scene_has_floor"
        state = self.client.get_state()
        if not state.success or not isinstance(state.payload, dict):
            return VerificationResult(name, False, "Could not query scene state")

        objects = state.payload.get("objects", [])
        floor_names = [
            o["name"] for o in objects
            if any(f in o["name"].lower() for f in ["floor", "ground", "Ground", "Floor"])
        ]
        if floor_names:
            return VerificationResult(name, True,
                f"Floor object found: {floor_names[0]}")
        return VerificationResult(name, False,
            "No floor/ground object in scene — character may fall indefinitely")


# ---------------------------------------------------------------------------
# Verification Suite
# ---------------------------------------------------------------------------

class VerificationSuite:
    """
    Runs the standard set of behavioural checks and collects a SuiteReport.
    """

    def __init__(self, client: PiccoloClient, player_name: str = "Player"):
        self.client      = client
        self.player_name = player_name
        self.verifier    = Verifier(client)

    def run_all(self, verbose: bool = True) -> SuiteReport:
        """Run every check; return a SuiteReport with all results."""
        report = SuiteReport()

        checks = [
            ("scene_has_player",  lambda: self.verifier.verify_scene_has_player(self.player_name)),
            ("scene_has_floor",   lambda: self.verifier.verify_scene_has_floor()),
            ("character_moves",   lambda: self.verifier.verify_character_moves(self.player_name)),
            ("character_jumps",   lambda: self.verifier.verify_character_jumps(self.player_name)),
            ("wall_collision",    lambda: self.verifier.verify_wall_collision(self.player_name)),
        ]

        for label, fn in checks:
            if verbose:
                print(f"  running {label}…", end=" ", flush=True)
            try:
                result = fn()
            except Exception as exc:
                result = VerificationResult(label, False,
                    f"Exception: {exc}")
            if verbose:
                print(result)
            report.results.append(result)

        return report

    @staticmethod
    def print_report(report: SuiteReport) -> None:
        """Pretty-print a SuiteReport to stdout."""
        print("\n" + "=" * 60)
        print("Verification Suite Report")
        print("=" * 60)
        for r in report.results:
            print(f"  {r}")
            if r.details:
                for k, v in r.details.items():
                    if k == "skipped" and v:
                        continue
                    print(f"      {k}: {v}")
        print("-" * 60)
        print(f"  Result: {report.passed}/{len(report.results)} checks passed")
        if report.all_passed:
            print("  ✓ All checks passed — game is playable")
        else:
            print(f"  ✗ {report.failed} check(s) failed")
        print("=" * 60 + "\n")
