/**************************************************************************/
/*  mcp_server.cpp                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "mcp_server.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/ip_address.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/version.h"
#include "mcp_variant_codec.h"

namespace {

constexpr int64_t MAX_HTTP_HEADER_BYTES = 64 * 1024;
constexpr int64_t MAX_HTTP_BODY_BYTES = 16 * 1024 * 1024;
constexpr uint64_t IDLE_CONNECTION_TIMEOUT_MSEC = 30000;
constexpr int MAX_CONCURRENT_CONNECTIONS = 32;

int _find_bytes(const PackedByteArray &p_haystack, const char *p_needle, int p_from = 0) {
	const int needle_len = (int)strlen(p_needle);
	const int hay_len = p_haystack.size();
	if (needle_len <= 0 || hay_len < needle_len || p_from < 0) {
		return -1;
	}
	const uint8_t *ptr = p_haystack.ptr();
	for (int i = p_from; i <= hay_len - needle_len; i++) {
		if (memcmp(ptr + i, p_needle, needle_len) == 0) {
			return i;
		}
	}
	return -1;
}

bool _decode_chunked_body(const uint8_t *p_data, int p_len, PackedByteArray &r_out, int &r_consumed) {
	r_out.clear();
	int pos = 0;
	while (pos < p_len) {
		int line_end = -1;
		for (int i = pos; i + 1 < p_len; i++) {
			if (p_data[i] == '\r' && p_data[i + 1] == '\n') {
				line_end = i;
				break;
			}
		}
		if (line_end < 0) {
			return false; // Incomplete chunk size line.
		}
		String size_line = String::utf8((const char *)(p_data + pos), line_end - pos).strip_edges();
		int semi = size_line.find_char(';');
		if (semi >= 0) {
			size_line = size_line.substr(0, semi).strip_edges();
		}
		int64_t chunk_size = size_line.hex_to_int();
		if (chunk_size < 0 || chunk_size > MAX_HTTP_BODY_BYTES) {
			return false;
		}
		pos = line_end + 2;
		if (chunk_size == 0) {
			// Expect trailing CRLF (or trailer headers followed by CRLF).
			if (pos + 2 <= p_len && p_data[pos] == '\r' && p_data[pos + 1] == '\n') {
				r_consumed = pos + 2;
				return true;
			}
			for (int i = pos; i + 3 < p_len; i++) {
				if (p_data[i] == '\r' && p_data[i + 1] == '\n' && p_data[i + 2] == '\r' && p_data[i + 3] == '\n') {
					r_consumed = i + 4;
					return true;
				}
			}
			return false;
		}
		if (pos + chunk_size + 2 > p_len) {
			return false; // Incomplete chunk data.
		}
		int old_sz = r_out.size();
		if (old_sz + chunk_size > MAX_HTTP_BODY_BYTES) {
			return false;
		}
		r_out.resize(old_sz + (int)chunk_size);
		memcpy(r_out.ptrw() + old_sz, p_data + pos, (size_t)chunk_size);
		pos += (int)chunk_size + 2; // Skip chunk data + CRLF.
	}
	return false;
}

String _extract_query_param(const String &p_query, const String &p_key) {
	if (p_query.is_empty()) {
		return String();
	}
	PackedStringArray pairs = p_query.split("&", false);
	for (int i = 0; i < pairs.size(); i++) {
		int eq = pairs[i].find_char('=');
		String k = eq >= 0 ? pairs[i].substr(0, eq) : pairs[i];
		if (k == p_key) {
			return eq >= 0 ? pairs[i].substr(eq + 1).uri_decode() : String();
		}
	}
	return String();
}

} // namespace

MinaMCPServer *MinaMCPServer::singleton = nullptr;
int MinaMCPServer::port_override = -1;
bool MinaMCPServer::disabled_override = false;
Vector<MinaMCPServer::ProviderCallback> MinaMCPServer::provider_callbacks;

MinaMCPServer::MinaMCPServer() {
	singleton = this;
	_install_log_handlers();
}

MinaMCPServer::~MinaMCPServer() {
	stop();
	_remove_log_handlers();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void MinaMCPServer::add_provider_callback(ProviderCallback p_callback) {
	if (!p_callback) {
		return;
	}
	if (!provider_callbacks.has(p_callback)) {
		provider_callbacks.push_back(p_callback);
	}
	if (singleton) {
		p_callback(singleton);
	}
}

void MinaMCPServer::invoke_provider_callbacks() {
	for (int i = 0; i < provider_callbacks.size(); i++) {
		if (provider_callbacks[i]) {
			provider_callbacks[i](this);
		}
	}
}

void MinaMCPServer::_print_handler(void *p_userdata, const String &p_string, bool p_error, bool p_rich) {
	MinaMCPServer *self = static_cast<MinaMCPServer *>(p_userdata);
	if (!self) {
		return;
	}
	String trimmed = p_string.strip_edges(false, true);
	if (trimmed.is_empty()) {
		return;
	}
	self->_push_log(p_error ? "error" : "info", "stdout", trimmed);
}

void MinaMCPServer::_error_handler(void *p_userdata, const char *p_func, const char *p_file, int p_line, const char *p_error, const char *p_errorexp, bool p_editor_notify, ErrorHandlerType p_type) {
	MinaMCPServer *self = static_cast<MinaMCPServer *>(p_userdata);
	if (!self) {
		return;
	}
	String level = (p_type == ERR_HANDLER_WARNING) ? "warning" : "error";
	String source = vformat("%s:%d (%s)", p_file ? String::utf8(p_file) : String("?"), p_line, p_func ? String::utf8(p_func) : String("?"));
	String msg;
	if (p_errorexp && p_errorexp[0]) {
		msg = String::utf8(p_errorexp);
	} else if (p_error && p_error[0]) {
		msg = String::utf8(p_error);
	}
	if (!msg.is_empty()) {
		self->_push_log(level, source, msg);
	}
}

void MinaMCPServer::_install_log_handlers() {
	if (log_handlers_installed) {
		return;
	}
	print_handler.printfunc = _print_handler;
	print_handler.userdata = this;
	add_print_handler(&print_handler);

	error_handler.errfunc = _error_handler;
	error_handler.userdata = this;
	add_error_handler(&error_handler);

	log_handlers_installed = true;
}

void MinaMCPServer::_remove_log_handlers() {
	if (!log_handlers_installed) {
		return;
	}
	remove_print_handler(&print_handler);
	remove_error_handler(&error_handler);
	log_handlers_installed = false;
}

void MinaMCPServer::_push_log(const String &p_level, const String &p_source, const String &p_message) {
	MutexLock lock(log_mutex);
	LogEntry entry;
	entry.id = next_log_id++;
	entry.timestamp_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
	entry.level = p_level;
	entry.source = p_source;
	entry.message = p_message;
	log_entries.push_back(entry);
	if (log_entries.size() > MAX_LOG_ENTRIES) {
		log_entries.remove_at(0);
	}
}

bool MinaMCPServer::_is_loopback_host(const String &p_host) {
	String h = p_host.strip_edges().to_lower();
	return h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "[::1]";
}

bool MinaMCPServer::_is_allowed_http_host(const String &p_host_header) const {
	if (allow_remote) {
		return true;
	}
	String h = p_host_header.strip_edges().to_lower();
	if (h.is_empty()) {
		return true;
	}
	// Strip optional :port (handling IPv6 [::1]:port).
	String hostname = h;
	if (hostname.begins_with("[")) {
		int close = hostname.find_char(']');
		if (close >= 0) {
			hostname = hostname.substr(0, close + 1);
		}
	} else {
		int colon = hostname.rfind_char(':');
		if (colon >= 0) {
			hostname = hostname.substr(0, colon);
		}
	}
	return _is_loopback_host(hostname);
}

bool MinaMCPServer::_is_allowed_origin(const String &p_origin_header) const {
	if (allow_remote) {
		return true;
	}
	String o = p_origin_header.strip_edges().to_lower();
	if (o.is_empty() || o == "null") {
		return true; // Non-browser CLI agents (Claude Code, Codex, curl) omit Origin.
	}
	if (o.begins_with("http://127.0.0.1") || o.begins_with("http://localhost") || o.begins_with("http://[::1]") ||
			o.begins_with("https://127.0.0.1") || o.begins_with("https://localhost") || o.begins_with("https://[::1]")) {
		return true;
	}
	return false;
}

String MinaMCPServer::_generate_session_id() {
	uint64_t ticks = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	uint64_t pid = OS::get_singleton() ? (uint64_t)OS::get_singleton()->get_process_id() : 0;
	uint64_t seq = next_session_seq++;
	return vformat("mina-%x-%x-%x", pid, ticks, seq);
}

Error MinaMCPServer::start(int p_port, const String &p_host, bool p_allow_remote) {
	stop();

	String bind_host = p_host.strip_edges();
	if (bind_host.is_empty()) {
		bind_host = "127.0.0.1";
	}
	if (!p_allow_remote && !_is_loopback_host(bind_host)) {
		last_error = vformat("Refused non-loopback bind address '%s' because remote access is disabled. Use 127.0.0.1 or explicitly enable allow_remote.", bind_host);
		state = STATE_ERROR;
		WARN_PRINT("Mina MCP Server: " + last_error);
		_notify_status_changed();
		return ERR_UNAUTHORIZED;
	}
	if (p_port < 1 || p_port > 65535) {
		last_error = vformat("Invalid MCP port %d (must be 1..65535).", p_port);
		state = STATE_ERROR;
		WARN_PRINT("Mina MCP Server: " + last_error);
		_notify_status_changed();
		return ERR_INVALID_PARAMETER;
	}

	host = bind_host;
	port = p_port;
	allow_remote = p_allow_remote;
	last_error = String();

	tcp_server.instantiate();
	IPAddress ip(host == "localhost" ? String("127.0.0.1") : host);
	if (!ip.is_valid()) {
		ip = IPAddress("127.0.0.1");
	}
	Error err = tcp_server->listen((uint16_t)port, ip);
	if (err != OK) {
		tcp_server.unref();
		state = STATE_ERROR;
		last_error = vformat("Failed to listen on %s:%d (error %d: %s).", host, port, (int)err, error_names[err]);
		WARN_PRINT("Mina MCP Server: " + last_error);
		_notify_status_changed();
		return err;
	}

	state = STATE_RUNNING;
	started_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
	print_line(vformat("[Mina MCP] Server listening on %s (mode: %s, tools: %d)", get_endpoint_url(), mode_name, tool_order.size()));
	_notify_status_changed();
	return OK;
}

void MinaMCPServer::stop() {
	bool had_sessions = !sessions.is_empty();
	bool was_active = (state != STATE_STOPPED) || tcp_server.is_valid();

	for (int i = 0; i < connections.size(); i++) {
		if (connections[i].peer.is_valid()) {
			connections[i].peer->disconnect_from_host();
		}
	}
	connections.clear();
	sessions.clear();

	if (tcp_server.is_valid()) {
		tcp_server->stop();
		tcp_server.unref();
	}

	state = STATE_STOPPED;
	started_msec = 0;

	if (had_sessions) {
		_notify_sessions_changed();
	}
	if (was_active) {
		print_line("[Mina MCP] Server stopped.");
		_notify_status_changed();
	}
}

Error MinaMCPServer::restart() {
	int p = port;
	String h = host;
	bool remote = allow_remote;
	stop();
	return start(p, h, remote);
}

String MinaMCPServer::get_endpoint_url() const {
	return vformat("http://%s:%d/mcp", host, port);
}

uint64_t MinaMCPServer::get_uptime_msec() const {
	if (state != STATE_RUNNING || started_msec == 0 || !OS::get_singleton()) {
		return 0;
	}
	return OS::get_singleton()->get_ticks_msec() - started_msec;
}

void MinaMCPServer::poll() {
	if (state != STATE_RUNNING || tcp_server.is_null() || polling) {
		return;
	}
	polling = true;
	_accept_new_connections();
	_poll_connections();
	polling = false;
}

void MinaMCPServer::_accept_new_connections() {
	while (tcp_server.is_valid() && tcp_server->is_connection_available()) {
		Ref<StreamPeerTCP> peer = tcp_server->take_connection();
		if (peer.is_null()) {
			break;
		}
		if (connections.size() >= MAX_CONCURRENT_CONNECTIONS) {
			peer->disconnect_from_host();
			continue;
		}
		peer->set_no_delay(true);
		HTTPConnection conn;
		conn.peer = peer;
		conn.created_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
		connections.push_back(conn);
	}
}

void MinaMCPServer::_poll_connections() {
	const uint64_t now = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;

	for (int i = connections.size() - 1; i >= 0; i--) {
		HTTPConnection &conn = connections.write[i];
		if (conn.peer.is_null()) {
			connections.remove_at(i);
			continue;
		}

		conn.peer->poll();
		StreamPeerSocket::Status st = conn.peer->get_status();
		if (st == StreamPeerSocket::STATUS_NONE || st == StreamPeerSocket::STATUS_ERROR) {
			connections.remove_at(i);
			continue;
		}
		if (st != StreamPeerSocket::STATUS_CONNECTED) {
			if (now - conn.created_msec > IDLE_CONNECTION_TIMEOUT_MSEC) {
				conn.peer->disconnect_from_host();
				connections.remove_at(i);
			}
			continue;
		}

		// SSE connections stay open until client disconnects.
		if (conn.is_sse) {
			continue;
		}

		int avail = conn.peer->get_available_bytes();
		if (avail > 0) {
			int to_read = MIN(avail, 256 * 1024);
			PackedByteArray chunk;
			chunk.resize(to_read);
			int received = 0;
			Error err = conn.peer->get_partial_data(chunk.ptrw(), to_read, received);
			if (err != OK) {
				conn.peer->disconnect_from_host();
				connections.remove_at(i);
				continue;
			}
			if (received > 0) {
				int old_size = conn.read_buffer.size();
				if (old_size + received > MAX_HTTP_HEADER_BYTES + MAX_HTTP_BODY_BYTES) {
					Dictionary err_obj;
					err_obj["error"] = "Request payload too large";
					_send_http_json(conn.peer, 413, "Payload Too Large", err_obj);
					conn.peer->disconnect_from_host();
					connections.remove_at(i);
					continue;
				}
				conn.read_buffer.resize(old_size + received);
				memcpy(conn.read_buffer.ptrw() + old_size, chunk.ptr(), (size_t)received);
				conn.created_msec = now;
			}
		}

		if (!conn.read_buffer.is_empty()) {
			bool done = _try_process_http_request(conn);
			if (done && !conn.is_sse) {
				conn.peer->disconnect_from_host();
				connections.remove_at(i);
				continue;
			}
		} else if (now - conn.created_msec > IDLE_CONNECTION_TIMEOUT_MSEC) {
			conn.peer->disconnect_from_host();
			connections.remove_at(i);
			continue;
		}
	}
}

bool MinaMCPServer::_try_process_http_request(HTTPConnection &p_conn) {
	int header_end = _find_bytes(p_conn.read_buffer, "\r\n\r\n");
	if (header_end < 0) {
		if (p_conn.read_buffer.size() > MAX_HTTP_HEADER_BYTES) {
			Dictionary err_obj;
			err_obj["error"] = "HTTP headers too large";
			_send_http_json(p_conn.peer, 431, "Request Header Fields Too Large", err_obj);
			return true;
		}
		return false; // Need more header bytes.
	}

	String header_str = String::utf8((const char *)p_conn.read_buffer.ptr(), header_end);
	PackedStringArray lines = header_str.split("\r\n");
	if (lines.is_empty()) {
		Dictionary err_obj;
		err_obj["error"] = "Malformed HTTP request";
		_send_http_json(p_conn.peer, 400, "Bad Request", err_obj);
		return true;
	}

	PackedStringArray request_line = lines[0].split(" ", false);
	if (request_line.size() < 2) {
		Dictionary err_obj;
		err_obj["error"] = "Malformed HTTP request line";
		_send_http_json(p_conn.peer, 400, "Bad Request", err_obj);
		return true;
	}

	String method = request_line[0].to_upper();
	String raw_target = request_line[1];
	String path = raw_target;
	String query;
	int q_pos = raw_target.find_char('?');
	if (q_pos >= 0) {
		path = raw_target.substr(0, q_pos);
		query = raw_target.substr(q_pos + 1);
	}

	HashMap<String, String> headers;
	for (int i = 1; i < lines.size(); i++) {
		int colon = lines[i].find_char(':');
		if (colon > 0) {
			String k = lines[i].substr(0, colon).strip_edges().to_lower();
			String v = lines[i].substr(colon + 1).strip_edges();
			headers[k] = v;
		}
	}

	// Handle Expect: 100-continue before waiting for body.
	if (headers.has("expect") && headers["expect"].to_lower().contains("100-continue")) {
		const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
		p_conn.peer->put_data((const uint8_t *)cont, (int)strlen(cont));
	}

	const int body_offset = header_end + 4;
	const int body_avail = p_conn.read_buffer.size() - body_offset;
	PackedByteArray body_bytes;

	bool is_chunked = headers.has("transfer-encoding") && headers["transfer-encoding"].to_lower().contains("chunked");
	if (is_chunked) {
		int consumed = 0;
		if (!_decode_chunked_body(p_conn.read_buffer.ptr() + body_offset, body_avail, body_bytes, consumed)) {
			return false; // Wait for rest of chunked body.
		}
	} else if (headers.has("content-length")) {
		int64_t content_length = headers["content-length"].to_int();
		if (content_length < 0 || content_length > MAX_HTTP_BODY_BYTES) {
			Dictionary err_obj;
			err_obj["error"] = "Invalid Content-Length";
			_send_http_json(p_conn.peer, 400, "Bad Request", err_obj);
			return true;
		}
		if (body_avail < content_length) {
			return false; // Wait for full body.
		}
		if (content_length > 0) {
			body_bytes.resize((int)content_length);
			memcpy(body_bytes.ptrw(), p_conn.read_buffer.ptr() + body_offset, (size_t)content_length);
		}
	}

	// DNS rebinding & cross-origin protection (localhost-only by default).
	if (headers.has("host") && !_is_allowed_http_host(headers["host"])) {
		Dictionary err_obj;
		err_obj["error"] = "Forbidden Host header (localhost only)";
		_send_http_json(p_conn.peer, 403, "Forbidden", err_obj);
		return true;
	}
	if (headers.has("origin") && !_is_allowed_origin(headers["origin"])) {
		Dictionary err_obj;
		err_obj["error"] = "Forbidden Origin header (localhost only)";
		_send_http_json(p_conn.peer, 403, "Forbidden", err_obj);
		return true;
	}

	HashMap<String, String> cors_headers;
	if (headers.has("origin") && _is_allowed_origin(headers["origin"])) {
		cors_headers["Access-Control-Allow-Origin"] = headers["origin"];
		cors_headers["Access-Control-Allow-Methods"] = "GET, POST, DELETE, OPTIONS";
		cors_headers["Access-Control-Allow-Headers"] = "Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version";
		cors_headers["Access-Control-Expose-Headers"] = "Mcp-Session-Id";
	}

	// CORS Preflight.
	if (method == "OPTIONS") {
		_send_http_response(p_conn.peer, 204, "No Content", "text/plain", PackedByteArray(), cors_headers);
		return true;
	}

	// Health / Discovery endpoint.
	if (method == "GET" && (path == "/" || path == "/health")) {
		_send_http_json(p_conn.peer, 200, "OK", _build_health_payload(), cors_headers);
		return true;
	}

	// Streamable HTTP: DELETE /mcp terminates a session.
	if (method == "DELETE" && path == "/mcp") {
		String session_id = headers.has("mcp-session-id") ? headers["mcp-session-id"] : String();
		if (!session_id.is_empty() && sessions.has(session_id)) {
			sessions.erase(session_id);
			_notify_sessions_changed();
		}
		_send_http_response(p_conn.peer, 204, "No Content", "text/plain", PackedByteArray(), cors_headers);
		return true;
	}

	// Streamable HTTP: GET /mcp opens an SSE stream for server notifications.
	if (method == "GET" && path == "/mcp") {
		String session_id = headers.has("mcp-session-id") ? headers["mcp-session-id"] : String();
		if (session_id.is_empty()) {
			session_id = _generate_session_id();
		}
		p_conn.is_sse = true;
		p_conn.sse_session_id = session_id;
		p_conn.read_buffer.clear();
		_send_sse_headers(p_conn.peer, session_id);
		return false; // Keep connection open!
	}

	// Legacy HTTP+SSE transport (MCP 2024-11-05): GET /sse
	if (method == "GET" && path == "/sse") {
		String session_id = _generate_session_id();
		SessionInfo info;
		info.id = session_id;
		info.transport = "sse";
		info.connected_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
		info.last_activity_msec = info.connected_msec;
		sessions[session_id] = info;
		_notify_sessions_changed();

		p_conn.is_sse = true;
		p_conn.sse_session_id = session_id;
		p_conn.read_buffer.clear();
		_send_sse_headers(p_conn.peer, session_id);
		_send_sse_event(p_conn.peer, "endpoint", "/messages?sessionId=" + session_id);
		return false; // Keep SSE open!
	}

	// Streamable HTTP (POST /mcp) AND Legacy SSE message posting (POST /messages?sessionId=...)
	if (method == "POST" && (path == "/mcp" || path == "/messages")) {
		const bool is_legacy_sse_post = (path == "/messages");
		String session_id = is_legacy_sse_post
				? _extract_query_param(query, "sessionId")
				: (headers.has("mcp-session-id") ? headers["mcp-session-id"] : String());

		String body_str = body_bytes.is_empty() ? String() : String::utf8((const char *)body_bytes.ptr(), body_bytes.size());
		Ref<JSON> parser;
		parser.instantiate();
		Error parse_err = parser->parse(body_str);
		if (parse_err != OK) {
			Dictionary rpc_err = _make_jsonrpc_error(Variant(), -32700, "Parse error: " + parser->get_error_message());
			_send_http_json(p_conn.peer, 400, "Bad Request", rpc_err, cors_headers);
			return true;
		}

		Variant payload = parser->get_data();
		String new_session_id = session_id;
		Variant response_payload;
		bool has_response = false;

		if (payload.get_type() == Variant::ARRAY) {
			Array batch = payload;
			Array batch_responses;
			for (int i = 0; i < batch.size(); i++) {
				bool item_has_resp = false;
				Variant item_resp = _handle_jsonrpc_message(batch[i], new_session_id, new_session_id, item_has_resp);
				if (item_has_resp) {
					batch_responses.push_back(item_resp);
				}
			}
			if (!batch_responses.is_empty()) {
				response_payload = batch_responses;
				has_response = true;
			}
		} else if (payload.get_type() == Variant::DICTIONARY) {
			response_payload = _handle_jsonrpc_message(payload, session_id, new_session_id, has_response);
		} else {
			response_payload = _make_jsonrpc_error(Variant(), -32600, "Invalid Request: expected JSON-RPC object or array.");
			has_response = true;
		}

		HashMap<String, String> resp_headers(cors_headers);
		if (!new_session_id.is_empty()) {
			resp_headers["Mcp-Session-Id"] = new_session_id;
		}

		if (is_legacy_sse_post) {
			if (has_response && !new_session_id.is_empty()) {
				_broadcast_sse_to_session(new_session_id, "message", JSON::stringify(response_payload));
			}
			_send_http_response(p_conn.peer, 202, "Accepted", "text/plain", PackedByteArray(), resp_headers);
			return true;
		}

		if (!has_response) {
			// JSON-RPC notification -> 202 Accepted per MCP Streamable HTTP spec.
			_send_http_response(p_conn.peer, 202, "Accepted", "text/plain", PackedByteArray(), resp_headers);
			return true;
		}

		_send_http_json(p_conn.peer, 200, "OK", response_payload, resp_headers);
		return true;
	}

	Dictionary not_found;
	not_found["error"] = vformat("Route not found: %s %s. Use POST /mcp or GET /health.", method, path);
	_send_http_json(p_conn.peer, 404, "Not Found", not_found, cors_headers);
	return true;
}

void MinaMCPServer::_send_http_response(const Ref<StreamPeerTCP> &p_peer, int p_status_code, const String &p_status_text, const String &p_content_type, const PackedByteArray &p_body, const HashMap<String, String> &p_extra_headers) {
	if (p_peer.is_null()) {
		return;
	}
	String header = vformat("HTTP/1.1 %d %s\r\n", p_status_code, p_status_text);
	header += "Server: Mina-Engine-MCP\r\n";
	if (!p_content_type.is_empty()) {
		header += "Content-Type: " + p_content_type + "\r\n";
	}
	header += vformat("Content-Length: %d\r\n", p_body.size());
	header += "Connection: close\r\n";
	for (const KeyValue<String, String> &E : p_extra_headers) {
		header += E.key + ": " + E.value + "\r\n";
	}
	header += "\r\n";

	CharString header_utf8 = header.utf8();
	p_peer->put_data((const uint8_t *)header_utf8.get_data(), header_utf8.length());
	if (!p_body.is_empty()) {
		p_peer->put_data(p_body.ptr(), p_body.size());
	}
}

void MinaMCPServer::_send_http_json(const Ref<StreamPeerTCP> &p_peer, int p_status_code, const String &p_status_text, const Variant &p_json, const HashMap<String, String> &p_extra_headers) {
	String json_str = JSON::stringify(p_json);
	CharString utf8 = json_str.utf8();
	PackedByteArray bytes;
	bytes.resize(utf8.length());
	if (utf8.length() > 0) {
		memcpy(bytes.ptrw(), utf8.get_data(), (size_t)utf8.length());
	}
	_send_http_response(p_peer, p_status_code, p_status_text, "application/json; charset=utf-8", bytes, p_extra_headers);
}

void MinaMCPServer::_send_sse_headers(const Ref<StreamPeerTCP> &p_peer, const String &p_session_id) {
	if (p_peer.is_null()) {
		return;
	}
	String header = "HTTP/1.1 200 OK\r\n";
	header += "Content-Type: text/event-stream; charset=utf-8\r\n";
	header += "Cache-Control: no-cache, no-transform\r\n";
	header += "Connection: keep-alive\r\n";
	if (!p_session_id.is_empty()) {
		header += "Mcp-Session-Id: " + p_session_id + "\r\n";
	}
	header += "\r\n";
	CharString utf8 = header.utf8();
	p_peer->put_data((const uint8_t *)utf8.get_data(), utf8.length());
}

bool MinaMCPServer::_send_sse_event(const Ref<StreamPeerTCP> &p_peer, const String &p_event, const String &p_data) {
	if (p_peer.is_null()) {
		return false;
	}
	String frame = vformat("event: %s\ndata: %s\n\n", p_event, p_data.replace("\n", "\ndata: "));
	CharString utf8 = frame.utf8();
	return p_peer->put_data((const uint8_t *)utf8.get_data(), utf8.length()) == OK;
}

void MinaMCPServer::_broadcast_sse_to_session(const String &p_session_id, const String &p_event, const String &p_data) {
	for (int i = 0; i < connections.size(); i++) {
		if (connections[i].is_sse && connections[i].sse_session_id == p_session_id) {
			_send_sse_event(connections[i].peer, p_event, p_data);
		}
	}
}

Dictionary MinaMCPServer::_make_jsonrpc_result(const Variant &p_id, const Variant &p_result) const {
	Dictionary d;
	d["jsonrpc"] = "2.0";
	d["id"] = p_id;
	d["result"] = p_result;
	return d;
}

Dictionary MinaMCPServer::_make_jsonrpc_error(const Variant &p_id, int p_code, const String &p_message, const Variant &p_data) const {
	Dictionary err;
	err["code"] = p_code;
	err["message"] = p_message;
	if (p_data.get_type() != Variant::NIL) {
		err["data"] = p_data;
	}
	Dictionary d;
	d["jsonrpc"] = "2.0";
	d["id"] = p_id;
	d["error"] = err;
	return d;
}

Variant MinaMCPServer::_handle_jsonrpc_message(const Variant &p_msg, const String &p_session_id, String &r_new_session_id, bool &r_has_response) {
	r_has_response = false;
	if (p_msg.get_type() != Variant::DICTIONARY) {
		r_has_response = true;
		return _make_jsonrpc_error(Variant(), -32600, "Invalid Request");
	}
	Dictionary msg = p_msg;
	String method = msg.get("method", "");
	bool has_id = msg.has("id") && msg["id"].get_type() != Variant::NIL;
	Variant id = has_id ? msg["id"] : Variant();
	Dictionary params = msg.get("params", Dictionary());

	total_requests++;

	const uint64_t now = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
	if (!p_session_id.is_empty() && sessions.has(p_session_id)) {
		SessionInfo &s = sessions[p_session_id];
		s.last_activity_msec = now;
		s.request_count++;
	}

	// Notifications (no id -> no response required).
	if (method == "notifications/initialized") {
		if (!p_session_id.is_empty() && sessions.has(p_session_id)) {
			sessions[p_session_id].initialized = true;
			_notify_sessions_changed();
		}
		return Variant();
	}
	if (method == "notifications/cancelled" || method.begins_with("notifications/")) {
		return Variant();
	}

	if (!has_id) {
		// Unhandled notification: silently accept per JSON-RPC 2.0.
		return Variant();
	}

	r_has_response = true;

	if (method == "initialize") {
		Dictionary result = _mcp_initialize(params, p_session_id, r_new_session_id);
		return _make_jsonrpc_result(id, result);
	}
	if (method == "ping") {
		return _make_jsonrpc_result(id, Dictionary());
	}
	if (method == "tools/list") {
		return _make_jsonrpc_result(id, _mcp_tools_list(params));
	}
	if (method == "tools/call") {
		return _make_jsonrpc_result(id, _mcp_tools_call(params));
	}
	if (method == "resources/list") {
		return _make_jsonrpc_result(id, _mcp_resources_list(params));
	}
	if (method == "resources/read") {
		Dictionary res = _mcp_resources_read(params);
		if (res.has("_rpc_error")) {
			return _make_jsonrpc_error(id, -32602, String(res["_rpc_error"]));
		}
		return _make_jsonrpc_result(id, res);
	}
	if (method == "prompts/list") {
		return _make_jsonrpc_result(id, _mcp_prompts_list(params));
	}
	if (method == "logging/setLevel") {
		return _make_jsonrpc_result(id, Dictionary());
	}

	return _make_jsonrpc_error(id, -32601, vformat("Method not found: '%s'", method));
}

Dictionary MinaMCPServer::_mcp_initialize(const Dictionary &p_params, const String &p_existing_session_id, String &r_session_id) {
	String requested_version = p_params.get("protocolVersion", "2025-03-26");
	String negotiated_version = "2025-03-26";
	if (requested_version == "2025-06-18" || requested_version == "2025-03-26" || requested_version == "2024-11-05") {
		negotiated_version = requested_version;
	}

	String sid = p_existing_session_id.is_empty() ? _generate_session_id() : p_existing_session_id;
	r_session_id = sid;

	Dictionary client_info = p_params.get("clientInfo", Dictionary());
	SessionInfo &info = sessions[sid];
	info.id = sid;
	info.client_name = client_info.get("name", "unknown-client");
	info.client_version = client_info.get("version", "");
	info.protocol_version = negotiated_version;
	info.connected_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
	info.last_activity_msec = info.connected_msec;
	info.request_count = MAX<uint64_t>(info.request_count, 1);
	if (info.transport.is_empty()) {
		info.transport = "streamable_http";
	}
	_notify_sessions_changed();

	Dictionary server_info;
	server_info["name"] = "mina-engine";
	server_info["version"] = VERSION_FULL_CONFIG;

	Dictionary tools_cap;
	tools_cap["listChanged"] = true;
	Dictionary resources_cap;
	resources_cap["listChanged"] = true;
	Dictionary capabilities;
	capabilities["tools"] = tools_cap;
	capabilities["resources"] = resources_cap;
	capabilities["logging"] = Dictionary();

	Dictionary result;
	result["protocolVersion"] = negotiated_version;
	result["capabilities"] = capabilities;
	result["serverInfo"] = server_info;
	result["instructions"] =
			"You are connected directly to a running Mina Engine instance via MCP. "
			"Use engine_get_status and project_get_info first to understand the current engine mode (editor or project_manager) and project state. "
			"All scene, node, asset, script, animation, lighting, playback, debug, and export operations invoke Mina Engine's internal APIs directly.";
	return result;
}

Dictionary MinaMCPServer::_mcp_tools_list(const Dictionary &p_params) const {
	Array list;
	for (int i = 0; i < tool_order.size(); i++) {
		const String &name = tool_order[i];
		const ToolDefinition *def = tools.getptr(name);
		if (!def) {
			continue;
		}
		Dictionary item;
		item["name"] = def->name;
		if (!def->title.is_empty()) {
			item["title"] = def->title;
		}
		item["description"] = def->description;
		item["inputSchema"] = def->input_schema.is_empty() ? mcp::make_tool_schema(Dictionary()) : def->input_schema;
		if (!def->annotations.is_empty()) {
			item["annotations"] = def->annotations;
		}
		list.push_back(item);
	}
	Dictionary out;
	out["tools"] = list;
	return out;
}

Dictionary MinaMCPServer::_mcp_tools_call(const Dictionary &p_params) {
	String tool_name = p_params.get("name", "");
	Dictionary arguments = p_params.get("arguments", Dictionary());

	uint64_t t0 = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	Dictionary raw_result = execute_tool_direct(tool_name, arguments);
	uint64_t t1 = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : t0;
	double duration_ms = double(t1 - t0) / 1000.0;

	bool ok = raw_result.get("ok", true);
	String summary = ok ? String("ok") : String(raw_result.get("error", "error"));
	_record_activity(tool_name, ok, summary, duration_ms);

	Array content;
	if (raw_result.has("_mcp_image_base64")) {
		Dictionary img_block;
		img_block["type"] = "image";
		img_block["data"] = raw_result["_mcp_image_base64"];
		img_block["mimeType"] = raw_result.get("_mcp_image_mime", "image/png");
		content.push_back(img_block);

		Dictionary meta = raw_result.duplicate();
		meta.erase("_mcp_image_base64");
		meta.erase("_mcp_image_mime");

		Dictionary text_block;
		text_block["type"] = "text";
		text_block["text"] = JSON::stringify(meta, "  ");
		content.push_back(text_block);

		Dictionary mcp_result;
		mcp_result["content"] = content;
		mcp_result["structuredContent"] = meta;
		mcp_result["isError"] = !ok;
		return mcp_result;
	}

	Dictionary text_block;
	text_block["type"] = "text";
	text_block["text"] = JSON::stringify(raw_result, "  ");
	content.push_back(text_block);

	Dictionary mcp_result;
	mcp_result["content"] = content;
	mcp_result["structuredContent"] = raw_result;
	mcp_result["isError"] = !ok;
	return mcp_result;
}

Dictionary MinaMCPServer::_mcp_resources_list(const Dictionary &p_params) const {
	Array list;
	for (int i = 0; i < resource_order.size(); i++) {
		const ResourceDefinition *def = resources.getptr(resource_order[i]);
		if (!def) {
			continue;
		}
		Dictionary item;
		item["uri"] = def->uri;
		item["name"] = def->name;
		item["description"] = def->description;
		item["mimeType"] = def->mime_type;
		list.push_back(item);
	}
	Dictionary out;
	out["resources"] = list;
	return out;
}

Dictionary MinaMCPServer::_mcp_resources_read(const Dictionary &p_params) const {
	String uri = p_params.get("uri", "");
	const ResourceDefinition *def = resources.getptr(uri);
	if (!def) {
		Dictionary err;
		err["_rpc_error"] = vformat("Resource not found: '%s'", uri);
		return err;
	}

	Dictionary payload;
	if (def->reader) {
		payload = def->reader();
	} else if (def->script_reader.is_valid()) {
		Variant ret = def->script_reader.call();
		if (ret.get_type() == Variant::DICTIONARY) {
			payload = ret;
		} else {
			payload["value"] = mcp::variant_to_json(ret);
		}
	}

	Dictionary content_item;
	content_item["uri"] = def->uri;
	content_item["mimeType"] = def->mime_type;
	content_item["text"] = JSON::stringify(payload, "  ");

	Array contents;
	contents.push_back(content_item);
	Dictionary out;
	out["contents"] = contents;
	return out;
}

Dictionary MinaMCPServer::_mcp_prompts_list(const Dictionary &p_params) const {
	Dictionary out;
	out["prompts"] = Array();
	return out;
}

Dictionary MinaMCPServer::_build_health_payload() const {
	Dictionary d = get_status_dictionary();
	d["engine"] = "Mina Engine";
	d["version"] = VERSION_FULL_CONFIG;
	if (ProjectSettings::get_singleton()) {
		d["project_name"] = String(GLOBAL_GET("application/config/name"));
		d["project_path"] = ProjectSettings::get_singleton()->get_resource_path();
	}
	return d;
}

void MinaMCPServer::register_tool(const ToolDefinition &p_tool) {
	ERR_FAIL_COND(p_tool.name.is_empty());
	if (!tools.has(p_tool.name)) {
		tool_order.push_back(p_tool.name);
	}
	tools[p_tool.name] = p_tool;
}

void MinaMCPServer::unregister_tool(const String &p_name) {
	if (tools.erase(p_name)) {
		tool_order.erase(p_name);
	}
}

bool MinaMCPServer::has_tool(const String &p_name) const {
	return tools.has(p_name);
}

PackedStringArray MinaMCPServer::get_tool_names() const {
	PackedStringArray out;
	out.resize(tool_order.size());
	for (int i = 0; i < tool_order.size(); i++) {
		out.write[i] = tool_order[i];
	}
	return out;
}

void MinaMCPServer::register_resource(const ResourceDefinition &p_resource) {
	ERR_FAIL_COND(p_resource.uri.is_empty());
	if (!resources.has(p_resource.uri)) {
		resource_order.push_back(p_resource.uri);
	}
	resources[p_resource.uri] = p_resource;
}

void MinaMCPServer::unregister_resource(const String &p_uri) {
	if (resources.erase(p_uri)) {
		resource_order.erase(p_uri);
	}
}

void MinaMCPServer::register_script_tool(const String &p_name, const String &p_description, const Dictionary &p_input_schema, const Callable &p_handler, const String &p_category) {
	ERR_FAIL_COND(p_name.is_empty() || !p_handler.is_valid());
	ToolDefinition def;
	def.name = p_name;
	def.description = p_description;
	def.category = p_category;
	def.input_schema = p_input_schema.is_empty() ? mcp::make_tool_schema(Dictionary()) : p_input_schema;
	def.script_handler = p_handler;
	register_tool(def);
}

void MinaMCPServer::register_script_resource(const String &p_uri, const String &p_name, const String &p_description, const String &p_mime_type, const Callable &p_reader) {
	ERR_FAIL_COND(p_uri.is_empty() || !p_reader.is_valid());
	ResourceDefinition def;
	def.uri = p_uri;
	def.name = p_name;
	def.description = p_description;
	def.mime_type = p_mime_type.is_empty() ? String("application/json") : p_mime_type;
	def.script_reader = p_reader;
	register_resource(def);
}

Dictionary MinaMCPServer::execute_tool_direct(const String &p_name, const Dictionary &p_arguments) {
	const ToolDefinition *def = tools.getptr(p_name);
	if (!def) {
		return mcp::make_error(vformat("Unknown tool: '%s'. Call tools/list to see available tools.", p_name), "unknown_tool");
	}
	if (def->handler) {
		return def->handler(p_arguments);
	}
	if (def->script_handler.is_valid()) {
		Variant ret = def->script_handler.call(p_arguments);
		if (ret.get_type() == Variant::DICTIONARY) {
			return ret;
		}
		Dictionary ok;
		ok["result"] = mcp::variant_to_json(ret);
		return mcp::make_ok(ok);
	}
	return mcp::make_error(vformat("Tool '%s' has no executable handler.", p_name), "invalid_handler");
}

Array MinaMCPServer::get_sessions() const {
	Array out;
	for (const KeyValue<String, SessionInfo> &E : sessions) {
		Dictionary d;
		d["id"] = E.value.id;
		d["client_name"] = E.value.client_name;
		d["client_version"] = E.value.client_version;
		d["protocol_version"] = E.value.protocol_version;
		d["initialized"] = E.value.initialized;
		d["transport"] = E.value.transport;
		d["connected_msec"] = (int64_t)E.value.connected_msec;
		d["last_activity_msec"] = (int64_t)E.value.last_activity_msec;
		d["request_count"] = (int64_t)E.value.request_count;
		out.push_back(d);
	}
	return out;
}

Array MinaMCPServer::get_logs(const String &p_level, uint64_t p_since_id, int p_limit, const String &p_search) const {
	MutexLock lock(log_mutex);
	Array out;
	String level_filter = p_level.strip_edges().to_lower();
	bool filter_level = !level_filter.is_empty() && level_filter != "all";
	String search_lower = p_search.strip_edges().to_lower();
	int limit = CLAMP(p_limit, 1, 1000);

	// Collect matching entries from the tail to return the newest logs up to p_limit.
	Vector<LogEntry> matched;
	for (int i = log_entries.size() - 1; i >= 0 && matched.size() < limit; i--) {
		const LogEntry &e = log_entries[i];
		if (e.id <= p_since_id) {
			break;
		}
		if (filter_level && e.level != level_filter) {
			continue;
		}
		if (!search_lower.is_empty() && !e.message.to_lower().contains(search_lower) && !e.source.to_lower().contains(search_lower)) {
			continue;
		}
		matched.push_back(e);
	}

	for (int i = matched.size() - 1; i >= 0; i--) {
		const LogEntry &e = matched[i];
		Dictionary d;
		d["id"] = (int64_t)e.id;
		d["timestamp_msec"] = (int64_t)e.timestamp_msec;
		d["level"] = e.level;
		d["source"] = e.source;
		d["message"] = e.message;
		out.push_back(d);
	}
	return out;
}

void MinaMCPServer::clear_logs() {
	MutexLock lock(log_mutex);
	log_entries.clear();
}

void MinaMCPServer::_record_activity(const String &p_tool_name, bool p_ok, const String &p_summary, double p_duration_ms) {
	ToolActivityEntry entry;
	entry.id = next_activity_id++;
	entry.timestamp_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
	entry.tool_name = p_tool_name;
	entry.ok = p_ok;
	entry.summary = p_summary;
	entry.duration_ms = p_duration_ms;
	activity_entries.push_back(entry);
	if (activity_entries.size() > MAX_ACTIVITY_ENTRIES) {
		activity_entries.remove_at(0);
	}
	emit_signal(SNAME("tool_executed"), p_tool_name, p_ok, p_summary);
}

Array MinaMCPServer::get_recent_activity(int p_limit) const {
	Array out;
	int limit = CLAMP(p_limit, 1, MAX_ACTIVITY_ENTRIES);
	int start_idx = MAX(0, activity_entries.size() - limit);
	for (int i = start_idx; i < activity_entries.size(); i++) {
		const ToolActivityEntry &e = activity_entries[i];
		Dictionary d;
		d["id"] = (int64_t)e.id;
		d["timestamp_msec"] = (int64_t)e.timestamp_msec;
		d["tool_name"] = e.tool_name;
		d["ok"] = e.ok;
		d["summary"] = e.summary;
		d["duration_ms"] = e.duration_ms;
		out.push_back(d);
	}
	return out;
}

void MinaMCPServer::clear_activity() {
	activity_entries.clear();
}

Dictionary MinaMCPServer::get_status_dictionary() const {
	Dictionary d;
	String state_str = "stopped";
	if (state == STATE_RUNNING) {
		state_str = "running";
	} else if (state == STATE_ERROR) {
		state_str = "error";
	}
	d["state"] = state_str;
	d["running"] = (state == STATE_RUNNING);
	d["host"] = host;
	d["port"] = port;
	d["allow_remote"] = allow_remote;
	d["endpoint_url"] = get_endpoint_url();
	d["sse_url"] = vformat("http://%s:%d/sse", host, port);
	d["mode"] = mode_name;
	d["last_error"] = last_error;
	d["sessions_count"] = sessions.size();
	d["tools_count"] = tool_order.size();
	d["resources_count"] = resource_order.size();
	d["total_requests"] = (int64_t)total_requests;
	d["uptime_msec"] = (int64_t)get_uptime_msec();
	return d;
}

void MinaMCPServer::_notify_status_changed() {
	emit_signal(SNAME("status_changed"));
}

void MinaMCPServer::_notify_sessions_changed() {
	emit_signal(SNAME("sessions_changed"));
}

void MinaMCPServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "port", "host", "allow_remote"), &MinaMCPServer::start, DEFVAL(DEFAULT_PORT), DEFVAL("127.0.0.1"), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("stop"), &MinaMCPServer::stop);
	ClassDB::bind_method(D_METHOD("restart"), &MinaMCPServer::restart);
	ClassDB::bind_method(D_METHOD("get_state"), &MinaMCPServer::get_state);
	ClassDB::bind_method(D_METHOD("is_running"), &MinaMCPServer::is_running);
	ClassDB::bind_method(D_METHOD("get_host"), &MinaMCPServer::get_host);
	ClassDB::bind_method(D_METHOD("get_port"), &MinaMCPServer::get_port);
	ClassDB::bind_method(D_METHOD("get_endpoint_url"), &MinaMCPServer::get_endpoint_url);
	ClassDB::bind_method(D_METHOD("get_last_error"), &MinaMCPServer::get_last_error);
	ClassDB::bind_method(D_METHOD("get_tool_count"), &MinaMCPServer::get_tool_count);
	ClassDB::bind_method(D_METHOD("get_tool_names"), &MinaMCPServer::get_tool_names);
	ClassDB::bind_method(D_METHOD("get_session_count"), &MinaMCPServer::get_session_count);
	ClassDB::bind_method(D_METHOD("get_sessions"), &MinaMCPServer::get_sessions);
	ClassDB::bind_method(D_METHOD("get_status_dictionary"), &MinaMCPServer::get_status_dictionary);
	ClassDB::bind_method(D_METHOD("register_script_tool", "name", "description", "input_schema", "handler", "category"), &MinaMCPServer::register_script_tool, DEFVAL("custom"));
	ClassDB::bind_method(D_METHOD("unregister_tool", "name"), &MinaMCPServer::unregister_tool);
	ClassDB::bind_method(D_METHOD("register_script_resource", "uri", "name", "description", "mime_type", "reader"), &MinaMCPServer::register_script_resource);
	ClassDB::bind_method(D_METHOD("unregister_resource", "uri"), &MinaMCPServer::unregister_resource);
	ClassDB::bind_method(D_METHOD("execute_tool_direct", "name", "arguments"), &MinaMCPServer::execute_tool_direct);

	ADD_SIGNAL(MethodInfo("status_changed"));
	ADD_SIGNAL(MethodInfo("sessions_changed"));
	ADD_SIGNAL(MethodInfo("tool_executed", PropertyInfo(Variant::STRING, "tool_name"), PropertyInfo(Variant::BOOL, "ok"), PropertyInfo(Variant::STRING, "summary")));

	BIND_ENUM_CONSTANT(STATE_STOPPED);
	BIND_ENUM_CONSTANT(STATE_RUNNING);
	BIND_ENUM_CONSTANT(STATE_ERROR);
}
