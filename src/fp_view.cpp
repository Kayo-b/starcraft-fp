// fp_view: render ONE first-person frame (PPM) from OpenBW simulation state. Read-only.
#include "fp_render.h"
#include <cstdio>
#include <iostream>
#include <string>

using namespace fp;

struct Args {
	std::string data, input, out = "out/fp.ppm", topdown, heightmap;
	int frames = 0, unit = -1, spot = -1, w = 640, h = 400; double cam_x = -1, cam_y = -1, heading = 0, pitch = 0; bool replay = false, print_tileset = false; RenderParams rp;
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
		else if (k == "--size") sscanf(next(), "%dx%d", &a.w, &a.h);
		else if (k == "--print-tileset") a.print_tileset = true;
		else if (k == "--spot") a.spot = atoi(next());
		else if (k == "--heightmap") a.heightmap = next();
		else if (k == "--eye") a.rp.eye_h = atof(next());
		else if (k == "--level-h") a.rp.level_h = atof(next());
		else if (k == "--view-dist") a.rp.view_dist = atof(next());
		else die("unknown option");
	}
	if (a.data.empty() || a.input.empty()) die("usage: fp_view --data DIR (--map FILE | --replay FILE) [--frames N] [--unit I | --cam X,Y | --spot K] [--heading DEG] [--pitch PX] [--out F.ppm] [--size WxH] [--topdown F.ppm] [--heightmap F.ppm] [--print-tileset] [--eye PX] [--level-h PX] [--view-dist PX]");
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
		Assets assets(a.data);
		Renderer r(player.st(), player.funcs(), assets, a.w, a.h, a.rp);
		r.update_units();
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
		if (!a.topdown.empty()) { // debug: 4 px per minitile, 128x128 minitile window around the camera, with the view cone
			const int R = 64, S = 4, N = 2 * R * S; std::vector<uint32_t> td((size_t)N * N);
			int cx0 = (int)(cam.x / 8) - R, cy0 = (int)(cam.y / 8) - R;
			for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) { uint8_t t[3]; r.tile_sample((cx0 + x / (double)S) * 8, (cy0 + y / (double)S) * 8, t); td[(size_t)y * N + x] = 0xFF000000u | (t[0] << 16) | (t[1] << 8) | t[2]; }
			for (auto& u : r.units()) { int px = (int)((u.x / 8 - cx0) * S), py = (int)((u.y / 8 - cy0) * S);
				for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) { int X = px + dx, Y = py + dy; if (X >= 0 && Y >= 0 && X < N && Y < N) td[(size_t)Y * N + X] = 0xFFC040C0; } }
			const double PI = 3.14159265358979323846, hd = cam.heading * PI / 180, fov = r.params().fov * PI / 180;
			double dx = std::cos(hd), dy = std::sin(hd), px = -dy * std::tan(fov / 2), py = dx * std::tan(fov / 2);
			for (double t = 0; t < 400; t += 1) for (int e = -1; e <= 1; ++e) { int X = (int)(((cam.x + (dx + px * e) * t) / 8 - cx0) * S), Y = (int)(((cam.y + (dy + py * e) * t) / 8 - cy0) * S); if (X >= 0 && Y >= 0 && X < N && Y < N) td[(size_t)Y * N + X] = 0xFFFFFFFF; }
			write(a.topdown, td, N, N);
		}
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
}
