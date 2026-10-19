// Terrain model: the map as a heightfield of minitiles (8 px cells), built once per map load from OpenBW's map data.
// It is the world the first-person renderer draws and the camera stands on (M1 of docs/PLAN.md).
//
// What the data says (tools/terrain_survey, on stock maps of four tilesets):
// - Walkable minitiles carry one of three levels (OpenBW get_ground_height_at: 0 low, 1 mid, 2 high).
// - StarCraft's view is a vertical oblique projection, so a cliff is drawn onto the map: a south-facing cliff (higher
//   level to its north) is an unwalkable band 4-16 minitiles deep, a north-facing one only 2-5.
// - Unwalkable regions bordering one level are obstacles (doodads, rocks, structures, shores); water and the space
//   platform's void are their own cv5 terrain types (offset 0 of a cv5 group; 1 = doodad on every tileset).
//
// Model: every unwalkable cell is a solid block. A cell with two walkable levels within kBand cells (through unwalkable
// ground) is a cliff topped at the higher level; otherwise an obstacle a little above its level. Walkable cells next to
// a different level over walkable ground (ramps) blend the two heights over kRamp cells, so ramps become slopes.
#pragma once
#include "bwgame.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace fp {
using namespace bwgame;

struct Terrain {
	enum Kind : uint8_t { floor, cliff, obstacle, water, space, edge };
	enum Flags : uint8_t { ramp = 1, doodad = 2, very_high = 4 };
	struct Cell {
		uint8_t kind = edge, level = 0, flags = 0, type = 0; // level: floor level, or the top level of a cliff
		float h = 0;                                         // top surface height in px above level-0 ground
	};
	static constexpr double kCell = 8.0;  // minitile size in pixels
	static constexpr int kBand = 16;      // deepest cliff band seen in the survey (minitiles)
	static constexpr int kRamp = 4;       // half-length of a ramp blend (minitiles)
	double level_h = 64;                  // px per level (a prototype choice; RenderParams::level_h explains it)

	int w = 0, h = 0;
	std::vector<Cell> cells;

	const Cell& at(int cx, int cy) const { static const Cell outside; return (cx < 0 || cy < 0 || cx >= w || cy >= h) ? outside : cells[(size_t)cy * w + cx]; }
	bool walkable(int cx, int cy) const { const Cell& c = at(cx, cy); return c.kind == floor; }
	// Height of the cell under a world position (px); solid cells report their top.
	double height_at(double x, double y) const { return at((int)std::floor(x / kCell), (int)std::floor(y / kCell)).h; }
	// Smooth floor height for eyes and feet: bilinear between the four nearest cell centres, floor cells only (a solid
	// neighbour counts as this cell's height), so walking a ramp does not climb in per-cell steps.
	double floor_height(double x, double y) const {
		double hs = height_at(x, y), fx = x / kCell - 0.5, fy = y / kCell - 0.5;
		int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy); double tx = fx - x0, ty = fy - y0;
		auto hh = [&](int cx, int cy) { const Cell& c = at(cx, cy); return c.kind == floor ? (double)c.h : hs; };
		double a = hh(x0, y0) + (hh(x0 + 1, y0) - hh(x0, y0)) * tx, b = hh(x0, y0 + 1) + (hh(x0 + 1, y0 + 1) - hh(x0, y0 + 1)) * tx;
		return a + (b - a) * ty;
	}

	// Water and void by cv5 terrain type, per tileset (OpenBW tileset_index). Only the tilesets the stock maps cover are
	// surveyed; Installation, Desert, Ice and Twilight are filled in by the M6 tileset sweep.
	static bool is_water(size_t tileset, int type) { return (tileset == 0 || tileset == 4) && type == 5; }
	static bool is_space(size_t tileset, int type) { return tileset == 1 && type == 2; }

	void build(const state& st, state_functions& fn) {
		const game_state& gs = *st.game;
		const a_vector<uint8_t>& cv5 = st.global->tileset_cv5.at(gs.tileset_index);
		w = (int)gs.map_walk_width; h = (int)gs.map_walk_height;
		cells.assign((size_t)w * h, Cell{});
		const int tw = (int)gs.map_tile_width;
		auto idx = [&](int x, int y) { return (size_t)y * w + x; };
		for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
			Cell& c = cells[idx(x, y)];
			tile_id t = gs.gfx_tiles.at((size_t)(y / 4) * tw + x / 4);
			size_t g = t.group_index(), off = g * 52;
			c.type = off + 1 < cv5.size() ? (uint8_t)std::min(255, cv5[off] | cv5[off + 1] << 8) : 0;
			if (g >= 1024) c.flags |= doodad;
			size_t mt = gs.cv5.at(g).mega_tile_index[t.subtile_index()];
			if (gs.vf4.at(mt).flags[(y % 4) * 4 + x % 4] & vf4_entry::flag_very_high) c.flags |= very_high;
			xy p{x * 8 + 4, y * 8 + 4};
			c.level = (uint8_t)fn.get_ground_height_at(p);
			if (fn.is_walkable(p)) c.kind = floor;
			else if (is_space(gs.tileset_index, c.type)) c.kind = space;
			else if (is_water(gs.tileset_index, c.type)) c.kind = water;
			else c.kind = obstacle; // cliff or obstacle: decided below
		}

		// Unwalkable blocks: multi-source BFS per level from the walkable cells, through unwalkable non-liquid cells.
		// near[l] = distance to the nearest walkable cell of level l (capped).
		const int cap = 255;
		std::vector<uint8_t> near[3];
		auto solid = [&](int x, int y) { uint8_t k = cells[idx(x, y)].kind; return k == obstacle; };
		for (int l = 0; l < 3; ++l) {
			near[l].assign((size_t)w * h, cap);
			std::deque<std::pair<int, int>> q;
			for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (cells[idx(x, y)].kind == floor && cells[idx(x, y)].level == l) { near[l][idx(x, y)] = 0; q.push_back({x, y}); }
			while (!q.empty()) {
				auto [x, y] = q.front(); q.pop_front(); int d = near[l][idx(x, y)];
				const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
				for (int k = 0; k < 4; ++k) {
					int nx = x + dx[k], ny = y + dy[k];
					if (nx < 0 || ny < 0 || nx >= w || ny >= h || !solid(nx, ny) || near[l][idx(nx, ny)] <= d + 1 || d + 1 >= cap) continue;
					near[l][idx(nx, ny)] = (uint8_t)(d + 1); q.push_back({nx, ny});
				}
			}
		}
		// Liquids and void take the level of the nearest floor, through any unwalkable ground (shores, platform edges).
		std::vector<uint8_t> shore((size_t)w * h, 0);
		{
			std::vector<uint8_t> seen((size_t)w * h, 0); std::deque<std::pair<int, int>> q;
			for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (cells[idx(x, y)].kind == floor) { seen[idx(x, y)] = 1; shore[idx(x, y)] = cells[idx(x, y)].level; q.push_back({x, y}); }
			while (!q.empty()) {
				auto [x, y] = q.front(); q.pop_front();
				const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
				for (int k = 0; k < 4; ++k) {
					int nx = x + dx[k], ny = y + dy[k];
					if (nx < 0 || ny < 0 || nx >= w || ny >= h || seen[idx(nx, ny)]) continue;
					seen[idx(nx, ny)] = 1; shore[idx(nx, ny)] = shore[idx(x, y)]; q.push_back({nx, ny});
				}
			}
		}
		for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
			Cell& c = cells[idx(x, y)];
			if (c.kind == floor) { c.h = (float)(c.level * level_h); continue; }
			if (c.kind != obstacle) { // water a little below its shore's level; the void far below the platforms
				c.level = shore[idx(x, y)];
				c.h = (float)(c.kind == water ? c.level * level_h - 6 : c.level * level_h - 4 * level_h);
				continue;
			}
			int top = -1, n_levels = 0, nearest = -1, nearest_d = cap;
			for (int l = 0; l < 3; ++l) {
				int d = near[l][idx(x, y)];
				if (d <= kBand) { top = l; ++n_levels; }
				if (d < nearest_d) { nearest_d = d; nearest = l; }
			}
			if (n_levels >= 2) { c.kind = cliff; c.level = (uint8_t)top; c.h = (float)(top * level_h); }
			else {
				int l = top >= 0 ? top : nearest >= 0 ? nearest : 0;
				c.level = (uint8_t)l; c.h = (float)(l * level_h + ((c.flags & very_high) ? 1.5 : 0.5) * level_h);
			}
		}

		// Ramps: walkable cells within kRamp walkable steps of a walkable cell of another level blend toward it, half way at
		// the boundary, so both sides meet. BFS over walkable cells from every level boundary.
		std::vector<uint8_t> dist((size_t)w * h, cap); std::vector<uint8_t> other((size_t)w * h, 0);
		std::deque<std::pair<int, int>> q;
		const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
		for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
			const Cell& c = cells[idx(x, y)]; if (c.kind != floor) continue;
			for (int k = 0; k < 4; ++k) {
				int nx = x + dx[k], ny = y + dy[k];
				if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
				const Cell& n = cells[idx(nx, ny)];
				if (n.kind == floor && n.level != c.level) { dist[idx(x, y)] = 0; other[idx(x, y)] = n.level; q.push_back({x, y}); break; }
			}
		}
		while (!q.empty()) {
			auto [x, y] = q.front(); q.pop_front(); int d = dist[idx(x, y)];
			if (d + 1 >= kRamp) continue;
			for (int k = 0; k < 4; ++k) {
				int nx = x + dx[k], ny = y + dy[k];
				if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
				const Cell& n = cells[idx(nx, ny)];
				if (n.kind != floor || n.level != cells[idx(x, y)].level || dist[idx(nx, ny)] <= d + 1) continue;
				dist[idx(nx, ny)] = (uint8_t)(d + 1); other[idx(nx, ny)] = other[idx(x, y)]; q.push_back({nx, ny});
			}
		}
		for (size_t i = 0; i < cells.size(); ++i) {
			if (dist[i] >= kRamp) continue;
			Cell& c = cells[i];
			double t = 0.5 * (1.0 - (dist[i] + 0.5) / kRamp); // 0.5 - a little at the boundary cell, 0 at kRamp
			c.h = (float)((c.level + (other[i] - (int)c.level) * t) * level_h);
			c.flags |= ramp;
		}
	}
};

} // namespace fp
