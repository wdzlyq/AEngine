"""
piccolo_client.py — HTTP/JSON-RPC 2.0 client for the Piccolo MCP server.

The engine must be running with AgentCommandSystem initialized on port 7777
(or whichever port is passed to PiccoloClient).  Every method blocks until
the engine's main thread processes the command.

Usage::

    from scripts.piccolo_client import PiccoloClient, InputCommand

    client = PiccoloClient()
    state  = client.get_state()          # {objects: [{name, id, position}…]}
    client.set_input(InputCommand.JUMP, duration_frames=20)
    result = client.step(30)             # advance 30 ticks, return new state
"""

import json
import time
from dataclasses import dataclass, field
from typing import Any

import requests

# ---------------------------------------------------------------------------
# Input command bitmask — keep in sync with input_system.h
# ---------------------------------------------------------------------------
class InputCommand:
    FORWARD     = 1 << 0
    BACKWARD    = 1 << 1
    LEFT        = 1 << 2
    RIGHT       = 1 << 3
    JUMP        = 1 << 4
    SQUAT       = 1 << 5
    SPRINT      = 1 << 6
    FIRE        = 1 << 7
    FREE_CAMERA = 1 << 8
    NONE        = 0


@dataclass
class MCPResult:
    success: bool
    payload: Any        # parsed JSON payload (dict / list / str / None)
    error: str = ""


class PiccoloClient:
    """Thin JSON-RPC 2.0 client around the Piccolo MCP server."""

    def __init__(self, host: str = "localhost", port: int = 7777, timeout: float = 30.0):
        self._url     = f"http://{host}:{port}/"
        self._timeout = timeout
        self._id      = 0
        self._initialize()

    # ------------------------------------------------------------------
    # Low-level transport
    # ------------------------------------------------------------------

    def _next_id(self) -> int:
        self._id += 1
        return self._id

    def _rpc(self, method: str, params: dict) -> dict:
        payload = {
            "jsonrpc": "2.0",
            "id":      self._next_id(),
            "method":  method,
            "params":  params,
        }
        resp = requests.post(self._url, json=payload, timeout=self._timeout)
        resp.raise_for_status()
        return resp.json()

    def _tool_call(self, tool_name: str, arguments: dict) -> MCPResult:
        rpc_resp = self._rpc("tools/call", {"name": tool_name, "arguments": arguments})

        if "error" in rpc_resp:
            return MCPResult(success=False, payload=None,
                             error=rpc_resp["error"].get("message", "RPC error"))

        result = rpc_resp.get("result", {})
        is_error = result.get("isError", False)
        content  = result.get("content", [])
        text     = content[0].get("text", "") if content else ""

        if is_error:
            return MCPResult(success=False, payload=None, error=text)

        # Try to parse the text as JSON; fall back to raw string
        try:
            parsed = json.loads(text)
        except (json.JSONDecodeError, TypeError):
            parsed = text

        return MCPResult(success=True, payload=parsed)

    def _initialize(self) -> None:
        """MCP handshake — called once in __init__."""
        try:
            self._rpc("initialize", {
                "protocolVersion": "2024-11-05",
                "clientInfo": {"name": "piccolo-agent-harness", "version": "0.2.0"},
                "capabilities": {}
            })
            self._rpc("notifications/initialized", {})
        except Exception:
            pass  # server may ignore or not respond to initialized notification

    # ------------------------------------------------------------------
    # Public API — mirrors the MCP tool list exactly
    # ------------------------------------------------------------------

    def get_capabilities(self) -> MCPResult:
        """Return the component schema JSON (field names, types, ranges)."""
        return self._tool_call("get_capabilities", {})

    def get_state(self) -> MCPResult:
        """Return all GObjects with their current positions."""
        return self._tool_call("get_state", {})

    def query_objects(self) -> MCPResult:
        """Alias for get_state."""
        return self._tool_call("query_objects", {})

    def create_object(self, name: str, definition: str,
                      position: dict | None = None) -> MCPResult:
        """
        Create a new GObject from an asset definition.

        Parameters
        ----------
        name       : unique name for the new object
        definition : path relative to the asset root, e.g.
                     "asset/objects/character/player/player.object.json"
        position   : optional {"x": …, "y": …, "z": …}
        """
        args: dict = {"name": name, "definition": definition}
        if position:
            args["position"] = position
        return self._tool_call("create_object", args)

    def delete_object(self, name: str) -> MCPResult:
        return self._tool_call("delete_object", {"name": name})

    def set_field(self, object_name: str, component_type: str,
                  field_name: str, value: Any) -> MCPResult:
        return self._tool_call("set_field", {
            "object_name":    object_name,
            "component_type": component_type,
            "field_name":     field_name,
            "value":          value,
        })

    def get_field(self, object_name: str, component_type: str,
                  field_name: str) -> MCPResult:
        return self._tool_call("get_field", {
            "object_name":    object_name,
            "component_type": component_type,
            "field_name":     field_name,
        })

    def set_input(self, command_mask: int, duration_frames: int = -1) -> MCPResult:
        """
        Inject a game input bitmask.
        duration_frames=-1 means hold until the next set_input call.
        """
        return self._tool_call("set_input", {
            "command_mask":    command_mask,
            "duration_frames": duration_frames,
        })

    def step(self, n_frames: int = 1) -> MCPResult:
        """
        Advance exactly n_frames logic ticks, then return the new scene state.
        Blocks until the engine has processed all frames.
        """
        return self._tool_call("step", {"n_frames": n_frames})

    def snapshot(self, action: str, tag: str = "default") -> MCPResult:
        """
        action='save'    — capture current level state.
        action='restore' — roll back to the named snapshot.
        """
        return self._tool_call("snapshot", {"action": action, "tag": tag})

    def save_level(self) -> MCPResult:
        """Persist the current in-memory level back to its JSON file."""
        return self._tool_call("save_level", {})

    # ------------------------------------------------------------------
    # Convenience helpers
    # ------------------------------------------------------------------

    def get_object_position(self, name: str) -> dict | None:
        """Return {"x", "y", "z"} for the named object, or None if not found."""
        result = self.get_state()
        if not result.success or not isinstance(result.payload, dict):
            return None
        for obj in result.payload.get("objects", []):
            if obj.get("name") == name:
                return obj.get("position")
        return None

    def wait_for_engine(self, retries: int = 30, delay: float = 1.0) -> bool:
        """
        Poll until the engine responds.  Returns True if reachable within
        retries * delay seconds.
        """
        for _ in range(retries):
            try:
                r = self.get_state()
                if r.success:
                    return True
            except Exception:
                pass
            time.sleep(delay)
        return False

    def tools_list(self) -> list:
        """Return the tool definitions the server advertises."""
        rpc = self._rpc("tools/list", {})
        return rpc.get("result", {}).get("tools", [])
