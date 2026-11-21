/**************************************************************************/
/*  mcp_runtime_bridge.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                            MINA ENGINE                                 */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Mina Engine contributors.                   */
/**************************************************************************/

#include "mcp_runtime_bridge.h"

#include "core/config/engine.h"
#include "core/debugger/engine_debugger.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "main/performance.h"
#include "scene/3d/node_3d.h"
#include "scene/gui/control.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "servers/navigation_3d/navigation_server_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "mcp_variant_codec.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server.h"

#ifdef RD_ENABLED
#include "servers/rendering/rendering_device.h"
#endif

MCPRuntimeInputDriver *MCPRuntimeInputDriver::singleton = nullptr;
bool MCPRuntimeBridge::initialized = false;

MCPRuntimeInputDriver::MCPRuntimeInputDriver() {
	singleton = this;
	set_name("MCPRuntimeInputDriver");
	set_process_mode(PROCESS_MODE_ALWAYS);
	set_process_internal(true);
}

MCPRuntimeInputDriver::~MCPRuntimeInputDriver() {
	release_all_inputs(true);
	if (singleton == this) {
		singleton = nullptr;
	}
}

MCPRuntimeInputDriver *MCPRuntimeInputDriver::get_or_create() {
	if (singleton) {
		return singleton;
	}
	SceneTree *st = SceneTree::get_singleton();
	if (!st || !st->get_root()) {
		return nullptr;
	}
	MCPRuntimeInputDriver *driver = memnew(MCPRuntimeInputDriver);
	st->get_root()->add_child(driver);
	return driver;
}

void MCPRuntimeInputDriver::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			_tick_queue();
			_sample_runtime_events();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			release_all_inputs(true);
		} break;
	}
}

void MCPRuntimeInputDriver::_ensure_virtual_mouse_pos() {
	if (virtual_mouse_pos.x >= 0 && virtual_mouse_pos.y >= 0) {
		return;
	}
	Input *inp = Input::get_singleton();
	if (inp) {
		Vector2 cur = inp->get_mouse_position();
		if (cur.x >= 0 && cur.y >= 0) {
			virtual_mouse_pos = cur;
			return;
		}
	}
	SceneTree *st = SceneTree::get_singleton();
	if (st && st->get_root()) {
		Size2i sz = st->get_root()->get_size();
		virtual_mouse_pos = Vector2(sz.x * 0.5f, sz.y * 0.5f);
	} else {
		virtual_mouse_pos = Vector2(640, 360);
	}
}

void MCPRuntimeInputDriver::_ensure_virtual_joypad(int p_device) {
	if (connected_virtual_joypads.has(p_device)) {
		return;
	}
	Input *inp = Input::get_singleton();
	if (!inp) {
		return;
	}
	TypedArray<int> existing = inp->get_connected_joypads();
	bool already_present = false;
	for (int i = 0; i < existing.size(); i++) {
		if (int(existing[i]) == p_device) {
			already_present = true;
			break;
		}
	}
	if (!already_present) {
		inp->joy_connection_changed(p_device, true, "Mina MCP Virtual Gamepad", "030000004d696e614d43500000000000");
	}
	connected_virtual_joypads.insert(p_device);
}

BitField<MouseButtonMask> MCPRuntimeInputDriver::_current_mouse_mask() const {
	BitField<MouseButtonMask> mask = MouseButtonMask::NONE;
	for (const MouseButton &mb : held_mouse_buttons) {
		mask.set_flag(mouse_button_to_mask(mb));
	}
	return mask;
}

void MCPRuntimeInputDriver::_apply_modifiers(InputEventWithModifiers *p_ev) const {
	if (!p_ev) {
		return;
	}
	p_ev->set_shift_pressed(held_keys.has(Key::SHIFT));
	p_ev->set_ctrl_pressed(held_keys.has(Key::CTRL));
	p_ev->set_alt_pressed(held_keys.has(Key::ALT));
	p_ev->set_meta_pressed(held_keys.has(Key::META));
}

void MCPRuntimeInputDriver::_dispatch_key(Key p_key, bool p_pressed) {
	Input *inp = Input::get_singleton();
	if (!inp || p_key == Key::NONE) {
		return;
	}
	if (p_pressed) {
		held_keys.insert(p_key);
	} else {
		held_keys.erase(p_key);
	}

	Ref<InputEventKey> ev;
	ev.instantiate();
	ev->set_device(0);
	ev->set_keycode(p_key);
	ev->set_physical_keycode(p_key);
	ev->set_key_label(p_key);
	uint32_t code = uint32_t(p_key);
	if (p_pressed && code >= 32 && code <= 126) {
		if (code >= 'A' && code <= 'Z' && !held_keys.has(Key::SHIFT)) {
			ev->set_unicode(char32_t(code + ('a' - 'A')));
		} else {
			ev->set_unicode(char32_t(code));
		}
	}
	ev->set_pressed(p_pressed);
	ev->set_echo(false);
	_apply_modifiers(ev.ptr());
	inp->parse_input_event(ev);
}

void MCPRuntimeInputDriver::_dispatch_action(const StringName &p_action, bool p_pressed, float p_strength) {
	Input *inp = Input::get_singleton();
	if (!inp || p_action == StringName()) {
		return;
	}
	if (p_pressed) {
		held_actions.insert(p_action);
	} else {
		held_actions.erase(p_action);
	}
	Ref<InputEventAction> ev;
	ev.instantiate();
	ev->set_action(p_action);
	ev->set_pressed(p_pressed);
	ev->set_strength(p_pressed ? p_strength : 0.0f);
	inp->parse_input_event(ev);
}

void MCPRuntimeInputDriver::_dispatch_mouse_move_relative(const Vector2 &p_delta) {
	Input *inp = Input::get_singleton();
	if (!inp) {
		return;
	}
	_ensure_virtual_mouse_pos();
	virtual_mouse_pos += p_delta;

	Ref<InputEventMouseMotion> ev;
	ev.instantiate();
	ev->set_device(0);
	ev->set_position(virtual_mouse_pos);
	ev->set_global_position(virtual_mouse_pos);
	ev->set_relative(p_delta);
	ev->set_relative_screen_position(p_delta);
	ev->set_velocity(p_delta * 60.0f);
	ev->set_screen_velocity(p_delta * 60.0f);
	ev->set_button_mask(_current_mouse_mask());
	_apply_modifiers(ev.ptr());
	inp->parse_input_event(ev);
}

void MCPRuntimeInputDriver::_dispatch_mouse_move_absolute(const Vector2 &p_pos) {
	Input *inp = Input::get_singleton();
	if (!inp) {
		return;
	}
	_ensure_virtual_mouse_pos();
	Vector2 delta = p_pos - virtual_mouse_pos;
	virtual_mouse_pos = p_pos;

	Ref<InputEventMouseMotion> ev;
	ev.instantiate();
	ev->set_device(0);
	ev->set_position(virtual_mouse_pos);
	ev->set_global_position(virtual_mouse_pos);
	ev->set_relative(delta);
	ev->set_relative_screen_position(delta);
	ev->set_velocity(delta * 60.0f);
	ev->set_screen_velocity(delta * 60.0f);
	ev->set_button_mask(_current_mouse_mask());
	_apply_modifiers(ev.ptr());
	inp->parse_input_event(ev);
}

void MCPRuntimeInputDriver::_dispatch_mouse_button(MouseButton p_btn, bool p_pressed, bool p_double_click, float p_factor) {
	Input *inp = Input::get_singleton();
	if (!inp || p_btn == MouseButton::NONE) {
		return;
	}
	_ensure_virtual_mouse_pos();
	bool is_wheel = (p_btn == MouseButton::WHEEL_UP || p_btn == MouseButton::WHEEL_DOWN ||
			p_btn == MouseButton::WHEEL_LEFT || p_btn == MouseButton::WHEEL_RIGHT);
	if (!is_wheel) {
		if (p_pressed) {
			held_mouse_buttons.insert(p_btn);
		} else {
			held_mouse_buttons.erase(p_btn);
		}
	}

	Ref<InputEventMouseButton> ev;
	ev.instantiate();
	ev->set_device(0);
	ev->set_button_index(p_btn);
	ev->set_pressed(p_pressed);
	ev->set_double_click(p_double_click);
	ev->set_factor(p_factor);
	ev->set_position(virtual_mouse_pos);
	ev->set_global_position(virtual_mouse_pos);
	ev->set_button_mask(_current_mouse_mask());
	_apply_modifiers(ev.ptr());
	inp->parse_input_event(ev);
}

void MCPRuntimeInputDriver::_dispatch_joy_button(int p_device, JoyButton p_btn, bool p_pressed) {
	Input *inp = Input::get_singleton();
	if (!inp || p_btn == JoyButton::INVALID) {
		return;
	}
	_ensure_virtual_joypad(p_device);
	uint64_t k = _pack_joy_key(p_device, (int)p_btn);
	if (p_pressed) {
		held_joy_buttons.insert(k);
	} else {
		held_joy_buttons.erase(k);
	}

	Ref<InputEventJoypadButton> ev;
	ev.instantiate();
	ev->set_device(p_device);
	ev->set_button_index(p_btn);
	ev->set_pressed(p_pressed);
	ev->set_pressure(p_pressed ? 1.0f : 0.0f);
	inp->parse_input_event(ev);
}

void MCPRuntimeInputDriver::_dispatch_joy_axis(int p_device, JoyAxis p_axis, float p_value) {
	Input *inp = Input::get_singleton();
	if (!inp || p_axis == JoyAxis::INVALID) {
		return;
	}
	_ensure_virtual_joypad(p_device);
	float clamped = CLAMP(p_value, -1.0f, 1.0f);
	uint64_t k = _pack_joy_key(p_device, (int)p_axis);
	if (Math::is_zero_approx(clamped)) {
		held_joy_axes.erase(k);
	} else {
		held_joy_axes[k] = clamped;
	}

	Ref<InputEventJoypadMotion> ev;
	ev.instantiate();
	ev->set_device(p_device);
	ev->set_axis(p_axis);
	ev->set_axis_value(clamped);
	inp->parse_input_event(ev);
}

Key MCPRuntimeInputDriver::_parse_key_name(const String &p_name) {
	String s = p_name.strip_edges();
	if (s.is_empty()) {
		return Key::NONE;
	}
	String lower = s.to_lower();
	if (lower == "esc" || lower == "escape") {
		return Key::ESCAPE;
	}
	if (lower == "space" || lower == "spacebar" || lower == " ") {
		return Key::SPACE;
	}
	if (lower == "enter" || lower == "return") {
		return Key::ENTER;
	}
	if (lower == "tab") {
		return Key::TAB;
	}
	if (lower == "backspace" || lower == "bs") {
		return Key::BACKSPACE;
	}
	if (lower == "delete" || lower == "del") {
		return Key::KEY_DELETE;
	}
	if (lower == "shift" || lower == "lshift" || lower == "rshift") {
		return Key::SHIFT;
	}
	if (lower == "ctrl" || lower == "control" || lower == "lctrl" || lower == "rctrl") {
		return Key::CTRL;
	}
	if (lower == "alt" || lower == "lalt" || lower == "ralt") {
		return Key::ALT;
	}
	if (lower == "meta" || lower == "super" || lower == "win" || lower == "cmd") {
		return Key::META;
	}
	if (lower == "up" || lower == "arrow_up" || lower == "up_arrow") {
		return Key::UP;
	}
	if (lower == "down" || lower == "arrow_down" || lower == "down_arrow") {
		return Key::DOWN;
	}
	if (lower == "left" || lower == "arrow_left" || lower == "left_arrow") {
		return Key::LEFT;
	}
	if (lower == "right" || lower == "arrow_right" || lower == "right_arrow") {
		return Key::RIGHT;
	}
	if (s.length() == 1) {
		char32_t c = s[0];
		if (c >= 'a' && c <= 'z') {
			c = c - 'a' + 'A';
		}
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			return (Key)c;
		}
	}
	return find_keycode(s);
}

MouseButton MCPRuntimeInputDriver::_parse_mouse_button(const String &p_name) {
	String lower = p_name.strip_edges().to_lower();
	if (lower == "left" || lower == "lmb" || lower == "1") {
		return MouseButton::LEFT;
	}
	if (lower == "right" || lower == "rmb" || lower == "2") {
		return MouseButton::RIGHT;
	}
	if (lower == "middle" || lower == "mmb" || lower == "3") {
		return MouseButton::MIDDLE;
	}
	if (lower == "wheel_up" || lower == "up") {
		return MouseButton::WHEEL_UP;
	}
	if (lower == "wheel_down" || lower == "down") {
		return MouseButton::WHEEL_DOWN;
	}
	if (lower == "wheel_left") {
		return MouseButton::WHEEL_LEFT;
	}
	if (lower == "wheel_right") {
		return MouseButton::WHEEL_RIGHT;
	}
	if (lower == "xbutton1" || lower == "mb4" || lower == "back") {
		return MouseButton::MB_XBUTTON1;
	}
	if (lower == "xbutton2" || lower == "mb5" || lower == "forward") {
		return MouseButton::MB_XBUTTON2;
	}
	return MouseButton::NONE;
}

JoyButton MCPRuntimeInputDriver::_parse_joy_button(const Variant &p_val) {
	if (p_val.get_type() == Variant::INT || p_val.get_type() == Variant::FLOAT) {
		int idx = int(p_val);
		if (idx >= 0 && idx < (int)JoyButton::MAX) {
			return (JoyButton)idx;
		}
		return JoyButton::INVALID;
	}
	String lower = String(p_val).strip_edges().to_lower();
	if (lower == "a" || lower == "cross" || lower == "south") {
		return JoyButton::A;
	}
	if (lower == "b" || lower == "circle" || lower == "east") {
		return JoyButton::B;
	}
	if (lower == "x" || lower == "square" || lower == "west") {
		return JoyButton::X;
	}
	if (lower == "y" || lower == "triangle" || lower == "north") {
		return JoyButton::Y;
	}
	if (lower == "back" || lower == "select" || lower == "share" || lower == "view") {
		return JoyButton::BACK;
	}
	if (lower == "guide" || lower == "home" || lower == "ps" || lower == "xbox") {
		return JoyButton::GUIDE;
	}
	if (lower == "start" || lower == "options" || lower == "menu") {
		return JoyButton::START;
	}
	if (lower == "left_stick" || lower == "ls" || lower == "l3") {
		return JoyButton::LEFT_STICK;
	}
	if (lower == "right_stick" || lower == "rs" || lower == "r3") {
		return JoyButton::RIGHT_STICK;
	}
	if (lower == "left_shoulder" || lower == "lb" || lower == "l1") {
		return JoyButton::LEFT_SHOULDER;
	}
	if (lower == "right_shoulder" || lower == "rb" || lower == "r1") {
		return JoyButton::RIGHT_SHOULDER;
	}
	if (lower == "dpad_up" || lower == "up") {
		return JoyButton::DPAD_UP;
	}
	if (lower == "dpad_down" || lower == "down") {
		return JoyButton::DPAD_DOWN;
	}
	if (lower == "dpad_left" || lower == "left") {
		return JoyButton::DPAD_LEFT;
	}
	if (lower == "dpad_right" || lower == "right") {
		return JoyButton::DPAD_RIGHT;
	}
	if (lower == "touchpad") {
		return JoyButton::TOUCHPAD;
	}
	return JoyButton::INVALID;
}

JoyAxis MCPRuntimeInputDriver::_parse_joy_axis(const String &p_name) {
	String lower = p_name.strip_edges().to_lower();
	if (lower == "left_x" || lower == "lx") {
		return JoyAxis::LEFT_X;
	}
	if (lower == "left_y" || lower == "ly") {
		return JoyAxis::LEFT_Y;
	}
	if (lower == "right_x" || lower == "rx") {
		return JoyAxis::RIGHT_X;
	}
	if (lower == "right_y" || lower == "ry") {
		return JoyAxis::RIGHT_Y;
	}
	if (lower == "trigger_left" || lower == "lt" || lower == "l2") {
		return JoyAxis::TRIGGER_LEFT;
	}
	if (lower == "trigger_right" || lower == "rt" || lower == "r2") {
		return JoyAxis::TRIGGER_RIGHT;
	}
	return JoyAxis::INVALID;
}

bool MCPRuntimeInputDriver::_parse_step(const Dictionary &p_dict, InputStep &r_step, String &r_error) const {
	r_step.type = String(p_dict.get("type", "")).strip_edges().to_lower();
	r_step.mode = String(p_dict.get("mode", "")).strip_edges().to_lower();
	r_step.duration_sec = MAX(0.0, double(p_dict.get("duration", 0.0)));
	r_step.device = MAX(0, int(p_dict.get("device", 0)));

	if (r_step.type == "key" || r_step.type == "keys" || r_step.type == "keyboard") {
		r_step.type = "key";
		if (p_dict.has("keys") && p_dict["keys"].get_type() == Variant::ARRAY) {
			Array arr = p_dict["keys"];
			for (int i = 0; i < arr.size(); i++) {
				Key k = _parse_key_name(String(arr[i]));
				if (k == Key::NONE) {
					r_error = vformat("Unknown key name '%s'.", String(arr[i]));
					return false;
				}
				r_step.keys.push_back(k);
			}
		}
		if (p_dict.has("key")) {
			Key k = _parse_key_name(String(p_dict["key"]));
			if (k == Key::NONE) {
				r_error = vformat("Unknown key name '%s'.", String(p_dict["key"]));
				return false;
			}
			r_step.keys.push_back(k);
		}
		if (r_step.keys.is_empty()) {
			r_error = "Key step requires 'key' or 'keys'.";
			return false;
		}
		if (r_step.mode.is_empty()) {
			r_step.mode = (r_step.duration_sec > 0.0) ? "hold" : "tap";
		}
		if (r_step.mode == "tap" && r_step.duration_sec <= 0.0) {
			r_step.duration_sec = 0.06;
		}
		return true;
	}

	if (r_step.type == "action") {
		String act = String(p_dict.get("action", p_dict.get("name", ""))).strip_edges();
		if (act.is_empty()) {
			r_error = "Action step requires 'action' (InputMap action name).";
			return false;
		}
		r_step.action_name = StringName(act);
		r_step.strength = float(p_dict.get("strength", 1.0));
		if (r_step.mode.is_empty()) {
			r_step.mode = (r_step.duration_sec > 0.0) ? "hold" : "tap";
		}
		if (r_step.mode == "tap" && r_step.duration_sec <= 0.0) {
			r_step.duration_sec = 0.06;
		}
		return true;
	}

	if (r_step.type == "mouse_move") {
		if (p_dict.has("relative") && mcp::parse_vector2(p_dict["relative"], r_step.relative)) {
			r_step.has_relative = true;
		}
		if (p_dict.has("position") && mcp::parse_vector2(p_dict["position"], r_step.position)) {
			r_step.has_position = true;
		}
		if (!r_step.has_relative && !r_step.has_position) {
			r_error = "mouse_move requires 'relative' [dx, dy] or 'position' [x, y].";
			return false;
		}
		r_step.move_steps = CLAMP(int(p_dict.get("move_steps", p_dict.get("steps_count", 1))), 1, 240);
		if (r_step.duration_sec > 0.0 && r_step.move_steps == 1) {
			r_step.move_steps = CLAMP(int(Math::ceil(r_step.duration_sec * 60.0)), 1, 240);
		}
		return true;
	}

	if (r_step.type == "mouse_button" || r_step.type == "mouse_click") {
		r_step.type = "mouse_button";
		r_step.mouse_button = _parse_mouse_button(String(p_dict.get("button", "left")));
		if (r_step.mouse_button == MouseButton::NONE) {
			r_error = vformat("Unknown mouse button '%s'.", String(p_dict.get("button", "")));
			return false;
		}
		if (p_dict.has("position") && mcp::parse_vector2(p_dict["position"], r_step.position)) {
			r_step.has_position = true;
		}
		if (r_step.mode.is_empty()) {
			r_step.mode = (r_step.duration_sec > 0.0) ? "hold" : "click";
		}
		if ((r_step.mode == "click" || r_step.mode == "tap" || r_step.mode == "double_click") && r_step.duration_sec <= 0.0) {
			r_step.duration_sec = 0.05;
		}
		return true;
	}

	if (r_step.type == "mouse_scroll" || r_step.type == "scroll") {
		r_step.type = "mouse_scroll";
		String dir = String(p_dict.get("direction", "down")).strip_edges().to_lower();
		if (dir == "up") {
			r_step.mouse_button = MouseButton::WHEEL_UP;
		} else if (dir == "down") {
			r_step.mouse_button = MouseButton::WHEEL_DOWN;
		} else if (dir == "left") {
			r_step.mouse_button = MouseButton::WHEEL_LEFT;
		} else if (dir == "right") {
			r_step.mouse_button = MouseButton::WHEEL_RIGHT;
		} else {
			r_error = vformat("Unknown scroll direction '%s' (use up, down, left, right).", dir);
			return false;
		}
		r_step.scroll_steps = CLAMP(int(p_dict.get("scroll_steps", p_dict.get("count", 1))), 1, 50);
		r_step.scroll_factor = float(p_dict.get("factor", 1.0));
		if (p_dict.has("position") && mcp::parse_vector2(p_dict["position"], r_step.position)) {
			r_step.has_position = true;
		}
		return true;
	}

	if (r_step.type == "gamepad_button") {
		r_step.joy_button = _parse_joy_button(p_dict.get("button", ""));
		if (r_step.joy_button == JoyButton::INVALID) {
			r_error = vformat("Unknown gamepad button '%s'.", String(p_dict.get("button", "")));
			return false;
		}
		if (r_step.mode.is_empty()) {
			r_step.mode = (r_step.duration_sec > 0.0) ? "hold" : "tap";
		}
		if (r_step.mode == "tap" && r_step.duration_sec <= 0.0) {
			r_step.duration_sec = 0.06;
		}
		return true;
	}

	if (r_step.type == "gamepad_axis" || r_step.type == "gamepad_stick") {
		r_step.type = "gamepad_axis";
		if (p_dict.has("stick")) {
			String st = String(p_dict["stick"]).strip_edges().to_lower();
			r_step.has_stick = true;
			if (st == "right") {
				r_step.stick_x_axis = JoyAxis::RIGHT_X;
				r_step.stick_y_axis = JoyAxis::RIGHT_Y;
			} else {
				r_step.stick_x_axis = JoyAxis::LEFT_X;
				r_step.stick_y_axis = JoyAxis::LEFT_Y;
			}
			r_step.stick_value = Vector2(float(p_dict.get("x", 0.0)), float(p_dict.get("y", 0.0)));
			return true;
		}
		r_step.joy_axis = _parse_joy_axis(String(p_dict.get("axis", "")));
		if (r_step.joy_axis == JoyAxis::INVALID) {
			r_error = vformat("Unknown gamepad axis '%s' (use left_x, left_y, right_x, right_y, trigger_left, trigger_right, or stick='left'/'right').", String(p_dict.get("axis", "")));
			return false;
		}
		r_step.axis_value = float(p_dict.get("value", 0.0));
		return true;
	}

	if (r_step.type == "wait") {
		return true;
	}

	if (r_step.type == "release_all") {
		return true;
	}

	r_error = vformat("Unsupported input type '%s'.", r_step.type);
	return false;
}

void MCPRuntimeInputDriver::_sample_runtime_events() {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) return;
	ObjectID scene_id = tree->get_current_scene() ? tree->get_current_scene()->get_instance_id() : ObjectID();
	int node_count = tree->get_node_count();
	auto add_event = [&](const String &p_type, const String &p_detail) {
		Dictionary event;
		event["id"] = (int64_t)next_runtime_event_id++;
		event["timestamp_msec"] = (int64_t)(OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0);
		event["type"] = p_type;
		event["detail"] = p_detail;
		runtime_events.push_back(event);
		if (runtime_events.size() > 500) runtime_events.remove_at(0);
	};
	if (!event_baseline_initialized) {
		last_scene_id = scene_id;
		last_runtime_node_count = node_count;
		event_baseline_initialized = true;
		return;
	}
	if (scene_id != last_scene_id) {
		Node *scene = tree->get_current_scene();
		add_event("scene_changed", scene ? String(scene->get_name()) : String());
		last_scene_id = scene_id;
	}
	if (node_count != last_runtime_node_count) {
		add_event(node_count > last_runtime_node_count ? "nodes_added" : "nodes_removed", itos(node_count - last_runtime_node_count));
		last_runtime_node_count = node_count;
	}
}

void MCPRuntimeInputDriver::enqueue_command(int64_t p_request_id, const Dictionary &p_payload, bool p_ack_on_complete) {
	// Immediate cancellation shortcut when top-level type == "release_all".
	String top_type = String(p_payload.get("type", "")).strip_edges().to_lower();
	if (top_type == "release_all" && !p_payload.has("steps")) {
		release_all_inputs(true);
		_send_ack(p_request_id, true, 1);
		return;
	}

	QueuedRequest req;
	req.request_id = p_request_id;
	req.ack_on_complete = p_ack_on_complete;

	if (p_payload.has("steps") && p_payload["steps"].get_type() == Variant::ARRAY) {
		Array raw_steps = p_payload["steps"];
		if (raw_steps.is_empty()) {
			_send_ack(p_request_id, false, 0, "Parameter 'steps' is empty.");
			return;
		}
		for (int i = 0; i < raw_steps.size(); i++) {
			if (raw_steps[i].get_type() != Variant::DICTIONARY) {
				_send_ack(p_request_id, false, 0, vformat("Step %d must be an object.", i));
				return;
			}
			InputStep step;
			String err;
			if (!_parse_step(raw_steps[i], step, err)) {
				_send_ack(p_request_id, false, 0, vformat("Step %d: %s", i, err));
				return;
			}
			req.steps.push_back(step);
		}
	} else {
		InputStep step;
		String err;
		if (!_parse_step(p_payload, step, err)) {
			_send_ack(p_request_id, false, 0, err);
			return;
		}
		req.steps.push_back(step);
	}

	queue.push_back(req);
	if (!p_ack_on_complete) {
		_send_ack(p_request_id, true, 0);
	}
	// Run immediate tick so 0-delay single-shot actions take effect right away.
	_tick_queue();
}

void MCPRuntimeInputDriver::_begin_step(QueuedRequest &p_req, InputStep &p_step, uint64_t p_now) {
	p_req.step_started = true;
	p_req.step_start_msec = p_now;
	p_req.step_end_msec = p_now + uint64_t(Math::round(p_step.duration_sec * 1000.0));
	p_req.move_steps_done = 0;
	p_req.move_accum = Vector2();

	if (p_step.type == "release_all") {
		release_all_inputs(false);
		return;
	}

	if (p_step.type == "key") {
		bool press = (p_step.mode != "release");
		for (int i = 0; i < p_step.keys.size(); i++) {
			_dispatch_key(p_step.keys[i], press);
		}
		return;
	}

	if (p_step.type == "action") {
		bool press = (p_step.mode != "release");
		_dispatch_action(p_step.action_name, press, p_step.strength);
		return;
	}

	if (p_step.type == "mouse_move") {
		if (p_step.move_steps <= 1) {
			if (p_step.has_position) {
				_dispatch_mouse_move_absolute(p_step.position);
			} else if (p_step.has_relative) {
				_dispatch_mouse_move_relative(p_step.relative);
			}
			p_req.move_steps_done = 1;
		} else {
			// Spread smooth movement over frames; first increment runs now.
			if (p_step.has_position) {
				_ensure_virtual_mouse_pos();
				p_step.relative = p_step.position - virtual_mouse_pos;
			}
			Vector2 step_delta = p_step.relative / float(p_step.move_steps);
			p_req.move_accum += step_delta;
			p_req.move_steps_done = 1;
			_dispatch_mouse_move_relative(step_delta);
		}
		return;
	}

	if (p_step.type == "mouse_button") {
		if (p_step.has_position) {
			_dispatch_mouse_move_absolute(p_step.position);
		}
		bool press = (p_step.mode != "release");
		bool dbl = (p_step.mode == "double_click");
		_dispatch_mouse_button(p_step.mouse_button, press, dbl);
		return;
	}

	if (p_step.type == "mouse_scroll") {
		if (p_step.has_position) {
			_dispatch_mouse_move_absolute(p_step.position);
		}
		for (int i = 0; i < p_step.scroll_steps; i++) {
			_dispatch_mouse_button(p_step.mouse_button, true, false, p_step.scroll_factor);
			_dispatch_mouse_button(p_step.mouse_button, false, false, p_step.scroll_factor);
		}
		return;
	}

	if (p_step.type == "gamepad_button") {
		bool press = (p_step.mode != "release");
		_dispatch_joy_button(p_step.device, p_step.joy_button, press);
		return;
	}

	if (p_step.type == "gamepad_axis") {
		if (p_step.has_stick) {
			_dispatch_joy_axis(p_step.device, p_step.stick_x_axis, p_step.stick_value.x);
			_dispatch_joy_axis(p_step.device, p_step.stick_y_axis, p_step.stick_value.y);
		} else {
			_dispatch_joy_axis(p_step.device, p_step.joy_axis, p_step.axis_value);
		}
		return;
	}
}

void MCPRuntimeInputDriver::_finish_step(InputStep &p_step) {
	// Reliably release keys/buttons/actions/axes when hold/tap ends!
	if (p_step.type == "key" && (p_step.mode == "tap" || p_step.mode == "hold")) {
		for (int i = p_step.keys.size() - 1; i >= 0; i--) {
			_dispatch_key(p_step.keys[i], false);
		}
	} else if (p_step.type == "action" && (p_step.mode == "tap" || p_step.mode == "hold")) {
		_dispatch_action(p_step.action_name, false, 0.0f);
	} else if (p_step.type == "mouse_button" && (p_step.mode == "click" || p_step.mode == "tap" || p_step.mode == "double_click" || p_step.mode == "hold")) {
		_dispatch_mouse_button(p_step.mouse_button, false);
	} else if (p_step.type == "gamepad_button" && (p_step.mode == "tap" || p_step.mode == "hold")) {
		_dispatch_joy_button(p_step.device, p_step.joy_button, false);
	} else if (p_step.type == "gamepad_axis" && p_step.duration_sec > 0.0) {
		if (p_step.has_stick) {
			_dispatch_joy_axis(p_step.device, p_step.stick_x_axis, 0.0f);
			_dispatch_joy_axis(p_step.device, p_step.stick_y_axis, 0.0f);
		} else {
			_dispatch_joy_axis(p_step.device, p_step.joy_axis, 0.0f);
		}
	}
}

void MCPRuntimeInputDriver::_tick_queue() {
	const uint64_t now = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;

	while (!queue.is_empty()) {
		QueuedRequest &req = queue.write[0];
		if (req.current_index >= req.steps.size()) {
			int64_t rid = req.request_id;
			int exec_count = req.executed_steps;
			bool should_ack = req.ack_on_complete;
			queue.remove_at(0);
			if (should_ack) {
				_send_ack(rid, true, exec_count);
			}
			continue;
		}

		InputStep &step = req.steps.write[req.current_index];
		if (!req.step_started) {
			_begin_step(req, step, now);
		}

		// Multi-frame smooth mouse movement.
		if (step.type == "mouse_move" && step.move_steps > 1) {
			if (req.move_steps_done < step.move_steps) {
				req.move_steps_done++;
				Vector2 delta = (req.move_steps_done == step.move_steps)
						? (step.relative - req.move_accum)
						: (step.relative / float(step.move_steps));
				req.move_accum += delta;
				_dispatch_mouse_move_relative(delta);
				if (req.move_steps_done < step.move_steps) {
					break; // Continue on next frame.
				}
			}
		}

		// Check if step needs to wait until step_end_msec.
		bool needs_wait = false;
		if (step.type == "wait" && step.duration_sec > 0.0) {
			needs_wait = true;
		} else if (step.type == "key" && (step.mode == "tap" || step.mode == "hold")) {
			needs_wait = true;
		} else if (step.type == "action" && (step.mode == "tap" || step.mode == "hold")) {
			needs_wait = true;
		} else if (step.type == "mouse_button" && (step.mode == "click" || step.mode == "tap" || step.mode == "double_click" || step.mode == "hold")) {
			needs_wait = true;
		} else if (step.type == "gamepad_button" && (step.mode == "tap" || step.mode == "hold")) {
			needs_wait = true;
		} else if (step.type == "gamepad_axis" && step.duration_sec > 0.0) {
			needs_wait = true;
		}

		if (needs_wait && now < req.step_end_msec) {
			break; // Still holding/waiting; preserve strict FIFO order.
		}

		_finish_step(step);
		req.executed_steps++;
		req.current_index++;
		req.step_started = false;
	}
}

void MCPRuntimeInputDriver::release_all_inputs(bool p_cancel_queue) {
	if (p_cancel_queue) {
		for (int i = 0; i < queue.size(); i++) {
			if (queue[i].ack_on_complete) {
				_send_ack(queue[i].request_id, false, queue[i].executed_steps, "Cancelled by release_all or game stop.");
			}
		}
		queue.clear();
	}

	Vector<Key> keys_to_release;
	for (const Key &k : held_keys) {
		keys_to_release.push_back(k);
	}
	for (int i = 0; i < keys_to_release.size(); i++) {
		_dispatch_key(keys_to_release[i], false);
	}
	held_keys.clear();

	Vector<MouseButton> mbs_to_release;
	for (const MouseButton &mb : held_mouse_buttons) {
		mbs_to_release.push_back(mb);
	}
	for (int i = 0; i < mbs_to_release.size(); i++) {
		_dispatch_mouse_button(mbs_to_release[i], false);
	}
	held_mouse_buttons.clear();

	Vector<StringName> actions_to_release;
	for (const StringName &a : held_actions) {
		actions_to_release.push_back(a);
	}
	for (int i = 0; i < actions_to_release.size(); i++) {
		_dispatch_action(actions_to_release[i], false, 0.0f);
	}
	held_actions.clear();

	Vector<uint64_t> joy_btns;
	for (const uint64_t &k : held_joy_buttons) {
		joy_btns.push_back(k);
	}
	for (int i = 0; i < joy_btns.size(); i++) {
		int dev = int(joy_btns[i] >> 32);
		JoyButton btn = (JoyButton)(uint32_t)(joy_btns[i] & 0xFFFFFFFFULL);
		_dispatch_joy_button(dev, btn, false);
	}
	held_joy_buttons.clear();

	Vector<uint64_t> joy_axes;
	for (const KeyValue<uint64_t, float> &E : held_joy_axes) {
		joy_axes.push_back(E.key);
	}
	for (int i = 0; i < joy_axes.size(); i++) {
		int dev = int(joy_axes[i] >> 32);
		JoyAxis axis = (JoyAxis)(uint32_t)(joy_axes[i] & 0xFFFFFFFFULL);
		_dispatch_joy_axis(dev, axis, 0.0f);
	}
	held_joy_axes.clear();

	if (Input::get_singleton()) {
		Input::get_singleton()->release_pressed_events();
	}
}

void MCPRuntimeInputDriver::_send_ack(int64_t p_request_id, bool p_ok, int p_executed_steps, const String &p_error) {
	if (!EngineDebugger::is_active() || !EngineDebugger::get_singleton()) {
		return;
	}
	Array arr;
	arr.push_back(p_request_id);
	arr.push_back(p_ok);
	arr.push_back(p_executed_steps);
	arr.push_back(p_error);
	EngineDebugger::get_singleton()->send_message("mina_mcp:input_ack", arr);
}

// ---------------- MCPRuntimeBridge ----------------

void MCPRuntimeBridge::initialize() {
	if (initialized || !EngineDebugger::is_active()) {
		return;
	}
	// Never attach game-side runtime capture inside the Editor itself.
	if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	EngineDebugger::register_message_capture("mina_mcp", EngineDebugger::Capture(nullptr, MCPRuntimeBridge::parse_message));
	initialized = true;
}

void MCPRuntimeBridge::deinitialize() {
	if (!initialized) {
		return;
	}
	if (MCPRuntimeInputDriver::get_singleton()) {
		MCPRuntimeInputDriver::get_singleton()->release_all_inputs(true);
	}
	EngineDebugger::unregister_message_capture("mina_mcp");
	initialized = false;
}

Error MCPRuntimeBridge::_handle_capture_runtime(const Array &p_args) {
	int64_t req_id = p_args.size() > 0 ? int64_t(p_args[0]) : 0;
	int max_width = p_args.size() > 1 ? CLAMP(int(p_args[1]), 128, 2560) : 1024;
	String out_path = p_args.size() > 2 ? String(p_args[2]) : String();

	auto send_result = [&](bool p_ok, int p_w, int p_h, const String &p_file, const String &p_err) {
		Array res;
		res.push_back(req_id);
		res.push_back(p_ok);
		res.push_back(p_w);
		res.push_back(p_h);
		res.push_back(p_file);
		res.push_back(p_err);
		EngineDebugger::get_singleton()->send_message("mina_mcp:capture_result", res);
	};

	SceneTree *st = SceneTree::get_singleton();
	Viewport *vp = st ? st->get_root() : nullptr;
	if (!vp) {
		send_result(false, 0, 0, String(), "Game root Viewport is not available.");
		return OK;
	}
	Ref<ViewportTexture> tex = vp->get_texture();
	if (tex.is_null()) {
		send_result(false, 0, 0, String(), "Game Viewport texture is not available.");
		return OK;
	}
	Ref<Image> img = tex->get_image();
	if (img.is_null() || img->is_empty()) {
		send_result(false, 0, 0, String(), "Game Viewport image is empty (headless dummy renderer or frame not yet rendered).");
		return OK;
	}

	img->clear_mipmaps();
	img->convert(Image::FORMAT_RGBA8);
#ifdef RD_ENABLED
	if (RD::get_singleton() && RenderingServer::get_singleton()->viewport_is_using_hdr_2d(vp->get_viewport_rid())) {
		img->linear_to_srgb();
	}
#endif
	if (img->get_width() > max_width) {
		int new_h = MAX(1, int(double(img->get_height()) * double(max_width) / double(img->get_width())));
		img->resize(max_width, new_h, Image::INTERPOLATE_BILINEAR);
	}

	String target_path = out_path;
	if (target_path.is_empty()) {
		String temp_dir = OS::get_singleton()->get_temp_path();
		target_path = temp_dir.path_join(vformat("mina-mcp-rt-%d-%d.png", (int64_t)OS::get_singleton()->get_process_id(), req_id));
	}
	DirAccess::make_dir_recursive_absolute(target_path.get_base_dir());
	Error serr = img->save_png(target_path);
	if (serr != OK) {
		send_result(false, 0, 0, String(), vformat("Failed to save runtime PNG screenshot (error %d).", (int)serr));
		return OK;
	}

	send_result(true, img->get_width(), img->get_height(), target_path, String());
	return OK;
}

Error MCPRuntimeBridge::_handle_send_input(const Array &p_args) {
	int64_t req_id = p_args.size() > 0 ? int64_t(p_args[0]) : 0;
	String json_payload = p_args.size() > 1 ? String(p_args[1]) : String();
	bool wait_complete = p_args.size() > 2 ? bool(p_args[2]) : true;

	MCPRuntimeInputDriver *driver = MCPRuntimeInputDriver::get_or_create();
	if (!driver) {
		Array arr;
		arr.push_back(req_id);
		arr.push_back(false);
		arr.push_back(0);
		arr.push_back(String("SceneTree is not ready inside game process."));
		EngineDebugger::get_singleton()->send_message("mina_mcp:input_ack", arr);
		return OK;
	}

	Ref<JSON> parser;
	parser.instantiate();
	if (parser->parse(json_payload) != OK || parser->get_data().get_type() != Variant::DICTIONARY) {
		Array arr;
		arr.push_back(req_id);
		arr.push_back(false);
		arr.push_back(0);
		arr.push_back(String("Malformed input payload JSON."));
		EngineDebugger::get_singleton()->send_message("mina_mcp:input_ack", arr);
		return OK;
	}

	driver->enqueue_command(req_id, parser->get_data(), wait_complete);
	return OK;
}

Error MCPRuntimeBridge::_handle_release_all(const Array &p_args) {
	if (MCPRuntimeInputDriver::get_singleton()) {
		MCPRuntimeInputDriver::get_singleton()->release_all_inputs(true);
	}
	return OK;
}

namespace {

Node *_resolve_runtime_node(SceneTree *p_tree, const String &p_path) {
	if (!p_tree || !p_tree->get_root()) {
		return nullptr;
	}
	String path = p_path.strip_edges();
	if (path.is_empty() || path == "." || path == "/") {
		return p_tree->get_root();
	}
	return p_tree->get_root()->get_node_or_null(NodePath(path));
}

void _serialize_runtime_tree(Node *p_node, const String &p_path, int p_depth, int p_max_depth, int &r_count, int p_max_nodes, Array &r_out) {
	if (!p_node || r_count >= p_max_nodes || p_depth > p_max_depth) {
		return;
	}
	Dictionary item;
	item["name"] = String(p_node->get_name());
	item["class"] = p_node->get_class();
	item["path"] = p_path;
	item["child_count"] = p_node->get_child_count();
	item["process_enabled"] = p_node->is_processing();
	item["physics_process_enabled"] = p_node->is_physics_processing();
	if (Node3D *node_3d = Object::cast_to<Node3D>(p_node)) {
		item["position"] = mcp::variant_to_json(node_3d->get_position());
		item["rotation_degrees"] = mcp::variant_to_json(node_3d->get_rotation_degrees());
	} else if (Control *control = Object::cast_to<Control>(p_node)) {
		item["position"] = mcp::variant_to_json(control->get_position());
		item["size"] = mcp::variant_to_json(control->get_size());
	}
	r_out.push_back(item);
	r_count++;
	if (p_depth >= p_max_depth) {
		return;
	}
	for (int i = 0; i < p_node->get_child_count() && r_count < p_max_nodes; i++) {
		Node *child = p_node->get_child(i);
		String child_path = p_path.is_empty() ? String(child->get_name()) : p_path.path_join(String(child->get_name()));
		_serialize_runtime_tree(child, child_path, p_depth + 1, p_max_depth, r_count, p_max_nodes, r_out);
	}
}

Dictionary _runtime_properties(Object *p_object, const String &p_filter) {
	Dictionary properties;
	if (!p_object) {
		return properties;
	}
	List<PropertyInfo> list;
	p_object->get_property_list(&list);
	String filter = p_filter.to_lower();
	for (const PropertyInfo &info : list) {
		if (!(info.usage & (PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE)) || (info.usage & PROPERTY_USAGE_GROUP)) {
			continue;
		}
		String name = info.name;
		if (!filter.is_empty() && !name.to_lower().contains(filter)) {
			continue;
		}
		Dictionary entry;
		entry["type"] = Variant::get_type_name(info.type);
		entry["value"] = mcp::variant_to_json(p_object->get(info.name));
		properties[name] = entry;
	}
	return properties;
}

bool _runtime_method_allowed(const String &p_method) {
	static const char *allowed[] = { "get_name", "get_class", "is_inside_tree", "is_visible_in_tree", "is_processing", "is_physics_processing", "get_child_count", "get_position", "get_global_position", "get_rotation", "get_rotation_degrees", "get_scale", "get_global_transform", "get_text", "get_value", "get_item_count", "get_current_scene", "get_frame", "get_physics_frames", "get_process_frames" };
	for (const char *method : allowed) {
		if (p_method == method) {
			return true;
		}
	}
	return false;
}

Dictionary _handle_runtime_operation(const String &p_operation, const Dictionary &p_payload) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return mcp::make_error("Running game SceneTree is not available.", "runtime_unavailable");
	}
	if (p_operation == "runtime_get_tree") {
		Node *root = tree->get_root();
		String start_path = String(p_payload.get("node_path", "."));
		Node *start = _resolve_runtime_node(tree, start_path);
		if (!start) {
			return mcp::make_error(vformat("Runtime node '%s' was not found.", start_path), "node_not_found");
		}
		Array nodes;
		int count = 0;
		String prefix = start == root ? String() : String(start->get_name());
		_serialize_runtime_tree(start, prefix, 0, CLAMP(int(p_payload.get("max_depth", 8)), 0, 16), count, CLAMP(int(p_payload.get("max_nodes", 300)), 1, 1000), nodes);
		Dictionary out;
		out["scene_root"] = tree->get_current_scene() ? String(tree->get_current_scene()->get_name()) : String();
		out["node_count"] = count;
		out["nodes"] = nodes;
		return mcp::make_ok(out);
	}
	if (p_operation == "runtime_get_properties") {
		String path = String(p_payload.get("node_path", "."));
		Node *node = _resolve_runtime_node(tree, path);
		if (!node) {
			return mcp::make_error(vformat("Runtime node '%s' was not found.", path), "node_not_found");
		}
		Dictionary out;
		out["node_path"] = path;
		out["class"] = node->get_class();
		out["properties"] = _runtime_properties(node, String(p_payload.get("filter", "")));
		return mcp::make_ok(out);
	}
	if (p_operation == "runtime_set_properties") {
		String path = String(p_payload.get("node_path", "."));
		Node *node = _resolve_runtime_node(tree, path);
		if (!node) {
			return mcp::make_error(vformat("Runtime node '%s' was not found.", path), "node_not_found");
		}
		Dictionary values = p_payload.get("properties", Dictionary());
		Array applied;
		Array rejected;
		Array keys = values.keys();
		for (int key_index = 0; key_index < keys.size(); key_index++) {
			StringName property = keys[key_index];
			PropertyInfo info;
			bool found = false;
			List<PropertyInfo> list;
			node->get_property_list(&list);
			for (const PropertyInfo &candidate : list) {
				if (candidate.name == property && (candidate.usage & PROPERTY_USAGE_EDITOR)) {
					info = candidate;
					found = true;
					break;
				}
			}
			if (!found) {
				rejected.push_back(String(property));
				continue;
			}
			String conversion_error;
			Variant value = mcp::json_to_variant(values[property], info.type, &conversion_error);
			if (!conversion_error.is_empty()) {
				rejected.push_back(String(property) + ": " + conversion_error);
				continue;
			}
			node->set(property, value);
			applied.push_back(String(property));
		}
		Dictionary out;
		out["node_path"] = path;
		out["applied"] = applied;
		out["rejected"] = rejected;
		return mcp::make_ok(out);
	}
	if (p_operation == "runtime_call_method") {
		String path = String(p_payload.get("node_path", "."));
		Node *node = _resolve_runtime_node(tree, path);
		String method = String(p_payload.get("method", ""));
		if (!node) {
			return mcp::make_error(vformat("Runtime node '%s' was not found.", path), "node_not_found");
		}
		if (!_runtime_method_allowed(method)) {
			return mcp::make_error(vformat("Runtime method '%s' is not in the read-only allowlist.", method), "method_not_allowed");
		}
		Array args = p_payload.get("args", Array());
		if (!args.is_empty() || !node->has_method(method)) {
			return mcp::make_error("Only available read-only allowlisted methods with no arguments may be called.", "invalid_method");
		}
		Variant result = node->callv(method, args);
		Dictionary out;
		out["node_path"] = path;
		out["method"] = method;
		out["result"] = mcp::variant_to_json(result);
		return mcp::make_ok(out);
	}
	if (p_operation == "game_set_time_scale") {
		double scale = CLAMP(double(p_payload.get("scale", 1.0)), 0.05, 4.0);
		Engine::get_singleton()->set_time_scale(scale);
		Dictionary out;
		out["time_scale"] = Engine::get_singleton()->get_time_scale();
		return mcp::make_ok(out);
	}
	if (p_operation == "debug_pause_game") {
		bool paused = bool(p_payload.get("paused", true));
		tree->set_pause(paused);
		Dictionary out;
		out["paused"] = tree->is_paused();
		return mcp::make_ok(out);
	}
	if (p_operation == "debug_get_performance") {
		Performance *performance = Performance::get_singleton();
		if (!performance) {
			return mcp::make_error("Performance monitor is unavailable.", "unavailable");
		}
		Dictionary metrics;
		for (int monitor = 0; monitor < Performance::MONITOR_MAX; monitor++) {
			Performance::Monitor type = (Performance::Monitor)monitor;
			String name = performance->get_monitor_name(type);
			if (!name.is_empty()) {
				metrics[name] = performance->get_monitor(type);
			}
		}
		Dictionary out;
		out["timestamp_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
		out["metrics"] = metrics;
		return mcp::make_ok(out);
	}
	if (p_operation == "physics_raycast") {
		return mcp::make_error("This engine branch does not provide a safe deferred physics-query path through the debugger bridge.", "physics_query_unavailable");
	}
	if (p_operation == "navigation_get_path") {
		Vector3 from;
		Vector3 to;
		if (!mcp::parse_vector3(p_payload.get("from", Variant()), from) || !mcp::parse_vector3(p_payload.get("to", Variant()), to)) {
			return mcp::make_error("Parameters 'from' and 'to' must be Vector3 values.", "invalid_argument");
		}
		TypedArray<RID> maps = NavigationServer3D::get_singleton()->get_maps();
		if (maps.is_empty()) {
			return mcp::make_error("No runtime navigation maps are registered.", "no_navigation_map");
		}
		NavigationServer3D *navigation = NavigationServer3D::get_singleton();
		RID map;
		for (int i = 0; i < maps.size(); i++) {
			RID candidate = maps[i];
			if (navigation->map_is_active(candidate)) {
				map = candidate;
				break;
			}
		}
		if (!map.is_valid()) return mcp::make_error("No active runtime navigation map is available.", "no_navigation_map");
		Vector<Vector3> path = navigation->map_get_path(map, from, to, true, uint32_t(p_payload.get("navigation_layers", 1)));
		Array points;
		for (const Vector3 &point : path) {
			points.push_back(mcp::variant_to_json(point));
		}
		Dictionary out;
		out["point_count"] = points.size();
		out["points"] = points;
		return mcp::make_ok(out);
	}
	if (p_operation == "game_wait_for_condition") {
		String path = String(p_payload.get("node_path", "."));
		Node *node = _resolve_runtime_node(tree, path);
		String property = String(p_payload.get("property", ""));
		if (!node || property.is_empty() || !p_payload.has("equals")) {
			return mcp::make_error("Provide an existing runtime node_path, property, and equals value.", "invalid_argument");
		}
		Dictionary props = _runtime_properties(node, property);
		if (!props.has(property)) return mcp::make_error(vformat("Runtime property '%s' was not found or is not inspectable.", property), "property_not_found");
		Dictionary out;
		out["condition_met"] = node->get(property) == p_payload["equals"];
		out["value"] = mcp::variant_to_json(node->get(property));
		return mcp::make_ok(out);
	}
	if (p_operation == "game_get_events") {
		MCPRuntimeInputDriver *driver = MCPRuntimeInputDriver::get_or_create();
		if (!driver) return mcp::make_error("Runtime event collector could not initialize.", "unavailable");
		Array events;
		int64_t since_id = int64_t(p_payload.get("since_id", 0));
		int limit = CLAMP(int(p_payload.get("limit", 100)), 1, 500);
		Array all = driver->get_runtime_events();
		for (int i = 0; i < all.size(); i++) {
			Dictionary event = all[i];
			if (int64_t(event.get("id", 0)) > since_id) events.push_back(event);
			if (events.size() >= limit) break;
		}
		Dictionary out;
		out["count"] = events.size();
		out["events"] = events;
		Array kinds;
		kinds.push_back("scene_changed");
		kinds.push_back("nodes_added");
		kinds.push_back("nodes_removed");
		out["event_kinds"] = kinds;
		out["note"] = "Global scene-tree sampling reports scene switches and node-count deltas; it cannot identify specific door/death events unless the game exposes them as MCP events.";
		return mcp::make_ok(out);
	}
	if (p_operation == "debug_get_stacktrace") {
		Dictionary out;
		out["available"] = false;
		out["stack"] = Array();
		out["note"] = "Use debug_get_runtime_errors for captured error stack traces.";
		return mcp::make_ok(out);
	}
	if (p_operation == "debug_step_frame") {
		return mcp::make_error("Frame stepping is controlled by the editor debugger, not by the paused game runtime bridge.", "editor_debugger_required");
	}
	return mcp::make_error(vformat("Unsupported runtime operation '%s'.", p_operation), "unsupported_operation");
}

} // namespace

Error MCPRuntimeBridge::_handle_runtime_request(const Array &p_args) {
	int64_t request_id = p_args.size() > 0 ? int64_t(p_args[0]) : 0;
	String operation = p_args.size() > 1 ? String(p_args[1]) : String();
	String payload_text = p_args.size() > 2 ? String(p_args[2]) : String("{}");
	Dictionary result;
	Ref<JSON> parser;
	parser.instantiate();
	if (parser->parse(payload_text) != OK || parser->get_data().get_type() != Variant::DICTIONARY) {
		result = mcp::make_error("Malformed runtime request payload.", "invalid_payload");
	} else {
		result = _handle_runtime_operation(operation, parser->get_data());
	}
	Array response;
	response.push_back(request_id);
	response.push_back(bool(result.get("ok", false)));
	response.push_back(result);
	response.push_back(String(result.get("error", "")));
	EngineDebugger::get_singleton()->send_message("mina_mcp:runtime_result", response);
	return OK;
}

Error MCPRuntimeBridge::parse_message(void *p_user, const String &p_msg, const Array &p_args, bool &r_captured) {
	r_captured = true;
	if (p_msg == "capture_runtime") {
		return _handle_capture_runtime(p_args);
	}
	if (p_msg == "send_input") {
		return _handle_send_input(p_args);
	}
	if (p_msg == "runtime_request") {
		return _handle_runtime_request(p_args);
	}
	if (p_msg == "release_all") {
		return _handle_release_all(p_args);
	}
	r_captured = false;
	return OK;
}
