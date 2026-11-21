/**************************************************************************/
/*  mcp_tools_asset_download.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

// Free (CC0) asset search / download tools.
//
// Sources: Poly Haven (api.polyhaven.com) and ambientCG (ambientcg.com).
// Both publish their assets under CC0 1.0. Only HTTPS and an explicit host
// allowlist are permitted (including redirects); files are only written below
// res://, never outside the project.

#include "mcp_builtin_tools.h"

#include "../mcp_server.h"
#include "../mcp_variant_codec.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/file_system/editor_file_system.h"

namespace {

constexpr int MAX_REDIRECTS = 5;
constexpr int64_t DEFAULT_MAX_TOTAL_BYTES = 200LL * 1024 * 1024;
constexpr int64_t HARD_MAX_TOTAL_BYTES = 1024LL * 1024 * 1024;
constexpr int HTTP_TIMEOUT_SEC = 60;

bool _host_allowed(const String &p_host) {
	String h = p_host.to_lower();
	static const char *allowed[] = { "polyhaven.com", "polyhaven.org", "ambientcg.com", "struffelproductions.com" }; // last: ambientCG's download CDN
	for (const char *a : allowed) {
		String s = a;
		if (h == s || h.ends_with("." + s)) {
			return true;
		}
	}
	return false;
}

bool _valid_id(const String &p_id) {
	if (p_id.is_empty() || p_id.length() > 128) {
		return false;
	}
	for (int i = 0; i < p_id.length(); i++) {
		char32_t c = p_id[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

// Relative path inside the asset folder; rejects traversal / absolute paths.
bool _safe_rel(const String &p_rel) {
	String r = p_rel.replace("\\", "/");
	if (r.is_empty() || r.begins_with("/") || r.contains(":")) {
		return false;
	}
	Vector<String> parts = r.split("/", false);
	for (const String &s : parts) {
		if (s == ".." || s == "." || s.is_empty()) {
			return false;
		}
	}
	return !parts.is_empty();
}

String _url_encode(const String &p_s) {
	return p_s.uri_encode();
}

// Blocking HTTPS GET with redirect handling, host allowlist and size cap.
bool _http_get(const String &p_url, PackedByteArray &r_body, String &r_error, int64_t p_max_bytes) {
	String url = p_url;
	for (int redirect = 0; redirect <= MAX_REDIRECTS; redirect++) {
		if (!url.begins_with("https://")) {
			r_error = "Only https:// URLs are allowed: " + url;
			return false;
		}
		String rest = url.substr(8);
		int slash = rest.find_char('/');
		String host = slash < 0 ? rest : rest.substr(0, slash);
		String path = slash < 0 ? String("/") : rest.substr(slash);
		if (host.contains(":") || host.contains("@") || !_host_allowed(host)) {
			r_error = "Host is not on the free-asset allowlist: " + host;
			return false;
		}

		Ref<HTTPClient> client = HTTPClient::create();
		if (client.is_null()) {
			r_error = "HTTPClient is unavailable.";
			return false;
		}
		Error err = client->connect_to_host("https://" + host, 443);
		if (err != OK) {
			r_error = vformat("Failed to connect to %s (error %d).", host, int(err));
			return false;
		}

		uint64_t start = OS::get_singleton()->get_ticks_msec();
		auto timed_out = [&]() { return OS::get_singleton()->get_ticks_msec() - start > uint64_t(HTTP_TIMEOUT_SEC) * 1000; };

		while (client->get_status() == HTTPClient::STATUS_CONNECTING || client->get_status() == HTTPClient::STATUS_RESOLVING) {
			client->poll();
			OS::get_singleton()->delay_usec(1000);
			if (timed_out()) {
				r_error = "Connection timed out: " + host;
				return false;
			}
		}
		if (client->get_status() != HTTPClient::STATUS_CONNECTED) {
			r_error = vformat("Could not establish connection to %s (status %d).", host, int(client->get_status()));
			return false;
		}

		Vector<String> headers;
		headers.push_back("User-Agent: MinaEngine-MCP/1.0 (free asset downloader)");
		headers.push_back("Accept: */*");
		err = client->request(HTTPClient::METHOD_GET, path, headers, nullptr, 0);
		if (err != OK) {
			r_error = vformat("Request failed (error %d).", int(err));
			return false;
		}
		while (client->get_status() == HTTPClient::STATUS_REQUESTING) {
			client->poll();
			OS::get_singleton()->delay_usec(1000);
			if (timed_out()) {
				r_error = "Request timed out: " + url;
				return false;
			}
		}
		if (!client->has_response()) {
			r_error = "No response from server: " + url;
			return false;
		}

		int code = client->get_response_code();
		List<String> resp_headers;
		client->get_response_headers(&resp_headers);

		if (code >= 300 && code < 400) {
			String location;
			for (const String &h : resp_headers) {
				if (h.to_lower().begins_with("location:")) {
					location = h.substr(9).strip_edges();
					break;
				}
			}
			if (location.is_empty()) {
				r_error = vformat("HTTP %d redirect without Location header.", code);
				return false;
			}
			url = location.begins_with("/") ? "https://" + host + location : location;
			continue;
		}
		if (code != 200) {
			r_error = vformat("HTTP %d from %s", code, url);
			return false;
		}

		r_body.clear();
		while (client->get_status() == HTTPClient::STATUS_BODY) {
			client->poll();
			PackedByteArray chunk = client->read_response_body_chunk();
			if (chunk.is_empty()) {
				OS::get_singleton()->delay_usec(1000);
			} else {
				r_body.append_array(chunk);
				start = OS::get_singleton()->get_ticks_msec(); // progress resets idle timer
				if (int64_t(r_body.size()) > p_max_bytes) {
					r_error = "Download exceeded size limit.";
					return false;
				}
			}
			if (timed_out()) {
				r_error = "Download stalled: " + url;
				return false;
			}
		}
		return true;
	}
	r_error = "Too many redirects.";
	return false;
}

bool _http_get_json(const String &p_url, Variant &r_json, String &r_error) {
	PackedByteArray body;
	if (!_http_get(p_url, body, r_error, 64LL * 1024 * 1024)) {
		return false;
	}
	String text = String::utf8((const char *)body.ptr(), body.size());
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(text) != OK) {
		r_error = "Server returned invalid JSON.";
		return false;
	}
	r_json = json->get_data();
	return true;
}

bool _write_file(const String &p_res_path, const PackedByteArray &p_data, String &r_error) {
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(p_res_path.get_base_dir()));
	Ref<FileAccess> f = FileAccess::open(p_res_path, FileAccess::WRITE);
	if (f.is_null()) {
		r_error = "Cannot write file: " + p_res_path;
		return false;
	}
	f->store_buffer(p_data);
	return true;
}

String _dest_dir(const Dictionary &p_args, const String &p_asset_id, String &r_error) {
	String base = mcp::normalize_res_path(String(p_args.get("dest_dir", "res://assets/free")));
	if (!mcp::is_safe_res_path(base)) {
		r_error = "Parameter 'dest_dir' must be a safe res:// directory.";
		return String();
	}
	if (base.ends_with("/")) {
		base = base.substr(0, base.length() - 1);
	}
	return base + "/" + p_asset_id;
}

String _attribution(const String &p_source, const String &p_id, const String &p_page) {
	return vformat("Asset: %s\nSource: %s\nPage: %s\nLicense: CC0 1.0 Universal (public domain dedication)\nhttps://creativecommons.org/publicdomain/zero/1.0/\nDownloaded via Mina Engine MCP.\n", p_id, p_source, p_page);
}

bool _matches(const String &p_q, const Variant &p_a, const Variant &p_b, const Variant &p_c) {
	if (p_q.is_empty()) {
		return true;
	}
	return String(p_a).to_lower().contains(p_q) || String(p_b).to_lower().contains(p_q) || String(p_c).to_lower().contains(p_q);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

Dictionary _search_polyhaven(const String &p_query, const String &p_type, int p_limit) {
	String t = p_type == "model" ? "models" : (p_type == "hdri" ? "hdris" : (p_type == "texture" ? "textures" : "all"));
	Variant json;
	String error;
	if (!_http_get_json("https://api.polyhaven.com/assets?t=" + t, json, error)) {
		return mcp::make_error(error, "network_error");
	}
	if (json.get_type() != Variant::DICTIONARY) {
		return mcp::make_error("Unexpected Poly Haven response.", "bad_response");
	}
	Dictionary all = json;
	String q = p_query.to_lower();
	Array results;
	int total_matches = 0;
	Array keys = all.keys();
	for (int i = 0; i < keys.size(); i++) {
		String id = keys[i];
		Dictionary info = all[id];
		String name = info.get("name", id);
		Array tag_arr = info.get("tags", Array());
		Array cat_arr = info.get("categories", Array());
		String tag_str, cat_str;
		for (int j = 0; j < tag_arr.size(); j++) {
			tag_str += String(tag_arr[j]) + " ";
		}
		for (int j = 0; j < cat_arr.size(); j++) {
			cat_str += String(cat_arr[j]) + " ";
		}
		if (!_matches(q, id + " " + name, tag_str, cat_str)) {
			continue;
		}
		total_matches++;
		if (results.size() < p_limit) {
			Dictionary r;
			r["source"] = "polyhaven";
			r["asset_id"] = id;
			r["name"] = name;
			int type_id = int(info.get("type", -1));
			r["type"] = type_id == 0 ? "hdri" : (type_id == 1 ? "texture" : (type_id == 2 ? "model" : "unknown"));
			r["categories"] = cat_arr;
			r["tags"] = tag_arr;
			r["license"] = "CC0-1.0";
			r["page_url"] = "https://polyhaven.com/a/" + id;
			results.push_back(r);
		}
	}
	Dictionary out;
	out["source"] = "polyhaven";
	out["total_matches"] = total_matches;
	out["results"] = results;
	return mcp::make_ok(out);
}

Dictionary _search_ambientcg(const String &p_query, const String &p_type, int p_limit) {
	String url = "https://ambientcg.com/api/v2/full_json?limit=" + itos(p_limit) + "&include=downloadData,tagData&sort=Popular";
	if (!p_query.is_empty()) {
		url += "&q=" + _url_encode(p_query);
	}
	if (p_type == "texture") {
		url += "&type=Material";
	} else if (p_type == "hdri") {
		url += "&type=HDRI";
	} else if (p_type == "model") {
		url += "&type=3DModel";
	}
	Variant json;
	String error;
	if (!_http_get_json(url, json, error)) {
		return mcp::make_error(error, "network_error");
	}
	if (json.get_type() != Variant::DICTIONARY) {
		return mcp::make_error("Unexpected ambientCG response.", "bad_response");
	}
	Dictionary root = json;
	Array found = root.get("foundAssets", Array());
	Array results;
	for (int i = 0; i < found.size(); i++) {
		Dictionary a = found[i];
		Dictionary r;
		r["source"] = "ambientcg";
		r["asset_id"] = a.get("assetId", "");
		r["name"] = a.get("displayName", a.get("assetId", ""));
		String dt = String(a.get("dataType", ""));
		r["type"] = dt == "Material" ? "texture" : (dt == "HDRI" ? "hdri" : (dt == "3DModel" ? "model" : dt.to_lower()));
		r["tags"] = a.get("tags", Array());
		r["license"] = "CC0-1.0";
		r["page_url"] = "https://ambientcg.com/view?id=" + String(a.get("assetId", ""));

		Array attrs;
		if (a.has("downloadFolders")) {
			Dictionary folders = a["downloadFolders"];
			Dictionary def = folders.get("default", Dictionary());
			Dictionary cats = def.get("downloadFiletypeCategories", Dictionary());
			Dictionary zip = cats.get("zip", Dictionary());
			Array dls = zip.get("downloads", Array());
			for (int j = 0; j < dls.size(); j++) {
				Dictionary d = dls[j];
				attrs.push_back(d.get("attribute", ""));
			}
		}
		r["available_variants"] = attrs;
		results.push_back(r);
	}
	Dictionary out;
	out["source"] = "ambientcg";
	out["total_matches"] = int(root.get("numberOfResults", results.size()));
	out["results"] = results;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_search_free(const Dictionary &p_args) {
	String source = String(p_args.get("source", "polyhaven")).to_lower();
	String type = String(p_args.get("type", "any")).to_lower();
	String query = String(p_args.get("query", "")).strip_edges();
	int limit = CLAMP(int(p_args.get("limit", 20)), 1, 100);

	if (source == "polyhaven") {
		return _search_polyhaven(query, type, limit);
	}
	if (source == "ambientcg") {
		return _search_ambientcg(query, type, limit);
	}
	if (source == "all") {
		Dictionary a = _search_polyhaven(query, type, limit);
		Dictionary b = _search_ambientcg(query, type, limit);
		Array merged;
		Dictionary errors;
		if (bool(a.get("ok", false))) {
			merged.append_array(Array(a.get("results", Array())));
		} else {
			errors["polyhaven"] = a.get("error", "failed");
		}
		if (bool(b.get("ok", false))) {
			merged.append_array(Array(b.get("results", Array())));
		} else {
			errors["ambientcg"] = b.get("error", "failed");
		}
		Dictionary out;
		out["results"] = merged;
		if (!errors.is_empty()) {
			out["source_errors"] = errors;
		}
		return mcp::make_ok(out);
	}
	return mcp::make_error("Parameter 'source' must be 'polyhaven', 'ambientcg', or 'all'.", "invalid_argument");
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

void _finish_download(Dictionary &r_out, const String &p_dir, const String &p_source, const String &p_id, const String &p_page, const Array &p_files) {
	String error;
	PackedByteArray lic = _attribution(p_source, p_id, p_page).to_utf8_buffer();
	_write_file(p_dir + "/LICENSE_CC0.txt", lic, error);
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan();
	}
	r_out["asset_id"] = p_id;
	r_out["source"] = p_source;
	r_out["license"] = "CC0-1.0";
	r_out["directory"] = p_dir;
	r_out["files"] = p_files;
	r_out["file_count"] = p_files.size();
	r_out["page_url"] = p_page;
}

Dictionary _download_polyhaven(const Dictionary &p_args, const String &p_id) {
	String res = String(p_args.get("resolution", "1k")).to_lower();
	String fmt = String(p_args.get("format", "")).to_lower();
	int64_t max_total = int64_t(CLAMP(double(p_args.get("max_total_mb", 200.0)), 1.0, double(HARD_MAX_TOTAL_BYTES / (1024 * 1024)))) * 1024 * 1024;

	String derr;
	String dir = _dest_dir(p_args, p_id, derr);
	if (dir.is_empty()) {
		return mcp::make_error(derr, "invalid_argument");
	}

	Variant json;
	String error;
	if (!_http_get_json("https://api.polyhaven.com/files/" + p_id, json, error)) {
		return mcp::make_error(error, "network_error");
	}
	if (json.get_type() != Variant::DICTIONARY) {
		return mcp::make_error("Unexpected Poly Haven response. Check the asset_id.", "bad_response");
	}
	Dictionary files = json;
	if (files.has("message") && files.size() == 1) {
		return mcp::make_error("Poly Haven: " + String(files["message"]), "not_found");
	}

	struct Job {
		String url;
		String rel;
	};
	Vector<Job> jobs;
	String kind;

	if (files.has("hdri")) {
		kind = "hdri";
		Dictionary h = files["hdri"];
		if (!h.has(res)) {
			return mcp::make_error("Resolution '" + res + "' not available for this HDRI.", "variant_not_found");
		}
		Dictionary rd = h[res];
		String f = fmt.is_empty() ? (rd.has("hdr") ? "hdr" : "exr") : fmt;
		if (!rd.has(f)) {
			return mcp::make_error("Format '" + f + "' not available. Try 'hdr' or 'exr'.", "variant_not_found");
		}
		Dictionary fd = rd[f];
		jobs.push_back({ String(fd["url"]), p_id + "_" + res + "." + f });
	} else if (files.has("gltf")) {
		kind = "model";
		Dictionary g = files["gltf"];
		if (!g.has(res)) {
			return mcp::make_error("Resolution '" + res + "' not available for this model.", "variant_not_found");
		}
		Dictionary rd = g[res];
		Dictionary gd = rd.get("gltf", Dictionary());
		if (!gd.has("url")) {
			return mcp::make_error("No glTF file for this model.", "variant_not_found");
		}
		jobs.push_back({ String(gd["url"]), p_id + "_" + res + ".gltf" });
		Dictionary inc = gd.get("include", Dictionary());
		Array keys = inc.keys();
		for (int i = 0; i < keys.size(); i++) {
			String rel = keys[i];
			Dictionary item = inc[rel];
			jobs.push_back({ String(item.get("url", "")), rel });
		}
	} else {
		kind = "texture";
		String f = fmt.is_empty() ? "jpg" : fmt;
		static const char *skip[] = { "blend", "gltf", "mtlx", "usd", "fbx" };
		Array keys = files.keys();
		for (int i = 0; i < keys.size(); i++) {
			String map = keys[i];
			bool skipped = false;
			for (const char *s : skip) {
				if (map == s) {
					skipped = true;
				}
			}
			if (skipped || files[map].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary md = files[map];
			if (!md.has(res)) {
				continue;
			}
			Dictionary rd = md[res];
			String use = rd.has(f) ? f : (rd.has("png") ? "png" : (rd.has("jpg") ? "jpg" : String()));
			if (use.is_empty()) {
				continue;
			}
			Dictionary fd = rd[use];
			jobs.push_back({ String(fd.get("url", "")), map + "_" + res + "." + use });
		}
		if (jobs.is_empty()) {
			return mcp::make_error("No texture maps for resolution '" + res + "'.", "variant_not_found");
		}
	}

	Array written;
	int64_t total = 0;
	for (const Job &job : jobs) {
		if (!_safe_rel(job.rel)) {
			return mcp::make_error("Server provided an unsafe file path: " + job.rel, "unsafe_path");
		}
		PackedByteArray data;
		if (!_http_get(job.url, data, error, max_total - total)) {
			return mcp::make_error(error, "network_error", Dictionary());
		}
		total += data.size();
		String out_path = dir + "/" + job.rel.replace("\\", "/");
		if (!_write_file(out_path, data, error)) {
			return mcp::make_error(error, "write_failed");
		}
		written.push_back(out_path);
	}

	Dictionary out;
	out["asset_type"] = kind;
	out["resolution"] = res;
	out["total_bytes"] = total;
	_finish_download(out, dir, "Poly Haven", p_id, "https://polyhaven.com/a/" + p_id, written);
	return mcp::make_ok(out);
}

Dictionary _download_ambientcg(const Dictionary &p_args, const String &p_id) {
	String res = String(p_args.get("resolution", "1k")).to_upper();
	String fmt = String(p_args.get("format", "jpg")).to_upper();
	String attribute = String(p_args.get("variant", "")).strip_edges();
	int64_t max_total = int64_t(CLAMP(double(p_args.get("max_total_mb", 200.0)), 1.0, double(HARD_MAX_TOTAL_BYTES / (1024 * 1024)))) * 1024 * 1024;

	String derr;
	String dir = _dest_dir(p_args, p_id, derr);
	if (dir.is_empty()) {
		return mcp::make_error(derr, "invalid_argument");
	}

	Variant json;
	String error;
	if (!_http_get_json("https://ambientcg.com/api/v2/full_json?id=" + p_id + "&include=downloadData", json, error)) {
		return mcp::make_error(error, "network_error");
	}
	Dictionary root = json;
	Array found = root.get("foundAssets", Array());
	if (found.is_empty()) {
		return mcp::make_error("ambientCG asset not found: " + p_id, "not_found");
	}
	Dictionary a = found[0];
	Dictionary folders = a.get("downloadFolders", Dictionary());
	Dictionary def = folders.get("default", Dictionary());
	Dictionary cats = def.get("downloadFiletypeCategories", Dictionary());
	Dictionary zipcat = cats.get("zip", Dictionary());
	Array dls = zipcat.get("downloads", Array());

	if (attribute.is_empty()) {
		attribute = res + "-" + fmt;
	}
	String link;
	Array available;
	for (int i = 0; i < dls.size(); i++) {
		Dictionary d = dls[i];
		String attr = d.get("attribute", "");
		available.push_back(attr);
		if (attr.to_upper() == attribute.to_upper()) {
			link = d.get("downloadLink", "");
		}
	}
	if (link.is_empty()) {
		Dictionary details;
		details["available_variants"] = available;
		return mcp::make_error("Variant '" + attribute + "' not available. Pass 'variant' from available_variants.", "variant_not_found", details);
	}

	PackedByteArray zip_data;
	if (!_http_get(link, zip_data, error, max_total)) {
		return mcp::make_error(error, "network_error");
	}

	String zip_path = dir + "/_download.zip";
	if (!_write_file(zip_path, zip_data, error)) {
		return mcp::make_error(error, "write_failed");
	}

	Ref<RefCounted> reader = Ref<RefCounted>(Object::cast_to<RefCounted>(ClassDB::instantiate("ZIPReader")));
	if (reader.is_null()) {
		return mcp::make_error("ZIPReader is not available in this build; the archive was saved to " + zip_path, "zip_unavailable");
	}
	if (int(reader->call("open", zip_path)) != OK) {
		return mcp::make_error("Failed to open downloaded archive.", "zip_error");
	}
	PackedStringArray names = reader->call("get_files");
	Array written;
	int64_t total = 0;
	for (const String &n : names) {
		if (n.ends_with("/")) {
			continue;
		}
		if (!_safe_rel(n)) {
			reader->call("close");
			return mcp::make_error("Archive contains unsafe path: " + n, "unsafe_path");
		}
		PackedByteArray content = reader->call("read_file", n, true);
		total += content.size();
		if (total > max_total * 4) {
			reader->call("close");
			return mcp::make_error("Archive expands beyond the size limit.", "size_limit");
		}
		String out_path = dir + "/" + n;
		if (!_write_file(out_path, content, error)) {
			reader->call("close");
			return mcp::make_error(error, "write_failed");
		}
		written.push_back(out_path);
	}
	reader->call("close");
	DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(zip_path));

	Dictionary out;
	out["variant"] = attribute;
	out["total_bytes"] = total;
	_finish_download(out, dir, "ambientCG", p_id, "https://ambientcg.com/view?id=" + p_id, written);
	return mcp::make_ok(out);
}

Dictionary _tool_asset_download_free(const Dictionary &p_args) {
	if (!EditorFileSystem::get_singleton()) {
		return mcp::make_error("Editor is required to download assets into the project.", "editor_required");
	}
	String source = String(p_args.get("source", "polyhaven")).to_lower();
	String id = String(p_args.get("asset_id", "")).strip_edges();
	if (!_valid_id(id)) {
		return mcp::make_error("Parameter 'asset_id' is required and may only contain letters, digits, '_' and '-'.", "invalid_argument");
	}
	if (source == "polyhaven") {
		return _download_polyhaven(p_args, id);
	}
	if (source == "ambientcg") {
		return _download_ambientcg(p_args, id);
	}
	return mcp::make_error("Parameter 'source' must be 'polyhaven' or 'ambientcg'.", "invalid_argument");
}

} // namespace

void mcp_register_asset_download_tools(MinaMCPServer *p_server) {
	using namespace mcp;
	{
		Dictionary props;
		Array sources;
		sources.push_back("polyhaven");
		sources.push_back("ambientcg");
		sources.push_back("all");
		props["source"] = schema_string("Asset library (default: polyhaven). Both are CC0.", sources);
		props["query"] = schema_string("Search text matched against name, tags and categories (e.g. 'wood', 'rock', 'forest').");
		Array types;
		types.push_back("any");
		types.push_back("texture");
		types.push_back("model");
		types.push_back("hdri");
		props["type"] = schema_string("Asset type filter (default: any).", types);
		props["limit"] = schema_int("Maximum results (default: 20, max: 100).");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_search_free";
		t.title = "Search Free Assets";
		t.description = "Search free CC0 asset libraries (Poly Haven, ambientCG) for PBR textures, 3D models and HDRIs. Returns asset_id values for asset_download_free.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props);
		t.handler = _tool_asset_search_free;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		Array sources;
		sources.push_back("polyhaven");
		sources.push_back("ambientcg");
		props["source"] = schema_string("Asset library (default: polyhaven).", sources);
		props["asset_id"] = schema_string("Asset id from asset_search_free.");
		props["resolution"] = schema_string("Resolution such as '1k', '2k', '4k' (default: 1k).");
		props["format"] = schema_string("File format: textures 'jpg'/'png', HDRIs 'hdr'/'exr' (Poly Haven default: jpg/hdr; ambientCG default: jpg).");
		props["variant"] = schema_string("ambientCG only: exact download variant such as '2K-JPG' (overrides resolution/format).");
		props["dest_dir"] = schema_string("Destination res:// directory; a sub-folder per asset is created (default: res://assets/free).");
		props["max_total_mb"] = schema_number("Abort if the download exceeds this size in MB (default: 200, max: 1024).");
		Array req;
		req.push_back("asset_id");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_download_free";
		t.title = "Download Free Asset";
		t.description = "Download a CC0 asset from Poly Haven or ambientCG into the project (HTTPS and allowlisted hosts only), extract it, write a LICENSE_CC0.txt attribution file and rescan the filesystem.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_download_free;
		p_server->register_tool(t);
	}
}
