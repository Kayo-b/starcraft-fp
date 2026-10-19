// terrain_survey: what a map's terrain data says, per minitile, before we model it (M1 of docs/PLAN.md). Read-only.
//
//   terrain_survey DATA_DIR [--typemap DIR] MAP.scm...
//
// Prints, per map: walkable minitiles by level (OpenBW's get_ground_height_at: 0 low, 1 mid, 2 high) and with vf4's
// "very high" bit; unwalkable minitiles by vf4 flags; connected unwalkable regions by the levels of the walkable ground
// around them (two or more levels = cliff-like, one = obstacle-like, none = sealed); the raw cv5 group fields of the
// tiles used (type at offset 0, doodad width/height at offsets 14/16); and the north-south thickness of unwalkable bands
// between two walkable levels (a south-facing cliff has the higher level to its north).
// --typemap DIR also writes DIR/<map>.ppm per map: the terrain art (left) and the cv5 type of each tile (right, colour =
// type % 16, legend printed), one pixel per minitile, to see what each cv5 type is.
#include "bwgame.h"
#include "replay.h"
#include "dir_loader.h"
#include "fp_assets.h"
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace bwgame;

static uint16_t u16_at(const a_vector<uint8_t>& d, size_t off) { return off + 1 < d.size() ? (uint16_t)(d[off] | d[off + 1] << 8) : 0; }

int main(int argc, char** argv) {
	if (argc < 3) { std::cerr << "usage: terrain_survey DATA_DIR [--typemap DIR] MAP.scm...\n"; return 2; }
	std::string typemap; int first = 2;
	if (std::string(argv[2]) == "--typemap" && argc > 4) { typemap = argv[3]; first = 4; }
	fp::Assets assets(argv[1]);
	for (int mi = first; mi < argc; ++mi) {
		replay_player player; player.init(dir_loader(argv[1])); player.load_map_file(argv[mi]);
		state& st = player.st(); const game_state& gs = *st.game; state_functions& fn = player.funcs();
		const a_vector<uint8_t>& cv5raw = st.global->tileset_cv5.at(gs.tileset_index);
		const int W = (int)gs.map_walk_width, H = (int)gs.map_walk_height, TW = (int)gs.map_tile_width;
		auto vf4 = [&](int x, int y) -> int { // per-minitile flags, as get_ground_height_at reads them
			tile_id t = gs.gfx_tiles.at((size_t)(y / 4) * TW + x / 4);
			size_t mt = gs.cv5.at(t.group_index()).mega_tile_index[t.subtile_index()];
			return gs.vf4.at(mt).flags[(y % 4) * 4 + x % 4];
		};
		auto walk = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && fn.is_walkable(xy{x * 8 + 4, y * 8 + 4}); };
		auto level = [&](int x, int y) { return fn.get_ground_height_at(xy{x * 8 + 4, y * 8 + 4}); };

		std::printf("== %s: tileset %d %s, %dx%d minitiles, cv5 %zu groups\n", argv[mi], (int)gs.tileset_index, fp::tileset_name(gs.tileset_index), W, H, cv5raw.size() / 52);
		std::map<int, int> walk_lv, walk_vh, unwalk_flags; int mismatch = 0;
		for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
			int f = vf4(x, y);
			if (((f & 1) != 0) != walk(x, y)) ++mismatch; // vf4 walkable bit vs OpenBW is_walkable (creep, partial tiles)
			if (walk(x, y)) { ++walk_lv[level(x, y)]; if (f & 8) ++walk_vh[level(x, y)]; }
			else ++unwalk_flags[f & 0xe];
		}
		std::printf("walkable by level:"); for (auto& [l, n] : walk_lv) std::printf("  L%d %d (very high bit %d)", l, n, walk_vh[l]); std::printf("\n");
		std::printf("unwalkable by vf4 flags (mid=2 high=4 vhigh=8):"); for (auto& [f, n] : unwalk_flags) std::printf("  0x%x:%d", f, n); std::printf("\n");
		std::printf("vf4 walkable bit disagrees with is_walkable at %d minitiles\n", mismatch);

		// connected unwalkable regions (4-neighbour) and the walkable levels bordering them
		std::vector<int> comp((size_t)W * H, -1); std::map<std::string, std::pair<int, int>> kinds; // levels -> (regions, minitiles)
		std::vector<std::pair<int, int>> stack;
		for (int y0 = 0; y0 < H; ++y0) for (int x0 = 0; x0 < W; ++x0) {
			if (walk(x0, y0) || comp[(size_t)y0 * W + x0] >= 0) continue;
			int id = y0 * W + x0, size = 0; bool edge = false; unsigned lvmask = 0;
			stack.push_back({x0, y0}); comp[(size_t)y0 * W + x0] = id;
			while (!stack.empty()) {
				auto [x, y] = stack.back(); stack.pop_back(); ++size;
				const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
				for (int k = 0; k < 4; ++k) {
					int nx = x + dx[k], ny = y + dy[k];
					if (nx < 0 || ny < 0 || nx >= W || ny >= H) { edge = true; continue; }
					if (walk(nx, ny)) { lvmask |= 1u << level(nx, ny); continue; }
					if (comp[(size_t)ny * W + nx] < 0) { comp[(size_t)ny * W + nx] = id; stack.push_back({nx, ny}); }
				}
			}
			std::string k; for (int l = 0; l < 3; ++l) if (lvmask & (1u << l)) k += "L" + std::to_string(l);
			if (k.empty()) k = "none"; if (edge) k += "+edge";
			kinds[k].first++; kinds[k].second += size;
		}
		std::printf("unwalkable regions by bordering levels:"); for (auto& [k, v] : kinds) std::printf("  %s: %d regions / %d minitiles", k.c_str(), v.first, v.second); std::printf("\n");

		// cv5 groups used by the map: raw type field and doodad fields
		std::map<int, int> type_all, type_unwalk; std::map<std::pair<int, int>, int> dood_wh; int doodad_tiles = 0;
		for (int ty = 0; ty < (int)gs.map_tile_height; ++ty) for (int tx = 0; tx < TW; ++tx) {
			tile_id t = gs.gfx_tiles.at((size_t)ty * TW + tx); size_t g = t.group_index(), off = g * 52;
			int type = u16_at(cv5raw, off); int unwalk = 0;
			for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) unwalk += !walk(tx * 4 + sx, ty * 4 + sy);
			++type_all[type]; if (unwalk == 16) ++type_unwalk[type];
			if (g >= 1024) { ++doodad_tiles; ++dood_wh[{u16_at(cv5raw, off + 14), u16_at(cv5raw, off + 16)}]; }
		}
		std::printf("cv5 type (offset 0) of tiles used [fully unwalkable]:"); for (auto& [t, n] : type_all) std::printf("  %d:%d[%d]", t, n, type_unwalk[t]); std::printf("\n");
		std::printf("doodad tiles (group >= 1024): %d; their (width,height) at offsets 14/16:", doodad_tiles);
		int shown = 0; for (auto& [wh, n] : dood_wh) if (shown++ < 24) std::printf("  (%d,%d):%d", wh.first, wh.second, n); std::printf("\n");

		if (!typemap.empty()) {
			static const uint8_t pal[16][3] = {{230, 25, 75}, {60, 180, 75}, {255, 225, 25}, {0, 130, 200}, {245, 130, 48}, {145, 30, 180}, {70, 240, 240}, {240, 50, 230},
				{210, 245, 60}, {250, 190, 212}, {0, 128, 128}, {220, 190, 255}, {170, 110, 40}, {255, 250, 200}, {128, 0, 0}, {255, 255, 255}};
			const fp::HdTiles* hd = assets.tiles(gs.tileset_index);
			std::string name = argv[mi]; name = name.substr(name.find_last_of('/') + 1); name = name.substr(0, name.find_last_of('.'));
			for (auto& c : name) if (!isalnum((unsigned char)c)) c = '_';
			std::string path = typemap + "/" + name + ".ppm"; FILE* f = std::fopen(path.c_str(), "wb");
			if (!f) { std::cerr << "cannot write " << path << "\n"; return 1; }
			std::fprintf(f, "P6\n%d %d\n255\n", 2 * W, H);
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) { uint8_t c[3]; hd->sample(st.tiles_mega_tile_index[(size_t)(y / 4) * TW + x / 4] & 0x7fff, (x % 4) * 32 + 16, (y % 4) * 32 + 16, c); std::fwrite(c, 1, 3, f); }
				for (int x = 0; x < W; ++x) { int type = u16_at(cv5raw, gs.gfx_tiles.at((size_t)(y / 4) * TW + x / 4).group_index() * 52);
					uint8_t c[3] = {pal[type % 16][0], pal[type % 16][1], pal[type % 16][2]};
					if (!walk(x, y)) { c[0] /= 2; c[1] /= 2; c[2] /= 2; } // unwalkable minitiles darker
					std::fwrite(c, 1, 3, f); }
			}
			std::fclose(f);
			std::printf("typemap %s (unwalkable darker); colours:", path.c_str());
			const char* cn[16] = {"red", "green", "yellow", "blue", "orange", "purple", "cyan", "magenta", "lime", "pink", "teal", "lavender", "brown", "beige", "maroon", "white"};
			for (auto& [t, n] : type_all) std::printf("  %d=%s", t, cn[t % 16]); std::printf("\n");
		}

		// north-south unwalkable bands between two walkable minitiles: (level north, level south) -> thickness histogram
		std::map<std::pair<int, int>, std::map<int, int>> bands;
		for (int x = 0; x < W; ++x) for (int y = 1; y < H; ++y) {
			if (!walk(x, y - 1) || walk(x, y)) continue; // a band starts below a walkable minitile
			int y1 = y; while (y1 < H && !walk(x, y1)) ++y1;
			if (y1 >= H) break;
			++bands[{level(x, y - 1), level(x, y1)}][std::min(y1 - y, 16)];
			y = y1 - 1;
		}
		for (auto& [lv, hist] : bands) {
			std::printf("bands north L%d -> south L%d%s thickness:", lv.first, lv.second, lv.first > lv.second ? " (south-facing cliff)" : lv.first < lv.second ? " (north-facing cliff)" : "");
			for (auto& [t, n] : hist) std::printf(" %d:%d", t, n); std::printf("\n");
		}
	}
	return 0;
}
