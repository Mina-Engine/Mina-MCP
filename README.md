# Mina-MCP

[日本語版 README (README.ja.md)](README.ja.md)

**Mina-MCP** is the built-in **Model Context Protocol (MCP) server module** for [Mina Engine](https://github.com/Mina-Engine/Mina-Engine) (Godot 4.7.2 based).

It automatically starts alongside the game engine (in both **Project Manager** and **Editor** modes), stops cleanly on exit, and exposes Mina Engine's **internal C++ APIs** directly as MCP tools and live resources so external AI agents (**Claude Code**, **Codex**, and any standard MCP client) can execute full end-to-end game development — from project creation to scenes, nodes, assets, scripts, animations, lighting, playtesting, debugging, and build/export — without fragile GUI automation.

---

## Features

- **Automatic Engine Lifecycle Binding**
  - Auto-starts when Mina Engine launches (both Project Manager and Editor).
  - Auto-stops cleanly and releases sockets when the engine exits.
  - Fault-isolated: if the MCP server fails to start (e.g., port conflict), the game engine continues working normally.
- **Standard MCP Transports (Zero External Dependencies)**
  - **Streamable HTTP** (`POST /mcp`, `GET /mcp`, `DELETE /mcp`) — MCP `2025-06-18` & `2025-03-26`.
  - **Legacy HTTP+SSE** (`GET /sse`, `POST /messages?sessionId=...`) — MCP `2024-11-05`.
  - **Health / Discovery** (`GET /health`).
- **Localhost-Only by Default**
  - Binds exclusively to `127.0.0.1:6800` by default.
  - Validates `Host` and `Origin` headers to prevent unintended external exposure and DNS rebinding attacks.
- **Built-in Management Dock (`MCP Server`)**
  - Live status (`Running` / `Stopped` / `Error`), Start / Stop / Restart controls, port & auto-start settings, connected sessions list, one-click endpoint URL & generic MCP JSON config copy, and live tool execution feed.
- **Extensible Architecture**
  - C++ modules register tools via `MinaMCPServer::add_provider_callback(...)` without modifying core MCP code.
  - GDScript / GDExtension plugins register tools dynamically via `MinaMCPServer.register_script_tool(...)` and `register_script_resource(...)`.

---

## Quick Connect

Connect any MCP-compatible client (Streamable HTTP: `http://127.0.0.1:6800/mcp` or Legacy SSE: `http://127.0.0.1:6800/sse`).

### Standard MCP JSON Configuration

```json
{
  "mcpServers": {
    "mina-engine": {
      "url": "http://127.0.0.1:6800/mcp"
    }
  }
}
```

### Client Examples (CLI / TOML)

```bash
# Example: Claude Code
claude mcp add --transport http mina-engine http://127.0.0.1:6800/mcp
```

```toml
# Example: Codex (~/.codex/config.toml)
[mcp_servers.mina-engine]
url = "http://127.0.0.1:6800/mcp"
```

---

## Configuration

| Method | Options |
|---|---|
| **Editor Settings** | `network/mcp_server/auto_start` (`true`), `network/mcp_server/host` (`127.0.0.1`), `network/mcp_server/port` (`6800`), `network/mcp_server/allow_remote` (`false`) |
| **CLI Flags** | `--mcp-port <port>`, `--no-mcp` |
| **Environment Variables** | `MINA_MCP_PORT`, `MINA_MCP_HOST`, `MINA_MCP_DISABLE=1` |

---

## Exposed Tools & Resources

### Live MCP Resources
- `mina://engine/status` — Engine version, mode, MCP status, open scenes, playback state
- `mina://project/info` — Current project metadata, autoloads, input actions
- `mina://scene/current` — Live edited scene node tree
- `mina://logs/recent` — Recent captured editor and running-game stdout, warnings, and errors
- `mina://debug/runtime_errors` — Structured running-game runtime errors, warnings, debugger breaks, and stack traces

### Built-in Tools (76 core tools + extensible module tools)
- **Engine & Project (10):** `engine_get_status`, `project_create`, `project_list`, `project_open`, `project_get_info`, `project_get_setting`, `project_set_setting`, `project_list_settings`, `project_configure_input_action`, `project_manage_autoload`
- **Scene & Nodes & Editor (23):** `scene_list_open`, `scene_create`, `scene_open`, `scene_save`, `scene_close`, `scene_get_tree`, `scene_instantiate`, `node_create`, `node_delete`, `node_duplicate`, `node_reparent`, `node_rename`, `node_get_properties`, `node_set_properties`, `node_set_transform`, `node_manage_groups`, `node_connect_signal`, `node_disconnect_signal`, `node_list_signals`, `node_call_method`, `editor_select_nodes`, `editor_undo`, `editor_redo`
- **Assets & Scripts (17):** `asset_list_files`, `asset_get_info`, `asset_create_resource`, `asset_inspect_resource`, `asset_update_resource`, `asset_create_directory`, `asset_move_file`, `asset_copy_file`, `asset_delete_file`, `asset_scan_filesystem`, `asset_reimport`, `script_read`, `script_write`, `script_patch`, `script_attach`, `script_validate`, `script_open_in_editor`
- **Animation, Lighting & ClassDB Introspection (11):** `animation_list`, `animation_create`, `animation_delete`, `animation_add_track`, `animation_insert_key`, `animation_remove_key`, `animation_get_details`, `lighting_add_light`, `lighting_configure_environment`, `class_list`, `class_get_info`
- **Runtime, Input Automation, Debugging & Export (15):** `game_play`, `game_stop`, `game_get_status`, `game_capture_runtime` (live running game PNG screenshot), `game_send_input` (keyboard, chords, holds, mouse relative/absolute motion, clicks, scroll, gamepad buttons/sticks, ordered sequences, `release_all` — scoped exclusively to the target game process), `debug_get_logs`, `debug_clear_logs`, `debug_get_runtime_errors`, `debug_clear_runtime_errors`, `debug_set_breakpoint`, `debug_capture_viewport`, `export_list_platforms`, `export_list_presets`, `export_create_preset`, `export_project`
- **Example Module Extension (`examples/horror_mcp_tools/`):** `horror_get_director_state`, `horror_set_director_state`, `horror_create_interactable`

---

## Integration into Mina Engine / Godot 4.7+

1. Copy `modules/mcp/` into your engine source tree's `modules/mcp/` directory (or build using SCons `custom_modules=/path/to/Mina-MCP/modules`).
2. *(Optional, recommended)* Apply `integration/engine_integration.patch` to enable:
   - `network/mcp_server/*` defaults in `EditorSettings`
   - `--mcp-port <port>` and `--no-mcp` CLI flags
   - Auto-start in Project Manager mode (`MCPProjectManagerHost`)
3. Build the editor with SCons as usual.

---

## Extending Mina-MCP

### From a C++ Engine Module

```cpp
#include "modules/mcp/mcp_server.h"
#include "modules/mcp/mcp_variant_codec.h"

void my_module_register_mcp_tools(MinaMCPServer *p_server) {
    MinaMCPServer::ToolDefinition t;
    t.name = "my_module_do_something";
    t.description = "Invoke custom module functionality.";
    t.input_schema = mcp::make_tool_schema(Dictionary());
    t.handler = [](const Dictionary &p_args) -> Dictionary {
        return mcp::make_ok();
    };
    p_server->register_tool(t);
}

// Inside initialize_<module>_module (MODULE_INITIALIZATION_LEVEL_EDITOR):
MinaMCPServer::add_provider_callback(my_module_register_mcp_tools);
```

See `examples/horror_mcp_tools/horror_mcp_tools.cpp` for a complete example.

### From GDScript / GDExtension

```gdscript
MinaMCPServer.register_script_tool(
    "my_plugin_ping",
    "Custom tool exposed from GDScript",
    {"type": "object", "properties": {}},
    func(_args: Dictionary) -> Dictionary:
        return {"ok": true, "pong": true}
)
```

---

## Testing

Run the end-to-end integration test suite against a built editor binary:

```bash
python tests/test_mcp_e2e.py --engine /path/to/godot.windows.editor.x86_64.exe
```

---

## License

MIT License — see [LICENSE](LICENSE).
