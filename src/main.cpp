// ProShadeLite - addon ReShade (original, scris de la zero).
//
// Cum functioneaza:
//  * "Remove ..." = addonul sare peste draw call-urile care folosesc anumite shadere (identificate prin hash).
//    Hash-urile nu sunt cunoscute dinainte (depind de joc/versiune): le gasesti tu cu "Shader Finder" din
//    tab-ul Setup, le atribui unei optiuni, si ramane salvat in ProShadeLite.ini.
//  * Brightness / Brighter Nights / Lighter Shadows / Sky Color = efect ProShadeLite.fx comandat de addon.
//  * Protect UI From Effects = efectele ReShade se aplica inainte de primul draw de UI din cadru.

#include <imgui.h>
#include <reshade.hpp>

#include "crc32_hash.hpp"
#include "state_tracking.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace reshade::api;

// ---------------------------------------------------------------------------------------------
// Optiuni
// ---------------------------------------------------------------------------------------------
enum class kind { skip, adjust };

struct feature
{
	const char *key;
	const char *label;
	const char *group;
	kind type;
	bool in_boost; // este inclus in "FPS Boost"
	bool on = false;
	std::unordered_set<uint32_t> ps, vs; // hash-uri de pixel / vertex shader
};

enum : int
{
	F_CLOUDS, F_SKYCOLOR, F_SUN, F_MOON, F_STARS,
	F_STREAKS, F_ORBS, F_GLOW, F_HALOS, F_NIGHTS, F_BRIGHT,
	F_SHADOWS, F_SUNLIGHT, F_LIGHTSHADOWS, F_CONTACT,
	F_FOG, F_RAIN, F_SNOW, F_SMOKE, F_PUDDLES,
	F_HIDEUI, F_STREAMER,
	F_COUNT
};

static feature g_f[F_COUNT] = {
	{ "clouds",       "Remove Clouds",          "Sky",     kind::skip,   true  },
	{ "skycolor",     "Sky Color",              "Sky",     kind::adjust, false },
	{ "sun",          "Sun",                    "Sky",     kind::skip,   false },
	{ "moon",         "Moon",                   "Sky",     kind::skip,   false },
	{ "stars",        "Stars",                  "Sky",     kind::skip,   false },
	{ "streaks",      "Remove Light Streaks",   "Lights",  kind::skip,   true  },
	{ "orbs",         "Remove Light Orbs",      "Lights",  kind::skip,   true  },
	{ "glow",         "Glow Off",               "Lights",  kind::skip,   true  },
	{ "halos",        "Light Halos Off",        "Lights",  kind::skip,   true  },
	{ "nights",       "Brighter Nights",        "Lights",  kind::adjust, false },
	{ "brightness",   "Brightness",             "Lights",  kind::adjust, false },
	{ "shadows",      "Remove Shadows & Folds", "Shadows", kind::skip,   false },
	{ "sunlight",     "Remove Sunlight",        "Shadows", kind::skip,   true  },
	{ "lightshadows", "Lighter Shadows",        "Shadows", kind::adjust, false },
	{ "contact",      "Contact Shadows",        "Shadows", kind::skip,   false },
	{ "fog",          "Remove Fog",             "Weather", kind::skip,   true  },
	{ "rain",         "Remove Rain",            "Weather", kind::skip,   true  },
	{ "snow",         "Remove Snowflakes",      "Weather", kind::skip,   true  },
	{ "smoke",        "Remove Smoke",           "Weather", kind::skip,   true  },
	{ "puddles",      "Remove Rain Puddles",    "Weather", kind::skip,   true  },
	{ "hideui",       "Hide Game UI",           "UI",      kind::skip,   false },
	{ "streamer",     "Streamer Mode",          "UI",      kind::skip,   false },
};

// Setari globale
static bool g_fps_boost = false;
static bool g_protect_ui = false;
static int g_protect_mode = 0; // 0 = Early, 1 = Classic
static bool g_before_after = false; // true = arata imaginea originala
static float g_brightness = 0.0f, g_nights = 0.4f, g_lighter_shadows = 0.4f, g_sky_amount = 0.7f;
static float g_sky_color[3] = { 0.35f, 0.55f, 1.0f };

// Stare runtime
static std::shared_mutex g_mutex; // protejeaza g_pipelines + seturile "active"
static std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_pipelines; // pipeline -> (vs, ps)
static std::unordered_set<uint32_t> g_active_ps, g_active_vs;
static std::unordered_set<uint32_t> g_ui_ps, g_ui_vs; // marker pentru "Early"
static std::atomic<bool> g_dirty { true };
static std::atomic<bool> g_uniforms_dirty { true };
static std::atomic<bool> g_early_done { false };
static effect_runtime *g_runtime = nullptr;
static bool g_effects_state_before = true;

// Shader Finder
static std::mutex g_seen_mutex;
static std::unordered_map<uint32_t, uint32_t> g_seen_ps_cur, g_seen_vs_cur;
static std::vector<std::pair<uint32_t, uint32_t>> g_seen_ps, g_seen_vs; // sortat dupa nr. draw-uri
static std::atomic<bool> g_finder_on { false };
static std::atomic<uint32_t> g_preview_ps { 0 }, g_preview_vs { 0 };
static int g_sel_ps = -1, g_sel_vs = -1;
static std::string g_status;

struct __declspec(uuid("5b2a6f3c-7d1e-4f0a-9a21-3c0a5f7e9b10")) cmd_data
{
	uint32_t vs = 0, ps = 0;
};

// ---------------------------------------------------------------------------------------------
// Config (ProShadeLite.ini langa ReShade.ini)
// ---------------------------------------------------------------------------------------------
static std::filesystem::path base_path()
{
	size_t n = 0;
	reshade::get_reshade_base_path(nullptr, &n);
	std::string s(n, '\0');
	reshade::get_reshade_base_path(s.data(), &n);
	s.resize(n ? n - 1 : 0);
	return std::filesystem::path(s);
}

static std::string hex_list(const std::unordered_set<uint32_t> &set)
{
	std::string out;
	char buf[16];
	for (uint32_t h : set)
	{
		std::snprintf(buf, sizeof(buf), "%s%08X", out.empty() ? "" : ",", h);
		out += buf;
	}
	return out;
}
static void parse_hex_list(const std::string &text, std::unordered_set<uint32_t> &set)
{
	std::stringstream ss(text);
	std::string tok;
	while (std::getline(ss, tok, ','))
		if (!tok.empty())
			set.insert(static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 16)));
}

static void save_config()
{
	std::ofstream f(base_path() / "ProShadeLite.ini");
	if (!f)
		return;
	f << "fps_boost=" << g_fps_boost << "\nprotect_ui=" << g_protect_ui << "\nprotect_mode=" << g_protect_mode << '\n';
	f << "brightness=" << g_brightness << "\nnights=" << g_nights << "\nlighter_shadows=" << g_lighter_shadows
	  << "\nsky_amount=" << g_sky_amount << "\nsky_r=" << g_sky_color[0] << "\nsky_g=" << g_sky_color[1]
	  << "\nsky_b=" << g_sky_color[2] << '\n';
	for (const feature &x : g_f)
	{
		f << x.key << ".on=" << x.on << '\n';
		f << x.key << ".ps=" << hex_list(x.ps) << '\n';
		f << x.key << ".vs=" << hex_list(x.vs) << '\n';
	}
}

static void load_config()
{
	std::ifstream f(base_path() / "ProShadeLite.ini");
	if (!f)
		return;
	std::unordered_map<std::string, std::string> kv;
	std::string line;
	while (std::getline(f, line))
	{
		const size_t eq = line.find('=');
		if (eq != std::string::npos)
			kv[line.substr(0, eq)] = line.substr(eq + 1);
	}
	auto b = [&](const char *k, bool d) { auto it = kv.find(k); return it == kv.end() ? d : it->second == "1"; };
	auto fl = [&](const char *k, float d) { auto it = kv.find(k); return it == kv.end() ? d : std::strtof(it->second.c_str(), nullptr); };
	g_fps_boost = b("fps_boost", false);
	g_protect_ui = b("protect_ui", false);
	g_protect_mode = static_cast<int>(fl("protect_mode", 0));
	g_brightness = fl("brightness", 0.0f);
	g_nights = fl("nights", 0.4f);
	g_lighter_shadows = fl("lighter_shadows", 0.4f);
	g_sky_amount = fl("sky_amount", 0.7f);
	g_sky_color[0] = fl("sky_r", 0.35f);
	g_sky_color[1] = fl("sky_g", 0.55f);
	g_sky_color[2] = fl("sky_b", 1.0f);
	for (feature &x : g_f)
	{
		x.on = b((std::string(x.key) + ".on").c_str(), false);
		x.ps.clear();
		x.vs.clear();
		if (auto it = kv.find(std::string(x.key) + ".ps"); it != kv.end()) parse_hex_list(it->second, x.ps);
		if (auto it = kv.find(std::string(x.key) + ".vs"); it != kv.end()) parse_hex_list(it->second, x.vs);
	}
}

// Recalculeaza seturile de shadere care trebuie sarite
static void rebuild_active()
{
	std::unordered_set<uint32_t> aps, avs, ups, uvs;
	for (int i = 0; i < F_COUNT; ++i)
	{
		const feature &x = g_f[i];
		if (x.type != kind::skip)
			continue;
		if (i == F_HIDEUI)
		{
			ups = x.ps;
			uvs = x.vs;
		}
		const bool active = x.on || (g_fps_boost && x.in_boost);
		if (active && !g_before_after)
		{
			aps.insert(x.ps.begin(), x.ps.end());
			avs.insert(x.vs.begin(), x.vs.end());
		}
	}
	if (!g_before_after)
	{
		if (g_preview_ps) aps.insert(g_preview_ps.load());
		if (g_preview_vs) avs.insert(g_preview_vs.load());
	}
	std::unique_lock lock(g_mutex);
	g_active_ps = std::move(aps);
	g_active_vs = std::move(avs);
	g_ui_ps = std::move(ups);
	g_ui_vs = std::move(uvs);
	g_dirty = false;
}

// ---------------------------------------------------------------------------------------------
// Evenimente
// ---------------------------------------------------------------------------------------------
static void on_init_command_list(command_list *cl) { cl->create_private_data<cmd_data>(); }
static void on_destroy_command_list(command_list *cl) { cl->destroy_private_data<cmd_data>(); }

static void on_init_effect_runtime(effect_runtime *rt) { g_runtime = rt; g_uniforms_dirty = true; }
static void on_destroy_effect_runtime(effect_runtime *rt) { if (g_runtime == rt) g_runtime = nullptr; }
static void on_reloaded_effects(effect_runtime *) { g_uniforms_dirty = true; }

static void on_init_pipeline(device *, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline p)
{
	uint32_t vs = 0, ps = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		if (subobjects[i].count == 0 || subobjects[i].data == nullptr)
			continue;
		const auto *sh = static_cast<const shader_desc *>(subobjects[i].data);
		if (sh->code_size == 0)
			continue;
		if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
			vs = compute_crc32(static_cast<const uint8_t *>(sh->code), sh->code_size);
		else if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
			ps = compute_crc32(static_cast<const uint8_t *>(sh->code), sh->code_size);
	}
	if (vs == 0 && ps == 0)
		return;
	std::unique_lock lock(g_mutex);
	g_pipelines[p.handle] = { vs, ps };
}
static void on_destroy_pipeline(device *, pipeline p)
{
	std::unique_lock lock(g_mutex);
	g_pipelines.erase(p.handle);
}

static void on_bind_pipeline(command_list *cl, pipeline_stage stages, pipeline p)
{
	auto *d = cl->get_private_data<cmd_data>();
	if (d == nullptr)
		return;
	const bool has_vs = (stages & pipeline_stage::vertex_shader) != 0;
	const bool has_ps = (stages & pipeline_stage::pixel_shader) != 0;
	if (!has_vs && !has_ps)
		return;

	uint32_t vs = 0, ps = 0;
	if (p.handle != 0)
	{
		std::shared_lock lock(g_mutex);
		if (auto it = g_pipelines.find(p.handle); it != g_pipelines.end())
			std::tie(vs, ps) = it->second;
	}
	if (has_vs) d->vs = vs;
	if (has_ps) d->ps = ps;
}

// Randeaza efectele ReShade inainte de primul draw de UI din cadru
static void render_effects_early(command_list *cl)
{
	if (g_runtime == nullptr || g_early_done.exchange(true))
		return;
	auto *st = cl->get_private_data<state_tracking>();
	if (st == nullptr || st->render_targets.size() != 1)
	{
		g_early_done = false;
		return;
	}
	device *dev = cl->get_device();
	uint32_t w = 0, h = 0;
	g_runtime->get_screenshot_width_and_height(&w, &h);
	const resource_desc rd = dev->get_resource_desc(dev->get_resource_from_view(st->render_targets[0]));
	if (rd.texture.width != w || rd.texture.height != h)
	{
		g_early_done = false;
		return;
	}
	const resource_view rtv = st->render_targets[0];
	g_runtime->render_effects(cl, rtv, rtv);
	st->apply(cl); // render_effects poate modifica starea
}

static bool process_draw(command_list *cl)
{
	auto *d = cl->get_private_data<cmd_data>();
	if (d == nullptr)
		return false;

	if (g_finder_on.load(std::memory_order_relaxed))
	{
		std::lock_guard lock(g_seen_mutex);
		if (d->ps) ++g_seen_ps_cur[d->ps];
		if (d->vs) ++g_seen_vs_cur[d->vs];
	}

	if (g_dirty.load(std::memory_order_relaxed))
		rebuild_active();

	std::shared_lock lock(g_mutex);

	if (g_protect_ui && g_protect_mode == 0 && !g_early_done.load(std::memory_order_relaxed) &&
		((d->ps && g_ui_ps.count(d->ps)) || (d->vs && g_ui_vs.count(d->vs))))
	{
		lock.unlock();
		render_effects_early(cl);
		lock.lock();
	}

	return (d->ps && g_active_ps.count(d->ps)) || (d->vs && g_active_vs.count(d->vs));
}

static bool on_draw(command_list *cl, uint32_t, uint32_t, uint32_t, uint32_t) { return process_draw(cl); }
static bool on_draw_indexed(command_list *cl, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return process_draw(cl); }
static bool on_draw_indirect(command_list *cl, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
{
	if (type == indirect_command::draw || type == indirect_command::draw_indexed)
		return process_draw(cl);
	return false;
}

// ---------------------------------------------------------------------------------------------
// Uniforme / tehnica pentru ProShadeLite.fx
// ---------------------------------------------------------------------------------------------
static void apply_uniforms(effect_runtime *rt)
{
	auto set1 = [&](const char *name, float v) {
		const effect_uniform_variable var = rt->find_uniform_variable("ProShadeLite.fx", name);
		if (var.handle != 0)
			rt->set_uniform_value_float(var, &v, 1);
	};
	const bool bypass = g_before_after;
	set1("PSL_Brightness", (g_f[F_BRIGHT].on && !bypass) ? g_brightness : 0.0f);
	set1("PSL_BrighterNights", (g_f[F_NIGHTS].on && !bypass) ? g_nights : 0.0f);
	set1("PSL_LighterShadows", (g_f[F_LIGHTSHADOWS].on && !bypass) ? g_lighter_shadows : 0.0f);
	set1("PSL_SkyAmount", (g_f[F_SKYCOLOR].on && !bypass) ? g_sky_amount : 0.0f);
	const effect_uniform_variable col = rt->find_uniform_variable("ProShadeLite.fx", "PSL_SkyColor");
	if (col.handle != 0)
		rt->set_uniform_value_float(col, g_sky_color, 3);

	const bool any = !bypass && (g_f[F_BRIGHT].on || g_f[F_NIGHTS].on || g_f[F_LIGHTSHADOWS].on || g_f[F_SKYCOLOR].on);
	const effect_technique tech = rt->find_technique("ProShadeLite.fx", "ProShadeLite");
	if (tech.handle != 0)
		rt->set_technique_state(tech, any);
	g_uniforms_dirty = false;
}

static void set_before_after(bool v)
{
	if (v == g_before_after)
		return;
	g_before_after = v;
	if (g_runtime != nullptr)
	{
		if (v)
		{
			g_effects_state_before = g_runtime->get_effects_state();
			g_runtime->set_effects_state(false);
		}
		else
		{
			g_runtime->set_effects_state(g_effects_state_before);
		}
	}
	g_dirty = true;
	g_uniforms_dirty = true;
}

static void on_present(effect_runtime *rt)
{
	g_early_done = false;

	if (g_finder_on.load())
	{
		std::unordered_map<uint32_t, uint32_t> ps, vs;
		{
			std::lock_guard lock(g_seen_mutex);
			ps.swap(g_seen_ps_cur);
			vs.swap(g_seen_vs_cur);
		}
		static int frame = 0;
		if (++frame % 15 == 0) // actualizeaza lista ~4x/s ca sa nu "sara" in UI
		{
			auto to_sorted = [](const auto &m) {
				std::vector<std::pair<uint32_t, uint32_t>> v(m.begin(), m.end());
				std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first < b.first; });
				return v;
			};
			g_seen_ps = to_sorted(ps);
			g_seen_vs = to_sorted(vs);
		}
	}

	if (g_uniforms_dirty.load())
		apply_uniforms(rt);
}

// ---------------------------------------------------------------------------------------------
// Screenshot (BMP 32-bit)
// ---------------------------------------------------------------------------------------------
static void take_screenshot(effect_runtime *rt)
{
	uint32_t w = 0, h = 0;
	rt->get_screenshot_width_and_height(&w, &h);
	const format fmt = rt->get_device()->get_resource_desc(rt->get_current_back_buffer()).texture.format;

	bool swap_rb;
	switch (fmt)
	{
	case format::r8g8b8a8_unorm: case format::r8g8b8a8_unorm_srgb: swap_rb = false; break;
	case format::b8g8r8a8_unorm: case format::b8g8r8a8_unorm_srgb:
	case format::b8g8r8x8_unorm: swap_rb = true; break;
	default: g_status = "Format back buffer neacceptat pentru screenshot."; return;
	}

	std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
	if (!rt->capture_screenshot(px.data()))
	{
		g_status = "Captura a esuat.";
		return;
	}

	if (!swap_rb) // BMP vrea BGRA
		for (size_t i = 0; i < px.size(); i += 4)
			std::swap(px[i], px[i + 2]);
	for (size_t i = 3; i < px.size(); i += 4)
		px[i] = 255;

	std::filesystem::path dir = base_path() / "ProShadeLite Screenshots";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);

	char name[64];
	const std::time_t t = std::time(nullptr);
	std::tm tmv {};
	localtime_s(&tmv, &t);
	std::strftime(name, sizeof(name), "ProShadeLite_%Y%m%d_%H%M%S.bmp", &tmv);
	const std::filesystem::path file = dir / name;

#pragma pack(push, 1)
	struct { uint16_t type = 0x4D42; uint32_t size; uint32_t reserved = 0; uint32_t offset = 54; } fh;
	struct { uint32_t size = 40; int32_t w, h; uint16_t planes = 1, bpp = 32; uint32_t comp = 0, img = 0; int32_t xp = 2835, yp = 2835; uint32_t used = 0, imp = 0; } ih;
#pragma pack(pop)
	fh.size = 54 + static_cast<uint32_t>(px.size());
	ih.w = static_cast<int32_t>(w);
	ih.h = -static_cast<int32_t>(h); // top-down

	std::ofstream f(file, std::ios::binary);
	f.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
	f.write(reinterpret_cast<const char *>(&ih), sizeof(ih));
	f.write(reinterpret_cast<const char *>(px.data()), static_cast<std::streamsize>(px.size()));
	g_status = "Salvat: " + file.string();
}

// ---------------------------------------------------------------------------------------------
// UI (tema mov)
// ---------------------------------------------------------------------------------------------
static const ImU32 ACCENT = IM_COL32(139, 71, 255, 255);
static const ImU32 OFFCOL = IM_COL32(48, 42, 56, 255);

// Intrerupator in stil "toggle"; intoarce true cand se schimba
static bool toggle(const char *label, bool *v)
{
	ImGui::PushID(label);
	const float h = ImGui::GetFrameHeight() * 0.95f, w = h * 1.8f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float avail = ImGui::GetContentRegionAvail().x;
	const bool clicked = ImGui::InvisibleButton("##t", ImVec2(avail, h * 1.3f), 0);
	if (clicked)
		*v = !*v;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 rmax(p.x + avail, p.y + h * 1.3f);
	dl->AddRectFilled(p, rmax, *v ? IM_COL32(70, 30, 140, 200) : IM_COL32(24, 20, 30, 230), 10.0f, 0);
	dl->AddText(ImVec2(p.x + 12.0f, p.y + h * 0.25f), IM_COL32(255, 255, 255, 255), label);
	const ImVec2 tp(rmax.x - w - 10.0f, p.y + h * 0.15f);
	dl->AddRectFilled(tp, ImVec2(tp.x + w, tp.y + h), *v ? ACCENT : OFFCOL, h * 0.5f, 0);
	dl->AddCircleFilled(ImVec2(*v ? tp.x + w - h * 0.5f : tp.x + h * 0.5f, tp.y + h * 0.5f), h * 0.38f, IM_COL32(255, 255, 255, 255), 0);
	ImGui::PopID();
	return clicked;
}

static void feature_toggle(int i)
{
	if (toggle(g_f[i].label, &g_f[i].on))
	{
		g_dirty = true;
		g_uniforms_dirty = true;
		save_config();
	}
	if (g_f[i].type == kind::skip && g_f[i].ps.empty() && g_f[i].vs.empty() && g_f[i].on)
		ImGui::TextDisabled("   Niciun shader atribuit - foloseste Setup > Shader Finder");
}

static void slider_if_on(int i, const char *label, float *v, float lo, float hi)
{
	if (!g_f[i].on)
		return;
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::SliderFloat(label, v, lo, hi, "%.2f", 0))
	{
		g_uniforms_dirty = true;
		save_config();
	}
}

static void draw_group(const char *group)
{
	ImGui::SeparatorText(group);
	for (int i = 0; i < F_COUNT; ++i)
	{
		if (std::string(g_f[i].group) != group || i == F_HIDEUI || i == F_STREAMER)
			continue;
		feature_toggle(i);
		if (i == F_SKYCOLOR && g_f[i].on)
		{
			slider_if_on(i, "Amount##sky", &g_sky_amount, 0.0f, 1.0f);
			if (ImGui::ColorEdit3("##skycol", g_sky_color, 0))
				{ g_uniforms_dirty = true; save_config(); }
			ImGui::TextDisabled("   Foloseste depth buffer-ul; poate necesita Generic Depth.");
		}
		if (i == F_BRIGHT) slider_if_on(i, "##b", &g_brightness, -1.0f, 1.0f);
		if (i == F_NIGHTS) slider_if_on(i, "##n", &g_nights, 0.0f, 1.0f);
		if (i == F_LIGHTSHADOWS) slider_if_on(i, "##ls", &g_lighter_shadows, 0.0f, 1.0f);
	}
}

static void draw_finder()
{
	ImGui::SeparatorText("Shader Finder");
	bool on = g_finder_on.load();
	if (ImGui::Checkbox("Scaneaza shadere (activeaza doar cand cauti; costa performanta)", &on))
	{
		g_finder_on = on;
		if (!on)
		{
			g_preview_ps = g_preview_vs = 0;
			g_dirty = true;
		}
	}
	if (!on)
	{
		ImGui::TextWrapped("Pas cu pas: porneste scanarea, mergi in joc unde e efectul (ploaie, ceata...), apoi alege un shader "
		                   "din lista cu sagetile. Shaderul ales e ASCUNS imediat - cand dispare exact efectul dorit, "
		                   "atribuie-l optiunii de mai jos.");
		return;
	}

	auto list = [](const char *title, const std::vector<std::pair<uint32_t, uint32_t>> &v, int &sel, std::atomic<uint32_t> &preview) {
		ImGui::PushID(title);
		ImGui::TextUnformatted(title);
		int mv = 0;
		if (ImGui::SmallButton("<")) mv = -1;
		ImGui::SameLine();
		if (ImGui::SmallButton(">")) mv = 1;
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset")) { sel = -1; preview = 0; g_dirty = true; }
		if (mv != 0 && !v.empty())
		{
			sel = std::clamp(sel + mv, 0, static_cast<int>(v.size()) - 1);
			preview = v[sel].first;
			g_dirty = true;
		}
		if (sel >= 0 && sel < static_cast<int>(v.size()))
			ImGui::Text("Ales: %08X (%u draw-uri/cadru) [%d/%zu]", v[sel].first, v[sel].second, sel + 1, v.size());
		else
			ImGui::TextDisabled("Niciun shader ales (%zu gasite)", v.size());
		ImGui::PopID();
	};
	list("Pixel shadere", g_seen_ps, g_sel_ps, g_preview_ps);
	ImGui::Spacing();
	list("Vertex shadere", g_seen_vs, g_sel_vs, g_preview_vs);

	static int target = 0;
	std::vector<const char *> names;
	std::vector<int> idx;
	for (int i = 0; i < F_COUNT; ++i)
		if (g_f[i].type == kind::skip) { names.push_back(g_f[i].label); idx.push_back(i); }
	target = std::clamp(target, 0, static_cast<int>(names.size()) - 1);
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::Combo("##target", &target, names.data(), static_cast<int>(names.size()), -1);

	const uint32_t ps = g_preview_ps.load(), vs = g_preview_vs.load();
	if (ImGui::Button("Atribuie shaderul ales") && (ps || vs))
	{
		feature &f = g_f[idx[target]];
		if (ps) f.ps.insert(ps);
		if (vs) f.vs.insert(vs);
		g_preview_ps = g_preview_vs = 0;
		g_sel_ps = g_sel_vs = -1;
		g_dirty = true;
		save_config();
		g_status = std::string("Atribuit la: ") + f.label;
	}
	ImGui::SameLine();
	if (ImGui::Button("Goleste optiunea"))
	{
		g_f[idx[target]].ps.clear();
		g_f[idx[target]].vs.clear();
		g_dirty = true;
		save_config();
	}
	const feature &f = g_f[idx[target]];
	ImGui::TextDisabled("%s: %zu pixel, %zu vertex shadere", f.label, f.ps.size(), f.vs.size());
}

static void draw_ui(effect_runtime *rt)
{
	if (ImGui::BeginTabBar("##psl", 0))
	{
		if (ImGui::BeginTabItem("Home", nullptr, 0))
		{
			bool b = g_fps_boost;
			if (toggle("FPS Boost", &b))
			{
				g_fps_boost = b;
				g_dirty = true;
				save_config();
			}
			ImGui::TextDisabled("   Ascunde: ploaie, ceata, fum, dungi/orbe/halouri de lumina, nori, zapada, balti, glow, soare");

			if (toggle("Protect UI From Effects", &g_protect_ui))
				save_config();
			if (g_protect_ui)
			{
				if (ImGui::RadioButton("Early", g_protect_mode == 0)) { g_protect_mode = 0; save_config(); }
				ImGui::SameLine();
				if (ImGui::RadioButton("Classic", g_protect_mode == 1)) { g_protect_mode = 1; save_config(); }
				if (g_protect_mode == 0 && g_f[F_HIDEUI].ps.empty() && g_f[F_HIDEUI].vs.empty())
					ImGui::TextDisabled("   Early are nevoie de un shader de UI atribuit la 'Hide Game UI'.");
			}

			feature_toggle(F_HIDEUI);
			bool ba = g_before_after;
			if (toggle("Before / After", &ba))
				set_before_after(ba);
			feature_toggle(F_STREAMER);

			if (ImGui::Button("Take a screenshot", ImVec2(-1.0f, 0.0f)))
				take_screenshot(rt);
			if (!g_status.empty())
				ImGui::TextWrapped("%s", g_status.c_str());
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Sky", nullptr, 0)) { draw_group("Sky"); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Lights", nullptr, 0)) { draw_group("Lights"); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Shadows", nullptr, 0)) { draw_group("Shadows"); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Weather", nullptr, 0)) { draw_group("Weather"); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Setup", nullptr, 0)) { draw_finder(); ImGui::EndTabItem(); }
		ImGui::EndTabBar();
	}
}

// ---------------------------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		state_tracking::register_events(); // trebuie inregistrat primul
		load_config();
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_overlay("ProShadeLite", draw_ui);
		break;
	case DLL_PROCESS_DETACH:
		save_config();
		reshade::unregister_overlay("ProShadeLite", draw_ui);
		reshade::unregister_addon(hModule);
		state_tracking::unregister_events();
		break;
	}
	return TRUE;
}
