// Headless tests of the terrain model (fp_terrain.h) against OpenBW's own map functions.
//
//   fp_terraintest DATA_DIR MAP.scm [MAP.scm...]
#include "fp_terrain.h"
#include "dir_loader.h"
#include "replay.h"
#include <cstring>
#include <iostream>
#include <map>
#include <string>

using namespace fp;

static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }

int main(int argc, char** argv) {
	std::cout << std::unitbuf;
	if (argc < 3) { std::cerr << "usage: fp_terraintest DATA_DIR MAP.scm [MAP.scm...]\n"; return 2; }
	try {
		for (int mi = 2; mi < argc; ++mi) {
			replay_player player; player.init(dir_loader(argv[1])); player.load_map_file(argv[mi]);
			state& st = player.st(); state_functions& fn = player.funcs();
			Terrain t; t.build(st, fn);
			std::cout << argv[mi] << ": " << t.w << "x" << t.h << " minitiles, cliffs " << (t.drape ? "draped" : "blocks") << "\n";
			const double L = t.level_h;
			int walk_bad = 0, level_bad = 0, ramp_bad = 0, ramps = 0, sink = 0, step = 0, cliff_low = 0, liquid_high = 0, drape_wall = 0, drape_range = 0; std::map<int, int> kinds;
			const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
			for (int y = 0; y < t.h; ++y) for (int x = 0; x < t.w; ++x) {
				const Terrain::Cell& c = t.at(x, y); xy p{x * 8 + 4, y * 8 + 4}; ++kinds[c.kind];
				if (t.walkable(x, y) != fn.is_walkable(p)) ++walk_bad;
				if (c.kind == Terrain::water || c.kind == Terrain::space) // liquids and void lie below everything around them that is not liquid
					for (int k = 0; k < 4; ++k) { const Terrain::Cell& n = t.at(x + dx[k], y + dy[k]); if (n.kind != Terrain::water && n.kind != Terrain::space && n.kind != Terrain::edge && n.h <= c.h) ++liquid_high; }
				if (c.kind != Terrain::floor) {
					if (c.kind == Terrain::cliff) for (int k = 0; k < 4; ++k) { const Terrain::Cell& n = t.at(x + dx[k], y + dy[k]); if (n.kind == Terrain::floor && n.level > c.level) ++cliff_low; }
					if (c.kind == Terrain::obstacle && t.drape) // a mound above the floor beside it
						for (int k = 0; k < 4; ++k) { const Terrain::Cell& n = t.at(x + dx[k], y + dy[k]); if (n.kind == Terrain::floor && n.h > c.h) ++drape_range; }
					if (c.kind == Terrain::cliff && t.drape) { // a slope between ground and its top level, meeting the floor without a wall
						if (c.h < 0 || c.h > c.level * L) ++drape_range;
						for (int k = 0; k < 4; ++k) { const Terrain::Cell& n = t.at(x + dx[k], y + dy[k]); if (n.kind == Terrain::floor && std::abs(n.h - c.h) > 0.5 * L + 1) ++drape_wall; }
					}
					continue;
				}
				if (c.level != fn.get_ground_height_at(p)) ++level_bad;
				if (!(c.flags & Terrain::ramp) && c.h != (float)(c.level * L)) ++level_bad;
				ramps += (c.flags & Terrain::ramp) != 0;
				if ((c.flags & Terrain::ramp) && (c.h < 0 || c.h > 2 * L || c.h == (float)(c.level * L))) ++ramp_bad;
				for (int k = 0; k < 4; ++k) {
					const Terrain::Cell& n = t.at(x + dx[k], y + dy[k]);
					if (t.is_solid(n.kind) && n.h < c.h) ++sink;
					if (n.kind == Terrain::floor && std::abs(n.h - c.h) > 0.6 * L) ++step;
				}
			}
			check(walk_bad == 0, "walkable cells match OpenBW is_walkable (" + std::to_string(walk_bad) + " differ)");
			check(level_bad == 0, "floor levels match OpenBW get_ground_height_at, at level x " + std::to_string((int)L) + " px (" + std::to_string(level_bad) + " differ)");
			check(ramp_bad == 0 && kinds[Terrain::floor] > 0, "ramp cells lie between their two levels (" + std::to_string(ramp_bad) + " do not)");
			check(cliff_low == 0, "no floor next to a cliff is higher than the cliff's top (" + std::to_string(cliff_low) + " are)");
			check(liquid_high == 0, "water and void lie below the ground around them (" + std::to_string(liquid_high) + " edges do not)");
			check(sink == 0, "no solid block sinks below the floor next to it (" + std::to_string(sink) + " do)");
			if (t.drape) {
				check(drape_range == 0, "draped cliffs lie between the ground and their top level, draped obstacles above the floor beside them (" + std::to_string(drape_range) + " do not)");
				check(drape_wall == 0, "draped cliffs meet the floor without a wall, half a level at most (" + std::to_string(drape_wall) + " steps)");
			}
			check(step == 0, "walkable neighbours never jump more than 0.6 level (ramps are slopes; " + std::to_string(step) + " jumps)");
			Terrain t2; t2.build(st, fn);
			bool same = t2.w == t.w && t2.h == t.h && std::memcmp(t2.cells.data(), t.cells.data(), t.cells.size() * sizeof(Terrain::Cell)) == 0;
			check(same, "building the model twice gives the same cells");
			std::cout << "  kinds: floor " << kinds[Terrain::floor] << " (ramp " << ramps << "), cliff " << kinds[Terrain::cliff] << ", obstacle " << kinds[Terrain::obstacle]
				<< ", water " << kinds[Terrain::water] << ", space " << kinds[Terrain::space] << "\n";
		}
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
	std::cout << (failures ? "TERRAINTEST FAILED" : "TERRAINTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
