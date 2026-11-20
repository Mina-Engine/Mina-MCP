/**************************************************************************/
/*  mcp_variant_codec.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "mcp_variant_codec.h"

#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/variant/variant_parser.h"

namespace mcp {

Dictionary schema_string(const String &p_description, const Array &p_enum) {
	Dictionary s;
	s["type"] = "string";
	if (!p_description.is_empty()) {
		s["description"] = p_description;
	}
	if (!p_enum.is_empty()) {
		s["enum"] = p_enum;
	}
	return s;
}

Dictionary schema_int(const String &p_description) {
	Dictionary s;
	s["type"] = "integer";
	if (!p_description.is_empty()) {
		s["description"] = p_description;
	}
	return s;
}

Dictionary schema_number(const String &p_description) {
	Dictionary s;
	s["type"] = "number";
	if (!p_description.is_empty()) {
		s["description"] = p_description;
	}
	return s;
}

Dictionary schema_bool(const String &p_description) {
	Dictionary s;
	s["type"] = "boolean";
	if (!p_description.is_empty()) {
		s["description"] = p_description;
	}
	return s;
}

Dictionary schema_array(const String &p_description, const Dictionary &p_items) {
	Dictionary s;
	s["type"] = "array";
	if (!p_description.is_empty()) {
		s["description"] = p_description;
	}
	if (!p_items.is_empty()) {
		s["items"] = p_items;
	}
	return s;
}

Dictionary schema_object(const String &p_description, const Dictionary &p_properties, const Array &p_required, bool p_additional_properties) {
	Dictionary s;
	s["type"] = "object";
	if (!p_description.is_empty()) {
		s["description"] = p_description;
	}
	if (!p_properties.is_empty()) {
		s["properties"] = p_properties;
	}
	if (!p_required.is_empty()) {
		s["required"] = p_required;
	}
	s["additionalProperties"] = p_additional_properties;
	return s;
}

Dictionary schema_any(const String &p_description) {
	Dictionary s;
	if (!p_description.is_empty()) {
		s["description"] = p_description;
	}
	return s;
}

Dictionary make_tool_schema(const Dictionary &p_properties, const Array &p_required) {
	Dictionary s;
	s["type"] = "object";
	s["properties"] = p_properties;
	if (!p_required.is_empty()) {
		s["required"] = p_required;
	}
	return s;
}

Dictionary make_ok(const Dictionary &p_data) {
	Dictionary result = p_data.duplicate();
	result["ok"] = true;
	return result;
}

Dictionary make_error(const String &p_message, const String &p_code, const Dictionary &p_details) {
	Dictionary result;
	result["ok"] = false;
	result["error"] = p_message;
	result["code"] = p_code;
	if (!p_details.is_empty()) {
		result["details"] = p_details;
	}
	return result;
}

Dictionary make_image_result(const String &p_base64_png, const Dictionary &p_metadata) {
	Dictionary result = p_metadata.duplicate();
	result["ok"] = true;
	result["_mcp_image_base64"] = p_base64_png;
	result["_mcp_image_mime"] = "image/png";
	return result;
}

bool parse_vector2(const Variant &p_value, Vector2 &r_out) {
	if (p_value.get_type() == Variant::VECTOR2) {
		r_out = p_value;
		return true;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array arr = p_value;
		if (arr.size() >= 2) {
			r_out = Vector2(double(arr[0]), double(arr[1]));
			return true;
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_value;
		if (d.has("x") && d.has("y")) {
			r_out = Vector2(double(d["x"]), double(d["y"]));
			return true;
		}
	}
	return false;
}

bool parse_vector3(const Variant &p_value, Vector3 &r_out) {
	if (p_value.get_type() == Variant::VECTOR3) {
		r_out = p_value;
		return true;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array arr = p_value;
		if (arr.size() >= 3) {
			r_out = Vector3(double(arr[0]), double(arr[1]), double(arr[2]));
			return true;
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_value;
		if (d.has("x") && d.has("y") && d.has("z")) {
			r_out = Vector3(double(d["x"]), double(d["y"]), double(d["z"]));
			return true;
		}
	}
	return false;
}

bool parse_color(const Variant &p_value, Color &r_out) {
	if (p_value.get_type() == Variant::COLOR) {
		r_out = p_value;
		return true;
	}
	if (p_value.get_type() == Variant::STRING) {
		String s = String(p_value).strip_edges();
		if (Color::html_is_valid(s)) {
			r_out = Color::html(s);
			return true;
		}
		int idx = Color::find_named_color(s);
		if (idx >= 0) {
			r_out = Color::get_named_color(idx);
			return true;
		}
		return false;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array arr = p_value;
		if (arr.size() >= 3) {
			r_out = Color(float(arr[0]), float(arr[1]), float(arr[2]), arr.size() >= 4 ? float(arr[3]) : 1.0f);
			return true;
		}
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_value;
		if (d.has("r") && d.has("g") && d.has("b")) {
			r_out = Color(float(d["r"]), float(d["g"]), float(d["b"]), d.has("a") ? float(d["a"]) : 1.0f);
			return true;
		}
	}
	return false;
}

Variant variant_to_json(const Variant &p_value, int p_depth) {
	if (p_depth > 8) {
		return p_value.stringify();
	}

	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
			return p_value;

		case Variant::STRING_NAME:
			return String(p_value);

		case Variant::NODE_PATH:
			return String(NodePath(p_value));

		case Variant::VECTOR2: {
			Vector2 v = p_value;
			Dictionary d;
			d["x"] = v.x;
			d["y"] = v.y;
			return d;
		}
		case Variant::VECTOR2I: {
			Vector2i v = p_value;
			Dictionary d;
			d["x"] = v.x;
			d["y"] = v.y;
			return d;
		}
		case Variant::VECTOR3: {
			Vector3 v = p_value;
			Dictionary d;
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			return d;
		}
		case Variant::VECTOR3I: {
			Vector3i v = p_value;
			Dictionary d;
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			return d;
		}
		case Variant::VECTOR4: {
			Vector4 v = p_value;
			Dictionary d;
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			d["w"] = v.w;
			return d;
		}
		case Variant::VECTOR4I: {
			Vector4i v = p_value;
			Dictionary d;
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			d["w"] = v.w;
			return d;
		}
		case Variant::RECT2: {
			Rect2 r = p_value;
			Dictionary d;
			d["position"] = variant_to_json(r.position, p_depth + 1);
			d["size"] = variant_to_json(r.size, p_depth + 1);
			return d;
		}
		case Variant::RECT2I: {
			Rect2i r = p_value;
			Dictionary d;
			d["position"] = variant_to_json(r.position, p_depth + 1);
			d["size"] = variant_to_json(r.size, p_depth + 1);
			return d;
		}
		case Variant::COLOR: {
			Color c = p_value;
			Dictionary d;
			d["r"] = c.r;
			d["g"] = c.g;
			d["b"] = c.b;
			d["a"] = c.a;
			d["html"] = "#" + c.to_html(true);
			return d;
		}
		case Variant::QUATERNION: {
			Quaternion q = p_value;
			Dictionary d;
			d["x"] = q.x;
			d["y"] = q.y;
			d["z"] = q.z;
			d["w"] = q.w;
			return d;
		}
		case Variant::AABB: {
			AABB aabb = p_value;
			Dictionary d;
			d["position"] = variant_to_json(aabb.position, p_depth + 1);
			d["size"] = variant_to_json(aabb.size, p_depth + 1);
			return d;
		}
		case Variant::TRANSFORM2D: {
			Transform2D t = p_value;
			Dictionary d;
			d["origin"] = variant_to_json(t.get_origin(), p_depth + 1);
			d["rotation_degrees"] = Math::rad_to_deg(t.get_rotation());
			d["scale"] = variant_to_json(t.get_scale(), p_depth + 1);
			return d;
		}
		case Variant::TRANSFORM3D: {
			Transform3D t = p_value;
			Dictionary d;
			d["origin"] = variant_to_json(t.origin, p_depth + 1);
			Vector3 euler = t.basis.get_euler();
			Dictionary rot;
			rot["x"] = Math::rad_to_deg(euler.x);
			rot["y"] = Math::rad_to_deg(euler.y);
			rot["z"] = Math::rad_to_deg(euler.z);
			d["rotation_degrees"] = rot;
			d["scale"] = variant_to_json(t.basis.get_scale(), p_depth + 1);
			return d;
		}
		case Variant::BASIS: {
			Basis b = p_value;
			Dictionary d;
			Vector3 euler = b.get_euler();
			d["rotation_degrees"] = variant_to_json(Vector3(Math::rad_to_deg(euler.x), Math::rad_to_deg(euler.y), Math::rad_to_deg(euler.z)), p_depth + 1);
			d["scale"] = variant_to_json(b.get_scale(), p_depth + 1);
			return d;
		}
		case Variant::ARRAY: {
			Array in_arr = p_value;
			Array out_arr;
			int limit = MIN(in_arr.size(), 256);
			for (int i = 0; i < limit; i++) {
				out_arr.push_back(variant_to_json(in_arr[i], p_depth + 1));
			}
			return out_arr;
		}
		case Variant::DICTIONARY: {
			Dictionary in_dict = p_value;
			Dictionary out_dict;
			Array keys = in_dict.keys();
			int limit = MIN(keys.size(), 256);
			for (int i = 0; i < limit; i++) {
				Variant k = keys[i];
				out_dict[String(k)] = variant_to_json(in_dict[k], p_depth + 1);
			}
			return out_dict;
		}
		case Variant::PACKED_STRING_ARRAY: {
			PackedStringArray psa = p_value;
			Array out_arr;
			for (int i = 0; i < psa.size(); i++) {
				out_arr.push_back(psa[i]);
			}
			return out_arr;
		}
		case Variant::PACKED_INT32_ARRAY: {
			PackedInt32Array pia = p_value;
			Array out_arr;
			int limit = MIN(pia.size(), 256);
			for (int i = 0; i < limit; i++) {
				out_arr.push_back(pia[i]);
			}
			return out_arr;
		}
		case Variant::PACKED_FLOAT32_ARRAY: {
			PackedFloat32Array pfa = p_value;
			Array out_arr;
			int limit = MIN(pfa.size(), 256);
			for (int i = 0; i < limit; i++) {
				out_arr.push_back(pfa[i]);
			}
			return out_arr;
		}
		case Variant::OBJECT: {
			Object *obj = p_value.get_validated_object();
			if (!obj) {
				return Variant();
			}
			Dictionary d;
			d["class"] = obj->get_class();
			Resource *res = Object::cast_to<Resource>(obj);
			if (res) {
				if (!res->get_path().is_empty()) {
					d["path"] = res->get_path();
				}
				if (!res->get_name().is_empty()) {
					d["name"] = res->get_name();
				}
			}
			return d;
		}
		default:
			return p_value.stringify();
	}
}

static Ref<Resource> _instantiate_inline_resource(const Dictionary &p_dict, String *r_error) {
	if (p_dict.has("path")) {
		String path = p_dict["path"];
		Ref<Resource> res = ResourceLoader::load(path);
		if (res.is_null() && r_error) {
			*r_error = vformat("Could not load resource at '%s'.", path);
		}
		return res;
	}
	if (!p_dict.has("class") && !p_dict.has("_class")) {
		return Ref<Resource>();
	}
	String cls = p_dict.has("class") ? String(p_dict["class"]) : String(p_dict["_class"]);
	if (!ClassDB::class_exists(cls) || !ClassDB::is_parent_class(cls, "Resource") || !ClassDB::can_instantiate(cls)) {
		if (r_error) {
			*r_error = vformat("Cannot instantiate Resource class '%s'.", cls);
		}
		return Ref<Resource>();
	}
	Object *obj = ClassDB::instantiate(cls);
	Ref<Resource> res = Object::cast_to<Resource>(obj);
	if (res.is_null()) {
		if (obj) {
			memdelete(obj);
		}
		if (r_error) {
			*r_error = vformat("Failed to create Resource '%s'.", cls);
		}
		return Ref<Resource>();
	}
	Dictionary props = p_dict.has("properties") ? Dictionary(p_dict["properties"]) : p_dict;
	Array keys = props.keys();
	for (int i = 0; i < keys.size(); i++) {
		String key = keys[i];
		if (key == "class" || key == "_class" || key == "properties" || key == "path") {
			continue;
		}
		PropertyInfo pinfo;
		Variant::Type hint_type = Variant::NIL;
		if (ClassDB::get_property_info(cls, key, &pinfo)) {
			hint_type = pinfo.type;
		} else {
			Variant cur = res->get(key);
			hint_type = cur.get_type();
		}
		String prop_err;
		Variant converted = json_to_variant(props[key], hint_type, &prop_err);
		if (!prop_err.is_empty() && r_error) {
			*r_error = prop_err;
		}
		res->set(key, converted);
	}
	return res;
}

Variant json_to_variant(const Variant &p_json, Variant::Type p_hint_type, String *r_error) {
	if (p_json.get_type() == Variant::NIL) {
		return Variant();
	}

	// Explicit typed wrapper object: {"_type": "Vector3", ...} or {"_class": "BoxMesh", ...}
	if (p_json.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_json;
		if (d.has("_type")) {
			String t = String(d["_type"]);
			if (t == "Vector2") {
				p_hint_type = Variant::VECTOR2;
			} else if (t == "Vector2i") {
				p_hint_type = Variant::VECTOR2I;
			} else if (t == "Vector3") {
				p_hint_type = Variant::VECTOR3;
			} else if (t == "Vector3i") {
				p_hint_type = Variant::VECTOR3I;
			} else if (t == "Vector4") {
				p_hint_type = Variant::VECTOR4;
			} else if (t == "Color") {
				p_hint_type = Variant::COLOR;
			} else if (t == "Quaternion") {
				p_hint_type = Variant::QUATERNION;
			} else if (t == "NodePath") {
				p_hint_type = Variant::NODE_PATH;
			} else if (t == "StringName") {
				p_hint_type = Variant::STRING_NAME;
			} else if (t == "Resource") {
				p_hint_type = Variant::OBJECT;
			}
		} else if ((d.has("_class") || d.has("class")) && (p_hint_type == Variant::OBJECT || p_hint_type == Variant::NIL)) {
			Ref<Resource> res = _instantiate_inline_resource(d, r_error);
			if (res.is_valid()) {
				return res;
			}
		}
	}

	// Resource shorthand string: "res://..." when target is OBJECT.
	if (p_hint_type == Variant::OBJECT && p_json.get_type() == Variant::STRING) {
		String s = String(p_json).strip_edges();
		if (s.is_empty()) {
			return Variant();
		}
		if (s.begins_with("res://") || s.begins_with("uid://")) {
			Ref<Resource> res = ResourceLoader::load(s);
			if (res.is_null() && r_error) {
				*r_error = vformat("Could not load resource '%s'.", s);
			}
			return res;
		}
	}

	if (p_hint_type == Variant::OBJECT && p_json.get_type() == Variant::DICTIONARY) {
		return _instantiate_inline_resource(p_json, r_error);
	}

	switch (p_hint_type) {
		case Variant::BOOL:
			return bool(p_json);
		case Variant::INT:
			return int64_t(p_json);
		case Variant::FLOAT:
			return double(p_json);
		case Variant::STRING:
			return String(p_json);
		case Variant::STRING_NAME:
			return StringName(String(p_json));
		case Variant::NODE_PATH: {
			if (p_json.get_type() == Variant::DICTIONARY && Dictionary(p_json).has("value")) {
				return NodePath(String(Dictionary(p_json)["value"]));
			}
			return NodePath(String(p_json));
		}
		case Variant::VECTOR2: {
			Vector2 v;
			if (parse_vector2(p_json, v)) {
				return v;
			}
			break;
		}
		case Variant::VECTOR2I: {
			Vector2 v;
			if (parse_vector2(p_json, v)) {
				return Vector2i(int(v.x), int(v.y));
			}
			break;
		}
		case Variant::VECTOR3: {
			Vector3 v;
			if (parse_vector3(p_json, v)) {
				return v;
			}
			break;
		}
		case Variant::VECTOR3I: {
			Vector3 v;
			if (parse_vector3(p_json, v)) {
				return Vector3i(int(v.x), int(v.y), int(v.z));
			}
			break;
		}
		case Variant::VECTOR4: {
			if (p_json.get_type() == Variant::ARRAY) {
				Array a = p_json;
				if (a.size() >= 4) {
					return Vector4(float(a[0]), float(a[1]), float(a[2]), float(a[3]));
				}
			} else if (p_json.get_type() == Variant::DICTIONARY) {
				Dictionary d = p_json;
				if (d.has("x") && d.has("y") && d.has("z") && d.has("w")) {
					return Vector4(float(d["x"]), float(d["y"]), float(d["z"]), float(d["w"]));
				}
			}
			break;
		}
		case Variant::COLOR: {
			Color c;
			if (parse_color(p_json, c)) {
				return c;
			}
			break;
		}
		case Variant::QUATERNION: {
			if (p_json.get_type() == Variant::ARRAY) {
				Array a = p_json;
				if (a.size() >= 4) {
					return Quaternion(float(a[0]), float(a[1]), float(a[2]), float(a[3]));
				}
			} else if (p_json.get_type() == Variant::DICTIONARY) {
				Dictionary d = p_json;
				if (d.has("x") && d.has("y") && d.has("z") && d.has("w")) {
					return Quaternion(float(d["x"]), float(d["y"]), float(d["z"]), float(d["w"]));
				}
			}
			break;
		}
		case Variant::RECT2: {
			if (p_json.get_type() == Variant::DICTIONARY) {
				Dictionary d = p_json;
				Vector2 pos, sz;
				if (parse_vector2(d.get("position", Vector2()), pos) && parse_vector2(d.get("size", Vector2()), sz)) {
					return Rect2(pos, sz);
				}
			}
			break;
		}
		case Variant::AABB: {
			if (p_json.get_type() == Variant::DICTIONARY) {
				Dictionary d = p_json;
				Vector3 pos, sz;
				if (parse_vector3(d.get("position", Vector3()), pos) && parse_vector3(d.get("size", Vector3()), sz)) {
					return AABB(pos, sz);
				}
			}
			break;
		}
		case Variant::TRANSFORM3D: {
			if (p_json.get_type() == Variant::DICTIONARY) {
				Dictionary d = p_json;
				Transform3D t;
				Vector3 origin;
				if (d.has("origin") && parse_vector3(d["origin"], origin)) {
					t.origin = origin;
				}
				Vector3 rot_deg;
				if (d.has("rotation_degrees") && parse_vector3(d["rotation_degrees"], rot_deg)) {
					t.basis = Basis::from_euler(Vector3(Math::deg_to_rad(rot_deg.x), Math::deg_to_rad(rot_deg.y), Math::deg_to_rad(rot_deg.z)));
				}
				Vector3 scl;
				if (d.has("scale") && parse_vector3(d["scale"], scl)) {
					t.basis.scale(scl);
				}
				return t;
			}
			break;
		}
		case Variant::PACKED_STRING_ARRAY: {
			if (p_json.get_type() == Variant::ARRAY) {
				Array a = p_json;
				PackedStringArray psa;
				for (int i = 0; i < a.size(); i++) {
					psa.push_back(String(a[i]));
				}
				return psa;
			}
			break;
		}
		case Variant::PACKED_INT32_ARRAY: {
			if (p_json.get_type() == Variant::ARRAY) {
				Array a = p_json;
				PackedInt32Array pia;
				for (int i = 0; i < a.size(); i++) {
					pia.push_back(int32_t(a[i]));
				}
				return pia;
			}
			break;
		}
		case Variant::PACKED_FLOAT32_ARRAY: {
			if (p_json.get_type() == Variant::ARRAY) {
				Array a = p_json;
				PackedFloat32Array pfa;
				for (int i = 0; i < a.size(); i++) {
					pfa.push_back(float(a[i]));
				}
				return pfa;
			}
			break;
		}
		default:
			break;
	}

	// Fallback: Godot Variant expression syntax if string looks like Vector3(...), Color(...), etc.
	if (p_json.get_type() == Variant::STRING && p_hint_type != Variant::STRING && p_hint_type != Variant::NIL) {
		Variant parsed;
		String err_str;
		int err_line = 0;
		VariantParser::StreamString ss;
		ss.s = String(p_json);
		if (VariantParser::parse(&ss, parsed, err_str, err_line) == OK) {
			return parsed;
		}
	}

	return p_json;
}

bool is_safe_res_path(const String &p_path, bool p_allow_root) {
	String trimmed = p_path.strip_edges().replace("\\", "/");
	if (trimmed.is_empty() || !trimmed.begins_with("res://")) {
		return false;
	}
	if (trimmed == "res://" || trimmed == "res://.") {
		return p_allow_root;
	}
	String relative = trimmed.substr(6);
	PackedStringArray parts = relative.split("/", false);
	if (parts.is_empty()) {
		return p_allow_root;
	}
	for (int i = 0; i < parts.size(); i++) {
		if (parts[i] == ".." || parts[i] == ".") {
			return false;
		}
	}
	return true;
}

String normalize_res_path(const String &p_path) {
	String trimmed = p_path.strip_edges().replace("\\", "/");
	if (!trimmed.begins_with("res://")) {
		if (trimmed.begins_with("/")) {
			trimmed = "res://" + trimmed.substr(1);
		} else {
			trimmed = "res://" + trimmed;
		}
	}
	while (trimmed.length() > 6 && trimmed.ends_with("/")) {
		trimmed = trimmed.substr(0, trimmed.length() - 1);
	}
	return trimmed;
}

} // namespace mcp
