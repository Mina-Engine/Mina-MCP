/**************************************************************************/
/*  mcp_server.h                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#pragma once

#include "core/error/error_macros.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"

#include <functional>

class MinaMCPServer : public Object {
	GDCLASS(MinaMCPServer, Object);

public:
	enum ServerState {
		STATE_STOPPED = 0,
		STATE_RUNNING = 1,
		STATE_ERROR = 2,
	};

	using ToolHandler = std::function<Dictionary(const Dictionary &)>;
	using ResourceReader = std::function<Dictionary()>;
	using ProviderCallback = void (*)(MinaMCPServer *);

	struct ToolDefinition {
		String name;
		String title;
		String description;
		String category;
		Dictionary input_schema;
		Dictionary annotations;
		ToolHandler handler;
		Callable script_handler;
	};

	struct ResourceDefinition {
		String uri;
		String name;
		String description;
		String mime_type = "application/json";
		ResourceReader reader;
		Callable script_reader;
	};

	struct SessionInfo {
		String id;
		String client_name;
		String client_version;
		String protocol_version;
		bool initialized = false;
		uint64_t connected_msec = 0;
		uint64_t last_activity_msec = 0;
		uint64_t request_count = 0;
		String transport = "streamable_http";
	};

	struct LogEntry {
		uint64_t id = 0;
		uint64_t timestamp_msec = 0;
		String level; // "info", "warning", "error"
		String source;
		String message;
	};

	struct ToolActivityEntry {
		uint64_t id = 0;
		uint64_t timestamp_msec = 0;
		String tool_name;
		bool ok = true;
		String summary;
		double duration_ms = 0.0;
	};

	static int port_override;
	static bool disabled_override;
	static constexpr int DEFAULT_PORT = 6800;

private:
	struct HTTPConnection {
		Ref<StreamPeerTCP> peer;
		PackedByteArray read_buffer;
		uint64_t created_msec = 0;
		bool is_sse = false;
		String sse_session_id;
		bool close_after_response = true;
	};

	static MinaMCPServer *singleton;
	static Vector<ProviderCallback> provider_callbacks;

	ServerState state = STATE_STOPPED;
	String host = "127.0.0.1";
	int port = DEFAULT_PORT;
	bool allow_remote = false;
	String last_error;
	String mode_name = "editor"; // "editor" or "project_manager"
	bool polling = false;
	uint64_t total_requests = 0;
	uint64_t started_msec = 0;

	Ref<TCPServer> tcp_server;
	Vector<HTTPConnection> connections;
	HashMap<String, SessionInfo> sessions;

	HashMap<String, ToolDefinition> tools;
	Vector<String> tool_order;
	HashMap<String, ResourceDefinition> resources;
	Vector<String> resource_order;

	// Captured engine logs & errors (thread-safe).
	mutable Mutex log_mutex;
	Vector<LogEntry> log_entries;
	uint64_t next_log_id = 1;
	static constexpr int MAX_LOG_ENTRIES = 2000;
	bool log_handlers_installed = false;
	PrintHandlerList print_handler;
	ErrorHandlerList error_handler;

	// Tool call activity log for UI visibility.
	Vector<ToolActivityEntry> activity_entries;
	uint64_t next_activity_id = 1;
	static constexpr int MAX_ACTIVITY_ENTRIES = 200;

	uint64_t next_session_seq = 1;

	static void _print_handler(void *p_userdata, const String &p_string, bool p_error, bool p_rich);
	static void _error_handler(void *p_userdata, const char *p_func, const char *p_file, int p_line, const char *p_error, const char *p_errorexp, bool p_editor_notify, ErrorHandlerType p_type);
	void _install_log_handlers();
	void _remove_log_handlers();
	void _push_log(const String &p_level, const String &p_source, const String &p_message);

	static bool _is_loopback_host(const String &p_host);
	bool _is_allowed_http_host(const String &p_host_header) const;
	bool _is_allowed_origin(const String &p_origin_header) const;
	String _generate_session_id();

	void _accept_new_connections();
	void _poll_connections();
	bool _try_process_http_request(HTTPConnection &p_conn);
	void _send_http_response(const Ref<StreamPeerTCP> &p_peer, int p_status_code, const String &p_status_text, const String &p_content_type, const PackedByteArray &p_body, const HashMap<String, String> &p_extra_headers = HashMap<String, String>());
	void _send_http_json(const Ref<StreamPeerTCP> &p_peer, int p_status_code, const String &p_status_text, const Variant &p_json, const HashMap<String, String> &p_extra_headers = HashMap<String, String>());
	void _send_sse_headers(const Ref<StreamPeerTCP> &p_peer, const String &p_session_id);
	bool _send_sse_event(const Ref<StreamPeerTCP> &p_peer, const String &p_event, const String &p_data);
	void _broadcast_sse_to_session(const String &p_session_id, const String &p_event, const String &p_data);

	// MCP JSON-RPC 2.0 handlers.
	Variant _handle_jsonrpc_message(const Variant &p_msg, const String &p_session_id, String &r_new_session_id, bool &r_has_response);
	Dictionary _make_jsonrpc_result(const Variant &p_id, const Variant &p_result) const;
	Dictionary _make_jsonrpc_error(const Variant &p_id, int p_code, const String &p_message, const Variant &p_data = Variant()) const;
	Dictionary _mcp_initialize(const Dictionary &p_params, const String &p_existing_session_id, String &r_session_id);
	Dictionary _mcp_tools_list(const Dictionary &p_params) const;
	Dictionary _mcp_tools_call(const Dictionary &p_params);
	Dictionary _mcp_resources_list(const Dictionary &p_params) const;
	Dictionary _mcp_resources_read(const Dictionary &p_params) const;
	Dictionary _mcp_prompts_list(const Dictionary &p_params) const;
	Dictionary _build_health_payload() const;

	void _record_activity(const String &p_tool_name, bool p_ok, const String &p_summary, double p_duration_ms);
	void _notify_status_changed();
	void _notify_sessions_changed();

protected:
	static void _bind_methods();

public:
	static MinaMCPServer *get_singleton() { return singleton; }

	// Extensibility: register callbacks from other engine modules (e.g. horror module).
	static void add_provider_callback(ProviderCallback p_callback);
	void invoke_provider_callbacks();

	// Lifecycle management.
	Error start(int p_port = DEFAULT_PORT, const String &p_host = "127.0.0.1", bool p_allow_remote = false);
	void stop();
	Error restart();
	void poll();

	ServerState get_state() const { return state; }
	bool is_running() const { return state == STATE_RUNNING; }
	String get_host() const { return host; }
	int get_port() const { return port; }
	bool is_remote_allowed() const { return allow_remote; }
	String get_endpoint_url() const;
	String get_last_error() const { return last_error; }
	void set_mode_name(const String &p_mode) { mode_name = p_mode; }
	String get_mode_name() const { return mode_name; }
	uint64_t get_total_requests() const { return total_requests; }
	uint64_t get_uptime_msec() const;

	// Tool & resource registration API (C++).
	void register_tool(const ToolDefinition &p_tool);
	void unregister_tool(const String &p_name);
	bool has_tool(const String &p_name) const;
	int get_tool_count() const { return tool_order.size(); }
	PackedStringArray get_tool_names() const;

	void register_resource(const ResourceDefinition &p_resource);
	void unregister_resource(const String &p_uri);
	int get_resource_count() const { return resource_order.size(); }

	// Tool & resource registration API (GDScript / GDExtension via ClassDB).
	void register_script_tool(const String &p_name, const String &p_description, const Dictionary &p_input_schema, const Callable &p_handler, const String &p_category = "custom");
	void register_script_resource(const String &p_uri, const String &p_name, const String &p_description, const String &p_mime_type, const Callable &p_reader);

	// Direct invocation (useful for internal tests or chaining).
	Dictionary execute_tool_direct(const String &p_name, const Dictionary &p_arguments);

	// Connection, log, and activity telemetry.
	Array get_sessions() const;
	int get_session_count() const { return sessions.size(); }
	Array get_logs(const String &p_level = "all", uint64_t p_since_id = 0, int p_limit = 200, const String &p_search = String()) const;
	void clear_logs();
	Array get_recent_activity(int p_limit = 50) const;
	void clear_activity();
	Dictionary get_status_dictionary() const;

	MinaMCPServer();
	~MinaMCPServer();
};

VARIANT_ENUM_CAST(MinaMCPServer::ServerState);
