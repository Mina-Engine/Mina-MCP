/**************************************************************************/
/*  mcp_tools_runtime_export.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "mcp_builtin_tools.h"

#include "../mcp_server.h"
#include "../mcp_variant_codec.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/export/editor_export.h"
#include "editor/export/editor_export_platform.h"
#include "editor/export/editor_export_preset.h"
#include "editor/run/editor_run_bar.h"
#include "scene/main/viewport.h"
#include "scene/resources/texture.h"

namespace {

bool _require_editor_runtime(Dictionary &r_err) {
	if (!EditorInterface::get_singleton() || !EditorNode::get_singleton()) {
		r_err = mcp::make_error("This tool requires an open project in the Mina Engine Editor.", "editor_required");
		return false;
	}
	return true;
}

Dictionary _tool_game_play(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	EditorInterface *ei = EditorInterface::get_singleton();
	String mode = String(p_args.get("mode", "main")).strip_edges().to_lower();
	String scene_path = String(p_args.get("scene_path", "")).strip_edges();
	bool save_before_play = bool(p_args.get("save_before_play", true));

	if (save_before_play) {
		ei->save_all_scenes();
	}

	if (!scene_path.is_empty() || mode == "custom") {
		String norm = mcp::normalize_res_path(scene_path);
		if (!mcp::is_safe_res_path(norm) || !FileAccess::exists(norm)) {
			return mcp::make_error(vformat("Custom scene does not exist: '%s'.", norm), "not_found");
		}
		ei->play_custom_scene(norm);
	} else if (mode == "current") {
		if (!ei->get_edited_scene_root()) {
			return mcp::make_error("No active edited scene to play.", "no_scene");
		}
		ei->play_current_scene();
	} else {
		String main_scene = String(GLOBAL_GET("application/run/main_scene"));
		if (main_scene.is_empty()) {
			return mcp::make_error("Project has no main_scene configured. Set 'application/run/main_scene' or pass mode='current' / scene_path.", "no_main_scene");
		}
		ei->play_main_scene();
	}

	Dictionary out;
	out["mode"] = mode;
	out["is_playing"] = ei->is_playing_scene();
	out["playing_scene"] = ei->get_playing_scene();
	return mcp::make_ok(out);
}

Dictionary _tool_game_stop(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	EditorInterface::get_singleton()->stop_playing_scene();
	Dictionary out;
	out["is_playing"] = EditorInterface::get_singleton()->is_playing_scene();
	return mcp::make_ok(out);
}

Dictionary _tool_game_get_status(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	EditorInterface *ei = EditorInterface::get_singleton();
	Dictionary out;
	out["is_playing"] = ei->is_playing_scene();
	out["playing_scene"] = ei->get_playing_scene();
	if (EditorRunBar::get_singleton()) {
		out["current_pid"] = (int64_t)EditorRunBar::get_singleton()->get_current_process();
	}
	return mcp::make_ok(out);
}

Dictionary _tool_debug_get_logs(const Dictionary &p_args) {
	MinaMCPServer *server = MinaMCPServer::get_singleton();
	if (!server) {
		return mcp::make_error("MCP server is unavailable.", "unavailable");
	}
	String level = String(p_args.get("level", "all"));
	uint64_t since_id = uint64_t(int64_t(p_args.get("since_id", 0)));
	int limit = CLAMP(int(p_args.get("limit", 100)), 1, 1000);
	String search = String(p_args.get("search", ""));

	Array logs = server->get_logs(level, since_id, limit, search);
	Dictionary out;
	out["count"] = logs.size();
	out["logs"] = logs;
	return mcp::make_ok(out);
}

Dictionary _tool_debug_clear_logs(const Dictionary &p_args) {
	if (MinaMCPServer::get_singleton()) {
		MinaMCPServer::get_singleton()->clear_logs();
	}
	return mcp::make_ok();
}

Dictionary _tool_debug_set_breakpoint(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	if (!EditorDebuggerNode::get_singleton()) {
		return mcp::make_error("EditorDebuggerNode is not available.", "unavailable");
	}
	String path = mcp::normalize_res_path(String(p_args.get("script_path", "")));
	int line = int(p_args.get("line", 1));
	bool enabled = bool(p_args.get("enabled", true));
	if (!mcp::is_safe_res_path(path)) {
		return mcp::make_error("Parameter 'script_path' must be a valid res:// path.", "invalid_path");
	}
	if (line < 1) {
		return mcp::make_error("Parameter 'line' must be >= 1.", "invalid_argument");
	}
	EditorDebuggerNode::get_singleton()->set_breakpoint(path, line, enabled);
	Dictionary out;
	out["script_path"] = path;
	out["line"] = line;
	out["enabled"] = enabled;
	return mcp::make_ok(out);
}

Dictionary _tool_debug_capture_viewport(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	String vp_type = String(p_args.get("viewport", "3d")).strip_edges().to_lower();
	int max_width = CLAMP(int(p_args.get("max_width", 1024)), 128, 2048);
	String save_path = String(p_args.get("save_path", "")).strip_edges();

	SubViewport *vp = (vp_type == "2d")
			? EditorInterface::get_singleton()->get_editor_viewport_2d()
			: EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	if (!vp) {
		return mcp::make_error(vformat("Editor %s viewport is not available.", vp_type), "unavailable");
	}
	Ref<ViewportTexture> tex = vp->get_texture();
	if (tex.is_null()) {
		return mcp::make_error("Viewport texture is not ready.", "unavailable");
	}
	Ref<Image> img = tex->get_image();
	if (img.is_null() || img->is_empty()) {
		return mcp::make_error("Viewport image is empty (ensure the 3D/2D viewport is visible).", "empty_image");
	}

	if (img->get_width() > max_width) {
		int new_h = MAX(1, int(double(img->get_height()) * double(max_width) / double(img->get_width())));
		img->resize(max_width, new_h, Image::INTERPOLATE_BILINEAR);
	}

	if (!save_path.is_empty()) {
		String norm = mcp::normalize_res_path(save_path);
		if (!mcp::is_safe_res_path(norm)) {
			return mcp::make_error("Parameter 'save_path' must be a valid res:// path.", "invalid_path");
		}
		String abs = ProjectSettings::get_singleton()->globalize_path(norm);
		DirAccess::make_dir_recursive_absolute(abs.get_base_dir());
		Error serr = img->save_png(abs);
		if (serr != OK) {
			return mcp::make_error(vformat("Failed to save screenshot to '%s' (error %d).", norm, (int)serr), "io_error");
		}
		Dictionary out;
		out["saved_path"] = norm;
		out["width"] = img->get_width();
		out["height"] = img->get_height();
		return mcp::make_ok(out);
	}

	PackedByteArray png = img->save_png_to_buffer();
	if (png.is_empty()) {
		return mcp::make_error("Failed to encode viewport screenshot to PNG.", "encode_failed");
	}
	String b64 = CryptoCore::b64_encode_str(png.ptr(), png.size());
	Dictionary meta;
	meta["viewport"] = vp_type;
	meta["width"] = img->get_width();
	meta["height"] = img->get_height();
	meta["bytes"] = png.size();
	return mcp::make_image_result(b64, meta);
}

Dictionary _tool_export_list_platforms(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	EditorExport *ee = EditorExport::get_singleton();
	if (!ee) {
		return mcp::make_error("EditorExport is not available.", "unavailable");
	}
	Array platforms;
	for (int i = 0; i < ee->get_export_platform_count(); i++) {
		Ref<EditorExportPlatform> plat = ee->get_export_platform(i);
		if (plat.is_null()) {
			continue;
		}
		Dictionary pd;
		pd["index"] = i;
		pd["name"] = plat->get_name();
		pd["os_name"] = plat->get_os_name();
		platforms.push_back(pd);
	}
	Dictionary out;
	out["platforms"] = platforms;
	out["count"] = platforms.size();
	return mcp::make_ok(out);
}

Dictionary _tool_export_list_presets(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	EditorExport *ee = EditorExport::get_singleton();
	if (!ee) {
		return mcp::make_error("EditorExport is not available.", "unavailable");
	}
	Array presets;
	for (int i = 0; i < ee->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> preset = ee->get_export_preset(i);
		if (preset.is_null()) {
			continue;
		}
		Ref<EditorExportPlatform> plat = preset->get_platform();
		Dictionary pd;
		pd["index"] = i;
		pd["name"] = preset->get_name();
		pd["platform"] = plat.is_valid() ? plat->get_name() : String();
		pd["export_path"] = preset->get_export_path();
		pd["runnable"] = preset->is_runnable();
		if (plat.is_valid()) {
			String can_err;
			bool missing_templates = false;
			pd["can_export"] = plat->can_export(preset, can_err, missing_templates, false);
			pd["missing_templates"] = missing_templates;
			if (!can_err.is_empty()) {
				pd["configuration_error"] = can_err;
			}
		}
		presets.push_back(pd);
	}
	Dictionary out;
	out["presets"] = presets;
	out["count"] = presets.size();
	return mcp::make_ok(out);
}

Dictionary _tool_export_create_preset(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	EditorExport *ee = EditorExport::get_singleton();
	if (!ee) {
		return mcp::make_error("EditorExport is not available.", "unavailable");
	}
	String platform_name = String(p_args.get("platform", "Windows Desktop")).strip_edges();
	String preset_name = String(p_args.get("name", platform_name)).strip_edges();
	String export_path = String(p_args.get("export_path", "")).strip_edges();
	bool runnable = bool(p_args.get("runnable", true));

	int p_idx = ee->get_export_platform_index_by_name(platform_name);
	if (p_idx < 0) {
		return mcp::make_error(vformat("Export platform '%s' not found. Call export_list_platforms to see available platforms.", platform_name), "platform_not_found");
	}
	Ref<EditorExportPlatform> plat = ee->get_export_platform(p_idx);
	Ref<EditorExportPreset> preset = plat->create_preset();
	if (preset.is_null()) {
		return mcp::make_error("Failed to create export preset.", "create_failed");
	}

	preset->set_name(preset_name);
	if (!export_path.is_empty()) {
		preset->set_export_path(export_path);
	}

	if (p_args.has("options") && p_args["options"].get_type() == Variant::DICTIONARY) {
		Dictionary opts = p_args["options"];
		Array keys = opts.keys();
		for (int i = 0; i < keys.size(); i++) {
			String k = String(keys[i]);
			preset->set(k, mcp::json_to_variant(opts[k]));
		}
	}

	ee->add_export_preset(preset);
	if (runnable) {
		ee->set_runnable_preset(preset);
	}

	Dictionary out;
	out["name"] = preset->get_name();
	out["platform"] = plat->get_name();
	out["export_path"] = preset->get_export_path();
	out["preset_index"] = ee->get_export_preset_count() - 1;
	return mcp::make_ok(out);
}

Dictionary _tool_export_project(const Dictionary &p_args) {
	Dictionary err;
	if (!_require_editor_runtime(err)) {
		return err;
	}
	EditorExport *ee = EditorExport::get_singleton();
	if (!ee) {
		return mcp::make_error("EditorExport is not available.", "unavailable");
	}
	String preset_name = String(p_args.get("preset", "")).strip_edges();
	String output_path = String(p_args.get("output_path", "")).strip_edges();
	String mode = String(p_args.get("mode", "release")).strip_edges().to_lower();
	String format = String(p_args.get("format", "project")).strip_edges().to_lower(); // "project", "pack", "zip"

	Ref<EditorExportPreset> target;
	for (int i = 0; i < ee->get_export_preset_count(); i++) {
		Ref<EditorExportPreset> p = ee->get_export_preset(i);
		if (p.is_valid() && (preset_name.is_empty() || p->get_name() == preset_name)) {
			target = p;
			break;
		}
	}
	if (target.is_null()) {
		return mcp::make_error(vformat("Export preset '%s' not found. Use export_list_presets or export_create_preset first.", preset_name), "preset_not_found");
	}

	Ref<EditorExportPlatform> plat = target->get_platform();
	if (plat.is_null()) {
		return mcp::make_error("Export preset has no valid platform.", "invalid_preset");
	}

	String final_path = output_path.is_empty() ? target->get_export_path() : output_path;
	if (final_path.is_empty()) {
		return mcp::make_error("No output_path specified and preset has no default export_path.", "invalid_path");
	}
	if (final_path.begins_with("res://")) {
		final_path = ProjectSettings::get_singleton()->globalize_path(final_path);
	}
	DirAccess::make_dir_recursive_absolute(final_path.get_base_dir());

	bool is_debug = (mode == "debug");

	// Save scenes & run pre-export build callbacks first.
	EditorInterface::get_singleton()->save_all_scenes();
	if (!EditorNode::get_singleton()->call_build()) {
		return mcp::make_error("Pre-export build step failed.", "build_failed");
	}

	plat->clear_messages();
	Error exp_err = OK;
	if (format == "pack" || final_path.ends_with(".pck")) {
		exp_err = plat->export_pack(target, is_debug, final_path);
	} else if (format == "zip" || final_path.ends_with(".zip")) {
		exp_err = plat->export_zip(target, is_debug, final_path);
	} else {
		String config_err;
		bool missing_templates = false;
		if (!plat->can_export(target, config_err, missing_templates, is_debug)) {
			Dictionary details;
			details["missing_templates"] = missing_templates;
			return mcp::make_error(vformat("Cannot export preset '%s': %s", target->get_name(), config_err), "export_not_ready", details);
		}
		exp_err = plat->export_project(target, is_debug, final_path, 0, false);
	}

	Array messages;
	for (int i = 0; i < plat->get_message_count(); i++) {
		EditorExportPlatform::ExportMessage msg = plat->get_message(i);
		Dictionary md;
		md["category"] = msg.category;
		md["text"] = msg.text;
		md["type"] = (int)msg.msg_type;
		messages.push_back(md);
	}

	if (exp_err != OK) {
		Dictionary details;
		details["error_code"] = (int)exp_err;
		details["messages"] = messages;
		return mcp::make_error(vformat("Export failed (error %d: %s).", (int)exp_err, error_names[exp_err]), "export_failed", details);
	}

	Dictionary out;
	out["preset"] = target->get_name();
	out["platform"] = plat->get_name();
	out["mode"] = is_debug ? "debug" : "release";
	out["format"] = format;
	out["output_path"] = final_path;
	out["messages"] = messages;
	Ref<FileAccess> f = FileAccess::open(final_path, FileAccess::READ);
	if (f.is_valid()) {
		out["size_bytes"] = (int64_t)f->get_length();
	}
	return mcp::make_ok(out);
}

} // namespace

void mcp_register_runtime_export_tools(MinaMCPServer *p_server) {
	using namespace mcp;

	{
		Dictionary props;
		Array modes;
		modes.push_back("main");
		modes.push_back("current");
		modes.push_back("custom");
		props["mode"] = schema_string("'main', 'current', or 'custom' (default: 'main').", modes);
		props["scene_path"] = schema_string("res:// scene path when mode is 'custom'.");
		props["save_before_play"] = schema_bool("Save all open scenes before running (default: true).");

		MinaMCPServer::ToolDefinition t;
		t.name = "game_play";
		t.title = "Play Game";
		t.description = "Launch the game (main scene, current edited scene, or a custom scene) directly from Mina Engine.";
		t.category = "runtime";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_game_play;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "game_stop";
		t.title = "Stop Game";
		t.description = "Stop the currently running game instance.";
		t.category = "runtime";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_game_stop;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "game_get_status";
		t.title = "Get Game Playback Status";
		t.description = "Check whether a game is currently running, which scene is playing, and its process ID.";
		t.category = "runtime";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_game_get_status;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		Array levels;
		levels.push_back("all");
		levels.push_back("info");
		levels.push_back("warning");
		levels.push_back("error");
		props["level"] = schema_string("Filter log level (default: 'all').", levels);
		props["since_id"] = schema_int("Only return logs with id > since_id (for incremental polling).");
		props["limit"] = schema_int("Maximum log entries to return (default: 100, max: 1000).");
		props["search"] = schema_string("Optional case-insensitive substring filter.");

		MinaMCPServer::ToolDefinition t;
		t.name = "debug_get_logs";
		t.title = "Get Engine Logs & Errors";
		t.description = "Read captured engine stdout logs, warnings, script errors, and C++ errors.";
		t.category = "debug";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_debug_get_logs;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "debug_clear_logs";
		t.title = "Clear Captured Logs";
		t.description = "Clear the MCP server's captured engine log buffer.";
		t.category = "debug";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_debug_clear_logs;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["script_path"] = schema_string("res:// script path.");
		props["line"] = schema_int("1-based line number.");
		props["enabled"] = schema_bool("true to set breakpoint, false to remove it (default: true).");
		Array req;
		req.push_back("script_path");
		req.push_back("line");

		MinaMCPServer::ToolDefinition t;
		t.name = "debug_set_breakpoint";
		t.title = "Set Script Breakpoint";
		t.description = "Set or clear a debugger breakpoint on a script line in EditorDebuggerNode.";
		t.category = "debug";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_debug_set_breakpoint;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		Array vps;
		vps.push_back("3d");
		vps.push_back("2d");
		props["viewport"] = schema_string("Editor viewport to capture: '3d' or '2d' (default: '3d').", vps);
		props["max_width"] = schema_int("Maximum image width in pixels (default: 1024).");
		props["save_path"] = schema_string("Optional res:// PNG path to save instead of returning inline MCP image.");

		MinaMCPServer::ToolDefinition t;
		t.name = "debug_capture_viewport";
		t.title = "Capture Editor Viewport Screenshot";
		t.description = "Capture a PNG image of the live 3D or 2D editor viewport so visual AI agents can inspect the scene directly.";
		t.category = "debug";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_debug_capture_viewport;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "export_list_platforms";
		t.title = "List Export Platforms";
		t.description = "List all registered export platforms available in Mina Engine (e.g. Windows Desktop, Linux, macOS, Web).";
		t.category = "export";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_export_list_platforms;
		p_server->register_tool(t);
	}
	{
		MinaMCPServer::ToolDefinition t;
		t.name = "export_list_presets";
		t.title = "List Export Presets";
		t.description = "List configured export presets, their target platforms, export paths, and template readiness.";
		t.category = "export";
		t.input_schema = make_tool_schema(Dictionary());
		t.handler = _tool_export_list_presets;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["platform"] = schema_string("Target export platform name (e.g. 'Windows Desktop', 'Linux', 'Web').");
		props["name"] = schema_string("Preset display name.");
		props["export_path"] = schema_string("Default output executable/archive path.");
		props["runnable"] = schema_bool("Mark preset as runnable for this platform (default: true).");
		props["options"] = schema_object("Optional platform-specific preset option overrides.");
		Array req;
		req.push_back("platform");

		MinaMCPServer::ToolDefinition t;
		t.name = "export_create_preset";
		t.title = "Create Export Preset";
		t.description = "Create and save a new export preset in export_presets.cfg.";
		t.category = "export";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_export_create_preset;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["preset"] = schema_string("Name of the export preset (defaults to first preset if omitted).");
		props["output_path"] = schema_string("Destination output file path (overrides preset's default export_path).");
		Array modes;
		modes.push_back("release");
		modes.push_back("debug");
		props["mode"] = schema_string("'release' or 'debug' (default: 'release').", modes);
		Array formats;
		formats.push_back("project");
		formats.push_back("pack");
		formats.push_back("zip");
		props["format"] = schema_string("'project' (full build), 'pack' (.pck), or 'zip' (.zip) (default: 'project').", formats);

		MinaMCPServer::ToolDefinition t;
		t.name = "export_project";
		t.title = "Build & Export Project";
		t.description = "Run pre-export build steps and export the project as a full executable build, .pck pack, or .zip archive.";
		t.category = "export";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_export_project;
		p_server->register_tool(t);
	}
}
