/**************************************************************************/
/*  mcp_runtime_debugger_plugin.cpp                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "mcp_runtime_debugger_plugin.h"

#include "../mcp_variant_codec.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_interface.h"

MCPRuntimeDebuggerPlugin *MCPRuntimeDebuggerPlugin::singleton = nullptr;

MCPRuntimeDebuggerPlugin::MCPRuntimeDebuggerPlugin() {
	singleton = this;
}

MCPRuntimeDebuggerPlugin::~MCPRuntimeDebuggerPlugin() {
	_cancel_all_pending("Debugger plugin destroyed.");
	if (singleton == this) {
		singleton = nullptr;
	}
}

bool MCPRuntimeDebuggerPlugin::has_capture(const String &p_capture) const {
	return p_capture == "mina_mcp";
}

void MCPRuntimeDebuggerPlugin::setup_session(int p_idx) {
	Ref<EditorDebuggerSession> sess = get_session(p_idx);
	if (sess.is_null()) {
		return;
	}
	sess->connect(SNAME("started"), callable_mp(this, &MCPRuntimeDebuggerPlugin::_on_session_started).bind(p_idx));
	sess->connect(SNAME("stopped"), callable_mp(this, &MCPRuntimeDebuggerPlugin::_on_session_stopped).bind(p_idx));
	if (sess->is_active()) {
		active_session_id = p_idx;
	}
}

void MCPRuntimeDebuggerPlugin::_on_session_started(int p_session_id) {
	active_session_id = p_session_id;
}

void MCPRuntimeDebuggerPlugin::_on_session_stopped(int p_session_id) {
	if (active_session_id == p_session_id) {
		active_session_id = -1;
	}
	_cancel_all_pending("Game stopped.");
}

void MCPRuntimeDebuggerPlugin::_cancel_all_pending(const String &p_reason) {
	for (KeyValue<int64_t, PendingCapture> &E : pending_captures) {
		E.value.done = true;
		E.value.ok = false;
		E.value.error = p_reason;
	}
	for (KeyValue<int64_t, PendingInputAck> &E : pending_inputs) {
		E.value.done = true;
		E.value.ok = false;
		E.value.error = p_reason;
	}
}

bool MCPRuntimeDebuggerPlugin::capture(const String &p_message, const Array &p_data, int p_session) {
	if (p_message == "mina_mcp:capture_result") {
		if (p_data.size() >= 6) {
			int64_t rid = int64_t(p_data[0]);
			if (PendingCapture *pc = pending_captures.getptr(rid)) {
				pc->ok = bool(p_data[1]);
				pc->width = int(p_data[2]);
				pc->height = int(p_data[3]);
				pc->file_path = String(p_data[4]);
				pc->error = String(p_data[5]);
				pc->done = true;
			}
		}
		return true;
	}
	if (p_message == "mina_mcp:input_ack") {
		if (p_data.size() >= 4) {
			int64_t rid = int64_t(p_data[0]);
			if (PendingInputAck *pi = pending_inputs.getptr(rid)) {
				pi->ok = bool(p_data[1]);
				pi->executed_steps = int(p_data[2]);
				pi->error = String(p_data[3]);
				pi->done = true;
			}
		}
		return true;
	}
	return false;
}

Ref<EditorDebuggerSession> MCPRuntimeDebuggerPlugin::_get_active_session() {
	if (active_session_id >= 0) {
		Ref<EditorDebuggerSession> s = get_session(active_session_id);
		if (s.is_valid() && s->is_active()) {
			return s;
		}
	}
	Array all = get_sessions();
	for (int i = 0; i < all.size(); i++) {
		Ref<EditorDebuggerSession> s = all[i];
		if (s.is_valid() && s->is_active()) {
			active_session_id = i;
			return s;
		}
	}
	return Ref<EditorDebuggerSession>();
}

void MCPRuntimeDebuggerPlugin::_pump_debugger_once() {
	EditorDebuggerNode *edn = EditorDebuggerNode::get_singleton();
	if (!edn) {
		return;
	}
	edn->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
	ScriptEditorDebugger *dbg = edn->get_current_debugger();
	if (dbg) {
		dbg->notification(Node::NOTIFICATION_PROCESS);
	}
	ScriptEditorDebugger *def_dbg = edn->get_default_debugger();
	if (def_dbg && def_dbg != dbg) {
		def_dbg->notification(Node::NOTIFICATION_PROCESS);
	}
}

bool MCPRuntimeDebuggerPlugin::has_active_session() {
	_pump_debugger_once();
	return _get_active_session().is_valid();
}

bool MCPRuntimeDebuggerPlugin::wait_for_active_session(int p_timeout_msec) {
	if (has_active_session()) {
		return true;
	}
	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + uint64_t(MAX(0, p_timeout_msec));
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		_pump_debugger_once();
		if (_get_active_session().is_valid()) {
			return true;
		}
		if (EditorInterface::get_singleton() && !EditorInterface::get_singleton()->is_playing_scene()) {
			return false;
		}
		OS::get_singleton()->delay_usec(5000);
	}
	return _get_active_session().is_valid();
}

void MCPRuntimeDebuggerPlugin::send_release_all() {
	Ref<EditorDebuggerSession> sess = _get_active_session();
	if (sess.is_valid()) {
		sess->send_message("mina_mcp:release_all", Array());
		_pump_debugger_once();
	}
	_cancel_all_pending("Inputs cancelled.");
}

Dictionary MCPRuntimeDebuggerPlugin::request_capture_runtime(int p_max_width, const String &p_save_path, int p_timeout_msec) {
	if (!wait_for_active_session(2500)) {
		return mcp::make_error("No active running game session. Start the game with game_play first.", "game_not_running");
	}
	Ref<EditorDebuggerSession> sess = _get_active_session();
	if (sess.is_null()) {
		return mcp::make_error("No active running game session.", "game_not_running");
	}

	int64_t rid = next_request_id++;
	PendingCapture pc;
	pending_captures[rid] = pc;

	String abs_save;
	bool user_requested_save = !p_save_path.is_empty();
	if (user_requested_save) {
		String norm = mcp::normalize_res_path(p_save_path);
		if (!mcp::is_safe_res_path(norm)) {
			pending_captures.erase(rid);
			return mcp::make_error("Parameter 'save_path' must be a valid res:// path.", "invalid_path");
		}
		abs_save = ProjectSettings::get_singleton()->globalize_path(norm);
	}

	Array msg_args;
	msg_args.push_back(rid);
	msg_args.push_back(CLAMP(p_max_width, 128, 2560));
	msg_args.push_back(abs_save);
	sess->send_message("mina_mcp:capture_runtime", msg_args);

	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + uint64_t(CLAMP(p_timeout_msec, 500, 30000));
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		_pump_debugger_once();
		PendingCapture *cur = pending_captures.getptr(rid);
		if (!cur || cur->done) {
			break;
		}
		if (EditorInterface::get_singleton() && !EditorInterface::get_singleton()->is_playing_scene()) {
			cur->done = true;
			cur->ok = false;
			cur->error = "Game stopped before screenshot completed.";
			break;
		}
		OS::get_singleton()->delay_usec(4000);
	}

	PendingCapture result;
	if (pending_captures.has(rid)) {
		result = pending_captures[rid];
		pending_captures.erase(rid);
	}

	if (!result.done) {
		return mcp::make_error("Timed out waiting for running game to capture screenshot.", "timeout");
	}
	if (!result.ok) {
		return mcp::make_error(result.error.is_empty() ? String("Runtime capture failed.") : result.error, "capture_failed");
	}

	PackedByteArray png_bytes = FileAccess::get_file_as_bytes(result.file_path);
	if (!user_requested_save && !result.file_path.is_empty()) {
		DirAccess::remove_absolute(result.file_path);
	}
	if (png_bytes.is_empty()) {
		return mcp::make_error("Captured PNG file could not be read.", "io_error");
	}

	String b64 = CryptoCore::b64_encode_str(png_bytes.ptr(), png_bytes.size());
	Dictionary meta;
	meta["width"] = result.width;
	meta["height"] = result.height;
	meta["bytes"] = png_bytes.size();
	if (EditorInterface::get_singleton()) {
		meta["playing_scene"] = EditorInterface::get_singleton()->get_playing_scene();
	}
	if (user_requested_save) {
		meta["saved_path"] = mcp::normalize_res_path(p_save_path);
	}
	return mcp::make_image_result(b64, meta);
}

Dictionary MCPRuntimeDebuggerPlugin::request_send_input(const Dictionary &p_payload, bool p_wait_for_completion, int p_timeout_msec) {
	if (!wait_for_active_session(2500)) {
		return mcp::make_error("No active running game session. Start the game with game_play first.", "game_not_running");
	}
	Ref<EditorDebuggerSession> sess = _get_active_session();
	if (sess.is_null()) {
		return mcp::make_error("No active running game session.", "game_not_running");
	}

	int64_t rid = next_request_id++;
	PendingInputAck ack;
	pending_inputs[rid] = ack;

	Array msg_args;
	msg_args.push_back(rid);
	msg_args.push_back(JSON::stringify(p_payload));
	msg_args.push_back(p_wait_for_completion);
	sess->send_message("mina_mcp:send_input", msg_args);

	uint64_t deadline = OS::get_singleton()->get_ticks_msec() + uint64_t(CLAMP(p_timeout_msec, 500, 120000));
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		_pump_debugger_once();
		PendingInputAck *cur = pending_inputs.getptr(rid);
		if (!cur || cur->done) {
			break;
		}
		if (EditorInterface::get_singleton() && !EditorInterface::get_singleton()->is_playing_scene()) {
			cur->done = true;
			cur->ok = false;
			cur->error = "Game stopped while executing input.";
			break;
		}
		OS::get_singleton()->delay_usec(4000);
	}

	PendingInputAck result;
	if (pending_inputs.has(rid)) {
		result = pending_inputs[rid];
		pending_inputs.erase(rid);
	}

	if (!result.done) {
		return mcp::make_error("Timed out waiting for game process to acknowledge input.", "timeout");
	}
	if (!result.ok) {
		return mcp::make_error(result.error.is_empty() ? String("Input execution failed.") : result.error, "input_failed");
	}

	Dictionary out;
	out["executed_steps"] = result.executed_steps;
	out["waited_for_completion"] = p_wait_for_completion;
	return mcp::make_ok(out);
}
