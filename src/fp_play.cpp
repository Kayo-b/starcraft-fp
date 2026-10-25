// fp_play: interactive first-person viewer / sandbox over OpenBW (SDL2). Read-only with respect to game files.
//
// Modes
//   --replay FILE        watch a replay from inside the world (no orders)
//   --game MAP           live melee sandbox on a stock map: you are one player, a second base sits idle
//                        (OpenBW has no AI, so the enemy only acts if you control it)
//   --game MAP --fps     FPS wave mode: you are a hero unit, waves of the enemy race hunt you (fp_fps.h)
//   --game MAP --host [PORT] / --game MAP --join HOST[:PORT]
//                        1v1 over the network (fp_net.h): each player has a base and a hero; play the hero in first
//                        person or step out (F) to command the base. Both players load the same map file. Port 6112.
//   --map FILE           load a map with neutral units only
//
// Multiplayer options: --latency N (frames between a command and its effect, 1..8, default 2; both players alike),
//   --name NAME. Pause, sim speed and restart are off; --fire-rate/--range/--speed/--fps-move-shoot must match the other
//   player's. With --fps-move-shoot the camera shows your hero where your input is taking it (no wait for the latency).
//
// FPS options: --weapons MOD_DIR (gun art/sounds), --fps-move-shoot (free movement: walk and shoot at once),
//   --fire-rate N / --range N / --speed N (percent of the StarCraft data, 10..1000)
// Other options: --race R, --enemy R, --res WxH, --window WxH, --fullscreen, --sim-speed X, --start-frame N,
//   --no-capture, --no-vsync, --view-dist PX, --eye PX, --level-h PX, --selftest-input, --autotest [--screenshot F.ppm],
//   --models DIR (SC Evo Complete's SCEvo_Assets.SC2Mod/Base.SC2Assets: buildings and minerals as 3D models, fp_models.h),
//   --model-units (experimental: units with a model too, in its rest pose)
//
// Controls (game mode in brackets, FPS mode in braces)
//   Mouse look; W/A/S/D move (Shift = run); ESC release/capture mouse; Q quit; F12 screenshot (PPM in cwd)
//   P pause, = / - sim speed, R restart, F free camera, TAB next unit, M cycle 3D models (off / buildings / buildings + units)
//   [LMB select unit under crosshair (Shift adds), RMB order (move/attack/gather/rally), E possess/release the selected unit,
//    WASD then moves the possessed unit, T train worker, Y train combat unit, B build supply, X stop]
//   {LMB (hold) fire, RMB move to the crosshair; the camera stays on the hero}
//   Multiplayer: the hero view takes the FPS controls; F switches to commanding the base (free camera, the game-mode
//   controls in brackets; the hero fights on its own meanwhile) and back.
#include "fp_render.h"
#include "fp_game.h"
#include "fp_fps.h"
#include "fp_weapon.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>

using namespace fp;
using Clock = std::chrono::steady_clock;


struct Args {
	std::string data, input, game_map, screenshot, weapons, models; bool replay = true, autotest = false, fullscreen = false, no_capture = false, selftest_input = false, fps = false, direct = false, no_vsync = false, model_units = false; int fire_rate = 200, range_pct = 200, speed_pct = 90;
	int rw = 640, rh = 400, ww = 1280, wh = 800, start_frame = 0; double speed = 1.0;
	race_t race = race_t::terran, enemy = race_t::protoss;
	RenderParams rp;
	bool net = false; NetOptions netopt; // --host / --join
};
static void die(const char* m) { std::cerr << m << "\n"; std::exit(2); }
static race_t parse_race(const std::string& s) { if (s == "terran") return race_t::terran; if (s == "protoss") return race_t::protoss; if (s == "zerg") return race_t::zerg; die("race must be terran, protoss or zerg"); return race_t::terran; }

// A replay or a live game, plus its renderer.
struct Session {
	std::unique_ptr<replay_player> player; std::unique_ptr<Game> game; std::unique_ptr<Fps> fps; std::unique_ptr<Renderer> r; bool replay = true;
	ModelLibrary* models = nullptr; // 3D models (kept across restarts: loaded models stay cached)
	int model_mode = Renderer::kModelsBuildings; // kept across restarts; M cycles it
	void open(Assets& assets, const Args& a) {
		r.reset(); player.reset(); fps.reset(); game.reset();
		if (!a.game_map.empty() && a.net) { // blocks in the lobby (status on stdout) until the host starts the game
			replay = false; game = std::make_unique<Game>();
			game->open_net(a.data, a.game_map, a.race, a.netopt, [](const std::string& m) { std::cout << m << std::endl; }, Fps::settings_hash(a.fire_rate, a.range_pct, a.speed_pct, a.direct));
			fps = std::make_unique<Fps>(*game, 7, a.fire_rate, a.range_pct, a.speed_pct, a.direct, true);
			if (!fps->start()) error("the map has no room for the heroes at the start locations");
			std::cout << "Game started against " << game->opponent_name() << " (you are player " << game->owner() + 1 << ")" << std::endl;
			r = std::make_unique<Renderer>(game->st(), game->fn(), assets, a.rw, a.rh, a.rp);
			r->set_viewer(game->owner()); // the other player's units only where yours see them
		} else if (!a.game_map.empty()) {
			replay = false; game = std::make_unique<Game>(); game->open(a.data, a.game_map, a.race, a.enemy, 1, a.fps);
			if (a.fps) { fps = std::make_unique<Fps>(*game, 7, a.fire_rate, a.range_pct, a.speed_pct, a.direct); if (!fps->start()) error("FPS mode: the map has no usable start location"); }
			r = std::make_unique<Renderer>(game->st(), game->fn(), assets, a.rw, a.rh, a.rp);
		} else {
			player = std::make_unique<replay_player>(); player->init(dir_loader(a.data)); replay = a.replay;
			if (replay) player->load_replay_file(a.input); else player->load_map_file(a.input);
			r = std::make_unique<Renderer>(player->st(), player->funcs(), assets, a.rw, a.rh, a.rp);
			for (int i = 0; i < a.start_frame && !done(); ++i) step();
		}
		if (models) { r->set_models(models); r->set_model_mode(model_mode); }
		r->update_units();
	}
	bool live() const { return game != nullptr; }
	bool net() const { return game && game->net(); }
	bool done() { return replay && player && player->is_done(); }
	// One frame. Multiplayer: false while the other player has not caught up (or after the games disagreed).
	bool step() {
		if (game) { if (!game->step()) return false; if (fps) fps->tick(); }
		else if (replay) player->next_frame(); else static_cast<game_player&>(*player).next_frame();
		return true;
	}
	int frame() { return game ? game->frame() : player->st().current_frame; }
	int end_frame() { return (!game && replay) ? player->replay_st.end_frame : 0; }
	state& st() { return game ? game->st() : player->st(); }
	state_functions& fn() { return game ? game->fn() : player->funcs(); }
};

struct View {
	Camera cam; const unit_t* follow = nullptr; double radius = 5.0;
	void move(const Renderer& r, double fwd, double strafe) { // slide along walls
		const double PI = 3.14159265358979323846, h = cam.heading * PI / 180;
		double dx = std::cos(h) * fwd - std::sin(h) * strafe, dy = std::sin(h) * fwd + std::cos(h) * strafe;
		if (r.can_stand(cam.x + dx, cam.y, radius)) cam.x += dx;
		if (r.can_stand(cam.x, cam.y + dy, radius)) cam.y += dy;
	}
	void spawn(const Renderer& r) { // start at the first player-owned unit
		for (auto& u : r.units()) if (u.owner < 8) { cam.x = u.x; cam.y = u.y; return; }
		cam.x = r.map_w() / 2; cam.y = r.map_h() / 2;
	}
	// Next unit after the current one whose owner passes `want` (owner < 0: any player unit).
	void cycle_follow(const Renderer& r, int owner) {
		const auto& us = r.units(); size_t start = 0;
		for (size_t i = 0; i < us.size(); ++i) if (us[i].ptr == follow) start = i + 1;
		for (size_t k = 0; k < us.size(); ++k) { const auto& u = us[(start + k) % us.size()]; if (owner < 0 ? u.owner < 8 : u.owner == owner) { follow = u.ptr; return; } }
	}
	void apply_follow(const Renderer& r) {
		if (!follow) return;
		for (auto& u : r.units()) if (u.ptr == follow) { cam.x = u.x; cam.y = u.y; return; }
		follow = nullptr; // the unit is gone: keep the last position, back to free camera
	}
};

static void save_ppm(const std::string& path, const std::vector<uint32_t>& fb, int w, int h) {
	FILE* f = fopen(path.c_str(), "wb"); if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (uint32_t p : fb) { uint8_t c[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p}; fwrite(c, 1, 3, f); }
	fclose(f);
}

// ---- HUD text via SDL_ttf (optional: without a usable font only the window title is shown) ----
struct Hud {
	SDL_Renderer* ren = nullptr; TTF_Font* big = nullptr; TTF_Font* small = nullptr; bool ok = false;
	struct Entry { SDL_Texture* tex; int w, h; };
	std::map<std::string, Entry> cache;
	void init(SDL_Renderer* r, int win_h) {
		ren = r; if (TTF_Init() != 0) { std::cerr << "HUD disabled: " << TTF_GetError() << "\n"; return; }
		const char* env = getenv("FP_FONT");
		std::vector<std::string> cands; if (env) cands.push_back(env);
		for (const char* c : {"/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/LiberationSans-Regular.ttf"}) cands.push_back(c);
		for (auto& p : cands) {
			big = TTF_OpenFont(p.c_str(), std::max(16, win_h / 34)); small = TTF_OpenFont(p.c_str(), std::max(12, win_h / 52));
			if (big && small) { ok = true; return; }
			if (big) TTF_CloseFont(big); if (small) TTF_CloseFont(small); big = small = nullptr;
		}
		std::cerr << "HUD disabled: no usable font found (set FP_FONT=/path/to/font.ttf)\n";
	}
	void text(int x, int y, const std::string& s, SDL_Color c, bool sm = false, bool right = false) {
		if (!ok || s.empty()) return;
		char key[64]; snprintf(key, sizeof key, "%d|%02x%02x%02x|", sm, c.r, c.g, c.b);
		std::string k = key + s; auto it = cache.find(k);
		if (it == cache.end()) {
			if (cache.size() > 400) { for (auto& e : cache) SDL_DestroyTexture(e.second.tex); cache.clear(); }
			SDL_Surface* su = TTF_RenderUTF8_Blended(sm ? small : big, s.c_str(), c); if (!su) return;
			Entry e{SDL_CreateTextureFromSurface(ren, su), su->w, su->h}; SDL_FreeSurface(su);
			it = cache.emplace(k, e).first;
		}
		const Entry& e = it->second; if (right) x -= e.w;
		SDL_SetTextureColorMod(e.tex, 0, 0, 0); SDL_Rect sh{x + 1, y + 1, e.w, e.h}; SDL_RenderCopy(ren, e.tex, nullptr, &sh); // drop shadow
		SDL_SetTextureColorMod(e.tex, 255, 255, 255); SDL_Rect d{x, y, e.w, e.h}; SDL_RenderCopy(ren, e.tex, nullptr, &d);
	}
	int line_h(bool sm) const { return TTF_FontHeight(sm ? small : big) + 2; }
	void text_center(int x, int y, const std::string& s, SDL_Color c, bool sm = false) {
		int w = 0, h = 0; if (ok) TTF_SizeUTF8(sm ? small : big, s.c_str(), &w, &h); text(x - w / 2, y, s, c, sm);
	}
};

// ---- in-game commands (live mode) ----
struct Commands {
	Session& s; Assets& assets; std::string msg; double msg_t = 0;
	void say(const std::string& m) { msg = m; msg_t = 3.5; }
	std::string name(const unit_t* u) { return assets.names().unit((int)u->unit_type->id); }
	std::string name(UnitTypes t) { return assets.names().unit((int)t); }
	Game& g() { return *s.game; }

	bool own(const unit_t* u) { return u && u->owner == g().owner(); }
	bool train(UnitTypes t) {
		unit_t* sel = g().selected(); if (!sel) { say("Nothing selected"); return false; }
		auto* ut = g().fn().get_unit_type(t);
		if (g().minerals() < ut->mineral_cost) { say("Not enough minerals"); return false; }
		if (g().gas() < ut->gas_cost) { say("Not enough gas"); return false; }
		if (g().net() && !g().fn().unit_can_build(sel, ut)) { say(name(sel) + " cannot train " + name(t) + " now (tech or queue full?)"); return false; }
		if (!g().train(t)) { say(name(sel) + " cannot train " + name(t) + " now (supply, tech or queue full?)"); return false; }
		say("Training " + name(t)); return true;
	}
	// Worker/combat training for the selected building (Zerg: uses the nearest larva of the selected hatchery).
	void train_worker() {
		unit_t* sel = g().selected(); if (!sel) { say("Nothing selected"); return; }
		switch (sel->unit_type->id) {
			case UnitTypes::Terran_Command_Center: train(UnitTypes::Terran_SCV); break;
			case UnitTypes::Protoss_Nexus: train(UnitTypes::Protoss_Probe); break;
			case UnitTypes::Zerg_Hatchery: case UnitTypes::Zerg_Lair: case UnitTypes::Zerg_Hive: select_larva(sel); train(UnitTypes::Zerg_Drone); break;
			case UnitTypes::Zerg_Larva: train(UnitTypes::Zerg_Drone); break;
			default: say(name(sel) + " does not train workers");
		}
	}
	void train_combat() {
		unit_t* sel = g().selected(); if (!sel) { say("Nothing selected"); return; }
		switch (sel->unit_type->id) {
			case UnitTypes::Terran_Barracks: train(UnitTypes::Terran_Marine); break;
			case UnitTypes::Protoss_Gateway: train(UnitTypes::Protoss_Zealot); break;
			case UnitTypes::Zerg_Hatchery: case UnitTypes::Zerg_Lair: case UnitTypes::Zerg_Hive: select_larva(sel); train(UnitTypes::Zerg_Zergling); break;
			case UnitTypes::Zerg_Larva: train(UnitTypes::Zerg_Zergling); break;
			default: say(name(sel) + " does not train combat units here");
		}
	}
	void select_larva(unit_t* base) {
		unit_t* l = g().nearest(base->sprite->position.x, base->sprite->position.y, [&](unit_t* u) { return u->owner == g().owner() && u->unit_type->id == UnitTypes::Zerg_Larva; });
		if (l) g().select(l);
	}
	// Supply structure at the ground point under the crosshair, built by the selected worker.
	void build_supply(const Pick& pk) {
		unit_t* sel = g().selected(); if (!sel) { say("Select a worker first"); return; }
		if (!pk.ground) { say("Aim at the floor to place a building"); return; }
		UnitTypes t; Orders o;
		switch (sel->unit_type->id) {
			case UnitTypes::Terran_SCV: t = UnitTypes::Terran_Supply_Depot; o = Orders::PlaceBuilding; break;
			case UnitTypes::Protoss_Probe: t = UnitTypes::Protoss_Pylon; o = Orders::PlaceProtossBuilding; break;
			default: say(name(sel) + " cannot build a supply structure here (only SCV and Probe are supported)"); return;
		}
		auto* ut = g().fn().get_unit_type(t);
		if (g().minerals() < ut->mineral_cost) { say("Not enough minerals for " + name(t)); return; }
		xy_t<size_t> tile{(size_t)(pk.gx / 32), (size_t)(pk.gy / 32)};
		xy pos{(int)tile.x * 32 + ut->placement_size.x / 2, (int)tile.y * 32 + ut->placement_size.y / 2};
		if (!g().fn().can_place_building(sel, g().owner(), ut, pos, false, false)) { say("Cannot build " + name(t) + " there"); return; }
		if (g().build(o, t, tile)) say("Building " + name(t)); else say("Cannot build " + name(t) + " there");
	}
};

// Scripted checks driven through the live SDL event loop: --selftest-input.
struct SelfTest {
	struct Step { double t; std::string what; std::function<void()> fn; };
	std::vector<Step> plan; size_t next = 0; double elapsed = 0;
	std::vector<std::pair<std::string, bool>> results;
	void check(const std::string& w, bool ok) { results.push_back({w, ok}); }
	void due(double dt) { elapsed += dt; while (next < plan.size() && elapsed >= plan[next].t) plan[next++].fn(); }
	bool finished() const { return next >= plan.size(); }
};
static void push_key(SDL_EventType t, SDL_Scancode sc, SDL_Keycode sym, uint16_t mod = 0) { SDL_Event ev{}; ev.type = t; ev.key.keysym.scancode = sc; ev.key.keysym.sym = sym; ev.key.keysym.mod = mod; SDL_PushEvent(&ev); }
static void push_button(Uint8 b) { SDL_Event ev{}; ev.type = SDL_MOUSEBUTTONDOWN; ev.button.button = b; SDL_PushEvent(&ev); }
static void push_button_up(Uint8 b) { SDL_Event ev{}; ev.type = SDL_MOUSEBUTTONUP; ev.button.button = b; SDL_PushEvent(&ev); }
static void push_motion(int xrel) { SDL_Event ev{}; ev.type = SDL_MOUSEMOTION; ev.motion.xrel = xrel; SDL_PushEvent(&ev); }

// Scripted run without a window. Collision is checked against OpenBW's own is_walkable(), independent of the
// renderer's cached grid, so a grid/simulation mismatch or a broken move() is caught.
static bool sim_can_stand(Session& s, double x, double y, double r) {
	for (int dy = -1; dy <= 1; dy += 2) for (int dx = -1; dx <= 1; dx += 2) {
		int px = (int)std::floor(x + dx * r), py = (int)std::floor(y + dy * r);
		if (px < 0 || py < 0 || px >= (int)s.r->map_w() || py >= (int)s.r->map_h() || !s.fn().is_walkable(bwgame::xy{px, py})) return false;
	}
	return true;
}

static int autotest(Session& s, Assets&, const Args& a) {
	int failures = 0; View v; s.r->update_units(); v.spawn(*s.r);
	const double sx = v.cam.x, sy = v.cam.y;
	std::cout << "spawn (" << sx << "," << sy << ") stands per simulation: " << sim_can_stand(s, sx, sy, v.radius) << "\n";
	if (!sim_can_stand(s, sx, sy, v.radius)) { std::cout << "FAIL: spawn is not standable\n"; ++failures; }

	// 1. Walls stop movement where the simulation says terrain becomes unwalkable (4 axis-aligned pushes).
	const char* names[4] = {"east", "south", "west", "north"};
	for (int d = 0; d < 4; ++d) {
		v.cam.x = sx; v.cam.y = sy; v.cam.heading = 90.0 * d;
		const double ux = std::cos(d * 1.5707963267948966), uy = std::sin(d * 1.5707963267948966);
		int tb = -1; for (int t = 0; t < 6000; ++t) if (!sim_can_stand(s, sx + std::round(ux) * t, sy + std::round(uy) * t, v.radius)) { tb = t; break; }
		for (int i = 0; i < 1500; ++i) v.move(*s.r, 6.0, 0.0);
		double travelled = std::fabs(v.cam.x - sx) + std::fabs(v.cam.y - sy);
		double expected = tb < 0 ? -1 : std::floor((tb - 1) / 6.0) * 6.0; // last 6-px step before the first blocked pixel
		bool ok = tb >= 0 && std::fabs(travelled - expected) < 1e-6;
		std::cout << "push " << names[d] << ": travelled " << travelled << " px, expected " << expected << " (first blocked at " << tb << ") " << (ok ? "OK" : "FAIL") << "\n";
		if (!ok) ++failures;
	}

	// 2. Wander with periodic turns and strafing; every position must be standable per the simulation.
	v.cam.x = sx; v.cam.y = sy; v.cam.heading = 0;
	int violations = 0; double path = 0, sum_ms = 0, max_ms = 0; std::vector<uint32_t> fb((size_t)a.rw * a.rh);
	for (int i = 0; i < 240; ++i) {
		v.cam.heading = std::fmod(v.cam.heading + (i % 40 == 39 ? 97 : 2.0), 360.0);
		double ox = v.cam.x, oy = v.cam.y; v.move(*s.r, 5.0, (i % 5 == 0) ? 3.0 : 0.0);
		path += std::hypot(v.cam.x - ox, v.cam.y - oy);
		if (!sim_can_stand(s, v.cam.x, v.cam.y, v.radius)) ++violations;
		if (i % 3 == 0 && !s.done()) s.step();
		s.r->update_units();
		auto t0 = Clock::now(); s.r->render(v.cam, nullptr, fb.data());
		double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); sum_ms += ms; max_ms = std::max(max_ms, ms);
	}
	std::cout << "wander: path " << path << " px, collision violations " << violations << "\n";
	if (violations) ++failures;
	std::cout << "render ms: avg " << sum_ms / 240 << " max " << max_ms << " at " << a.rw << "x" << a.rh << ", sim frame " << s.frame() << "\n";
	if (!a.screenshot.empty()) { save_ppm(a.screenshot, fb, a.rw, a.rh); std::cout << "wrote " << a.screenshot << "\n"; }
	std::cout << (failures ? "AUTOTEST FAILED" : "AUTOTEST PASSED") << "\n";
	return failures ? 1 : 0;
}

int main(int argc, char** argv) {
	Args a;
	for (int i = 1; i < argc; ++i) {
		std::string k = argv[i];
		auto next = [&]() -> const char* { if (i + 1 >= argc) die("missing value"); return argv[++i]; };
		if (k == "--data") a.data = next();
		else if (k == "--replay") { a.input = next(); a.replay = true; }
		else if (k == "--map") { a.input = next(); a.replay = false; }
		else if (k == "--game") a.game_map = next();
		else if (k == "--fps") a.fps = true;
		else if (k == "--fire-rate") a.fire_rate = atoi(next());
		else if (k == "--range") a.range_pct = atoi(next());
		else if (k == "--speed") a.speed_pct = atoi(next());
		else if (k == "--fps-move-shoot") a.direct = true;
		else if (k == "--no-vsync") a.no_vsync = true;
		else if (k == "--weapons") a.weapons = next();
		else if (k == "--race") a.race = parse_race(next());
		else if (k == "--enemy") a.enemy = parse_race(next());
		else if (k == "--res") sscanf(next(), "%dx%d", &a.rw, &a.rh);
		else if (k == "--window") sscanf(next(), "%dx%d", &a.ww, &a.wh);
		else if (k == "--sim-speed") a.speed = atof(next());
		else if (k == "--start-frame") a.start_frame = atoi(next());
		else if (k == "--fullscreen") a.fullscreen = true;
		else if (k == "--autotest") a.autotest = true;
		else if (k == "--no-capture") a.no_capture = true;
		else if (k == "--selftest-input") { a.selftest_input = true; a.no_capture = true; }
		else if (k == "--screenshot") a.screenshot = next();
		else if (k == "--view-dist") a.rp.view_dist = atof(next());
		else if (k == "--models") a.models = next();
		else if (k == "--model-units") a.model_units = true;
		else if (k == "--eye") a.rp.eye_h = atof(next());
		else if (k == "--level-h") a.rp.level_h = atof(next());
		else if (k == "--host") { a.net = true; a.netopt.host = true; // optional port
			if (i + 1 < argc && argv[i + 1][0] && std::all_of(argv[i + 1], argv[i + 1] + strlen(argv[i + 1]), [](char c) { return std::isdigit((unsigned char)c) != 0; })) a.netopt.port = atoi(argv[++i]); }
		else if (k == "--join") { a.net = true; a.netopt.host = false; std::string hp = next(); size_t c = hp.rfind(':');
			a.netopt.address = hp.substr(0, c); if (c != std::string::npos) a.netopt.port = atoi(hp.c_str() + c + 1); }
		else if (k == "--latency") a.netopt.latency = atoi(next());
		else if (k == "--name") a.netopt.name = next();
		else die("unknown option");
	}
	if (a.fps && a.game_map.empty()) die("--fps needs --game MAP");
	if (a.fire_rate < 10 || a.fire_rate > 1000) die("--fire-rate must be 10..1000 (percent of the StarCraft fire rate)");
	if (a.range_pct < 10 || a.range_pct > 1000) die("--range must be 10..1000 (percent of the StarCraft weapon range)");
	if (a.speed_pct < 10 || a.speed_pct > 1000) die("--speed must be 10..1000 (percent of the StarCraft movement speed)");
	if (a.speed < 0.125 || a.speed > 16) die("--sim-speed must be 0.125..16 (the = / - keys use the same range)");
	if (a.rp.view_dist < 200 || a.rp.eye_h < 1 || a.rp.level_h < 8) die("--view-dist must be >= 200, --eye >= 1, --level-h >= 8 (px)");
	if (a.net) {
		if (a.game_map.empty()) die("--host / --join need --game MAP (the same map file on both computers)");
		if (a.autotest) die("--autotest is single player only");
		if (a.netopt.latency < 1 || a.netopt.latency > 8) die("--latency must be 1..8 frames");
		if (a.netopt.port < 1 || a.netopt.port > 65535 || a.netopt.address.empty()) die("--host [PORT] / --join HOST[:PORT]: port 1..65535");
		a.fps = true; a.speed = 1.0; a.netopt.wait_seconds = a.netopt.host ? 0 : 60; // the host waits for a player; a joiner retries for a minute
		if (a.netopt.name == "player") if (const char* u = getenv("USER")) if (*u) a.netopt.name = u;
	}
	if (a.data.empty() || (a.input.empty() && a.game_map.empty())) die("usage: fp_play --data DIR (--replay FILE | --game MAP [--race R] [--enemy R] [--host [PORT] | --join HOST[:PORT] [--latency N] [--name NAME]] [--fps [--weapons MOD_DIR] [--fps-move-shoot] [--fire-rate N] [--range N] [--speed N]] | --map FILE) [--res WxH] [--window WxH] [--sim-speed X] [--start-frame N] [--fullscreen] [--no-capture] [--no-vsync] [--view-dist PX] [--eye PX] [--level-h PX] [--models DIR [--model-units]] [--selftest-input] [--autotest [--screenshot F.ppm]]");
	try {
		Assets assets(a.data);
		std::unique_ptr<ModelLibrary> models;
		if (!a.models.empty()) { models = std::make_unique<ModelLibrary>(a.models); if (!models->ok()) die("--models: no files under that directory"); }
		Session s; s.models = models.get(); s.model_mode = a.model_units ? Renderer::kModelsUnits : Renderer::kModelsBuildings; s.open(assets, a);
		if (a.autotest) return autotest(s, assets, a);

		if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::cerr << "SDL_Init: " << SDL_GetError() << "\n"; return 1; }
		SDL_Window* win = SDL_CreateWindow("StarCraft FP", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, a.ww, a.wh, SDL_WINDOW_RESIZABLE | (a.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
		if (!win) { std::cerr << "SDL_CreateWindow: " << SDL_GetError() << "\n"; return 1; }
		SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (a.no_vsync ? 0 : SDL_RENDERER_PRESENTVSYNC));
		if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
		if (!ren) { std::cerr << "SDL_CreateRenderer: " << SDL_GetError() << "\n"; return 1; }
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
		SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, a.rw, a.rh);
		if (!a.no_capture) SDL_SetRelativeMouseMode(SDL_TRUE);
		Hud hud; { int ww, wh; SDL_GetRendererOutputSize(ren, &ww, &wh); hud.init(ren, wh); }
		Audio audio; ViewModel vm; bool have_audio = false;
		if (a.fps) {
			have_audio = audio.open(); if (!have_audio) std::cerr << "audio: no output device; continuing silently\n";
			if (a.weapons.empty()) std::cerr << "weapons: no --weapons MOD_DIR given; no gun is drawn\n";
			else if (!vm.load(ren, a.weapons)) std::cerr << "weapons: could not load the Boltgun frames/sounds from " << a.weapons << "; no gun is drawn\n";
		}

		View v; v.spawn(*s.r);
		Commands cmd{s, assets};
		auto owner_workers = [&](const UnitView& u) { return s.live() && u.owner == s.game->owner() && (u.type == (int)UnitTypes::Terran_SCV || u.type == (int)UnitTypes::Protoss_Probe || u.type == (int)UnitTypes::Zerg_Drone); };
		auto begin_game_view = [&]() { // live mode: possess the first worker (FPS: the hero), facing the map centre
			if (!s.live()) return;
			if (s.fps) { unit_t* h = s.fps->player(); if (h) { v.follow = h; s.game->select(h); v.cam.x = h->sprite->position.x; v.cam.y = h->sprite->position.y;
				xy e = s.game->start_location(s.game->enemy()); v.cam.heading = std::atan2(e.y - v.cam.y, e.x - v.cam.x) * 180.0 / 3.14159265358979323846; } return; }
			for (auto& u : s.r->units()) if (owner_workers(u)) { v.follow = u.ptr; s.game->select(const_cast<unit_t*>(u.ptr)); v.cam.x = u.x; v.cam.y = u.y;
				v.cam.heading = std::atan2(s.r->map_h() / 2 - u.y, s.r->map_w() / 2 - u.x) * 180.0 / 3.14159265358979323846; break; }
		};
		begin_game_view();

		std::vector<uint32_t> fb((size_t)a.rw * a.rh);
		bool run = true, paused = false, captured = !a.no_capture; bool keys[SDL_NUM_SCANCODES] = {};
		double speed = a.speed, acc = 0, fps = 0, render_ms = 0, title_t = 0;
		const double sim_dt = 1.0 / 23.81; // frames per second at "fastest" game speed
		auto last = Clock::now(); int shots = 0;
		Pick pick; Overlay ov; bool moving = false; int last_move_order = -100;
		bool firing = false, fire_ready = false, enemy_aimed = false; int last_shots = 0, last_hp = -1, pain_plays = 0, death_plays = 0; double flash = 0, pain_cd = 0; bool dead_seen = false;
		// Multiplayer: commanding the base (free camera, game-mode controls) instead of playing the hero; seconds spent
		// waiting for the other player's game; the hero last followed (a respawned hero is a new unit).
		bool commander = false; double stalled = 0; int last_hero_id = -1;
		auto set_commander = [&](bool on) {
			if (!s.fps || on == commander) return;
			commander = on; firing = false; moving = false;
			if (on) { v.follow = nullptr; if (s.fps->direct()) s.fps->halt(); s.fps->set_trigger_mode(Fps::kTriggerFree); cmd.say("Commanding your base: F back to your hero (it fights on its own meanwhile)"); }
			else { s.fps->set_trigger_mode(Fps::kTriggerUp); if (unit_t* h = s.fps->player()) { v.follow = h; s.game->select(h); } cmd.say("Back in your hero"); }
		};
		SelfTest st; bool live_test = a.selftest_input && s.live() && !s.fps, fps_test = a.selftest_input && s.fps && !s.net(), net_test = a.selftest_input && s.net();

		// ---- scripted selftest plans ----
		double st_x0 = v.cam.x, st_y0 = v.cam.y, st_h0 = v.cam.heading; int st_pause_f = 0, st_before_restart = 0; bool st_paused_at_check = false;
		if (a.selftest_input && !s.live()) {
			auto K = [&](double t, const char* w, std::function<void()> f) { st.plan.push_back({t, w, f}); };
			K(0.1, "note start", [&] { st_x0 = v.cam.x; st_y0 = v.cam.y; });
			K(0.6, "walk", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_W, SDLK_w); });
			K(1.5, "check walk", [&] { double d = std::hypot(v.cam.x - st_x0, v.cam.y - st_y0); st.check("W moves the free camera (" + std::to_string((int)d) + " px)", d > 20); });
			K(1.6, "look", [&] { st_h0 = v.cam.heading; push_motion(200); });
			K(1.9, "check look", [&] { double dh = std::fmod(v.cam.heading - st_h0 + 360.0, 360.0); st.check("mouse motion turns the camera by 30 degrees (got " + std::to_string(dh) + ")", std::fabs(dh - 30.0) < 1.0); });
			K(2.2, "stop", [&] { push_key(SDL_KEYUP, SDL_SCANCODE_W, SDLK_w); });
			K(2.4, "follow", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_TAB, SDLK_TAB); });
			K(2.7, "check follow", [&] { st.check("TAB follows a unit", v.follow != nullptr); });
			K(2.8, "shot", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_F12, SDLK_F12); });
			K(3.0, "pause", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_P, SDLK_p); });
			K(3.2, "note pause frame", [&] { st_pause_f = s.frame(); st_paused_at_check = paused; });
			K(3.7, "check pause", [&] { st.check("P pauses the simulation", st_paused_at_check && s.frame() == st_pause_f); push_key(SDL_KEYDOWN, SDL_SCANCODE_P, SDLK_p); });
			K(3.8, "restart", [&] { st_before_restart = s.frame(); push_key(SDL_KEYDOWN, SDL_SCANCODE_R, SDLK_r); });
			K(4.1, "check restart", [&] { st.check("R restarts the replay", s.frame() < st_before_restart && s.frame() >= a.start_frame && s.frame() < a.start_frame + 40);
				FILE* f = fopen("fp_shot_000.ppm", "rb"); st.check("F12 wrote a screenshot", f != nullptr); if (f) fclose(f); });
			K(4.3, "quit", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_Q, SDLK_q); });
		}
		// Helpers for the live-game plan live in this scope, not inside the if-block: the plan's lambdas run later in the main loop.
		auto G = [&]() -> Game& { return *s.game; };
		auto F_ = [&]() -> Fps& { return *s.fps; };
		auto aim_at = [&](const unit_t* u, double dist) { // teleport the free camera to a clear spot `dist` px from u, looking at it
			v.follow = nullptr; double ux = u->sprite->position.x, uy = u->sprite->position.y;
			for (int k = 0; k < 16; ++k) { double ang = k * 3.14159265358979 / 8, cx = ux + std::cos(ang) * dist, cy = uy + std::sin(ang) * dist;
				if (!s.r->can_stand(cx, cy, 6)) continue;
				v.cam.x = cx; v.cam.y = cy; v.cam.heading = std::atan2(uy - cy, ux - cx) * 180.0 / 3.14159265358979; v.cam.pitch = 0; return true; }
			return false; };
		auto aim_ground = [&](double dist) { v.cam.pitch = -(s.r->params().eye_h * (a.rw / 2.0 / std::tan(s.r->params().fov * 3.14159265358979 / 360.0))) / dist; };
		auto own_of = [&](UnitTypes t) { return G().find([&](unit_t* u) { return u->owner == G().owner() && u->unit_type->id == t; }); };
		// Camera spot + floor target for the live tests: terrain clear between them, no unit near the target or the line,
		// and (for building) a placement OpenBW itself accepts. Sets the camera and returns the target in gx/gy.
		auto aim_open = [&](const unit_t* actor, double dist, const unit_type_t* build_type, double& gx, double& gy) {
			v.follow = nullptr; double ux = actor->sprite->position.x, uy = actor->sprite->position.y;
			for (int ring = 0; ring < 3; ++ring) for (int k = 0; k < 24; ++k) {
				double ang = k * 3.14159265358979 / 12, cd = 30 + ring * 40, cx = ux + std::cos(ang) * cd, cy = uy + std::sin(ang) * cd;
				double tx = cx + std::cos(ang) * dist, ty = cy + std::sin(ang) * dist;
				if (!s.r->can_stand(cx, cy, 6) || !s.r->can_stand(tx, ty, 6)) continue;
				bool clear = true; for (double t = 0; t <= dist && clear; t += 8) clear = s.r->can_stand(cx + std::cos(ang) * t, cy + std::sin(ang) * t, 6);
				for (auto& un : s.r->units()) { if (un.ptr == actor) continue; // keep units away from the line of sight, the target and the camera
					double dx = un.x - tx, dy = un.y - ty; if (std::hypot(dx, dy) < 90 + un.r) clear = false;
					for (double t = 0; t <= dist && clear; t += 16) if (std::hypot(un.x - (cx + std::cos(ang) * t), un.y - (cy + std::sin(ang) * t)) < 40 + un.r) clear = false;
					if (std::hypot(un.x - cx, un.y - cy) < 40 + un.r) clear = false; }
				if (!clear) continue;
				if (build_type) { xy pos{(int)(tx / 32) * 32 + build_type->placement_size.x / 2, (int)(ty / 32) * 32 + build_type->placement_size.y / 2};
					if (!s.fn().can_place_building(actor, actor->owner, build_type, pos, false, false)) continue; }
				v.cam.x = cx; v.cam.y = cy; v.cam.heading = ang * 180.0 / 3.14159265358979; aim_ground(dist); gx = tx; gy = ty; return true; }
			return false; };
		if (live_test) { // live-game plan: aim, select, order, train, possess, build, all through real input events
			speed = 4.0;
			auto K = [&](double t, const char* w, std::function<void()> f) { st.plan.push_back({t, w, f}); };
			static unit_t* cc = nullptr; static unit_t* w = nullptr; static double gx = 0, gy = 0; static int minerals0 = 0, depots0 = 0;
			K(0.5, "aim at the Command Center", [&] { cc = own_of(UnitTypes::Terran_Command_Center); st.check("camera placed in front of the Command Center", cc && aim_at(cc, 200)); });
			K(1.0, "LMB", [&] { push_button(SDL_BUTTON_LEFT); });
			K(1.4, "check select CC", [&] { st.check("left-click selects the unit under the crosshair (Command Center)", G().selected() == cc && cc != nullptr); });
			K(1.5, "train", [&] { minerals0 = G().minerals(); push_key(SDL_KEYDOWN, SDL_SCANCODE_T, SDLK_t); });
			K(1.9, "check train", [&] { st.check("T trains an SCV (minerals " + std::to_string(minerals0) + " -> " + std::to_string(G().minerals()) + ")", G().minerals() == minerals0 - 50); });
			K(2.0, "aim at an SCV", [&] { w = own_of(UnitTypes::Terran_SCV); st.check("camera placed in front of an SCV", w && aim_at(w, 150)); });
			K(2.4, "LMB", [&] { push_button(SDL_BUTTON_LEFT); });
			K(2.8, "check select SCV", [&] { st.check("left-click selects an SCV", G().selected() != nullptr && G().selected()->unit_type->id == UnitTypes::Terran_SCV); w = G().selected(); });
			K(2.9, "aim at floor", [&] { st.check("found a clear, reachable floor point to aim at", aim_open(w, 130, nullptr, gx, gy)); });
			K(3.3, "RMB", [&] { std::cout << "  [diag] at RMB: pick.unit=" << (pick.unit ? cmd.name(pick.unit) : std::string("none")) << " pick.ground=" << pick.ground << " (" << (int)pick.gx << "," << (int)pick.gy << ") aimed (" << (int)gx << "," << (int)gy << ") SCV at (" << w->sprite->position.x << "," << w->sprite->position.y << ")\n"; push_button(SDL_BUTTON_RIGHT); });
			K(5.5, "check move", [&] { double d = std::hypot(w->sprite->position.x - gx, w->sprite->position.y - gy); st.check("right-click on the floor moves the SCV there (ended " + std::to_string((int)d) + " px from the aimed point)", d < 40); });
			K(5.6, "possess", [&] { G().select(w); push_key(SDL_KEYDOWN, SDL_SCANCODE_E, SDLK_e); });
			K(5.9, "check possess", [&] { st.check("E possesses the selected unit", v.follow == w); st_x0 = w->sprite->position.x; st_y0 = w->sprite->position.y; });
			K(6.0, "W down", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_W, SDLK_w); });
			K(7.2, "W up", [&] { push_key(SDL_KEYUP, SDL_SCANCODE_W, SDLK_w); });
			K(7.6, "check drive", [&] { double d = std::hypot(w->sprite->position.x - st_x0, w->sprite->position.y - st_y0); st.check("W drives the possessed SCV (moved " + std::to_string((int)d) + " px)", d > 40); });
			K(8.2, "check stopped", [&] { static double px = w->sprite->position.x, py = w->sprite->position.y; (void)px; (void)py; st.check(s.fps && s.fps->direct() ? "releasing W stops it (velocity " + std::to_string((int)F_().speed()) + ")" : "releasing W stops it (order id " + std::to_string((int)w->order_type->id) + ")",
					s.fps && s.fps->direct() ? F_().speed() < 2.0 : (w->order_type->id == Orders::Stop || w->order_type->id == Orders::Nothing || w->order_type->id == Orders::PlayerGuard)); });
			K(8.3, "build", [&] { G().st().current_minerals[G().owner()] = 500; depots0 = G().count([&](unit_t* u) { return u->owner == G().owner() && u->unit_type->id == UnitTypes::Terran_Supply_Depot; });
				G().select(w); st.check("found a valid Supply Depot site to aim at", aim_open(w, 110, G().fn().get_unit_type(UnitTypes::Terran_Supply_Depot), gx, gy)); });
			K(8.5, "B", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_B, SDLK_b); });
			K(8.9, "diag build", [&] { std::cout << "  [diag] after B: msg=\"" << cmd.msg << "\" minerals " << G().minerals() << " SCV order " << (int)w->order_type->id << " at (" << w->sprite->position.x << "," << w->sprite->position.y << ")\n"; });
			K(14.0, "check build", [&] { int n = G().count([&](unit_t* u) { return u->owner == G().owner() && u->unit_type->id == UnitTypes::Terran_Supply_Depot; }); st.check("B places a Supply Depot (" + std::to_string(depots0) + " -> " + std::to_string(n) + ")", n == depots0 + 1); });
			K(14.1, "quit", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_Q, SDLK_q); });
		}

		// --screenshot during the FPS self-test: frames at named steps (defined here: the plan's lambdas run after the block below)
		auto shot = [&](const char* tag) { if (!a.screenshot.empty()) { std::string f = a.screenshot; size_t dot = f.rfind('.'); f.insert(dot == std::string::npos ? f.size() : dot, tag); save_ppm(f, fb, a.rw, a.rh); } };
		if (fps_test) { // FPS plan: aim, hold fire, feedback (animation, sound, pain), death and restart, all through real input events
			speed = 4.0;
			auto K = [&](double t, const char* w, std::function<void()> f) { st.plan.push_back({t, w, f}); };
			static unit_id zid{}; static int hp_before = 0; static bool vm_ok = false; (void)hp_before;
			K(0.5, "spawn an enemy in front", [&] { unit_t* h = F_().player(); const double PI = 3.14159265358979323846, r = v.cam.heading * PI / 180; unit_t* z = nullptr;
				for (double d : {100.0, 90.0, 110.0, 80.0}) { if (z) break; z = G().spawn(UnitTypes::Zerg_Zergling, xy{(int)(h->sprite->position.x + std::cos(r) * d), (int)(h->sprite->position.y + std::sin(r) * d)}, G().enemy()); }
				if (z) zid = G().fn().get_unit_id(z); vm_ok = vm.loaded(); st.check("spawned a Zergling in front of the hero", z != nullptr); });
			K(1.2, "check crosshair", [&] { unit_t* z = G().fn().get_unit(zid); shot("_crosshair");
				st.check("crosshair is green on an enemy in weapon range", ov.crosshair == 0xFF40FF40);
				st.check("trigger up: the idle hero does not auto-fire", z && F_().shots == 0 && (int)z->hp.integer_part() == (int)z->unit_type->hitpoints.integer_part()); });
			K(1.3, "LMB down", [&] { push_button(SDL_BUTTON_LEFT); });
			K(3.0, "check kill", [&] { st.check("holding the left button kills the enemy under the crosshair (" + std::to_string(F_().shots) + " shots)", G().fn().get_unit(zid) == nullptr && F_().shots >= 1);
				if (vm_ok) st.check("every shot started the gun animation (" + std::to_string(vm.shots) + " of " + std::to_string(F_().shots) + ")", vm.shots == F_().shots);
				if (vm_ok && have_audio) st.check("every shot played the fire sound and the extra layer (" + std::to_string(audio.started()) + " voices)", audio.started() >= 2 * F_().shots); });
			K(3.1, "LMB up", [&] { push_button_up(SDL_BUTTON_LEFT); });
			static unit_id farid{}; static int far_hp0 = 0;
			K(3.2, "spawn a far enemy", [&] { unit_t* h = F_().player(); if (!h) { st.check(std::string("hero alive to spawn the far enemy"), false); return; }
				const double PI = 3.14159265358979323846, r = v.cam.heading * PI / 180; unit_t* z = nullptr;
				for (double d : {450.0, 430.0, 470.0}) { if (z) break; z = G().spawn(UnitTypes::Zerg_Zergling, xy{(int)(h->sprite->position.x + std::cos(r) * d), (int)(h->sprite->position.y + std::sin(r) * d)}, G().enemy()); }
				if (z) { farid = G().fn().get_unit_id(z); far_hp0 = (int)z->hp.integer_part(); }
				st.check("spawned a Zergling beyond weapon range", z != nullptr);
				if (vm_ok && have_audio) st.check("being hit played a pain voice (" + std::to_string(pain_plays) + ")", pain_plays >= 1); });
			K(3.5, "check red crosshair", [&] { st.check("crosshair is red on an enemy out of range", ov.crosshair == 0xFFFF3030); });
			K(3.6, "LMB down (miss)", [&] { push_button(SDL_BUTTON_LEFT); });
			K(5.2, "check miss", [&] { unit_t* z = G().fn().get_unit(farid);
				st.check("firing out of range takes miss shots (" + std::to_string(F_().misses) + ")", F_().misses >= 1);
				st.check("the far enemy is unhurt (HP " + std::to_string(far_hp0) + " -> " + std::to_string(z ? (int)z->hp.integer_part() : 0) + ")", z && (int)z->hp.integer_part() == far_hp0); });
			K(5.3, "LMB up (miss)", [&] { push_button_up(SDL_BUTTON_LEFT); });
			K(5.35, "screenshot", [&] { shot("_fight"); });
			K(5.4, "overwhelm", [&] { unit_t* h = F_().player(); if (!h) return; for (int i = 0; i < 40; ++i) { unit_t* z = G().spawn(UnitTypes::Zerg_Zergling, xy{h->sprite->position.x + 20 + (i % 8) * 6, h->sprite->position.y + 22 + (i / 8) * 6}, G().enemy()); if (z) G().attack_as(G().enemy(), z, h); } });
			K(7.0, "check death", [&] { st.check("the hero dies and the game notices", !F_().alive() && dead_seen);
				if (vm_ok && have_audio) st.check("death played a voice line (" + std::to_string(death_plays) + ")", death_plays == 1); });
			K(7.1, "restart", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_R, SDLK_r); });
			K(7.6, "check restart", [&] { st.check("R restarts: a fresh hero at full HP (" + std::to_string(F_().player_hp()) + "/" + std::to_string(F_().player_max_hp()) + ")", F_().alive() && F_().player_hp() == F_().player_max_hp() && !dead_seen && v.follow != nullptr); });
			static int mode0 = -1;
			K(8.3, "M", [&] { mode0 = s.r->model_mode(); push_key(SDL_KEYDOWN, SDL_SCANCODE_M, SDLK_m); });
			K(8.5, "check M", [&] { if (s.models) st.check("M cycles the 3D models (" + std::to_string(mode0) + " -> " + std::to_string(s.r->model_mode()) + ")", s.r->model_mode() == (mode0 + 1) % 3);
			                         else st.check("M without models says so", cmd.msg.find("No models") != std::string::npos); });
			K(8.7, "quit", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_Q, SDLK_q); });
		}

		if (net_test) { // multiplayer plan, run by both players' games (start both with --selftest-input)
			auto K = [&](double t, const char* w, std::function<void()> f) { st.plan.push_back({t, w, f}); };
			K(2.5, "check lockstep", [&] { st.check("the game runs in lockstep with the other player's (frame " + std::to_string(s.frame()) + ", " + (s.game->failure().empty() ? std::string("no failure") : s.game->failure()) + ")", s.frame() > 30 && s.game->failure().empty() && !s.game->opponent_left()); });
			K(2.6, "check hero", [&] { st.check("your hero is in the game and the camera is on it", s.fps->alive() && v.follow == s.fps->player()); });
			K(2.7, "check fog", [&] { int seen = 0; for (auto& u : s.r->units()) if (u.owner == s.game->enemy()) ++seen;
				st.check("the other player's base, far away in the fog, is not drawn (" + std::to_string(seen) + " of their units drawn)", seen == 0); });
			K(2.8, "F", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_F, SDLK_f); });
			K(3.1, "check commander", [&] { st.check("F steps out of the hero to command the base (free camera)", commander && v.follow == nullptr); });
			K(3.2, "F", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_F, SDLK_f); });
			K(3.5, "check hero view", [&] { st.check("F again: back in the hero", !commander && v.follow == s.fps->player() && s.fps->player() != nullptr); });
			K(3.6, "P", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_P, SDLK_p); });
			K(3.9, "check no pause", [&] { st.check("P does not pause a multiplayer game", !paused && cmd.msg.find("No pause") != std::string::npos); });
			if (s.fps->direct()) { // free movement: the hero walks on W, the camera with it (ahead of the simulation by the latency)
				static double x0 = 0, y0 = 0;
				K(4.0, "W down", [&] { auto at = s.fps->mover_position(s.game->owner()); x0 = at.first; y0 = at.second; push_key(SDL_KEYDOWN, SDL_SCANCODE_W, SDLK_w); });
				K(4.6, "check walk", [&] { auto at = s.fps->mover_position(s.game->owner()); double d = std::hypot(at.first - x0, at.second - y0), lead = std::hypot(v.cam.x - at.first, v.cam.y - at.second);
					st.check("W walks the hero (free movement: " + std::to_string((int)d) + " px)", d > 30);
					st.check("the camera is on the hero, at most a few frames ahead (" + std::to_string((int)lead) + " px)", lead <= 4 * Fps::kTopSpeed / 256.0); });
				K(4.7, "W up", [&] { push_key(SDL_KEYUP, SDL_SCANCODE_W, SDLK_w); });
				K(5.7, "check rest", [&] { /* friction stops it in about 0.6 s, plus the latency */ double sp = s.fps->speed(), lead = 0; auto at = s.fps->mover_position(s.game->owner()); lead = std::hypot(v.cam.x - at.first, v.cam.y - at.second);
					st.check("releasing W stops the hero and the camera on it (" + std::to_string((int)sp) + " px/s, camera " + std::to_string((int)lead) + " px off)", sp < 2 && lead < 1); });
			}
			K(5.9, "check sync", [&] { st.check("still in sync with the other game at frame " + std::to_string(s.frame()) + " (state hashes compared every frame)", s.game->failure().empty() && s.game->desync_frame() < 0); });
			K(6.1, "quit", [&] { push_key(SDL_KEYDOWN, SDL_SCANCODE_Q, SDLK_q); });
		}

		while (run) {
			auto now = Clock::now(); double dt = std::min(0.1, std::chrono::duration<double>(now - last).count()); last = now;
			if (a.selftest_input) st.due(dt);
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				// selftest: only the scripted (windowID 0) input counts; real keyboard/mouse activity is ignored
				if (a.selftest_input && ((e.type == SDL_MOUSEMOTION && e.motion.windowID != 0) || (e.type == SDL_MOUSEBUTTONDOWN && e.button.windowID != 0) || ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && e.key.windowID != 0))) continue;
				const bool input_ok = captured || a.selftest_input;
				if (e.type == SDL_QUIT) run = false;
				else if (e.type == SDL_MOUSEMOTION && input_ok) { v.cam.heading = std::fmod(v.cam.heading + e.motion.xrel * 0.15 + 360.0, 360.0); v.cam.pitch = std::max(-kMaxPitch * a.rh, std::min(kMaxPitch * a.rh, v.cam.pitch - e.motion.yrel * 0.8)); }
				else if (e.type == SDL_MOUSEBUTTONUP && s.fps && e.button.button == SDL_BUTTON_LEFT) { firing = false; }
				else if (e.type == SDL_MOUSEBUTTONDOWN && input_ok && s.fps && !commander && e.button.button == SDL_BUTTON_LEFT) { firing = true; }
				else if (e.type == SDL_MOUSEBUTTONDOWN && input_ok && s.fps && s.fps->direct() && !commander && e.button.button == SDL_BUTTON_RIGHT) {
					if (pick.ground) s.fps->go_to(pick.gx, pick.gy); else cmd.say("Aim at the floor");
				}
				else if (e.type == SDL_MOUSEBUTTONDOWN && input_ok && s.live()) {
					Game& G = *s.game; bool shift = SDL_GetModState() & KMOD_SHIFT;
					if (e.button.button == SDL_BUTTON_LEFT) {
						if (pick.unit) { unit_t* u = const_cast<unit_t*>(pick.unit); if (shift && cmd.own(u)) G.select_add(u); else G.select(u); }
					} else if (e.button.button == SDL_BUTTON_RIGHT) {
						if (!G.selected()) cmd.say("Select a unit first (left-click)");
						else if (pick.unit) G.smart_order(pick.unit->sprite->position, const_cast<unit_t*>(pick.unit), shift);
						else if (pick.ground) G.smart_order(xy{(int)pick.gx, (int)pick.gy}, nullptr, shift);
						else cmd.say("Aim at a unit or the floor");
					}
				}
				else if (e.type == SDL_KEYUP) { keys[e.key.keysym.scancode & (SDL_NUM_SCANCODES - 1)] = false; }
				else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
					keys[e.key.keysym.scancode & (SDL_NUM_SCANCODES - 1)] = true;
					switch (e.key.keysym.sym) {
						case SDLK_q: run = false; break;
						case SDLK_ESCAPE: captured = !captured; SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE); break;
						case SDLK_p: if (s.net()) cmd.say("No pause in a multiplayer game"); else paused = !paused; break;
						case SDLK_EQUALS: case SDLK_PLUS: if (s.net()) cmd.say("The game speed is fixed in a multiplayer game"); else speed = std::min(16.0, speed * 2); break;
						case SDLK_MINUS: if (s.net()) cmd.say("The game speed is fixed in a multiplayer game"); else speed = std::max(0.125, speed / 2); break;
						case SDLK_TAB: if (s.net()) set_commander(true); v.cycle_follow(*s.r, s.live() ? s.game->owner() : -1); if (s.live() && v.follow) s.game->select(const_cast<unit_t*>(v.follow)); break;
						case SDLK_f: if (s.net()) set_commander(!commander); else v.follow = nullptr; break;
						case SDLK_r: if (s.net()) { cmd.say("No restart in a multiplayer game"); break; } s.open(assets, a); pick = Pick{}; v.follow = nullptr; /* the old game's units are gone: drop pointers into it */ v.spawn(*s.r); begin_game_view(); acc = 0; moving = false; firing = false; last_shots = 0; last_hp = -1; dead_seen = false; flash = 0; break;
						case SDLK_F12: save_ppm(bwgame::format("fp_shot_%03d.ppm", shots++), fb, a.rw, a.rh); break;
						case SDLK_m: if (s.models) { // cycle sprites only -> 3D buildings -> 3D buildings and units
							static const char* names[3] = {"Models: off (sprites only)", "Models: buildings and minerals", "Models: buildings and units (experimental, not animated)"};
							s.model_mode = (s.model_mode + 1) % 3; s.r->set_model_mode(s.model_mode); s.r->update_units(); cmd.say(names[s.model_mode]);
						} else cmd.say("No models loaded (run with --models DIR)"); break;
						case SDLK_e: if (s.live()) { // possess the selected unit / release
							if (s.net() && !commander) { cmd.say("Press F to command your base"); break; }
							if (s.net() && !v.follow && s.fps->player() && s.game->selected() == s.fps->player()) { set_commander(false); break; } // your hero: back into it
							if (v.follow) { v.follow = nullptr; cmd.say("Free camera"); }
							else if (unit_t* sel = s.game->selected()) { if (cmd.own(sel)) { v.follow = sel; cmd.say("Possessing " + cmd.name(sel)); } else cmd.say("You can only possess your own units"); }
							else cmd.say("Select one of your units first"); } break;
						case SDLK_t: if (s.live()) cmd.train_worker(); break;
						case SDLK_y: if (s.live()) cmd.train_combat(); break;
						case SDLK_x: if (s.live()) { s.game->stop(); moving = false; } break;
						case SDLK_b: if (s.live()) cmd.build_supply(pick); break;
					}
				}
			}
			if (s.fps && !commander) s.fps->set_trigger(firing && fire_ready); // the weapon fires only while the trigger is held on a target in range
			// simulation
			bool waiting = false; // multiplayer: the other player's game has not caught up yet (lockstep)
			if (!paused && !s.done()) { acc += dt * speed; int n = 0; while (acc >= sim_dt && n < 16 && !s.done()) { if (!s.step()) { waiting = true; acc = std::min(acc, 2 * sim_dt); break; } acc -= sim_dt; ++n; } if (n == 16) acc = 0; }
			stalled = waiting ? stalled + dt : 0;
			s.r->update_units();
			// FPS: firing, feedback events
			if (s.fps) {
				Fps& F = *s.fps; unit_t* hero = F.player();
				int hero_id = hero ? (int)s.game->fn().get_unit_id(hero).raw_value : 0;
				if (s.net() && hero_id != last_hero_id) { // a new hero (respawned): select it so RMB orders it
					last_hero_id = hero_id; if (hero) { dead_seen = false; if (!commander) s.game->select(hero); } }
				if (hero && !commander) v.follow = hero;
				const unit_t* tgt = nullptr; enemy_aimed = false; fire_ready = false;
				if (hero && !commander) {
					auto is_enemy = [&](const unit_t* u) { return F.is_enemy(u); };
					tgt = is_enemy(pick.unit) ? pick.unit : s.r->nearest_to_crosshair(v.cam, v.follow, 0.045 * a.rw, is_enemy); // crosshair target, with a little aim assist
					enemy_aimed = tgt != nullptr; fire_ready = tgt && F.in_range(tgt);
					if (firing && tgt) {
					if (F.fire(const_cast<unit_t*>(tgt)) == Fps::ShotResult::miss) { vm.shoot(have_audio ? &audio : nullptr); vm.miss(have_audio ? &audio : nullptr); }
				}
				}
				ov.crosshair = fire_ready ? 0xFF40FF40 : enemy_aimed ? 0xFFFF3030 : 0xFFFFFFFF; // green: in weapon range, red: out of range
				if (F.shots > last_shots) { for (int i = last_shots; i < F.shots; ++i) vm.shoot(have_audio ? &audio : nullptr); last_shots = F.shots; }
				int hp = F.player_hp();
				if (last_hp >= 0 && hp < last_hp && hp > 0) { flash = 0.35; if (pain_cd <= 0) { vm.pain(have_audio ? &audio : nullptr); ++pain_plays; pain_cd = 0.6; } }
				if (!F.alive() && !dead_seen) { dead_seen = true; vm.death(have_audio ? &audio : nullptr); ++death_plays; }
				last_hp = hp; flash = std::max(0.0, flash - dt); pain_cd = std::max(0.0, pain_cd - dt);
			}
			// movement: free camera flies; a possessed unit in game mode is driven with move orders
			double run_mul = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 3.0 : 1.0, sp = 110.0 * run_mul * dt;
			double fwd = (keys[SDL_SCANCODE_W] ? sp : 0) - (keys[SDL_SCANCODE_S] ? sp : 0), str = (keys[SDL_SCANCODE_D] ? sp : 0) - (keys[SDL_SCANCODE_A] ? sp : 0);
			if (s.fps && s.fps->direct() && !commander && !v.follow) s.fps->drive(0, 0); // no hero: no input left over for the next one
			if (s.fps && s.fps->direct() && !commander && v.follow) { // direct mode: walking input (sent before the next step), walk and shoot at once, no orders
				const double PI = 3.14159265358979323846, h = v.cam.heading * PI / 180;
				double f = fwd > 0 ? 1 : fwd < 0 ? -1 : 0, sd = str > 0 ? 1 : str < 0 ? -1 : 0;
				s.fps->drive(std::cos(h) * f - std::sin(h) * sd, std::sin(h) * f + std::cos(h) * sd);
				moving = false;
			} else if (s.live() && v.follow) {
				if ((fwd || str) && !(s.fps && firing && fire_ready) && (!s.fps || commander || s.fps->alive())) {
					if (!moving || s.frame() - last_move_order >= 6) {
						const double PI = 3.14159265358979323846, h = v.cam.heading * PI / 180; double f = fwd > 0 ? 1 : fwd < 0 ? -1 : 0, sd = str > 0 ? 1 : str < 0 ? -1 : 0;
						double dx = std::cos(h) * f - std::sin(h) * sd, dy = std::sin(h) * f + std::cos(h) * sd, len = std::hypot(dx, dy); dx /= len; dy /= len;
						double ux = v.follow->sprite->position.x, uy = v.follow->sprite->position.y, reach = 140;
						while (reach > 20 && !s.r->can_stand(ux + dx * reach, uy + dy * reach, 8)) reach -= 20; // shorten the leg until it ends on floor
						if (reach > 20) { unit_t* u = const_cast<unit_t*>(v.follow); if (s.game->selected() != u) s.game->select(u); s.game->smart_order(xy{(int)(ux + dx * reach), (int)(uy + dy * reach)}, nullptr); moving = true; last_move_order = s.frame(); }
					}
				} else if (moving && !(s.fps && firing && fire_ready)) { s.game->stop(); moving = false; }
			} else { if (moving) { moving = false; } if (fwd || str) { if (v.follow) v.follow = nullptr; v.move(*s.r, fwd, str); } }
			v.apply_follow(*s.r);
			if (s.fps && s.fps->direct() && !commander && v.follow && v.follow == s.fps->player()) { // ahead of the simulation by the input on its way, smooth between frames
				double x, y; if (s.fps->view_position(acc / sim_dt, x, y)) { v.cam.x = x; v.cam.y = y; } }
			// overlay: selection and hover
			ov.selected.clear(); ov.hover = pick.unit;
			if (s.live()) for (unit_t* u : s.game->selection()) ov.selected.push_back(u);
			// render
			auto t0 = Clock::now(); s.r->render(v.cam, v.follow, fb.data(), &ov);
			pick = s.r->pick_center(v.cam, v.follow);
			render_ms = render_ms * 0.9 + 0.1 * std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
			SDL_UpdateTexture(tex, nullptr, fb.data(), a.rw * 4); SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); // black letterbox (the HUD leaves other draw colours set)
			int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh); // letterbox the scene, keep aspect
			double sc = std::min((double)ow / a.rw, (double)oh / a.rh); SDL_Rect dst{(int)((ow - a.rw * sc) / 2), (int)((oh - a.rh * sc) / 2), (int)(a.rw * sc), (int)(a.rh * sc)};
			SDL_RenderCopy(ren, tex, nullptr, &dst);
			if (s.fps && !commander) {
				vm.update(dt, (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_D]) && s.fps->alive());
				if (s.fps->alive()) vm.draw(ren, dst);
				if (flash > 0) { SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(ren, 200, 0, 0, (Uint8)(flash / 0.35 * 110)); SDL_RenderFillRect(ren, &dst); }
				if (!s.fps->alive()) { SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(ren, 90, 0, 0, 150); SDL_RenderFillRect(ren, &dst); }
			}
			// HUD
			cmd.msg_t = std::max(0.0, cmd.msg_t - dt);
			if (hud.ok) {
				SDL_Color white{255, 255, 255, 255}, yellow{255, 230, 90, 255}, cyan{120, 220, 255, 255}, grey{200, 200, 200, 255}; int pad = 10, lh = hud.line_h(false), sl = hud.line_h(true);
				if (s.fps && !commander) {
					Fps& F = *s.fps; int hp = F.player_hp(), mx = std::max(1, F.player_max_hp());
					if (s.net()) { // hero score (kills : deaths) and the base's economy
						hud.text_center(dst.x + dst.w / 2, dst.y + pad, bwgame::format("You %d : %d %s", F.kills, F.deaths, s.game->opponent_name().c_str()), yellow);
						hud.text(dst.x + pad, dst.y + pad, bwgame::format("Minerals %d  Gas %d  Supply %d/%d", s.game->minerals(), s.game->gas(), s.game->supply_used(), s.game->supply_available()), white, true);
					} else hud.text(dst.x + dst.w / 2 - 170, dst.y + pad, bwgame::format("Wave %d     Kills %d     Enemies %d", F.wave, F.kills, F.enemies_alive()), yellow);
					int bw = dst.w / 4, bh = std::max(10, dst.h / 36), bx = dst.x + pad, by = dst.y + dst.h - pad - bh - lh;
					SDL_SetRenderDrawColor(ren, 20, 20, 20, 255); SDL_Rect bg{bx - 2, by - 2, bw + 4, bh + 4}; SDL_RenderFillRect(ren, &bg);
					double f = std::max(0.0, std::min(1.0, (double)hp / mx)); SDL_SetRenderDrawColor(ren, f > 0.5 ? 60 : 220, f > 0.5 ? 200 : f > 0.25 ? 170 : 40, 40, 255); SDL_Rect fg{bx, by, (int)(bw * f), bh}; SDL_RenderFillRect(ren, &fg);
					hud.text(bx, by - lh, bwgame::format("HP %d / %d", hp, mx), white);
					if (!F.alive()) { hud.text(dst.x + dst.w / 2 - 110, dst.y + dst.h / 2 - lh, "YOU DIED", SDL_Color{255, 90, 90, 255}); hud.text(dst.x + dst.w / 2 - 130, dst.y + dst.h / 2 + 4, s.net() ? std::string(bwgame::format("Back in %d s", (F.respawn_in() + 23) / 24)) : std::string("Press R to restart"), white, true); }
				} else if (s.live()) {
					Game& G = *s.game;
					hud.text(dst.x + pad, dst.y + pad, bwgame::format("Minerals %d    Gas %d    Supply %d/%d", G.minerals(), G.gas(), G.supply_used(), G.supply_available()), yellow);
					int y = dst.y + dst.h - pad - lh * 2;
					int nsel = 0; unit_t* first = nullptr; for (unit_t* u : G.selection()) { if (!first) first = u; ++nsel; }
					if (first) hud.text(dst.x + pad, y, nsel > 1 ? bwgame::format("%s +%d more", cmd.name(first).c_str(), nsel - 1) + "" : bwgame::format("%s   HP %d/%d", cmd.name(first).c_str(), first->hp.integer_part(), first->unit_type->hitpoints.integer_part()), white);
					else hud.text(dst.x + pad, y, "Nothing selected", grey);
					if (v.follow && (!s.fps || commander)) hud.text(dst.x + pad, y - sl, "Possessing: " + cmd.name(v.follow) + "  (E to release, WASD to drive)", cyan, true);
					if (commander) hud.text(dst.x + pad, y - sl * 2, bwgame::format("Commanding your base (F: back to your hero)   You %d : %d %s", s.fps->kills, s.fps->deaths, G.opponent_name().c_str()), cyan, true);
				} else hud.text(dst.x + pad, dst.y + pad, bwgame::format("Replay  frame %d / %d", s.frame(), s.end_frame()), yellow);
				if (s.net()) { // the match: its end, the other player's connection, a failure
					Game& G = *s.game; std::string opp = G.opponent_name(), big, line;
					if (!G.failure().empty()) { big = "GAME STOPPED"; line = G.failure(); }
					else if (G.won()) big = "VICTORY";
					else if (G.lost()) big = "DEFEAT";
					if (G.failure().empty() && G.opponent_left()) line = G.opponent_dropped() ? "Lost the connection to " + opp : opp + " left the game";
					else if (G.failure().empty() && stalled > 0.3) line = "Waiting for " + opp + "...";
					if (!big.empty()) hud.text_center(dst.x + dst.w / 2, dst.y + dst.h / 3, big, SDL_Color{255, 220, 80, 255});
					if (!line.empty()) hud.text_center(dst.x + dst.w / 2, dst.y + dst.h / 3 + lh + 4, line, white);
				}
				if (pick.unit) { // what the crosshair is on
					const unit_t* u = pick.unit; const char* rel = s.live() ? (u->owner == s.game->owner() ? "yours" : u->owner == s.game->enemy() ? "enemy" : "neutral") : "";
					hud.text(dst.x + dst.w / 2, dst.y + dst.h / 2 + 16, bwgame::format("%s %s  %d/%d", cmd.name(u).c_str(), rel, u->hp.integer_part(), u->unit_type->hitpoints.integer_part()), white, true, false);
				}
				if (cmd.msg_t > 0) hud.text(dst.x + dst.w / 2 - 150, dst.y + dst.h - pad - lh * 4 - sl, cmd.msg, cyan);
				hud.text(dst.x + dst.w - pad, dst.y + pad, s.net() ? std::string(bwgame::format("%.0f fps  latency %d", fps, s.game->latency())) : std::string(bwgame::format("%.0f fps  x%.3g%s%s", fps, speed, paused ? "  PAUSED" : "", s.done() ? "  END" : "")), grey, true, true);
				hud.text(dst.x + dst.w - pad, dst.y + dst.h - pad - sl, (std::string(s.net() ? (commander ? "LMB select  RMB order  E possess  T worker  Y combat  B supply  X stop  F back to your hero" : "WASD move  Mouse aim  LMB (hold) fire  RMB move here  F command your base") : s.fps ? "WASD move  Mouse aim  LMB (hold) fire  RMB move here  R restart  P pause" : s.live() ? "LMB select  RMB order  E possess  T worker  Y combat  B supply  X stop  F free  P pause" : "TAB follow  F free  P pause  =/- speed  R restart") + (s.models ? "  M models" : "")), grey, true, true);
			}
			SDL_RenderPresent(ren);
			if (a.no_vsync) { // soft cap around 240 fps so the uncapped path does not burn the CPU
				double target = 1.0 / 240.0, elapsed = std::chrono::duration<double>(Clock::now() - now).count();
				if (elapsed < target) SDL_Delay((Uint32)((target - elapsed) * 1000.0));
			}
			fps = fps * 0.9 + 0.1 / std::max(1e-4, dt);
			if ((title_t += dt) > 0.4) { title_t = 0;
				std::string t = bwgame::format("StarCraft FP | %.0f fps (render %.1f ms) | frame %d/%d | x%.3g%s%s | %s", fps, render_ms, s.frame(), s.end_frame(), speed, paused ? " PAUSED" : "", s.done() ? " END" : "", v.follow ? "following unit" : "free camera");
				if (s.net()) t += " | 1v1 vs " + s.game->opponent_name();
				SDL_SetWindowTitle(win, t.c_str()); }
		}
		if (s.net()) s.game->leave(); // the other game sees this player leave rather than a dropped connection
		int rc = 0;
		if (a.selftest_input) {
			for (auto& r : st.results) std::cout << (r.second ? "  ok   " : "  FAIL ") << r.first << "\n";
			int bad = 0; for (auto& r : st.results) bad += !r.second; rc = (bad || !st.finished()) ? 1 : 0;
			if (!st.finished()) std::cout << "  FAIL scripted plan did not finish (" << st.next << "/" << st.plan.size() << " steps)\n";
			std::cout << (rc ? "SELFTEST-INPUT FAILED" : "SELFTEST-INPUT PASSED") << "\n";
		}
		if (!assets.missing_images().empty()) { std::cerr << "missing HD sprites (drawn as flat boxes): anim/main_NNN.anim for image ids:"; for (int id : assets.missing_images()) std::cerr << " " << id; std::cerr << "\n"; }
		SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
		return rc;
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
}
