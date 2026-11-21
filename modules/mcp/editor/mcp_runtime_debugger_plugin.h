/**************************************************************************/
/*  mcp_runtime_debugger_plugin.h                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "editor/debugger/editor_debugger_plugin.h"

class MCPRuntimeDebuggerPlugin : public EditorDebuggerPlugin {
	GDCLASS(MCPRuntimeDebuggerPlugin, EditorDebuggerPlugin);

	struct PendingCapture {
		bool done = false;
		bool ok = false;
		int width = 0;
		int height = 0;
		String file_path;
		String error;
	};

	struct PendingInputAck {
		bool done = false;
		bool ok = false;
		int executed_steps = 0;
		String error;
	};

	static MCPRuntimeDebuggerPlugin *singleton;

	int active_session_id = -1;
	int64_t next_request_id = 1;
	HashMap<int64_t, PendingCapture> pending_captures;
	HashMap<int64_t, PendingInputAck> pending_inputs;

	void _on_session_started(int p_session_id);
	void _on_session_stopped(int p_session_id);
	Ref<EditorDebuggerSession> _get_active_session();
	void _pump_debugger_once();
	void _cancel_all_pending(const String &p_reason);

protected:
	static void _bind_methods() {}

public:
	static MCPRuntimeDebuggerPlugin *get_singleton() { return singleton; }

	bool has_capture(const String &p_capture) const override;
	bool capture(const String &p_message, const Array &p_data, int p_session) override;
	void setup_session(int p_idx) override;

	bool has_active_session();
	bool wait_for_active_session(int p_timeout_msec = 3000);
	void send_release_all();

	Dictionary request_capture_runtime(int p_max_width, const String &p_save_path, int p_timeout_msec = 5000);
	Dictionary request_send_input(const Dictionary &p_payload, bool p_wait_for_completion, int p_timeout_msec = 10000);

	MCPRuntimeDebuggerPlugin();
	~MCPRuntimeDebuggerPlugin();
};
