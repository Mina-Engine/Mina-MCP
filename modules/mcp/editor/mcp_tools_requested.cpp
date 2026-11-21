#include "mcp_builtin_tools.h"

#include "../mcp_server.h"
#include "../mcp_variant_codec.h"
#include "mcp_runtime_debugger_plugin.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/os/os.h"
#include "editor/audio/audio_stream_preview.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "main/performance.h"
#include "servers/audio/audio_stream.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"
#include "servers/navigation_3d/navigation_server_3d.h"

namespace {

MinaMCPServer *requested_server = nullptr;
Ref<AudioStreamPreview> current_audio_preview;
Ref<AudioStream> current_audio_stream;
AudioStreamPlayer *audio_player = nullptr;

struct BatchOperation {
	String id;
	String state = "queued";
	int current_step = 0;
	bool cancelled = false;
	Array calls;
	Array results;
};
HashMap<String, BatchOperation> operations;
uint64_t next_operation_id = 1;
HashMap<String, String> scene_checkpoints;

Dictionary _runtime(const String &p_operation, const Dictionary &p_args) {
	MCPRuntimeDebuggerPlugin *plugin = MCPRuntimeDebuggerPlugin::get_singleton();
	if (!plugin) {
		return mcp::make_error("Runtime debugger bridge is unavailable. Start a game with game_play first.", "game_not_running");
	}
	return plugin->request_runtime(p_operation, p_args);
}

Dictionary _tool_runtime_get_tree(const Dictionary &p_args) { return _runtime("runtime_get_tree", p_args); }
Dictionary _tool_runtime_get_properties(const Dictionary &p_args) { return _runtime("runtime_get_properties", p_args); }
Dictionary _tool_runtime_set_properties(const Dictionary &p_args) { return _runtime("runtime_set_properties", p_args); }
Dictionary _tool_runtime_call_method(const Dictionary &p_args) { return _runtime("runtime_call_method", p_args); }
Dictionary _tool_game_wait_for_condition(const Dictionary &p_args) {
	int timeout_ms = CLAMP(int(p_args.get("timeout_ms", 5000)), 100, 30000);
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + uint64_t(timeout_ms);
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		Dictionary sample = _runtime("game_wait_for_condition", p_args);
		if (!bool(sample.get("ok", false))) return sample;
		Dictionary data = sample.get("result", Dictionary());
		if (bool(data.get("condition_met", false))) {
			data["timed_out"] = false;
			return mcp::make_ok(data);
		}
		OS::get_singleton()->delay_usec(50000);
	}
	Dictionary timeout;
	timeout["condition_met"] = false;
	timeout["timed_out"] = true;
	timeout["timeout_ms"] = timeout_ms;
	return mcp::make_ok(timeout);
}
Dictionary _tool_game_get_events(const Dictionary &p_args) { return _runtime("game_get_events", p_args); }
Dictionary _tool_game_set_time_scale(const Dictionary &p_args) { return _runtime("game_set_time_scale", p_args); }
Dictionary _tool_debug_pause_game(const Dictionary &p_args) { return _runtime("debug_pause_game", p_args); }
Dictionary _tool_debug_step_frame(const Dictionary &p_args) {
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger || !debugger->get_current_debugger()) return mcp::make_error("Editor debugger is unavailable.", "editor_debugger_required");
	ScriptEditorDebugger *session = debugger->get_current_debugger();
	if (!session->is_breaked() || !session->is_debuggable()) return mcp::make_error("Pause the game at a debuggable break before stepping.", "game_not_paused");
	debugger->debug_step();
	Dictionary out;
	out["accepted"] = true;
	out["step_kind"] = "script_debugger_step";
	out["note"] = "Godot exposes script-step, not a render-frame step, in this editor API.";
	return mcp::make_ok(out);
}
Dictionary _tool_debug_get_performance(const Dictionary &p_args) { return _runtime("debug_get_performance", p_args); }
Dictionary _tool_debug_get_stacktrace(const Dictionary &p_args) {
	Dictionary state = MinaMCPServer::get_singleton()->get_debugger_state();
	Dictionary out;
	out["stack"] = state.get("current_stack", Array());
	out["breaked"] = state.get("is_breaked", false);
	out["break_reason"] = state.get("break_reason", "");
	if (Array(out["stack"]).is_empty()) {
		Array errors = MinaMCPServer::get_singleton()->get_runtime_errors("all", 0, 1);
		if (!errors.is_empty()) out["stack"] = Dictionary(errors[0]).get("stack_trace", Array());
	}
	if (Array(out["stack"]).is_empty()) return mcp::make_error("No runtime stacktrace has been captured. Pause on a debugger break or trigger a runtime error first.", "stacktrace_unavailable");
	return mcp::make_ok(out);
}

void _validate_node_recursive(Node *p_node, Node *p_root, Array &r_diagnostics, int &r_count, int p_limit) {
	if (!p_node || r_count >= p_limit) return;
	r_count++;
	if (p_node != p_root && p_node->get_owner() == nullptr) {
		Dictionary d;
		d["severity"] = "warning";
		d["node_path"] = String(p_root->get_path_to(p_node));
		d["message"] = "Node has no owner and may not be persisted in the PackedScene.";
		r_diagnostics.push_back(d);
	}
	for (int i = 0; i < p_node->get_child_count() && r_count < p_limit; i++) _validate_node_recursive(p_node->get_child(i), p_root, r_diagnostics, r_count, p_limit);
}

Dictionary _tool_scene_validate(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()) return mcp::make_error("EditorInterface is unavailable.", "editor_required");
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) return mcp::make_error("No edited scene is open.", "no_scene");
	Array diagnostics;
	int count = 0;
	_validate_node_recursive(root, root, diagnostics, count, CLAMP(int(p_args.get("max_nodes", 2000)), 1, 10000));
	Dictionary out;
	out["valid"] = true;
	for (const Variant &variant : diagnostics) if (String(Dictionary(variant).get("severity", "")) == "error") out["valid"] = false;
	out["nodes_checked"] = count;
	out["diagnostics"] = diagnostics;
	out["scope"] = "edited_scene";
	return mcp::make_ok(out);
}

void _find_control_layout(Node *p_node, Node *p_root, Array &r_items) {
	if (Control *control = Object::cast_to<Control>(p_node)) {
		Dictionary item;
		item["path"] = String(p_root->get_path_to(control));
		item["class"] = control->get_class();
		item["position"] = mcp::variant_to_json(control->get_position());
		item["size"] = mcp::variant_to_json(control->get_size());
		r_items.push_back(item);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) _find_control_layout(p_node->get_child(i), p_root, r_items);
}

Dictionary _tool_ui_inspect_layout(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()) return mcp::make_error("EditorInterface is unavailable.", "editor_required");
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) return mcp::make_error("No edited scene is open.", "no_scene");
	String path = String(p_args.get("node_path", "."));
	Node *target = path == "." ? root : root->get_node_or_null(NodePath(path));
	if (!target) return mcp::make_error("Control root node was not found.", "node_not_found");
	Array items;
	_find_control_layout(target, root, items);
	Dictionary out;
	out["controls"] = items;
	out["count"] = items.size();
	return mcp::make_ok(out);
}

Dictionary _tool_asset_find_references(const Dictionary &p_args) {
	if (!EditorFileSystem::get_singleton()) return mcp::make_error("EditorFileSystem is unavailable.", "editor_required");
	String target = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(target) || !FileAccess::exists(target)) return mcp::make_error("Target file path does not exist under res://.", "not_found");
	Array files;
	EditorFileSystemDirectory *root = EditorFileSystem::get_singleton()->get_filesystem();
	if (!root) return mcp::make_error("EditorFileSystem scan is not ready.", "unavailable");
	Vector<EditorFileSystemDirectory *> dirs;
	dirs.push_back(root);
	while (!dirs.is_empty() && files.size() < 2000) {
		EditorFileSystemDirectory *dir = dirs[dirs.size() - 1];
		dirs.remove_at(dirs.size() - 1);
		for (int i = 0; i < dir->get_subdir_count(); i++) dirs.push_back(dir->get_subdir(i));
		for (int i = 0; i < dir->get_file_count(); i++) {
			Vector<String> deps = dir->get_file_deps(i);
			for (const String &dep : deps) {
				if (mcp::normalize_res_path(dep) == target) {
					files.push_back(dir->get_file_path(i));
					break;
				}
			}
		}
	}
	Dictionary out;
	out["path"] = target;
	out["count"] = files.size();
	out["truncated"] = files.size() >= 2000;
	out["references"] = files;
	out["relationship"] = "reverse_direct_dependencies";
	return mcp::make_ok(out);
}

Dictionary _tool_audio_preview(const Dictionary &p_args) {
	String action = String(p_args.get("action", "play"));
	if (action == "stop") {
		if (audio_player) {
			audio_player->stop();
			audio_player->queue_free();
			audio_player = nullptr;
		}
		current_audio_preview.unref();
		current_audio_stream.unref();
		return mcp::make_ok();
	}
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	if (!mcp::is_safe_res_path(path)) return mcp::make_error("Audio path must be a res:// path.", "invalid_path");
	Ref<AudioStream> stream = ResourceLoader::load(path);
	if (stream.is_null()) return mcp::make_error("Could not load audio stream.", "load_failed");
	AudioStreamPreviewGenerator *generator = AudioStreamPreviewGenerator::get_singleton();
	if (!generator) return mcp::make_error("Audio preview generator is unavailable.", "unavailable");
	if (!EditorNode::get_singleton()) return mcp::make_error("Editor is unavailable.", "editor_required");
	if (audio_player) {
		audio_player->stop();
		audio_player->queue_free();
	}
	current_audio_stream = stream;
	current_audio_preview = generator->generate_preview(stream);
	audio_player = memnew(AudioStreamPlayer);
	audio_player->set_name("MCPAudioPreview");
	audio_player->set_stream(stream);
	EditorNode::get_singleton()->add_child(audio_player);
	audio_player->play(float(p_args.get("from_seconds", 0.0)));
	Dictionary out;
	out["path"] = path;
	out["playing"] = audio_player->is_playing();
	out["length_seconds"] = stream->get_length();
	out["preview_generated"] = current_audio_preview.is_valid();
	return mcp::make_ok(out);
}

Dictionary _tool_shader_validate(const Dictionary &p_args) {
	String path = mcp::normalize_res_path(String(p_args.get("path", "")));
	String code = String(p_args.get("content", ""));
	if (code.is_empty()) {
		if (!mcp::is_safe_res_path(path) || !FileAccess::exists(path)) return mcp::make_error("Provide shader content or an existing res:// path.", "not_found");
		code = FileAccess::get_file_as_string(path);
	}
	Dictionary details;
	details["path"] = path;
	details["code_length"] = code.length();
	return mcp::make_error("This engine branch does not expose shader compiler diagnostics to C++; refusing to claim shader validation from resource loading alone.", "compiler_diagnostics_unavailable", details);
}

Dictionary _tool_mcp_get_capabilities(const Dictionary &p_args) {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	Dictionary out;
	Array tools;
	if (server) {
		PackedStringArray names = server->get_tool_names();
		for (int i = 0; i < names.size(); i++) tools.push_back(names[i]);
	}
	out["tools"] = tools;
	out["tool_count"] = tools.size();
	out["runtime_available"] = MCPRuntimeDebuggerPlugin::get_singleton() && MCPRuntimeDebuggerPlugin::get_singleton()->has_active_session();
	out["runtime_editor_scene_separation"] = true;
	out["batch_max_operations"] = 50;
	out["checkpoint_scope"] = "edited_scene_only";
	return mcp::make_ok(out);
}

Dictionary _operation_dictionary(const BatchOperation &p_operation) {
	Dictionary out;
	out["operation_id"] = p_operation.id;
	out["state"] = p_operation.state;
	out["completed_steps"] = p_operation.current_step;
	out["total_steps"] = p_operation.calls.size();
	out["results"] = p_operation.results.duplicate(true);
	return out;
}

Dictionary _tool_mcp_execute_batch(const Dictionary &p_args) {
	Array calls = p_args.get("operations", Array());
	if (calls.is_empty() || calls.size() > 50) return mcp::make_error("operations must contain 1..50 calls.", "invalid_argument");
	if (operations.size() >= 64) return mcp::make_error("Operation history is full; restart Mina Engine to clear it.", "operation_limit");
	for (int i = 0; i < calls.size(); i++) {
		if (calls[i].get_type() != Variant::DICTIONARY) return mcp::make_error(vformat("Operation %d must be an object.", i), "invalid_argument");
		String name = String(Dictionary(calls[i]).get("tool", ""));
		if (name == "mcp_execute_batch" || name == "scene_apply_batch" || name == "mcp_cancel_operation" || name == "mcp_get_operation_status" || name.begins_with("mcp_create_checkpoint") || name.begins_with("mcp_restore_checkpoint")) return mcp::make_error("Nested operation-management and checkpoint calls are not allowed in a batch.", "nested_operation");
	}
	BatchOperation operation;
	operation.id = "op-" + itos((int64_t)next_operation_id++);
	operation.calls = calls.duplicate(true);
	String operation_id = operation.id;
	operations[operation_id] = operation;
	if (bool(p_args.get("wait", false))) {
		int timeout_ms = CLAMP(int(p_args.get("timeout_ms", 30000)), 100, 120000);
		uint64_t deadline = OS::get_singleton()->get_ticks_msec() + uint64_t(timeout_ms);
		while (OS::get_singleton()->get_ticks_msec() < deadline && (operations[operation_id].state == "queued" || operations[operation_id].state == "running")) {
			mcp_requested_tools_process();
			OS::get_singleton()->delay_usec(1000);
		}
	}
	Dictionary out = _operation_dictionary(operations[operation_id]);
	out["accepted"] = true;
	return mcp::make_ok(out);
}

Dictionary _tool_mcp_get_operation_status(const Dictionary &p_args) {
	String id = String(p_args.get("operation_id", ""));
	if (!operations.has(id)) return mcp::make_error("Operation ID was not found.", "not_found");
	return mcp::make_ok(_operation_dictionary(operations[id]));
}

Dictionary _tool_mcp_cancel_operation(const Dictionary &p_args) {
	String id = String(p_args.get("operation_id", ""));
	if (!operations.has(id)) return mcp::make_error("Operation ID was not found.", "not_found");
	BatchOperation &operation = operations[id];
	if (operation.state == "queued" || operation.state == "running") {
		operation.cancelled = true;
		operation.state = "cancelling";
	}
	return mcp::make_ok(_operation_dictionary(operation));
}

Dictionary _tool_scene_apply_batch(const Dictionary &p_args) {
	Array calls = p_args.get("operations", Array());
	if (calls.is_empty() || calls.size() > 50) return mcp::make_error("operations must contain 1..50 scene edits.", "invalid_argument");
	if (!requested_server || !EditorInterface::get_singleton() || !EditorInterface::get_singleton()->get_edited_scene_root()) return mcp::make_error("An edited scene and MCP server are required.", "no_scene");
	static const char *allowed[] = { "node_create", "node_delete", "node_duplicate", "node_reparent", "node_rename", "node_set_properties", "node_set_transform", "node_manage_groups", "node_connect_signal", "node_disconnect_signal" };
	Array results;
	for (int i = 0; i < calls.size(); i++) {
		if (calls[i].get_type() != Variant::DICTIONARY) return mcp::make_error(vformat("Scene operation %d must be an object.", i), "invalid_argument");
		Dictionary call = calls[i];
		String tool_name = String(call.get("tool", ""));
		bool tool_allowed = false;
		for (const char *candidate : allowed) {
			if (tool_name == candidate) {
				tool_allowed = true;
				break;
			}
		}
		if (!tool_allowed) return mcp::make_error(vformat("Tool '%s' is not allowed in scene_apply_batch.", tool_name), "tool_not_allowed");
		call["tool"] = tool_name;
		call["arguments"] = call.get("arguments", Dictionary());
	}
	Dictionary batch_args;
	batch_args["operations"] = calls;
	Dictionary result = _tool_mcp_execute_batch(batch_args);
	result["scope"] = "edited_scene";
	result["rollback"] = "each supported operation uses its normal editor UndoRedo action; failed later steps do not automatically reverse earlier steps";
	return result;
}

Dictionary _tool_checkpoint_create(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton() || !EditorInterface::get_singleton()->get_edited_scene_root()) return mcp::make_error("No edited scene is open.", "no_scene");
	String name = String(p_args.get("name", "checkpoint-" + itos(next_operation_id++))).strip_edges();
	if (name.is_empty() || name.contains("/") || name.contains("\\") || name.contains("..")) return mcp::make_error("Checkpoint name must be a simple filename.", "invalid_argument");
	String path = "res://.godot/mcp_checkpoints/" + name + ".tscn";
	String absolute_path = ProjectSettings::get_singleton()->globalize_path(path);
	DirAccess::make_dir_recursive_absolute(absolute_path.get_base_dir());
	Ref<PackedScene> snapshot;
	snapshot.instantiate();
	Error pack_error = snapshot->pack(EditorInterface::get_singleton()->get_edited_scene_root());
	if (pack_error != OK) return mcp::make_error("Failed to pack the edited scene for checkpoint.", "checkpoint_failed");
	Error save_error = ResourceSaver::save(snapshot, path);
	if (save_error != OK) return mcp::make_error(vformat("Failed to save checkpoint (error %d).", int(save_error)), "checkpoint_failed");
	scene_checkpoints[name] = path;
	Dictionary out;
	out["name"] = name;
	out["checkpoint_path"] = path;
	out["scope"] = "edited_scene_only";
	return mcp::make_ok(out);
}

Dictionary _tool_checkpoint_restore(const Dictionary &p_args) {
	if (!EditorInterface::get_singleton()) return mcp::make_error("EditorInterface is unavailable.", "editor_required");
	String name = String(p_args.get("name", ""));
	String default_checkpoint = scene_checkpoints.has(name) ? scene_checkpoints[name] : String();
	String checkpoint_path = String(p_args.get("checkpoint_path", default_checkpoint));
	if (!mcp::is_safe_res_path(checkpoint_path) || !FileAccess::exists(checkpoint_path)) return mcp::make_error("Checkpoint does not exist under res://.", "not_found");
	EditorInterface::get_singleton()->open_scene_from_path(checkpoint_path);
	Dictionary out;
	out["name"] = name;
	out["restored_path"] = checkpoint_path;
	out["scope"] = "edited_scene_only";
	return mcp::make_ok(out);
}

void _register_tool(MinaMCPServer *p_server, const String &p_name, const String &p_title, const String &p_description, const String &p_category, const Dictionary &p_props, const Array &p_required, MinaMCPServer::ToolHandler p_handler) {
	MinaMCPServer::ToolDefinition tool;
	tool.name = p_name;
	tool.title = p_title;
	tool.description = p_description;
	tool.category = p_category;
	tool.input_schema = mcp::make_tool_schema(p_props, p_required);
	tool.handler = p_handler;
	p_server->register_tool(tool);
}

} // namespace

void mcp_requested_tools_process() {
	for (KeyValue<String, BatchOperation> &entry : operations) {
		BatchOperation &operation = entry.value;
		if (operation.state != "queued" && operation.state != "running" && operation.state != "cancelling") continue;
		if (operation.cancelled) {
			operation.state = "cancelled";
			continue;
		}
		if (operation.current_step >= operation.calls.size()) {
			operation.state = "completed";
			continue;
		}
		operation.state = "running";
		Dictionary call = operation.calls[operation.current_step];
		String tool_name = String(call.get("tool", ""));
		Dictionary arguments = call.get("arguments", Dictionary());
		Dictionary result = requested_server ? requested_server->execute_tool_direct(tool_name, arguments) : mcp::make_error("MCP server is unavailable.", "unavailable");
		Dictionary step_result;
		step_result["index"] = operation.current_step;
		step_result["tool"] = tool_name;
		step_result["result"] = result;
		operation.results.push_back(step_result);
		operation.current_step++;
		if (!bool(result.get("ok", false))) operation.state = "failed";
		else if (operation.current_step >= operation.calls.size()) operation.state = "completed";
		else operation.state = "queued";
		break;
	}
}

void mcp_register_requested_tools(MinaMCPServer *p_server) {
	requested_server = p_server;
	using namespace mcp;
	Dictionary runtime_tree;
	runtime_tree["node_path"] = schema_string("Path relative to the running game's SceneTree root (default '.').");
	runtime_tree["max_depth"] = schema_int("Maximum depth (default 8, max 16).");
	runtime_tree["max_nodes"] = schema_int("Maximum nodes (default 300, max 1000).");
	_register_tool(p_server, "runtime_get_tree", "Get Runtime Node Tree", "Get the running game's live SceneTree (not the editor's edited scene).", "runtime", runtime_tree, Array(), _tool_runtime_get_tree);

	Dictionary node_props;
	node_props["node_path"] = schema_string("Running-game node path relative to the SceneTree root.");
	node_props["filter"] = schema_string("Optional case-insensitive property-name filter.");
	Array node_req; node_req.push_back("node_path");
	_register_tool(p_server, "runtime_get_properties", "Get Runtime Node Properties", "Read state and properties from a node in the running game process.", "runtime", node_props, node_req, _tool_runtime_get_properties);

	Dictionary set_props = node_props;
	set_props["properties"] = schema_object("Runtime property values to update.");
	Array set_req; set_req.push_back("node_path"); set_req.push_back("properties");
	_register_tool(p_server, "runtime_set_properties", "Set Runtime Node Properties", "Change editor-visible properties on a node in the running game process.", "runtime", set_props, set_req, _tool_runtime_set_properties);

	Dictionary call_props = node_props;
	call_props["method"] = schema_string("Method name from the runtime-safe read-only allowlist.");
	Array call_req; call_req.push_back("node_path"); call_req.push_back("method");
	_register_tool(p_server, "runtime_call_method", "Call Runtime Node Method", "Call a constrained read-only method on a running-game node.", "runtime", call_props, call_req, _tool_runtime_call_method);

	Dictionary wait_props = node_props;
	wait_props["property"] = schema_string("Property to compare on the runtime node.");
	wait_props["equals"] = schema_any("Expected property value.");
	wait_props["timeout_ms"] = schema_int("Maximum polling duration in milliseconds (default 5000, max 30000).");
	Array wait_req; wait_req.push_back("node_path"); wait_req.push_back("property"); wait_req.push_back("equals");
	_register_tool(p_server, "game_wait_for_condition", "Wait For Runtime Condition", "Poll a declarative runtime property-equals condition until it matches or the bounded timeout expires; does not evaluate arbitrary code.", "runtime", wait_props, wait_req, _tool_game_wait_for_condition);

	_register_tool(p_server, "debug_get_stacktrace", "Get Runtime Stacktrace", "Read the most recently captured runtime break/error stacktrace.", "debug", Dictionary(), Array(), _tool_debug_get_stacktrace);
	Dictionary scene_batch_props; scene_batch_props["operations"] = schema_array("Ordered calls to supported node_* scene-edit tools; max 50.", schema_object("{tool, arguments}"));
	Array scene_batch_req; scene_batch_req.push_back("operations");
	_register_tool(p_server, "scene_apply_batch", "Apply Scene Batch", "Queue ordered edited-scene node operations. Returns an operation ID; status and cancellation use mcp_get_operation_status and mcp_cancel_operation. Failure stops at the failed step; prior UndoRedo changes remain.", "scene", scene_batch_props, scene_batch_req, _tool_scene_apply_batch);

	Dictionary time_props; time_props["scale"] = schema_number("Game time scale from 0.05 to 4.0.");
	Array time_req; time_req.push_back("scale");
	_register_tool(p_server, "game_set_time_scale", "Set Game Time Scale", "Set running-game time scale for slow motion or faster play.", "runtime", time_props, time_req, _tool_game_set_time_scale);
	Dictionary pause_props; pause_props["paused"] = schema_bool("true pauses, false resumes (default true).");
	_register_tool(p_server, "debug_pause_game", "Pause Running Game", "Pause or resume the running game's SceneTree.", "debug", pause_props, Array(), _tool_debug_pause_game);
	_register_tool(p_server, "debug_step_frame", "Step Paused Game", "Advance a paused game using the editor debugger's script-step operation (not a guaranteed render-frame step).", "debug", Dictionary(), Array(), _tool_debug_step_frame);
	_register_tool(p_server, "debug_get_performance", "Get Runtime Performance", "Read available FPS, process, memory, rendering, physics, audio, and navigation monitors from the running game.", "debug", Dictionary(), Array(), _tool_debug_get_performance);
	_register_tool(p_server, "game_get_events", "Get Runtime Events", "Read runtime scene/game event history after an event ID.", "runtime", Dictionary(), Array(), _tool_game_get_events);
	_register_tool(p_server, "scene_validate", "Validate Edited Scene", "Run non-mutating structural checks on the currently edited scene.", "scene", Dictionary(), Array(), _tool_scene_validate);

	Dictionary asset_props; asset_props["path"] = schema_string("Target res:// file whose reverse dependencies should be found.");
	Array asset_req; asset_req.push_back("path");
	_register_tool(p_server, "asset_find_references", "Find Asset References", "Find project files with a direct EditorFileSystem dependency on the target.", "assets", asset_props, asset_req, _tool_asset_find_references);
	Dictionary ui_props; ui_props["node_path"] = schema_string("Edited scene subtree to inspect (default '.').");
	_register_tool(p_server, "ui_inspect_layout", "Inspect UI Layout", "Inspect edited-scene Control local positions and sizes.", "ui", ui_props, Array(), _tool_ui_inspect_layout);
	Dictionary audio_props; audio_props["action"] = schema_string("'play' starts preview playback; 'stop' stops the current preview."); audio_props["path"] = schema_string("res:// audio stream path required for play."); audio_props["from_seconds"] = schema_number("Playback offset in seconds (default 0).");
	_register_tool(p_server, "audio_preview", "Preview Audio Asset", "Play an AudioStream in an editor-owned player, or stop the current preview.", "audio", audio_props, Array(), _tool_audio_preview);
	Dictionary physics_props; physics_props["from"] = schema_any("World-space ray start [x,y,z]."); physics_props["to"] = schema_any("World-space ray end [x,y,z]."); physics_props["collision_mask"] = schema_int("Collision mask (default all bits).");
	Array physics_req; physics_req.push_back("from"); physics_req.push_back("to");
	_register_tool(p_server, "physics_raycast", "Raycast Runtime Physics", "Raycast the running game's 3D physics space during a physics frame.", "physics", physics_props, physics_req, [](const Dictionary &a) { return _runtime("physics_raycast", a); });
	Dictionary nav_props; nav_props["from"] = schema_any("World-space start [x,y,z]."); nav_props["to"] = schema_any("World-space destination [x,y,z]."); nav_props["navigation_layers"] = schema_int("Navigation layer mask (default 1).");
	Array nav_req; nav_req.push_back("from"); nav_req.push_back("to");
	_register_tool(p_server, "navigation_get_path", "Get Runtime Navigation Path", "Query a path through the first active runtime NavigationServer3D map.", "navigation", nav_props, nav_req, [](const Dictionary &a) { return _runtime("navigation_get_path", a); });
	Dictionary shader_props; shader_props["path"] = schema_string("Optional res:// shader file."); shader_props["content"] = schema_string("Optional raw shader source.");
	_register_tool(p_server, "shader_validate", "Validate Shader", "Return shader source and compiler validation availability; never reports successful validation without compiler diagnostics.", "shaders", shader_props, Array(), _tool_shader_validate);

	Dictionary batch_props; batch_props["operations"] = schema_array("Ordered MCP tool calls (maximum 50).", schema_object("{tool, arguments}")); batch_props["wait"] = schema_bool("Wait for completion before returning (default false)."); batch_props["timeout_ms"] = schema_int("Optional bounded wait timeout in milliseconds (default 30000, max 120000).");
	Array batch_req; batch_req.push_back("operations");
	_register_tool(p_server, "mcp_execute_batch", "Execute MCP Batch", "Queue ordered bounded MCP tool calls and return an operation ID.", "mcp", batch_props, batch_req, _tool_mcp_execute_batch);
	Dictionary op_props; op_props["operation_id"] = schema_string("ID returned by mcp_execute_batch.");
	Array op_req; op_req.push_back("operation_id");
	_register_tool(p_server, "mcp_get_operation_status", "Get MCP Operation Status", "Read queued operation state and step results.", "mcp", op_props, op_req, _tool_mcp_get_operation_status);
	_register_tool(p_server, "mcp_cancel_operation", "Cancel MCP Operation", "Cancel a queued MCP operation at its next step boundary.", "mcp", op_props, op_req, _tool_mcp_cancel_operation);
	Dictionary checkpoint_props; checkpoint_props["name"] = schema_string("Checkpoint name.");
	_register_tool(p_server, "mcp_create_checkpoint", "Create Scene Checkpoint", "Create a packed snapshot of the currently edited scene only without changing the active scene.", "mcp", checkpoint_props, Array(), _tool_checkpoint_create);
	Dictionary restore_props = checkpoint_props;
	restore_props["checkpoint_path"] = schema_string("Existing checkpoint res:// path, for checkpoints created in a previous editor session.");
	Array restore_req; restore_req.push_back("name");
	_register_tool(p_server, "mcp_restore_checkpoint", "Restore Scene Checkpoint", "Open a checkpoint snapshot as the edited scene. This does not restore runtime state or other project files.", "mcp", restore_props, restore_req, _tool_checkpoint_restore);
	_register_tool(p_server, "mcp_get_capabilities", "Get MCP Capabilities", "List tools, runtime/editor separation, and declared limits.", "mcp", Dictionary(), Array(), _tool_mcp_get_capabilities);
}
