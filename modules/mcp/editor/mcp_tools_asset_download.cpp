/**************************************************************************/
/*  mcp_tools_asset_download.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

// Free asset search / download tools + PBR StandardMaterial3D generator.
//
// Sources:
//   - Poly Haven (api.polyhaven.com): CC0 PBR textures, 3D models (glTF), HDRIs
//   - ambientCG (ambientcg.com): CC0 PBR textures, HDRIs, 3D models
//   - Lots of Sounds (api.lotsofsounds.com): CC0 sound effects (MP3)
//   - Wikimedia Commons (commons.wikimedia.org): free audio / SFX (WAV, OGG, MP3, FLAC)
//
// Only HTTPS and an explicit host allowlist are permitted (including redirects);
// files are only written below res://, never outside the project.

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
	static const char *allowed[] = {
		"polyhaven.com",
		"polyhaven.org",
		"ambientcg.com",
		"struffelproductions.com", // ambientCG download CDN
		"lotsofsounds.com", // Lots of Sounds CC0 SFX API & CDN
		"wikimedia.org", // Wikimedia Commons API & upload CDN
	};
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

String _strip_html(const String &p_html) {
	String out;
	bool in_tag = false;
	for (int i = 0; i < p_html.length(); i++) {
		char32_t c = p_html[i];
		if (c == '<') {
			in_tag = true;
		} else if (c == '>') {
			in_tag = false;
		} else if (!in_tag) {
			out += c;
		}
	}
	return out.strip_edges();
}

String _sanitize_filename(const String &p_name, const String &p_fallback_ext = "") {
	String out;
	for (int i = 0; i < p_name.length(); i++) {
		char32_t c = p_name[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
		if (ok) {
			out += c;
		} else if (c == ' ') {
			out += '_';
		}
	}
	while (out.begins_with(".") || out.begins_with("_")) {
		out = out.substr(1);
	}
	out = out.replace("..", "_");
	if (out.is_empty()) {
		out = "asset" + p_fallback_ext;
	}
	if (out.length() > 96) {
		String ext = out.get_extension();
		String base = out.get_basename().substr(0, 80);
		out = ext.is_empty() ? base : (base + "." + ext);
	}
	return out;
}

bool _is_cc0_or_pd(const String &p_license) {
	String l = p_license.to_lower();
	return l.contains("cc0") || l.contains("public domain") || l == "pd" || l.begins_with("pd-");
}

Dictionary _search_lotsofsounds(const String &p_query, int p_limit) {
	String url = "https://api.lotsofsounds.com/api/v1/sounds/sample?limit=" + itos(CLAMP(p_limit, 1, 50));
	if (!p_query.is_empty()) {
		url += "&q=" + _url_encode(p_query);
	}
	Variant json;
	String error;
	if (!_http_get_json(url, json, error)) {
		return mcp::make_error(error, "network_error");
	}
	if (json.get_type() != Variant::DICTIONARY) {
		return mcp::make_error("Unexpected Lots of Sounds response.", "bad_response");
	}
	Dictionary root = json;
	Array items = root.get("data", Array());
	Array results;
	for (int i = 0; i < items.size(); i++) {
		if (items[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary a = items[i];
		Dictionary r;
		r["source"] = "lotsofsounds";
		r["asset_id"] = String(a.get("id", ""));
		r["name"] = String(a.get("name", a.get("id", "")));
		r["description"] = String(a.get("description", ""));
		r["type"] = "audio";
		r["format"] = "mp3";
		r["duration_sec"] = double(a.get("duration", 0.0));
		r["tags"] = a.get("tags", Array());
		r["license"] = "CC0-1.0";
		results.push_back(r);
	}
	Dictionary out;
	out["source"] = "lotsofsounds";
	out["total_matches"] = results.size();
	out["results"] = results;
	return mcp::make_ok(out);
}

Dictionary _search_wikimedia(const String &p_query, int p_limit, bool p_cc0_only) {
	String search_term = p_query.is_empty() ? "filetype:audio sound effect" : ("filetype:audio " + p_query);
	int fetch_limit = CLAMP(p_cc0_only ? p_limit * 3 : p_limit, 1, 50);
	String url = "https://commons.wikimedia.org/w/api.php?action=query&generator=search&gsrnamespace=6&prop=imageinfo&iiprop=url%7Cmime%7Csize%7Cextmetadata&format=json&gsrlimit=" + itos(fetch_limit) + "&gsrsearch=" + _url_encode(search_term);
	Variant json;
	String error;
	if (!_http_get_json(url, json, error)) {
		return mcp::make_error(error, "network_error");
	}
	if (json.get_type() != Variant::DICTIONARY) {
		return mcp::make_error("Unexpected Wikimedia Commons response.", "bad_response");
	}
	Dictionary root = json;
	Dictionary query_dict = root.get("query", Dictionary());
	Dictionary pages = query_dict.get("pages", Dictionary());
	Array results;
	Array keys = pages.keys();
	for (int i = 0; i < keys.size() && results.size() < p_limit; i++) {
		Dictionary p = pages[keys[i]];
		Array ii_arr = p.get("imageinfo", Array());
		if (ii_arr.is_empty() || ii_arr[0].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary ii = ii_arr[0];
		String file_url = String(ii.get("url", ""));
		String ext = file_url.get_slice("?", 0).get_extension().to_lower();
		if (ext != "wav" && ext != "ogg" && ext != "mp3" && ext != "flac") {
			continue;
		}
		Dictionary meta = ii.get("extmetadata", Dictionary());
		Dictionary lic_d = meta.get("LicenseShortName", Dictionary());
		Dictionary art_d = meta.get("Artist", Dictionary());
		String lic = _strip_html(String(lic_d.get("value", "Unknown")));
		String artist = _strip_html(String(art_d.get("value", "")));
		if (p_cc0_only && !_is_cc0_or_pd(lic)) {
			continue;
		}
		String title = String(p.get("title", ""));
		if (title.begins_with("File:")) {
			title = title.substr(5);
		}
		Dictionary r;
		r["source"] = "wikimedia";
		r["asset_id"] = itos(int64_t(p.get("pageid", 0)));
		r["name"] = title;
		r["type"] = "audio";
		r["format"] = ext;
		r["mime"] = String(ii.get("mime", ""));
		r["size_bytes"] = int64_t(ii.get("size", 0));
		r["license"] = lic;
		r["artist"] = artist;
		r["page_url"] = "https://commons.wikimedia.org/?curid=" + itos(int64_t(p.get("pageid", 0)));
		results.push_back(r);
	}
	Dictionary out;
	out["source"] = "wikimedia";
	out["total_matches"] = results.size();
	out["results"] = results;
	return mcp::make_ok(out);
}

Dictionary _tool_asset_search_free(const Dictionary &p_args) {
	String type = String(p_args.get("type", "any")).to_lower();
	String default_source = (type == "audio") ? "lotsofsounds" : "polyhaven";
	String source = String(p_args.get("source", default_source)).to_lower();
	String query = String(p_args.get("query", "")).strip_edges();
	int limit = CLAMP(int(p_args.get("limit", 20)), 1, 100);
	bool cc0_only = bool(p_args.get("cc0_only", false));

	if (source == "polyhaven") {
		return _search_polyhaven(query, type, limit);
	}
	if (source == "ambientcg") {
		return _search_ambientcg(query, type, limit);
	}
	if (source == "lotsofsounds") {
		return _search_lotsofsounds(query, limit);
	}
	if (source == "wikimedia") {
		return _search_wikimedia(query, limit, cc0_only);
	}
	if (source == "all") {
		Array merged;
		Dictionary errors;
		auto collect = [&](const String &p_name, const Dictionary &p_res) {
			if (bool(p_res.get("ok", false))) {
				merged.append_array(Array(p_res.get("results", Array())));
			} else {
				errors[p_name] = p_res.get("error", "failed");
			}
		};
		if (type == "audio") {
			collect("lotsofsounds", _search_lotsofsounds(query, limit));
			collect("wikimedia", _search_wikimedia(query, limit, cc0_only));
		} else {
			collect("polyhaven", _search_polyhaven(query, type, limit));
			collect("ambientcg", _search_ambientcg(query, type, limit));
			if (type == "any") {
				collect("lotsofsounds", _search_lotsofsounds(query, MIN(limit, 10)));
			}
		}
		Dictionary out;
		out["results"] = merged;
		if (!errors.is_empty()) {
			out["source_errors"] = errors;
		}
		return mcp::make_ok(out);
	}
	return mcp::make_error("Parameter 'source' must be 'polyhaven', 'ambientcg', 'lotsofsounds', 'wikimedia', or 'all'.", "invalid_argument");
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PBR StandardMaterial3D (.tres) auto-generation
// ---------------------------------------------------------------------------

bool _is_image_ext(const String &p_ext) {
	String e = p_ext.to_lower();
	return e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "tga" || e == "bmp" || e == "exr";
}

bool _has_token(const String &p_stem, const char *p_tok) {
	String s = "_" + p_stem.to_lower().replace("-", "_").replace(" ", "_") + "_";
	String t = String("_") + p_tok + "_";
	return s.contains(t);
}

// Writes a StandardMaterial3D .tres referencing texture paths directly so it
// works even before EditorFileSystem finishes generating .import sidecars.
bool _generate_pbr_material_tres(
		const Array &p_file_paths,
		const String &p_save_path,
		bool p_enable_heightmap,
		bool p_triplanar,
		const Vector3 &p_uv_scale,
		Dictionary &r_maps,
		String &r_error) {
	String albedo, normal_gl, normal_fallback, orm, roughness, metallic, ao, heightmap, emission;

	for (int i = 0; i < p_file_paths.size(); i++) {
		String path = String(p_file_paths[i]);
		if (!_is_image_ext(path.get_extension())) {
			continue;
		}
		String stem = path.get_file().get_basename().to_lower();
		if (stem.contains("preview") || stem.contains("thumbnail")) {
			continue;
		}

		if (albedo.is_empty() && (_has_token(stem, "diff") || _has_token(stem, "diffuse") || _has_token(stem, "color") || _has_token(stem, "albedo") || _has_token(stem, "basecolor") || _has_token(stem, "base_color") || _has_token(stem, "col"))) {
			albedo = path;
		} else if (_has_token(stem, "nor_gl") || _has_token(stem, "normalgl") || _has_token(stem, "normal_gl")) {
			normal_gl = path;
		} else if (normal_fallback.is_empty() && (_has_token(stem, "normal") || _has_token(stem, "nor") || _has_token(stem, "nrm")) && !_has_token(stem, "nor_dx") && !_has_token(stem, "normaldx")) {
			normal_fallback = path;
		} else if (orm.is_empty() && (_has_token(stem, "arm") || _has_token(stem, "orm"))) {
			orm = path;
		} else if (roughness.is_empty() && (_has_token(stem, "rough") || _has_token(stem, "roughness"))) {
			roughness = path;
		} else if (metallic.is_empty() && (_has_token(stem, "metal") || _has_token(stem, "metallic") || _has_token(stem, "metalness"))) {
			metallic = path;
		} else if (ao.is_empty() && (_has_token(stem, "ao") || _has_token(stem, "ambientocclusion") || _has_token(stem, "ambient_occlusion"))) {
			ao = path;
		} else if (heightmap.is_empty() && (_has_token(stem, "disp") || _has_token(stem, "displacement") || _has_token(stem, "height"))) {
			heightmap = path;
		} else if (emission.is_empty() && (_has_token(stem, "emit") || _has_token(stem, "emission") || _has_token(stem, "emissive"))) {
			emission = path;
		}
	}

	String normal = !normal_gl.is_empty() ? normal_gl : normal_fallback;
	if (albedo.is_empty() && normal.is_empty() && orm.is_empty() && roughness.is_empty() && metallic.is_empty() && ao.is_empty() && emission.is_empty()) {
		r_error = "No recognizable PBR texture maps found.";
		return false;
	}

	HashMap<String, String> path_to_id;
	Vector<String> ordered_paths;
	auto reg_tex = [&](const String &p_path) -> String {
		if (p_path.is_empty()) {
			return String();
		}
		if (path_to_id.has(p_path)) {
			return path_to_id[p_path];
		}
		String id = itos(ordered_paths.size() + 1);
		path_to_id[p_path] = id;
		ordered_paths.push_back(p_path);
		return id;
	};

	String id_albedo = reg_tex(albedo);
	String id_normal = reg_tex(normal);
	String id_rough = reg_tex(!roughness.is_empty() ? roughness : orm);
	String id_metal = reg_tex(!metallic.is_empty() ? metallic : orm);
	String id_ao = reg_tex(!ao.is_empty() ? ao : orm);
	String id_emit = reg_tex(emission);
	String id_height = p_enable_heightmap ? reg_tex(heightmap) : String();

	String tres;
	tres += vformat("[gd_resource type=\"StandardMaterial3D\" load_steps=%d format=3]\n\n", ordered_paths.size() + 1);
	for (int i = 0; i < ordered_paths.size(); i++) {
		tres += vformat("[ext_resource type=\"Texture2D\" path=\"%s\" id=\"%d\"]\n", ordered_paths[i], i + 1);
	}
	tres += "\n[resource]\n";
	if (!id_albedo.is_empty()) {
		tres += vformat("albedo_texture = ExtResource(\"%s\")\n", id_albedo);
		r_maps["albedo"] = albedo;
	}
	if (!id_metal.is_empty()) {
		tres += "metallic = 1.0\n";
		tres += vformat("metallic_texture = ExtResource(\"%s\")\n", id_metal);
		if (metallic.is_empty() && !orm.is_empty()) {
			tres += "metallic_texture_channel = 2\n";
			r_maps["metallic"] = orm + " (B)";
		} else {
			r_maps["metallic"] = metallic;
		}
	}
	if (!id_rough.is_empty()) {
		tres += vformat("roughness_texture = ExtResource(\"%s\")\n", id_rough);
		if (roughness.is_empty() && !orm.is_empty()) {
			tres += "roughness_texture_channel = 1\n";
			r_maps["roughness"] = orm + " (G)";
		} else {
			r_maps["roughness"] = roughness;
		}
	}
	if (!id_emit.is_empty()) {
		tres += "emission_enabled = true\n";
		tres += vformat("emission_texture = ExtResource(\"%s\")\n", id_emit);
		r_maps["emission"] = emission;
	}
	if (!id_normal.is_empty()) {
		tres += "normal_enabled = true\n";
		tres += vformat("normal_texture = ExtResource(\"%s\")\n", id_normal);
		r_maps["normal"] = normal;
	}
	if (!id_ao.is_empty()) {
		tres += "ao_enabled = true\n";
		tres += vformat("ao_texture = ExtResource(\"%s\")\n", id_ao);
		if (ao.is_empty() && !orm.is_empty()) {
			tres += "ao_texture_channel = 0\n";
			r_maps["ao"] = orm + " (R)";
		} else {
			r_maps["ao"] = ao;
		}
	}
	if (!id_height.is_empty()) {
		tres += "heightmap_enabled = true\n";
		tres += "heightmap_scale = 5.0\n";
		tres += vformat("heightmap_texture = ExtResource(\"%s\")\n", id_height);
		r_maps["heightmap"] = heightmap;
	}
	if (p_uv_scale != Vector3(1, 1, 1)) {
		tres += vformat("uv1_scale = Vector3(%f, %f, %f)\n", p_uv_scale.x, p_uv_scale.y, p_uv_scale.z);
	}
	if (p_triplanar) {
		tres += "uv1_triplanar = true\n";
	}

	if (!_write_file(p_save_path, tres.to_utf8_buffer(), r_error)) {
		return false;
	}
	return true;
}

void _maybe_attach_pbr_material(const Dictionary &p_args, const String &p_dir, const String &p_id, Array &r_written, Dictionary &r_out) {
	if (!bool(p_args.get("create_material", true))) {
		return;
	}
	String mat_path = p_dir + "/" + p_id + ".tres";
	bool enable_heightmap = bool(p_args.get("enable_heightmap", false));
	bool triplanar = bool(p_args.get("triplanar", false));
	Vector3 uv_scale(1, 1, 1);
	if (p_args.has("uv_scale")) {
		mcp::parse_vector3(p_args["uv_scale"], uv_scale);
	}
	Dictionary maps;
	String merr;
	if (_generate_pbr_material_tres(r_written, mat_path, enable_heightmap, triplanar, uv_scale, maps, merr)) {
		r_written.push_back(mat_path);
		r_out["material_path"] = mat_path;
		r_out["material_maps"] = maps;
	}
}

void _finish_download(Dictionary &r_out, const String &p_dir, const String &p_source, const String &p_id, const String &p_page, const Array &p_files, const String &p_license = "CC0-1.0", const String &p_artist = "") {
	String error;
	String lic_filename = _is_cc0_or_pd(p_license) ? "LICENSE_CC0.txt" : "LICENSE.txt";
	String lic_text;
	if (_is_cc0_or_pd(p_license) && p_artist.is_empty()) {
		lic_text = _attribution(p_source, p_id, p_page);
	} else {
		lic_text = vformat("Asset: %s\nSource: %s\nPage: %s\nLicense: %s\n", p_id, p_source, p_page, p_license);
		if (!p_artist.is_empty()) {
			lic_text += "Artist / Author: " + p_artist + "\n";
		}
		lic_text += "Downloaded via Mina Engine MCP.\n";
	}
	_write_file(p_dir + "/" + lic_filename, lic_text.to_utf8_buffer(), error);
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan();
	}
	r_out["asset_id"] = p_id;
	r_out["source"] = p_source;
	r_out["license"] = p_license;
	if (!p_artist.is_empty()) {
		r_out["artist"] = p_artist;
	}
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

	bool has_pbr_maps = files.has("Diffuse") || files.has("diff") || files.has("nor_gl") || files.has("arm") || files.has("Rough") || files.has("rough");
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
	} else if (files.has("gltf") && !has_pbr_maps) {
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
	if (kind == "texture") {
		_maybe_attach_pbr_material(p_args, dir, p_id, written, out);
	}
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
	_maybe_attach_pbr_material(p_args, dir, p_id, written, out);
	_finish_download(out, dir, "ambientCG", p_id, "https://ambientcg.com/view?id=" + p_id, written);
	return mcp::make_ok(out);
}

Dictionary _download_lotsofsounds(const Dictionary &p_args, const String &p_id) {
	int64_t max_total = int64_t(CLAMP(double(p_args.get("max_total_mb", 200.0)), 1.0, double(HARD_MAX_TOTAL_BYTES / (1024 * 1024)))) * 1024 * 1024;
	String derr;
	String dir = _dest_dir(p_args, p_id, derr);
	if (dir.is_empty()) {
		return mcp::make_error(derr, "invalid_argument");
	}

	Variant json;
	String error;
	if (!_http_get_json("https://api.lotsofsounds.com/api/v1/sounds/sample/" + p_id + "/stream", json, error)) {
		return mcp::make_error(error, "network_error");
	}
	if (json.get_type() != Variant::DICTIONARY) {
		return mcp::make_error("Unexpected Lots of Sounds stream response.", "bad_response");
	}
	Dictionary root = json;
	Dictionary data = root.get("data", Dictionary());
	String stream_url = String(data.get("stream_url", ""));
	if (stream_url.is_empty()) {
		return mcp::make_error("Sound not found on Lots of Sounds: " + p_id, "not_found");
	}
	if (stream_url.begins_with("/")) {
		stream_url = "https://api.lotsofsounds.com" + stream_url;
	}

	PackedByteArray audio_bytes;
	if (!_http_get(stream_url, audio_bytes, error, max_total)) {
		return mcp::make_error(error, "network_error");
	}

	String raw_name = String(data.get("name", p_id));
	String fname = _sanitize_filename(raw_name, ".mp3");
	if (fname.get_extension().is_empty()) {
		fname += ".mp3";
	}
	String out_path = dir + "/" + fname;
	if (!_write_file(out_path, audio_bytes, error)) {
		return mcp::make_error(error, "write_failed");
	}

	Array written;
	written.push_back(out_path);
	Dictionary out;
	out["asset_type"] = "audio";
	out["format"] = fname.get_extension().to_lower();
	out["total_bytes"] = int64_t(audio_bytes.size());
	_finish_download(out, dir, "Lots of Sounds", p_id, "https://api.lotsofsounds.com/api/v1/sounds/sample/" + p_id, written, "CC0-1.0");
	return mcp::make_ok(out);
}

Dictionary _download_wikimedia(const Dictionary &p_args, const String &p_id) {
	int64_t max_total = int64_t(CLAMP(double(p_args.get("max_total_mb", 200.0)), 1.0, double(HARD_MAX_TOTAL_BYTES / (1024 * 1024)))) * 1024 * 1024;
	String derr;
	String dir = _dest_dir(p_args, p_id, derr);
	if (dir.is_empty()) {
		return mcp::make_error(derr, "invalid_argument");
	}

	Variant json;
	String error;
	String meta_url = "https://commons.wikimedia.org/w/api.php?action=query&prop=imageinfo&iiprop=url%7Cmime%7Csize%7Cextmetadata&format=json&pageids=" + _url_encode(p_id);
	if (!_http_get_json(meta_url, json, error)) {
		return mcp::make_error(error, "network_error");
	}
	if (json.get_type() != Variant::DICTIONARY) {
		return mcp::make_error("Unexpected Wikimedia Commons response.", "bad_response");
	}
	Dictionary root = json;
	Dictionary pages = Dictionary(root.get("query", Dictionary())).get("pages", Dictionary());
	if (!pages.has(p_id)) {
		return mcp::make_error("Wikimedia Commons pageid not found: " + p_id, "not_found");
	}
	Dictionary p = pages[p_id];
	Array ii_arr = p.get("imageinfo", Array());
	if (ii_arr.is_empty() || ii_arr[0].get_type() != Variant::DICTIONARY) {
		return mcp::make_error("No downloadable file on Wikimedia pageid: " + p_id, "not_found");
	}
	Dictionary ii = ii_arr[0];
	String file_url = String(ii.get("url", ""));
	String ext = file_url.get_slice("?", 0).get_extension().to_lower();
	if (ext != "wav" && ext != "ogg" && ext != "mp3" && ext != "flac") {
		return mcp::make_error("Unsupported Wikimedia file extension: " + ext, "unsupported_format");
	}

	PackedByteArray audio_bytes;
	if (!_http_get(file_url, audio_bytes, error, max_total)) {
		return mcp::make_error(error, "network_error");
	}

	String title = String(p.get("title", p_id));
	if (title.begins_with("File:")) {
		title = title.substr(5);
	}
	String fname = _sanitize_filename(title, "." + ext);
	if (fname.get_extension().to_lower() != ext) {
		fname += "." + ext;
	}
	String out_path = dir + "/" + fname;
	if (!_write_file(out_path, audio_bytes, error)) {
		return mcp::make_error(error, "write_failed");
	}

	Dictionary meta = ii.get("extmetadata", Dictionary());
	String lic = _strip_html(String(Dictionary(meta.get("LicenseShortName", Dictionary())).get("value", "Unknown")));
	String artist = _strip_html(String(Dictionary(meta.get("Artist", Dictionary())).get("value", "")));

	Array written;
	written.push_back(out_path);
	Dictionary out;
	out["asset_type"] = "audio";
	out["format"] = ext;
	out["total_bytes"] = int64_t(audio_bytes.size());
	_finish_download(out, dir, "Wikimedia Commons", p_id, "https://commons.wikimedia.org/?curid=" + p_id, written, lic, artist);
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
	if (source == "lotsofsounds") {
		return _download_lotsofsounds(p_args, id);
	}
	if (source == "wikimedia") {
		return _download_wikimedia(p_args, id);
	}
	return mcp::make_error("Parameter 'source' must be 'polyhaven', 'ambientcg', 'lotsofsounds', or 'wikimedia'.", "invalid_argument");
}

Dictionary _tool_asset_create_pbr_material(const Dictionary &p_args) {
	if (!EditorFileSystem::get_singleton()) {
		return mcp::make_error("Editor is required to create materials.", "editor_required");
	}
	String dir = mcp::normalize_res_path(String(p_args.get("directory", "")));
	if (!mcp::is_safe_res_path(dir)) {
		return mcp::make_error("Parameter 'directory' must be a safe res:// directory.", "invalid_argument");
	}
	if (dir.ends_with("/")) {
		dir = dir.substr(0, dir.length() - 1);
	}
	String abs_dir = ProjectSettings::get_singleton()->globalize_path(dir);
	Ref<DirAccess> da = DirAccess::open(abs_dir);
	if (da.is_null()) {
		return mcp::make_error("Directory does not exist: " + dir, "not_found");
	}

	Array files;
	da->list_dir_begin();
	String f = da->get_next();
	while (!f.is_empty()) {
		if (!f.begins_with(".")) {
			if (!da->current_is_dir()) {
				files.push_back(dir + "/" + f);
			} else if (f.to_lower() == "textures") {
				Ref<DirAccess> sub = DirAccess::open(abs_dir + "/" + f);
				if (sub.is_valid()) {
					sub->list_dir_begin();
					String sf = sub->get_next();
					while (!sf.is_empty()) {
						if (!sub->current_is_dir() && !sf.begins_with(".")) {
							files.push_back(dir + "/" + f + "/" + sf);
						}
						sf = sub->get_next();
					}
					sub->list_dir_end();
				}
			}
		}
		f = da->get_next();
	}
	da->list_dir_end();

	String save_path = String(p_args.get("save_path", "")).strip_edges();
	if (save_path.is_empty()) {
		String base = dir.get_file();
		if (base.is_empty()) {
			base = "pbr_material";
		}
		save_path = dir + "/" + base + ".tres";
	} else {
		save_path = mcp::normalize_res_path(save_path);
		if (!save_path.ends_with(".tres")) {
			save_path += ".tres";
		}
	}
	if (!mcp::is_safe_res_path(save_path)) {
		return mcp::make_error("Parameter 'save_path' must be a safe res:// .tres path.", "invalid_argument");
	}

	bool enable_heightmap = bool(p_args.get("enable_heightmap", false));
	bool triplanar = bool(p_args.get("triplanar", false));
	Vector3 uv_scale(1, 1, 1);
	if (p_args.has("uv_scale")) {
		mcp::parse_vector3(p_args["uv_scale"], uv_scale);
	}

	Dictionary maps;
	String error;
	if (!_generate_pbr_material_tres(files, save_path, enable_heightmap, triplanar, uv_scale, maps, error)) {
		return mcp::make_error(error, "pbr_generation_failed");
	}
	EditorFileSystem::get_singleton()->scan();

	Dictionary out;
	out["material_path"] = save_path;
	out["directory"] = dir;
	out["material_maps"] = maps;
	return mcp::make_ok(out);
}

} // namespace

void mcp_register_asset_download_tools(MinaMCPServer *p_server) {
	using namespace mcp;
	{
		Dictionary props;
		Array sources;
		sources.push_back("polyhaven");
		sources.push_back("ambientcg");
		sources.push_back("lotsofsounds");
		sources.push_back("wikimedia");
		sources.push_back("all");
		props["source"] = schema_string("Asset library (default: polyhaven, or lotsofsounds when type='audio').", sources);
		props["query"] = schema_string("Search text matched against name, tags and categories (e.g. 'wood', 'rock', 'footsteps', 'door creak').");
		Array types;
		types.push_back("any");
		types.push_back("texture");
		types.push_back("model");
		types.push_back("hdri");
		types.push_back("audio");
		props["type"] = schema_string("Asset type filter (default: any). Use 'audio' for sound effects & ambience.", types);
		props["cc0_only"] = schema_bool("wikimedia only: restrict results to CC0 / Public Domain audio (default: false).");
		props["limit"] = schema_int("Maximum results (default: 20, max: 100).");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_search_free";
		t.title = "Search Free Assets";
		t.description = "Search free asset libraries for PBR textures, 3D models, HDRIs (Poly Haven, ambientCG - CC0) and sound effects/audio (Lots of Sounds - CC0, Wikimedia Commons - WAV/OGG/MP3). Returns asset_id values for asset_download_free.";
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
		sources.push_back("lotsofsounds");
		sources.push_back("wikimedia");
		props["source"] = schema_string("Asset library (default: polyhaven).", sources);
		props["asset_id"] = schema_string("Asset id from asset_search_free.");
		props["resolution"] = schema_string("Resolution such as '1k', '2k', '4k' (default: 1k, textures/models/HDRIs only).");
		props["format"] = schema_string("File format: textures 'jpg'/'png', HDRIs 'hdr'/'exr' (Poly Haven default: jpg/hdr; ambientCG default: jpg).");
		props["variant"] = schema_string("ambientCG only: exact download variant such as '2K-JPG' (overrides resolution/format).");
		props["dest_dir"] = schema_string("Destination res:// directory; a sub-folder per asset is created (default: res://assets/free).");
		props["create_material"] = schema_bool("For PBR textures: automatically generate a StandardMaterial3D (.tres) linking Albedo/Normal/Roughness/Metallic/AO maps (default: true).");
		props["enable_heightmap"] = schema_bool("When create_material=true, also wire displacement/height map (default: false).");
		props["triplanar"] = schema_bool("When create_material=true, enable uv1_triplanar (default: false).");
		props["uv_scale"] = schema_any("When create_material=true, optional uv1_scale Vector3 (e.g. [2, 2, 2]).");
		props["max_total_mb"] = schema_number("Abort if the download exceeds this size in MB (default: 200, max: 1024).");
		Array req;
		req.push_back("asset_id");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_download_free";
		t.title = "Download Free Asset";
		t.description = "Download a free asset (PBR texture, 3D model, HDRI, or audio SFX) from Poly Haven, ambientCG, Lots of Sounds, or Wikimedia Commons into the project (HTTPS and allowlisted hosts only), auto-generate a StandardMaterial3D (.tres) for PBR textures, write a license file, and rescan the filesystem.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_download_free;
		p_server->register_tool(t);
	}
	{
		Dictionary props;
		props["directory"] = schema_string("res:// directory containing PBR texture maps (e.g. 'res://assets/free/Bricks113A').");
		props["save_path"] = schema_string("Optional res:// .tres output path (defaults to '<directory>/<folder_name>.tres').");
		props["enable_heightmap"] = schema_bool("Wire displacement/height map into heightmap_texture (default: false).");
		props["triplanar"] = schema_bool("Enable uv1_triplanar projection (default: false).");
		props["uv_scale"] = schema_any("Optional uv1_scale Vector3 (e.g. [2, 2, 2]).");
		Array req;
		req.push_back("directory");

		MinaMCPServer::ToolDefinition t;
		t.name = "asset_create_pbr_material";
		t.title = "Create PBR StandardMaterial3D";
		t.description = "Auto-detect PBR texture maps (Albedo, NormalGL, ARM/ORM, Roughness, Metallic, AO, Emission, Height) in a res:// folder and generate a ready-to-use StandardMaterial3D (.tres) resource.";
		t.category = "assets";
		t.input_schema = make_tool_schema(props, req);
		t.handler = _tool_asset_create_pbr_material;
		p_server->register_tool(t);
	}
}
