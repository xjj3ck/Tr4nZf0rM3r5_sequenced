/*
 * Tr4nZf0rM3r5_sequenced - a timed transformation sequencer video
 * filter for OBS Studio (rotate / zoom / scale / flip step sequences,
 * presets, auto-hide, image library).
 *
 * Copyright (C) 2026 xjj3ck
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <obs-module.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#else
#include <dirent.h>
#include <strings.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_STEPS 8
#define MAX_INSTANCES 16

enum step_type {
	STEP_ROTATE,
	STEP_ZOOM,
	STEP_SCALE,
	STEP_FLIP,
};

enum loop_mode {
	LOOP_NONE,
	LOOP_LAST,
	LOOP_ALL,
};

enum flip_axis {
	FLIP_AXIS_VERTICAL,
	FLIP_AXIS_HORIZONTAL,
};

struct step {
	int type;
	int64_t duration_ms;
	int turns;
	bool clockwise;
	float zoom;
	float sx, sy;
	int flips;
	int flip_axis;
};

struct spin_filter {
	obs_source_t *source;
	uint64_t start_ns;
	bool start_on_show;
	bool auto_hide;
	bool finished;
	int loop_mode;
	size_t step_count;
	struct step steps[MAX_STEPS];
	long long snap_count;
	int snap_types[MAX_STEPS];
	char snap_choice[128];
	char last_import_path[512];
	bool lib_enabled;
	bool lib_random;
	char lib_path[512];
	size_t lib_index;
	obs_weak_source_t *lib_parent_ws;
	char lib_original_file[512];
	bool lib_have_original;
};

/* ---- live-instance registry + safe deferred UI refresh ---- */

static struct spin_filter *live_instances[MAX_INSTANCES];
static bool refresh_in_progress = false;
static volatile bool refresh_pending = false;

static void live_register(struct spin_filter *f)
{
	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		if (!live_instances[i]) {
			live_instances[i] = f;
			break;
		}
	}
}

static void live_unregister(struct spin_filter *f)
{
	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		if (live_instances[i] == f)
			live_instances[i] = NULL;
	}
}

static struct spin_filter *find_by_settings(obs_data_t *s)
{
	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		struct spin_filter *f = live_instances[i];
		if (!f)
			continue;
		obs_data_t *fs = obs_source_get_settings(f->source);
		bool match = (fs == s);
		obs_data_release(fs);
		if (match)
			return f;
	}
	return NULL;
}

static void refresh_task(void *param)
{
	UNUSED_PARAMETER(param);
	if (refresh_in_progress)
		return;
	refresh_in_progress = true;
	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		if (live_instances[i])
			obs_source_update_properties(
				live_instances[i]->source);
	}
	refresh_in_progress = false;
}

/* ---- auto-hide: collect items, then hide them after enumeration ---- */

struct hide_collect {
	obs_source_t *target;
	obs_sceneitem_t *items[32];
	size_t count;
	obs_source_t *canvas_src;
};

static bool collect_item_cb(obs_scene_t *scene, obs_sceneitem_t *item,
			    void *param)
{
	UNUSED_PARAMETER(scene);
	struct hide_collect *hc = param;
	if (obs_sceneitem_get_source(item) == hc->target &&
	    hc->count < 32)
		hc->items[hc->count++] = item;
	return true;
}

static bool hide_in_scene_cb(void *param, obs_source_t *scene)
{
	struct hide_collect *hc = param;
	if (!hc->canvas_src)
		hc->canvas_src = scene;
	obs_scene_t *sc = obs_scene_from_source(scene);
	if (sc)
		obs_scene_enum_items(sc, collect_item_cb, param);
	return true;
}

static void hide_task(void *param)
{
	obs_weak_source_t *ws = param;
	obs_source_t *src = obs_weak_source_get_source(ws);
	if (src) {
		struct hide_collect hc;
		hc.target = src;
		hc.count = 0;
		hc.canvas_src = NULL;
		obs_enum_scenes(hide_in_scene_cb, &hc);
		for (size_t i = 0; i < hc.count; i++)
			obs_sceneitem_set_visible(hc.items[i], false);
		obs_source_release(src);
	}
	obs_weak_source_release(ws);
}

/* ------------------------- preset storage ------------------------- */

static const char *PRESET_FILE_NAME = ".tr4nzf0rm3r5_presets.json";

static const char *factory_names[3] = {
	"Factory: Fast 4-step loop (250ms each)",
	"Factory: Slow mix loop (500/1000/100/500)",
	"Factory: Loop last step only",
};

static void preset_file_path(char *out, size_t size)
{
	const char *home = getenv("HOME");
	if (!home)
		home = getenv("USERPROFILE");
	if (!home) {
		snprintf(out, size, "%s", PRESET_FILE_NAME);
		return;
	}
	snprintf(out, size, "%s/%s", home, PRESET_FILE_NAME);
}

static obs_data_t *load_user_presets(void)
{
	char path[512];
	preset_file_path(path, sizeof(path));
	obs_data_t *d = obs_data_create_from_json_file(path);
	if (!d)
		d = obs_data_create();
	return d;
}

static void save_user_presets(obs_data_t *d)
{
	char path[512];
	preset_file_path(path, sizeof(path));
	obs_data_save_json(d, path);
}

static void copy_step_keys(obs_data_t *from, obs_data_t *to, size_t i)
{
	char key[64];
	snprintf(key, sizeof(key), "step%zu_type", i);
	obs_data_set_int(to, key, obs_data_get_int(from, key));
	snprintf(key, sizeof(key), "step%zu_duration_ms", i);
	obs_data_set_int(to, key, obs_data_get_int(from, key));
	snprintf(key, sizeof(key), "step%zu_turns", i);
	obs_data_set_int(to, key, obs_data_get_int(from, key));
	snprintf(key, sizeof(key), "step%zu_clockwise", i);
	obs_data_set_bool(to, key, obs_data_get_bool(from, key));
	snprintf(key, sizeof(key), "step%zu_zoom", i);
	obs_data_set_double(to, key, obs_data_get_double(from, key));
	snprintf(key, sizeof(key), "step%zu_sx", i);
	obs_data_set_double(to, key, obs_data_get_double(from, key));
	snprintf(key, sizeof(key), "step%zu_sy", i);
	obs_data_set_double(to, key, obs_data_get_double(from, key));
	snprintf(key, sizeof(key), "step%zu_flips", i);
	obs_data_set_int(to, key, obs_data_get_int(from, key));
	snprintf(key, sizeof(key), "step%zu_flip_axis", i);
	obs_data_set_int(to, key, obs_data_get_int(from, key));
}

static void unset_step_keys(obs_data_t *s, size_t i)
{
	static const char *fields[] = {"type",     "duration_ms", "turns",
				       "clockwise", "zoom",       "sx",
				       "sy",       "flips",      "flip_axis"};
	char key[64];
	for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) {
		snprintf(key, sizeof(key), "step%zu_%s", i, fields[k]);
		obs_data_unset_user_value(s, key);
	}
}

static void apply_preset(obs_data_t *s, obs_data_t *preset)
{
	long long n = obs_data_get_int(preset, "step_count");
	if (n < 1)
		n = 1;
	if (n > MAX_STEPS)
		n = MAX_STEPS;
	long long old = obs_data_get_int(s, "step_count");
	for (long long i = n; i < old; i++)
		unset_step_keys(s, (size_t)i);
	obs_data_set_int(s, "step_count", n);
	obs_data_set_int(s, "loop_mode",
			 obs_data_get_int(preset, "loop_mode"));
	for (long long i = 0; i < n; i++)
		copy_step_keys(preset, s, (size_t)i);
}

static obs_data_t *capture_preset(obs_data_t *s)
{
	obs_data_t *p = obs_data_create();
	long long n = obs_data_get_int(s, "step_count");
	if (n < 1)
		n = 1;
	if (n > MAX_STEPS)
		n = MAX_STEPS;
	obs_data_set_int(p, "step_count", n);
	obs_data_set_int(p, "loop_mode", obs_data_get_int(s, "loop_mode"));
	for (long long i = 0; i < n; i++)
		copy_step_keys(s, p, (size_t)i);
	return p;
}

static void preset_step(obs_data_t *d, size_t i, int type, long long dur,
			int turns, bool cw, double zoom, double sx,
			double sy, int flips, int flip_axis)
{
	char key[64];
	snprintf(key, sizeof(key), "step%zu_type", i);
	obs_data_set_int(d, key, type);
	snprintf(key, sizeof(key), "step%zu_duration_ms", i);
	obs_data_set_int(d, key, dur);
	snprintf(key, sizeof(key), "step%zu_turns", i);
	obs_data_set_int(d, key, turns);
	snprintf(key, sizeof(key), "step%zu_clockwise", i);
	obs_data_set_bool(d, key, cw);
	snprintf(key, sizeof(key), "step%zu_zoom", i);
	obs_data_set_double(d, key, zoom);
	snprintf(key, sizeof(key), "step%zu_sx", i);
	obs_data_set_double(d, key, sx);
	snprintf(key, sizeof(key), "step%zu_sy", i);
	obs_data_set_double(d, key, sy);
	snprintf(key, sizeof(key), "step%zu_flips", i);
	obs_data_set_int(d, key, flips);
	snprintf(key, sizeof(key), "step%zu_flip_axis", i);
	obs_data_set_int(d, key, flip_axis);
}

static obs_data_t *factory_preset(int idx)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "step_count", 4);
	switch (idx) {
	case 0:
		obs_data_set_int(d, "loop_mode", LOOP_ALL);
		preset_step(d, 0, STEP_ROTATE, 250, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 1, STEP_ZOOM, 250, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 2, STEP_SCALE, 250, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 3, STEP_FLIP, 250, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		break;
	case 1:
		obs_data_set_int(d, "loop_mode", LOOP_ALL);
		preset_step(d, 0, STEP_ROTATE, 500, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 1, STEP_ZOOM, 1000, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 2, STEP_SCALE, 100, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 3, STEP_FLIP, 500, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		break;
	default:
		obs_data_set_int(d, "loop_mode", LOOP_LAST);
		preset_step(d, 0, STEP_ROTATE, 2000, 2, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 1, STEP_ZOOM, 2000, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 2, STEP_SCALE, 2000, 1, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		preset_step(d, 3, STEP_FLIP, 2000, 4, false, 2.0, 1.5,
			    1.5, 1, FLIP_AXIS_VERTICAL);
		break;
	}
	return d;
}

/* ------------------------- preset callbacks ------------------------- */

static bool preset_choice_modified_cb(obs_properties_t *pp,
				      obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);
	struct spin_filter *f = find_by_settings(s);
	if (!f)
		return false;
	const char *choice = obs_data_get_string(s, "preset_choice");
	if (!choice || !*choice)
		return false;
	if (strcmp(choice, f->snap_choice) == 0)
		return false; /* init/rebuild echo, not a user pick */

	obs_data_t *preset = NULL;
	for (int i = 0; i < 3 && !preset; i++) {
		if (strcmp(choice, factory_names[i]) == 0)
			preset = factory_preset(i);
	}
	if (!preset) {
		obs_data_t *user = load_user_presets();
		preset = obs_data_get_obj(user, choice);
		obs_data_release(user);
	}
	if (preset) {
		apply_preset(s, preset);
		obs_data_release(preset);
		obs_source_update(f->source, s);
		snprintf(f->snap_choice, sizeof(f->snap_choice), "%s",
			 choice);
		refresh_pending = true;
	}
	return false;
}

static bool save_preset_cb(obs_properties_t *pp, obs_property_t *p,
			   void *data)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);
	struct spin_filter *f = data;
	if (!f)
		return false;
	obs_data_t *s = obs_source_get_settings(f->source);
	const char *name = obs_data_get_string(s, "preset_new_name");
	if (name && *name) {
		obs_data_t *user = load_user_presets();
		obs_data_t *cap = capture_preset(s);
		obs_data_set_obj(user, name, cap);
		obs_data_release(cap);

		obs_data_array_t *arr = obs_data_get_array(user,
							   "preset_order");
		if (!arr)
			arr = obs_data_array_create();
		bool found = false;
		size_t cnt = obs_data_array_count(arr);
		for (size_t i = 0; i < cnt && !found; i++) {
			obs_data_t *it = obs_data_array_item(arr, i);
			if (strcmp(obs_data_get_string(it, ""), name) == 0)
				found = true;
			obs_data_release(it);
		}
		if (!found) {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "", name);
			obs_data_array_push_back(arr, entry);
			obs_data_release(entry);
		}
		obs_data_set_array(user, "preset_order", arr);
		obs_data_array_release(arr);

		save_user_presets(user);
		obs_data_release(user);
		refresh_pending = true;
	}
	obs_data_release(s);
	return false;
}

static bool delete_preset_cb(obs_properties_t *pp, obs_property_t *p,
			     void *data)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);
	struct spin_filter *f = data;
	if (!f)
		return false;
	obs_data_t *s = obs_source_get_settings(f->source);
	const char *choice = obs_data_get_string(s, "preset_choice");
	if (choice && *choice) {
		bool is_factory = false;
		for (int i = 0; i < 3 && !is_factory; i++) {
			if (strcmp(choice, factory_names[i]) == 0)
				is_factory = true;
		}
		if (!is_factory) {
			obs_data_t *user = load_user_presets();
			obs_data_unset_user_value(user, choice);

			obs_data_array_t *arr = obs_data_get_array(
				user, "preset_order");
			if (arr) {
				obs_data_array_t *keep =
					obs_data_array_create();
				size_t cnt = obs_data_array_count(arr);
				for (size_t i = 0; i < cnt; i++) {
					obs_data_t *it =
						obs_data_array_item(arr, i);
					if (strcmp(obs_data_get_string(
							   it, ""),
						   choice) != 0)
						obs_data_array_push_back(
							keep, it);
					obs_data_release(it);
				}
				obs_data_set_array(user, "preset_order",
						   keep);
				obs_data_array_release(keep);
				obs_data_array_release(arr);
			}
			save_user_presets(user);
			obs_data_release(user);
			refresh_pending = true;
		}
	}
	obs_data_release(s);
	return false;
}

static bool ends_with_json(const char *p)
{
	size_t n = strlen(p);
	if (n < 5)
		return false;
	const char *e = p + n - 5;
	return e[0] == '.' && (e[1] == 'j' || e[1] == 'J') &&
	       (e[2] == 's' || e[2] == 'S') && (e[3] == 'o' || e[3] == 'O') &&
	       (e[4] == 'n' || e[4] == 'N');
}

static obs_data_t *build_export_data(void)
{
	obs_data_t *out = load_user_presets();
	obs_data_array_t *order = obs_data_get_array(out, "preset_order");
	if (!order)
		order = obs_data_array_create();
	for (int i = 0; i < 3; i++) {
		obs_data_t *existing = obs_data_get_obj(out,
							factory_names[i]);
		if (existing) {
			obs_data_release(existing);
			continue;
		}
		obs_data_t *fp = factory_preset(i);
		obs_data_set_obj(out, factory_names[i], fp);
		obs_data_release(fp);
		obs_data_t *e = obs_data_create();
		obs_data_set_string(e, "", factory_names[i]);
		obs_data_array_push_back(order, e);
		obs_data_release(e);
	}
	obs_data_set_array(out, "preset_order", order);
	obs_data_array_release(order);
	return out;
}

static int apply_import(obs_data_t *in)
{
	obs_data_t *user = obs_data_create();
	obs_data_array_t *order = obs_data_array_create();
	obs_data_array_t *in_order = obs_data_get_array(in, "preset_order");
	int n = 0;
	if (in_order) {
		size_t cnt = obs_data_array_count(in_order);
		for (size_t i = 0; i < cnt; i++) {
			obs_data_t *it = obs_data_array_item(in_order, i);
			const char *nm = obs_data_get_string(it, "");
			bool is_factory = false;
			for (int k = 0; k < 3; k++) {
				if (nm && strcmp(nm, factory_names[k]) == 0)
					is_factory = true;
			}
			obs_data_t *obj = obs_data_get_obj(in, nm);
			if (obj && nm && *nm && !is_factory) {
				obs_data_set_obj(user, nm, obj);
				obs_data_t *e = obs_data_create();
				obs_data_set_string(e, "", nm);
				obs_data_array_push_back(order, e);
				obs_data_release(e);
				n++;
			}
			if (obj)
				obs_data_release(obj);
			obs_data_release(it);
		}
		obs_data_array_release(in_order);
	}
	obs_data_set_array(user, "preset_order", order);
	obs_data_array_release(order);
	save_user_presets(user);
	obs_data_release(user);
	return n;
}

#ifdef _WIN32
static bool win_file_dialog(bool save, char *out, size_t out_size)
{
	wchar_t wfile[1024] = {0};
	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter =
		L"Tr4nZ preset files (*.json)\0*.json\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = wfile;
	ofn.nMaxFile = 1024;
	ofn.lpstrDefExt = L"json";
	ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
			 : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
	BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
	if (!ok)
		return false;
	return WideCharToMultiByte(CP_UTF8, 0, wfile, -1, out, (int)out_size,
				   NULL, NULL) > 0;
}

static bool export_button_cb(obs_properties_t *pp, obs_property_t *p,
			     void *data)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	char path[1024] = {0};
	if (win_file_dialog(true, path, sizeof(path))) {
		char final_path[1032];
		snprintf(final_path, sizeof(final_path), "%s%s", path,
			 ends_with_json(path) ? "" : ".json");
		obs_data_t *out = build_export_data();
		obs_data_array_t *arr = obs_data_get_array(out,
							   "preset_order");
		int n = arr ? (int)obs_data_array_count(arr) : 0;
		if (arr)
			obs_data_array_release(arr);
		obs_data_save_json(out, final_path);
		obs_data_release(out);
		char msg[64];
		snprintf(msg, sizeof(msg), "Exported %d preset(s).", n);
		MessageBoxA(NULL, msg, "Tr4nZf0rM3r5_sequenced",
			    MB_OK | MB_ICONINFORMATION);
	}
	return false;
}

static bool import_button_cb(obs_properties_t *pp, obs_property_t *p,
			     void *data)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	char path[1024] = {0};
	if (win_file_dialog(false, path, sizeof(path))) {
		obs_data_t *in = obs_data_create_from_json_file(path);
		if (in) {
			int n = apply_import(in);
			obs_data_release(in);
			refresh_pending = true;
			char msg[64];
			snprintf(msg, sizeof(msg), "Imported %d preset(s).",
				 n);
			MessageBoxA(NULL, msg, "Tr4nZf0rM3r5_sequenced",
				    MB_OK | MB_ICONINFORMATION);
		} else {
			MessageBoxA(NULL, "Could not read that file.",
				    "Tr4nZf0rM3r5_sequenced",
				    MB_OK | MB_ICONWARNING);
		}
	}
	return false;
}
#else
static bool export_path_modified_cb(obs_properties_t *pp, obs_property_t *p,
				    obs_data_t *s)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);
	const char *path = obs_data_get_string(s, "export_path");
	if (path && *path) {
		char final_path[616];
		snprintf(final_path, sizeof(final_path), "%s%s", path,
			 ends_with_json(path) ? "" : ".json");
		obs_data_t *out = build_export_data();
		obs_data_save_json(out, final_path);
		obs_data_release(out);
	}
	return false;
}

static bool import_path_modified_cb(obs_properties_t *pp, obs_property_t *p,
				    obs_data_t *s)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);
	struct spin_filter *f = find_by_settings(s);
	const char *path = obs_data_get_string(s, "import_path");
	if (!path || !*path)
		return false;
	if (f && strcmp(path, f->last_import_path) == 0)
		return false; /* rebuild echo, not a user pick */
	if (f)
		snprintf(f->last_import_path, sizeof(f->last_import_path),
			 "%s", path);
	obs_data_t *in = obs_data_create_from_json_file(path);
	if (in) {
		apply_import(in);
		obs_data_release(in);
		refresh_pending = true;
	}
	return false;
}
#endif

/* ------------------------- image library ------------------------- */

struct lib_file_list {
	char **paths;
	size_t count;
};

static bool lib_has_img_ext(const char *nm)
{
	static const char *exts[] = {".png", ".jpg", ".jpeg",
				     ".bmp", ".tga", ".gif"};
	size_t n = strlen(nm);
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		size_t el = strlen(exts[i]);
		if (n > el) {
			const char *a = nm + n - el;
			const char *b = exts[i];
			bool same = true;
			for (size_t k = 0; k < el && same; k++) {
				char ca = a[k];
				if (ca >= 'A' && ca <= 'Z')
					ca += 32;
				if (ca != b[k])
					same = false;
			}
			if (same)
				return true;
		}
	}
	return false;
}

static int lib_cmp(const void *a, const void *b)
{
	const char *pa = *(const char *const *)a;
	const char *pb = *(const char *const *)b;
#ifdef _WIN32
	return _stricmp(pa, pb);
#else
	return strcasecmp(pa, pb);
#endif
}

static void lib_scan(const char *dir, struct lib_file_list *fl)
{
	fl->paths = NULL;
	fl->count = 0;
#ifdef _WIN32
	char pattern[700];
	snprintf(pattern, sizeof(pattern), "%s\\*", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		if (!lib_has_img_ext(fd.cFileName))
			continue;
		fl->paths = brealloc(fl->paths,
				     sizeof(char *) * (fl->count + 1));
		char full[800];
		snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
		fl->paths[fl->count++] = bstrdup(full);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (!lib_has_img_ext(ent->d_name))
			continue;
		fl->paths = brealloc(fl->paths,
				     sizeof(char *) * (fl->count + 1));
		char full[800];
		snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
		fl->paths[fl->count++] = bstrdup(full);
	}
	closedir(d);
#endif
	if (fl->count > 1)
		qsort(fl->paths, fl->count, sizeof(char *), lib_cmp);
}

static void lib_free(struct lib_file_list *fl)
{
	for (size_t i = 0; i < fl->count; i++)
		bfree(fl->paths[i]);
	bfree(fl->paths);
	fl->paths = NULL;
	fl->count = 0;
}

struct lib_apply {
	obs_weak_source_t *parent_ws;
	char *path;
	bool center;
};

static void lib_apply_task(void *param)
{
	struct lib_apply *la = param;
	obs_source_t *parent = obs_weak_source_get_source(la->parent_ws);
	if (parent) {
		obs_data_t *ps = obs_source_get_settings(parent);
		obs_data_set_string(ps, "file", la->path);
		obs_source_update(parent, ps);
		obs_data_release(ps);

		if (la->center) {
			struct hide_collect hc;
			hc.target = parent;
			hc.count = 0;
			hc.canvas_src = NULL;
			obs_enum_scenes(hide_in_scene_cb, &hc);
			uint32_t cw = hc.canvas_src
				? obs_source_get_base_width(hc.canvas_src)
				: 0;
			uint32_t ch = hc.canvas_src
				? obs_source_get_base_height(hc.canvas_src)
				: 0;
			for (size_t i = 0; i < hc.count; i++) {
				struct vec2 pos;
				vec2_set(&pos, (float)cw / 2.0f,
					 (float)ch / 2.0f);
				obs_sceneitem_set_alignment(hc.items[i],
							    OBS_ALIGN_CENTER);
				obs_sceneitem_set_pos(hc.items[i], &pos);
			}
		}
		obs_source_release(parent);
	}
	bfree(la->path);
	obs_weak_source_release(la->parent_ws);
	bfree(la);
}

static void lib_queue_apply(struct spin_filter *f, const char *path,
			    bool center)
{
	obs_source_t *parent = obs_filter_get_parent(f->source);
	if (!parent)
		return;
	struct lib_apply *la = bzalloc(sizeof(*la));
	la->parent_ws = obs_source_get_weak_source(parent);
	la->path = bstrdup(path);
	la->center = center;
	obs_queue_task(OBS_TASK_UI, lib_apply_task, la, false);
}

static void lib_restore(struct spin_filter *f)
{
	if (!f->lib_have_original || !f->lib_parent_ws)
		return;
	struct lib_apply *la = bzalloc(sizeof(*la));
	la->parent_ws = f->lib_parent_ws;
	la->path = bstrdup(f->lib_original_file);
	la->center = false;
	f->lib_parent_ws = NULL;
	f->lib_have_original = false;
	obs_queue_task(OBS_TASK_UI, lib_apply_task, la, false);
}

static void lib_pick_and_apply(struct spin_filter *f, bool first, bool center)
{
	if (!f->lib_path[0])
		return;
	if (!f->lib_have_original) {
		obs_source_t *parent = obs_filter_get_parent(f->source);
		if (parent) {
			obs_data_t *ps = obs_source_get_settings(parent);
			const char *cur = obs_data_get_string(ps, "file");
			snprintf(f->lib_original_file,
				 sizeof(f->lib_original_file), "%s",
				 cur ? cur : "");
			obs_data_release(ps);
			if (!f->lib_parent_ws)
				f->lib_parent_ws =
					obs_source_get_weak_source(parent);
			f->lib_have_original = true;
		}
	}
	struct lib_file_list fl;
	lib_scan(f->lib_path, &fl);
	if (fl.count == 0) {
		lib_free(&fl);
		return;
	}
	size_t idx;
	if (first) {
		idx = 0;
		f->lib_index = 0;
	} else if (f->lib_random) {
		idx = (size_t)rand() % fl.count;
	} else {
		f->lib_index = (f->lib_index + 1) % fl.count;
		idx = f->lib_index;
	}
	lib_queue_apply(f, fl.paths[idx], center);
	lib_free(&fl);
}

/* ------------------------- the engine ------------------------- */

static void step_apply(const struct step *s, float t)
{
	switch (s->type) {
	case STEP_ROTATE: {
		/* gs_matrix_rotaa4f order: (x, y, z, angle); Z = 2D spin */
		float dir = s->clockwise ? -1.0f : 1.0f;
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f,
				  dir * s->turns * 2.0f * (float)M_PI * t);
		break;
	}
	case STEP_ZOOM: {
		float k = 1.0f + (s->zoom - 1.0f) * sinf((float)M_PI * t);
		gs_matrix_scale3f(k, k, 1.0f);
		break;
	}
	case STEP_SCALE:
		gs_matrix_scale3f(1.0f + (s->sx - 1.0f) * t,
				  1.0f + (s->sy - 1.0f) * t, 1.0f);
		break;
	case STEP_FLIP: {
		float c = cosf((float)M_PI * s->flips * t);
		if (s->flip_axis == FLIP_AXIS_HORIZONTAL)
			gs_matrix_scale3f(1.0f, c, 1.0f);
		else
			gs_matrix_scale3f(c, 1.0f, 1.0f);
		break;
	}
	}
}

static void spin_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct spin_filter *f = data;

	if (refresh_pending) {
		refresh_pending = false;
		obs_queue_task(OBS_TASK_UI, refresh_task, NULL, false);
	}

	obs_source_t *parent = obs_filter_get_parent(f->source);
	if (!parent) {
		obs_source_skip_video_filter(f->source);
		return;
	}
	float w = (float)obs_source_get_base_width(parent);
	float h = (float)obs_source_get_base_height(parent);
	if (w <= 0.0f || h <= 0.0f) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	float elapsed =
		(float)((os_gettime_ns() - f->start_ns) / 1000000.0);

	float total = 0.0f;
	for (size_t i = 0; i < f->step_count; i++)
		total += (f->steps[i].duration_ms > 0)
				 ? (float)f->steps[i].duration_ms
				 : 1.0f;

	if (f->loop_mode == LOOP_ALL && total > 0.0f && elapsed >= total)
		elapsed = fmodf(elapsed, total);

	if (f->loop_mode == LOOP_NONE && f->auto_hide && total > 0.0f &&
	    elapsed >= total) {
		if (!f->finished) {
			f->finished = true;
			obs_weak_source_t *ws =
				obs_source_get_weak_source(parent);
			obs_queue_task(OBS_TASK_UI, hide_task, ws, false);
		}
	} else {
		f->finished = false;
	}

	gs_matrix_push();
	gs_matrix_translate3f(w * 0.5f, h * 0.5f, 0.0f);

	float cursor = 0.0f;
	for (size_t i = 0; i < f->step_count; i++) {
		struct step *s = &f->steps[i];
		float dur =
			(s->duration_ms > 0) ? (float)s->duration_ms : 1.0f;
		bool last = (i == f->step_count - 1);

		if (f->loop_mode == LOOP_LAST && last &&
		    elapsed >= cursor) {
			step_apply(s, fmodf((elapsed - cursor) / dur, 1.0f));
			goto draw;
		}
		if (elapsed < cursor + dur) {
			step_apply(s, (elapsed - cursor) / dur);
			goto draw;
		}
		step_apply(s, 1.0f);
		cursor += dur;
	}

draw:
	gs_matrix_translate3f(-w * 0.5f, -h * 0.5f, 0.0f);
	obs_source_process_filter_begin(f->source, GS_RGBA,
					OBS_ALLOW_DIRECT_RENDERING);
	obs_source_process_filter_end(
		f->source, obs_get_base_effect(OBS_EFFECT_DEFAULT),
		(uint32_t)w, (uint32_t)h);
	gs_matrix_pop();
}

/* ------------- settings backbone (flat keys: stepN_<field>) ------------- */

static void read_step(obs_data_t *s, size_t i, struct step *st)
{
	char key[64];
	snprintf(key, sizeof(key), "step%zu_type", i);
	int t = (int)obs_data_get_int(s, key);
	if (t < STEP_ROTATE || t > STEP_FLIP)
		t = STEP_ROTATE;
	st->type = t;
	snprintf(key, sizeof(key), "step%zu_duration_ms", i);
	st->duration_ms = obs_data_get_int(s, key);
	snprintf(key, sizeof(key), "step%zu_turns", i);
	st->turns = (int)obs_data_get_int(s, key);
	snprintf(key, sizeof(key), "step%zu_clockwise", i);
	st->clockwise = obs_data_get_bool(s, key);
	snprintf(key, sizeof(key), "step%zu_zoom", i);
	st->zoom = (float)obs_data_get_double(s, key);
	snprintf(key, sizeof(key), "step%zu_sx", i);
	st->sx = (float)obs_data_get_double(s, key);
	snprintf(key, sizeof(key), "step%zu_sy", i);
	st->sy = (float)obs_data_get_double(s, key);
	snprintf(key, sizeof(key), "step%zu_flips", i);
	st->flips = (int)obs_data_get_int(s, key);
	snprintf(key, sizeof(key), "step%zu_flip_axis", i);
	st->flip_axis = (int)obs_data_get_int(s, key);
}

static void spin_update(void *data, obs_data_t *settings)
{
	struct spin_filter *f = data;
	f->start_on_show = obs_data_get_bool(settings, "start_on_show");
	f->auto_hide = obs_data_get_bool(settings, "auto_hide");
	f->loop_mode = (int)obs_data_get_int(settings, "loop_mode");
	f->lib_random = obs_data_get_bool(settings, "lib_random");
	{
		bool en = obs_data_get_bool(settings, "use_image_library");
		const char *p = obs_data_get_string(settings, "lib_path");
		bool changed = (en != f->lib_enabled) ||
			       strcmp(p ? p : "", f->lib_path) != 0;
		bool was = f->lib_enabled;
		f->lib_enabled = en;
		snprintf(f->lib_path, sizeof(f->lib_path), "%s", p ? p : "");
		if (en && changed && f->lib_path[0])
			lib_pick_and_apply(f, true, true);
		else if (!en && was)
			lib_restore(f);
	}

	long long n = obs_data_get_int(settings, "step_count");
	if (n < 0)
		n = 0;
	if (n > MAX_STEPS)
		n = MAX_STEPS;
	f->step_count = (size_t)n;
	for (size_t i = 0; i < f->step_count; i++)
		read_step(settings, i, &f->steps[i]);
}

static void *spin_create(obs_data_t *settings, obs_source_t *source)
{
	struct spin_filter *f = bzalloc(sizeof(*f));
	f->source = source;
	f->start_ns = os_gettime_ns();
	spin_update(f, settings);
	{
		const char *c = obs_data_get_string(settings,
						    "preset_choice");
		snprintf(f->snap_choice, sizeof(f->snap_choice), "%s",
			 c ? c : "");
	}
	live_register(f);
	return f;
}

static void spin_destroy(void *data)
{
	struct spin_filter *f = data;
	lib_restore(f);
	live_unregister(f);
	bfree(f);
}

static void spin_show(void *data)
{
	struct spin_filter *f = data;
	if (f->start_on_show)
		f->start_ns = os_gettime_ns();
}

static void spin_hide(void *data)
{
	struct spin_filter *f = data;
	if (f->lib_enabled)
		lib_pick_and_apply(f, false, false);
}

static void spin_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "start_on_show", true);
	obs_data_set_default_bool(settings, "auto_hide", false);
	obs_data_set_default_bool(settings, "use_image_library", false);
	obs_data_set_default_bool(settings, "lib_random", false);
	obs_data_set_default_string(settings, "lib_path", "");
	obs_data_set_default_int(settings, "loop_mode", LOOP_NONE);
	obs_data_set_default_int(settings, "step_count", 1);
	obs_data_set_default_string(settings, "preset_choice",
				    factory_names[0]);
	for (size_t i = 0; i < MAX_STEPS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "step%zu_type", i);
		obs_data_set_default_int(settings, key, STEP_ROTATE);
		snprintf(key, sizeof(key), "step%zu_duration_ms", i);
		obs_data_set_default_int(settings, key, 2000);
		snprintf(key, sizeof(key), "step%zu_turns", i);
		obs_data_set_default_int(settings, key, 1);
		snprintf(key, sizeof(key), "step%zu_zoom", i);
		obs_data_set_default_double(settings, key, 2.0);
		snprintf(key, sizeof(key), "step%zu_sx", i);
		obs_data_set_default_double(settings, key, 1.5);
		snprintf(key, sizeof(key), "step%zu_sy", i);
		obs_data_set_default_double(settings, key, 1.5);
		snprintf(key, sizeof(key), "step%zu_flips", i);
		obs_data_set_default_int(settings, key, 1);
		snprintf(key, sizeof(key), "step%zu_flip_axis", i);
		obs_data_set_default_int(settings, key, FLIP_AXIS_VERTICAL);
	}
}

/* ------------------------- the step editor UI ------------------------- */

static bool rebuild_cb(obs_properties_t *pp, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(pp);
	UNUSED_PARAMETER(p);

	struct spin_filter *f = find_by_settings(s);
	if (!f)
		return false;

	long long n = obs_data_get_int(s, "step_count");
	if (n < 0)
		n = 0;
	if (n > MAX_STEPS)
		n = MAX_STEPS;

	bool changed = (n != f->snap_count);
	if (!changed) {
		for (size_t i = 0; i < (size_t)n; i++) {
			char key[64];
			snprintf(key, sizeof(key), "step%zu_type", i);
			int t = (int)obs_data_get_int(s, key);
			if (t < STEP_ROTATE || t > STEP_FLIP)
				t = STEP_ROTATE;
			if (t != f->snap_types[i]) {
				changed = true;
				break;
			}
		}
	}

	if (!changed)
		return false; /* init/rebuild echo, not a user edit */

	f->snap_count = n;
	for (size_t i = 0; i < (size_t)n; i++) {
		char key[64];
		snprintf(key, sizeof(key), "step%zu_type", i);
		int t = (int)obs_data_get_int(s, key);
		if (t < STEP_ROTATE || t > STEP_FLIP)
			t = STEP_ROTATE;
		f->snap_types[i] = t;
	}
	refresh_pending = true;
	return false;
}

static void add_user_preset_names(obs_property_t *pl)
{
	obs_data_t *user = load_user_presets();
	obs_data_array_t *arr = obs_data_get_array(user, "preset_order");
	if (arr) {
		size_t cnt = obs_data_array_count(arr);
		for (size_t i = 0; i < cnt; i++) {
			obs_data_t *it = obs_data_array_item(arr, i);
			const char *nm = obs_data_get_string(it, "");
			if (nm && *nm)
				obs_property_list_add_string(pl, nm, nm);
			obs_data_release(it);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(user);
}

static obs_properties_t *spin_properties(void *data)
{
	struct spin_filter *f = data;
	obs_properties_t *pp = obs_properties_create();
	obs_properties_add_bool(pp, "start_on_show",
				"Start sequence when source becomes visible");

	obs_property_t *loop = obs_properties_add_list(
		pp, "loop_mode", "Loop", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(loop, "No loop", LOOP_NONE);
	obs_property_list_add_int(loop, "Loop last step", LOOP_LAST);
	obs_property_list_add_int(loop, "Loop whole sequence", LOOP_ALL);

	obs_properties_add_bool(pp, "auto_hide",
				"Hide source when sequence ends (No loop)");

	obs_properties_add_bool(pp, "use_image_library",
				"Use Image Library");
	obs_properties_add_bool(pp, "lib_random", "Randomise");
	obs_properties_add_path(pp, "lib_path", "Image library folder",
				OBS_PATH_DIRECTORY, NULL, NULL);

	/* ---------------- Load / Save presets ---------------- */
	obs_properties_t *pg = obs_properties_create();

	obs_property_t *pl = obs_properties_add_list(
		pg, "preset_choice", "Preset", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < 3; i++)
		obs_property_list_add_string(pl, factory_names[i],
					     factory_names[i]);
	add_user_preset_names(pl);
	obs_property_set_modified_callback(pl, preset_choice_modified_cb);
	obs_properties_add_button2(pg, "delete_preset", "Delete selected",
				   delete_preset_cb, f);

	obs_properties_add_text(pg, "preset_new_name", "New preset name",
				OBS_TEXT_DEFAULT);
	obs_properties_add_button2(pg, "save_preset", "Save as preset",
				   save_preset_cb, f);

	obs_properties_add_group(pp, "presets_group", "Load / Save presets",
				 OBS_GROUP_NORMAL, pg);

	/* ---------------- step editor ---------------- */

	obs_property_t *count = obs_properties_add_int(
		pp, "step_count", "Number of steps", 1, MAX_STEPS, 1);
	obs_property_set_modified_callback(count, rebuild_cb);

	if (!f)
		return pp;

	obs_data_t *s = obs_source_get_settings(f->source);
	long long n = s ? obs_data_get_int(s, "step_count") : 0;
	if (n < 0)
		n = 0;
	if (n > MAX_STEPS)
		n = MAX_STEPS;

	for (size_t i = 0; i < (size_t)n; i++) {
		char name[64], title[32];
		char key[64];
		snprintf(key, sizeof(key), "step%zu_type", i);
		int type = s ? (int)obs_data_get_int(s, key) : STEP_ROTATE;
		if (type < STEP_ROTATE || type > STEP_FLIP)
			type = STEP_ROTATE;

		obs_properties_t *g = obs_properties_create();

		snprintf(name, sizeof(name), "step%zu_type", i);
		obs_property_t *type_p = obs_properties_add_list(
			g, name, "Transformation", OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(type_p, "Rotate", STEP_ROTATE);
		obs_property_list_add_int(type_p, "Zoom in & out", STEP_ZOOM);
		obs_property_list_add_int(type_p, "Scale", STEP_SCALE);
		obs_property_list_add_int(type_p, "Flip", STEP_FLIP);
		obs_property_set_modified_callback(type_p, rebuild_cb);

		snprintf(name, sizeof(name), "step%zu_duration_ms", i);
		obs_properties_add_int(g, name, "Duration (ms)", 1,
				       3600000, 1);

		obs_property_t *pr;
		switch (type) {
		case STEP_ROTATE:
			snprintf(name, sizeof(name), "step%zu_turns", i);
			obs_properties_add_int(g, name, "Full turns", 1,
					       1000, 1);
			snprintf(name, sizeof(name), "step%zu_clockwise",
				 i);
			obs_properties_add_bool(g, name, "Clockwise");
			break;
		case STEP_ZOOM:
			snprintf(name, sizeof(name), "step%zu_zoom", i);
			obs_properties_add_float_slider(g, name,
							"Zoom amount",
							0.1, 10.0, 0.1);
			break;
		case STEP_SCALE:
			snprintf(name, sizeof(name), "step%zu_sx", i);
			obs_properties_add_float_slider(g, name, "Scale X",
							0.1, 10.0, 0.05);
			snprintf(name, sizeof(name), "step%zu_sy", i);
			obs_properties_add_float_slider(g, name, "Scale Y",
							0.1, 10.0, 0.05);
			break;
		case STEP_FLIP:
			snprintf(name, sizeof(name), "step%zu_flips", i);
			obs_properties_add_int(g, name, "Number of flips",
					       1, 1000, 1);
			snprintf(name, sizeof(name), "step%zu_flip_axis",
				 i);
			pr = obs_properties_add_list(g, name, "Flip axis",
						     OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(pr,
						  "Vertical (left-right)",
						  FLIP_AXIS_VERTICAL);
			obs_property_list_add_int(pr,
						  "Horizontal (upside-down)",
						  FLIP_AXIS_HORIZONTAL);
			break;
		}

		snprintf(name, sizeof(name), "stepgroup%zu", i);
		snprintf(title, sizeof(title), "Step %zu", i + 1);
		obs_properties_add_group(pp, name, title, OBS_GROUP_NORMAL,
					 g);
	}

	obs_properties_t *io = obs_properties_create();
#ifdef _WIN32
	obs_properties_add_button2(io, "import_presets", "Import",
				   import_button_cb, f);
	obs_properties_add_button2(io, "export_presets", "Export",
				   export_button_cb, f);
#else
	obs_property_t *exp_p = obs_properties_add_path(
		io, "export_path", "Export presets to file",
		OBS_PATH_FILE_SAVE,
		"Tr4nZ preset files (*.json);;All files (*.*)", NULL);
	obs_property_set_modified_callback(exp_p, export_path_modified_cb);

	obs_property_t *imp_p = obs_properties_add_path(
		io, "import_path", "Import presets (replace list)",
		OBS_PATH_FILE,
		"Tr4nZ preset files (*.json);;All files (*.*)", NULL);
	obs_property_set_modified_callback(imp_p, import_path_modified_cb);
#endif
	obs_properties_add_group(pp, "io_group", "Import / Export",
				 OBS_GROUP_NORMAL, io);

	if (s) {
		/* refresh the echo-suppression snapshot */
		const char *c = obs_data_get_string(s, "preset_choice");
		snprintf(f->snap_choice, sizeof(f->snap_choice), "%s",
			 c ? c : "");
		f->snap_count = n;
		for (size_t i = 0; i < (size_t)n; i++) {
			char k2[64];
			snprintf(k2, sizeof(k2), "step%zu_type", i);
			int tt = (int)obs_data_get_int(s, k2);
			if (tt < STEP_ROTATE || tt > STEP_FLIP)
				tt = STEP_ROTATE;
			f->snap_types[i] = tt;
		}
		obs_data_release(s);
	}
	return pp;
}

static const char *spin_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Tr4nZf0rM3r5_sequenced";
}

static struct obs_source_info spin_filter_info = {
	.id = "tr4nzf0rm3r5_sequenced",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = spin_get_name,
	.create = spin_create,
	.destroy = spin_destroy,
	.update = spin_update,
	.video_render = spin_video_render,
	.get_properties = spin_properties,
	.get_defaults = spin_defaults,
	.show = spin_show,
	.hide = spin_hide,
};

bool obs_module_load(void)
{
	srand((unsigned)time(NULL));
	obs_register_source(&spin_filter_info);
	return true;
}

OBS_DECLARE_MODULE()