#!/usr/bin/env python3
"""End-to-end integration test suite for Mina-MCP."""

import argparse
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def http_req(port, method, path, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
    hdrs = headers.copy() if headers else {}
    raw = None
    if body is not None:
        raw = json.dumps(body).encode("utf-8")
        hdrs.setdefault("Content-Type", "application/json")
        hdrs.setdefault("Accept", "application/json, text/event-stream")
    conn.request(method, path, body=raw, headers=hdrs)
    resp = conn.getresponse()
    data = resp.read().decode("utf-8", errors="replace")
    resp_headers = {k.lower(): v for k, v in resp.getheaders()}
    conn.close()
    return resp.status, resp_headers, data


def wait_for_health(port, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status, _, data = http_req(port, "GET", "/health")
            if status == 200:
                return json.loads(data)
        except Exception:
            pass
        time.sleep(0.25)
    raise RuntimeError(f"MCP server on port {port} did not become ready within {timeout}s")


def wait_port_closed(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if s.connect_ex(("127.0.0.1", port)) != 0:
                return True
        time.sleep(0.2)
    return False


class MCPClient:
    def __init__(self, port):
        self.port = port
        self.session_id = None
        self.next_id = 1

    def rpc(self, method, params=None, is_notification=False):
        payload = {"jsonrpc": "2.0", "method": method}
        if not is_notification:
            payload["id"] = self.next_id
            self.next_id += 1
        if params is not None:
            payload["params"] = params
        headers = {}
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        status, resp_headers, data = http_req(self.port, "POST", "/mcp", payload, headers)
        if "mcp-session-id" in resp_headers:
            self.session_id = resp_headers["mcp-session-id"]
        if is_notification:
            assert status == 202, f"Expected 202 for notification, got {status}: {data}"
            return None
        assert status == 200, f"Expected 200 for {method}, got {status}: {data}"
        parsed = json.loads(data)
        assert "error" not in parsed, f"JSON-RPC error on {method}: {parsed}"
        return parsed["result"]

    def call_tool(self, name, arguments=None):
        res = self.rpc("tools/call", {"name": name, "arguments": arguments or {}})
        sc = res.get("structuredContent", {})
        return sc, res


def main():
    parser = argparse.ArgumentParser(description="Run E2E tests against Mina Engine built with Mina-MCP.")
    parser.add_argument(
        "--engine",
        default=os.environ.get("MINA_ENGINE_EXE", ""),
        help="Path to the Mina Engine editor executable (or set MINA_ENGINE_EXE).",
    )
    args = parser.parse_args()
    engine_exe = args.engine
    if not engine_exe or not os.path.exists(engine_exe):
        print("Error: Provide --engine /path/to/Mina-Engine-editor or set MINA_ENGINE_EXE.", file=sys.stderr)
        sys.exit(2)

    tmp_dir = tempfile.mkdtemp(prefix="mina_mcp_e2e_")
    proj_dir = os.path.join(tmp_dir, "MCPTestProject")
    pm_proc = None
    ed_proc = None
    blocker_sock = None

    try:
        print("[1/4] Testing Project Manager auto-start, security, and project_create...")
        pm_port = 16801
        pm_proc = subprocess.Popen(
            [engine_exe, "--headless", "--project-manager", "--mcp-port", str(pm_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        health = wait_for_health(pm_port, timeout=30)
        assert health["running"] is True and health["mode"] == "project_manager", health

        st_host, _, _ = http_req(pm_port, "GET", "/health", headers={"Host": "evil.example.com"})
        assert st_host == 403, f"Expected 403 for forged Host, got {st_host}"
        st_origin, _, _ = http_req(
            pm_port,
            "POST",
            "/mcp",
            {"jsonrpc": "2.0", "id": 1, "method": "ping"},
            headers={"Origin": "https://evil.example.com"},
        )
        assert st_origin == 403, f"Expected 403 for forged Origin, got {st_origin}"

        client = MCPClient(pm_port)
        init = client.rpc(
            "initialize",
            {
                "protocolVersion": "2025-03-26",
                "clientInfo": {"name": "mina-mcp-e2e", "version": "1.0.0"},
                "capabilities": {},
            },
        )
        assert init["serverInfo"]["name"] == "mina-engine", init
        assert client.session_id, "Missing Mcp-Session-Id"
        client.rpc("notifications/initialized", is_notification=True)

        tools_list = client.rpc("tools/list")["tools"]
        assert len(tools_list) >= 50, f"Expected >=50 tools, got {len(tools_list)}"

        created, _ = client.call_tool(
            "project_create",
            {
                "path": proj_dir,
                "name": "MCP E2E Test",
                "renderer": "gl_compatibility",
                "register_in_manager": False,
            },
        )
        assert created.get("ok") is True, created
        assert os.path.exists(os.path.join(proj_dir, "project.godot"))

        pm_proc.terminate()
        pm_proc.wait(timeout=15)
        pm_proc = None
        assert wait_port_closed(pm_port), "Port was not released on Project Manager exit"
        print("  PASS")

        print("[2/4] Testing Editor mode 1-to-100 MCP workflow...")
        ed_port = 16802
        ed_proc = subprocess.Popen(
            [engine_exe, "--headless", "--editor", "--path", proj_dir, "--mcp-port", str(ed_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        health = wait_for_health(ed_port, timeout=45)
        assert health["running"] is True and health["mode"] == "editor", health

        ec = MCPClient(ed_port)
        ec.rpc(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "mina-mcp-e2e", "version": "1.0.0"},
                "capabilities": {},
            },
        )
        ec.rpc("notifications/initialized", is_notification=True)

        r, _ = ec.call_tool("project_set_setting", {"name": "application/config/description", "value": "Built via MCP"})
        assert r.get("ok"), r
        r, _ = ec.call_tool("project_configure_input_action", {"action": "interact", "keys": ["E"]})
        assert r.get("ok"), r

        r, _ = ec.call_tool("scene_create", {"root_type": "Node3D", "root_name": "Main", "save_path": "res://scenes/test_level.tscn"})
        assert r.get("ok"), r
        r, _ = ec.call_tool(
            "node_create",
            {
                "type": "MeshInstance3D",
                "name": "Floor",
                "position": [0, -0.5, 0],
                "properties": {
                    "mesh": {"class": "BoxMesh", "size": [10, 1, 10]},
                    "material_override": {"class": "StandardMaterial3D", "albedo_color": "#202226"},
                },
            },
        )
        assert r.get("ok"), r
        r, _ = ec.call_tool("node_create", {"type": "AnimationPlayer", "name": "Anim"})
        assert r.get("ok"), r
        r, _ = ec.call_tool("lighting_add_light", {"light_type": "OmniLight3D", "name": "Lamp", "position": [0, 3, 0], "energy": 2.0})
        assert r.get("ok"), r
        r, _ = ec.call_tool("lighting_configure_environment", {"background_mode": "color", "background_color": "#050608", "fog_enabled": True})
        assert r.get("ok"), r

        r, _ = ec.call_tool("animation_create", {"player_path": "Anim", "animation": "flicker", "length": 1.0, "loop_mode": "linear"})
        assert r.get("ok"), r
        r, _ = ec.call_tool("animation_add_track", {"player_path": "Anim", "animation": "flicker", "track_path": "Lamp:light_energy"})
        assert r.get("ok"), r
        r, _ = ec.call_tool(
            "animation_insert_key",
            {
                "player_path": "Anim",
                "animation": "flicker",
                "track_index": r["track_index"],
                "keys": [{"time": 0.0, "value": 2.0}, {"time": 0.5, "value": 0.2}],
            },
        )
        assert r.get("ok"), r

        r, _ = ec.call_tool("script_attach", {"node_path": ".", "script_path": "res://scripts/main.gd", "content": "extends Node3D\n\nfunc _ready() -> void:\n\tpass\n"})
        assert r.get("ok"), r
        r, _ = ec.call_tool("script_validate", {"path": "res://scripts/main.gd"})
        assert r.get("ok") and r.get("valid") is True, r
        r, _ = ec.call_tool("scene_save")
        assert r.get("ok"), r

        tree, _ = ec.call_tool("scene_get_tree")
        assert tree.get("ok") and tree.get("node_count", 0) >= 4, tree
        logs, _ = ec.call_tool("debug_get_logs", {"limit": 10})
        assert logs.get("ok"), logs
        plats, _ = ec.call_tool("export_list_platforms")
        assert plats.get("ok") and plats.get("count", 0) > 0, plats
        print("  PASS")

        print("[3/4] Testing clean shutdown...")
        ed_proc.terminate()
        ed_proc.wait(timeout=15)
        ed_proc = None
        assert wait_port_closed(ed_port), "Port was not released on Editor exit"
        print("  PASS")

        print("[4/4] Testing fault isolation on port conflict...")
        busy_port = 16803
        blocker_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        blocker_sock.bind(("127.0.0.1", busy_port))
        blocker_sock.listen(1)

        res_proc = subprocess.run(
            [engine_exe, "--headless", "--editor", "--path", proj_dir, "--mcp-port", str(busy_port), "--quit-after", "20"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=45,
        )
        assert res_proc.returncode == 0, f"Engine failed on port conflict (rc={res_proc.returncode}):\n{res_proc.stdout}"
        print("  PASS")
        print("\nALL E2E MCP TESTS PASSED!")
    finally:
        if pm_proc:
            pm_proc.kill()
        if ed_proc:
            ed_proc.kill()
        if blocker_sock:
            blocker_sock.close()
        shutil.rmtree(tmp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
