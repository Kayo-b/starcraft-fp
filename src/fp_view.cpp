// fp_view: render ONE first-person frame (PPM) from OpenBW simulation state, and optionally the top-down view. Read-only.
#include "fp_render.h"
#include "fp_topdown.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

using namespace fp;

struct Args {
	std::string data, input, out = "out/fp.ppm", topdown, heightmap, models, model, eyelevel; double model_x = -1, model_y = -1, model_yaw = 0, model_scale = 32; int model_owner = 1; bool model_units = false;
	struct Spawn { int type, x, y, heading, owner; }; std::vector<Spawn> spawns;
	int frames = 0, unit = -1, spot = -1, w = 640, h = 400, viewer = -1; double zoom = 2; double cam_x = -1, cam_y = -1, heading = 0, pitch = 0; bool replay = false, print_tileset = false; RenderParams rp;
};
static void die(const char* m) { std::cerr << m << "\n"; std::exit(2); }

int main(int argc, char** argv) {
	Args a;
	for (int i = 1; i < argc; ++i) {
		std::string k = argv[i];
		auto next = [&]() -> const char* { if (i + 1 >= argc) die("missing value"); return argv[++i]; };
		if (k == "--data") a.data = next();
		else if (k == "--map") { a.input = next(); a.replay = false; }
		else if (k == "--replay") { a.input = next(); a.replay = true; }
		else if (k == "--frames") a.frames = atoi(next());
		else if (k == "--unit") a.unit = atoi(next());
		else if (k == "--cam") sscanf(next(), "%lf,%lf", &a.cam_x, &a.cam_y);
		else if (k == "--heading") a.heading = atof(next());
		else if (k == "--pitch") a.pitch = atof(next());
		else if (k == "--out") a.out = next();
		else if (k == "--topdown") a.topdown = next();
		else if (k == "--zoom") a.zoom = atof(next());
		else if (k == "--viewer") a.viewer = atoi(next());
		else if (k == "--size") sscanf(next(), "%dx%d", &a.w, &a.h);
		else if (k == "--print-tileset") a.print_tileset = true;
		else if (k == "--spot") a.spot = atoi(next());
		else if (k == "--heightmap") a.heightmap = next();
		else if (k == "--models") a.models = next();
		else if (k == "--model") a.model = next();
		else if (k == "--model-at") { double yaw = 0; int n = sscanf(next(), "%lf,%lf,%lf", &a.model_x, &a.model_y, &yaw); if (n == 3) a.model_yaw = yaw; }
		else if (k == "--model-scale") a.model_scale = atof(next());
		else if (k == "--model-owner") a.model_owner = atoi(next());
		else if (k == "--model-units") a.model_units = true;
		else if (k == "--eyelevel") a.eyelevel = next();
		else if (k == "--spawn") { Args::Spawn sp{0, 0, 0, 128, 0}; if (sscanf(next(), "%d,%d,%d,%d,%d", &sp.type, &sp.x, &sp.y, &sp.heading, &sp.owner) < 3) die("--spawn TYPE,X,Y[,HEADING[,OWNER]]"); a.spawns.push_back(sp); }
		else if (k == "--eye") a.rp.eye_h = atof(next());
		else if (k == "--level-h") a.rp.level_h = atof(next());
		else if (k == "--view-dist") a.rp.view_dist = atof(next());
		else die("unknown option");
	}
	if (a.data.empty() || a.input.empty()) die("usage: fp_view --data DIR (--map FILE | --replay FILE) [--frames N] [--unit I | --cam X,Y | --spot K] [--heading DEG] [--pitch PX] [--out F.ppm] [--size WxH] [--topdown F.ppm [--zoom Z]] [--viewer P] [--heightmap F.ppm] [--print-tileset] [--eye PX] [--level-h PX] [--view-dist PX] [--models DIR [--model FILE --model-at X,Y[,YAW] [--model-scale PX] [--model-owner P]] [--model-units]] [--eyelevel DIR] [--spawn TYPE,X,Y[,HEADING 0..255[,OWNER]]]...");
	try {
		replay_player player;
		player.init(dir_loader(a.data));
		if (a.replay) { player.load_replay_file(a.input); for (int i = 0; i < a.frames && !player.is_done(); ++i) player.next_frame(); }
		else { player.load_map_file(a.input); for (int i = 0; i < a.frames; ++i) static_cast<game_player&>(player).next_frame(); }
		if (a.print_tileset) { // map survey: tileset and size only, no rendering
			auto& gs = *player.st().game;
			std::cout << "tileset " << gs.tileset_index << " " << tileset_name(gs.tileset_index) << ", map " << gs.map_tile_width << "x" << gs.map_tile_height << " tiles\n";
			return 0;
		}
		for (auto& sp : a.spawns) { // test scenes: units placed by hand (heading 0 = facing north, 64 east, 128 south)
			auto& fn = player.funcs();
			unit_t* u = fn.create_unit(fn.get_unit_type((UnitTypes)sp.type), xy{sp.x, sp.y}, sp.owner);
			if (!u) die("--spawn: could not create the unit");
			fn.finish_building_unit(u);
			if (!fn.place_completed_unit(u)) die("--spawn: no room for the unit there");
			fn.complete_unit(u);
			fn.set_unit_heading(u, fn.direction_from_index(sp.heading & 255));
		}
		if (!a.spawns.empty()) { // one frame, so OpenBW sets what follows a unit's frame (turret offsets on their hulls)
			if (a.replay) player.next_frame(); else static_cast<game_player&>(player).next_frame();
		}
		Assets assets(a.data);
		if (!a.eyelevel.empty()) assets.set_eyelevel_dir(a.eyelevel);
		Renderer r(player.st(), player.funcs(), assets, a.w, a.h, a.rp);
		std::unique_ptr<ModelLibrary> models;
		if (!a.models.empty()) {
			models = std::make_unique<ModelLibrary>(a.models); if (!models->ok()) die("--models: no files under that directory");
			r.set_models(models.get()); r.set_model_mode(a.model_units ? Renderer::kModelsUnits : Renderer::kModelsBuildings);
			if (!a.model.empty()) { const M3Model* m = models->load(a.model); if (!m) die("--model: could not load");
				r.add_static_model(m, a.model_x, a.model_y, a.model_scale, a.model_yaw, a.model_owner);
				std::cout << "model " << a.model << ": " << m->triangles() << " triangles, " << m->tex.size() << " textures\n"; }
		}
		if (a.viewer >= 0) r.set_viewer(a.viewer);
		r.update_units();
		if (getenv("FP_PRINT_TURRETS")) for (auto& u : r.units()) if (u.turret_of >= 0) // debug: turret offsets per hull heading
			std::cout << "turret on unit " << u.turret_of << ": hull heading " << r.units()[u.turret_of].heading << " frame " << r.units()[u.turret_of].frame << " offset " << u.tox << "," << u.toy << " point " << u.ta[0] << "," << u.ta[1] << "," << u.ta[2] << (u.ta_ok ? "" : " (no fit)") << "\n";
		std::cout << "map " << r.map_w() << "x" << r.map_h() << " px, units " << r.units().size() << "\n";
		Camera cam; cam.heading = a.heading; cam.pitch = a.pitch; const unit_t* hide = nullptr;
		if (a.spot >= 0) {
			// Deterministic spots for golden views: the start locations, then the map centre and the four quarter points,
			// each moved to the nearest floor the camera can stand on.
			auto& gs = *player.st().game; int W = (int)gs.map_width, H = (int)gs.map_height;
			std::vector<xy> spots;
			for (auto& p : gs.start_locations) if (p.x || p.y) spots.push_back(p);
			for (xy p : {xy{W / 2, H / 2}, xy{W / 4, H / 4}, xy{3 * W / 4, H / 4}, xy{W / 4, 3 * H / 4}, xy{3 * W / 4, 3 * H / 4}}) spots.push_back(p);
			if (a.spot >= (int)spots.size()) die("--spot: no such spot on this map");
			xy p = spots[a.spot]; bool found = false;
			for (int ring = 0; ring <= 64 && !found; ++ring) for (int dy = -ring; dy <= ring && !found; ++dy) for (int dx = -ring; dx <= ring && !found; ++dx) {
				if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
				double x = p.x + dx * 8.0, y = p.y + dy * 8.0;
				if (r.can_stand(x, y, 8)) { cam.x = x; cam.y = y; found = true; }
			}
			if (!found) die("--spot: no floor near that spot");
			std::cout << "spot " << a.spot << " of " << spots.size() << " at (" << cam.x << "," << cam.y << ")\n";
		}
		else if (a.cam_x >= 0) { cam.x = a.cam_x; cam.y = a.cam_y; }
		else {
			const UnitView* eye = nullptr; int idx = 0;
			for (auto& u : r.units()) { if ((a.unit >= 0 && idx == a.unit) || (a.unit < 0 && u.owner < 8)) { eye = &u; break; } ++idx; }
			if (!eye) die("no eye unit found; use --cam X,Y");
			cam.x = eye->x; cam.y = eye->y; hide = eye->ptr;
			std::cout << "eye = unit #" << idx << " type " << eye->type << " owner " << eye->owner << " at (" << cam.x << "," << cam.y << ")\n";
		}
		std::vector<uint32_t> fb((size_t)a.w * a.h);
		r.render(cam, hide, fb.data());
		auto write = [](const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
			FILE* f = fopen(path.c_str(), "wb"); if (!f) die("cannot open output");
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (uint32_t p : px) { uint8_t c[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p}; fwrite(c, 1, 3, f); }
			fclose(f);
		};
		write(a.out, fb, a.w, a.h);
		std::cout << "wrote " << a.out << "\n";
		if (!a.heightmap.empty()) { // debug: the terrain model, one pixel per minitile (fp_terrain.h)
			const Terrain& t = r.terrain(); std::vector<uint32_t> hm((size_t)t.w * t.h);
			for (int y = 0; y < t.h; ++y) for (int x = 0; x < t.w; ++x) {
				const Terrain::Cell& c = t.at(x, y); double v = std::max(0.0, std::min(1.0, c.h / (3 * t.level_h))); int R = 0, G = 0, B = 0;
				switch (c.kind) {
					case Terrain::floor: G = (int)(70 + 170 * v); R = (c.flags & Terrain::ramp) ? 200 : 30; B = 30; break; // green by height; ramps yellow
					case Terrain::cliff: R = (int)(120 + 130 * v); G = 60; B = 30; break;                                 // brown-red by top level
					case Terrain::obstacle: R = G = B = (int)(70 + 150 * v); if (c.flags & Terrain::doodad) B = 255; break; // grey; doodads blue-grey
					case Terrain::water: R = 30; G = 60; B = 200; break;
					case Terrain::space: break;
					default: R = 255; B = 255;
				}
				hm[(size_t)y * t.w + x] = 0xFF000000u | (R << 16) | (G << 8) | B;
			}
			write(a.heightmap, hm, t.w, t.h);
			std::cout << "wrote " << a.heightmap << "\n";
		}
		if (!a.topdown.empty()) { // the top-down view centred on the camera, twice the first-person frame's size
			TopDown td(player.st(), player.funcs(), assets, 2 * a.w, 2 * a.h); TopView tv{cam.x, cam.y, a.zoom}; td.clamp(tv);
			std::vector<uint32_t> px((size_t)td.width() * td.height());
			auto t0 = std::chrono::steady_clock::now(); td.render(r, tv, px.data());
			double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			auto t1 = std::chrono::steady_clock::now(); td.render(r, tv, px.data());
			double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
			write(a.topdown, px, td.width(), td.height());
			std::cout << "wrote " << a.topdown << " (" << td.drawn() << " images, " << ms << " ms with decoding, " << ms2 << " ms again" << (td.creep_edges() ? "" : "; no creep edges: TileSet/*.dds.grp missing") << ")\n";
		}
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
}
