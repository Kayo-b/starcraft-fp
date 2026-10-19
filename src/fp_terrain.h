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
//
// Shape (what the renderer draws, build_shape): floors are one continuous surface, with heights at cell corners (the mean
// of the floor cells sharing the corner) interpolated inside each cell, so ramps are smooth slopes rather than steps.
// Walls follow StarCraft's isometric edges (the 64x32 diamond grid's 2:1 diagonals, which the 8 px grid turns into 2x1
// staircases): the solid/open field is smoothed and contoured per cell (marching squares), and a cell the contour
// crosses holds that straight line and what lies on each side of it. A cell keeps its own kind at its centre, so thin
// features survive; collision stays per cell (OpenBW's walkability).
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
	static constexpr int kRamp = 8;       // half-length of a ramp blend (minitiles): a level's rise over ~2 levels' length
	double level_h = 64;                  // px per level (a prototype choice; RenderParams::level_h explains it)

	int w = 0, h = 0;
	std::vector<Cell> cells;

	// A straight wall through a cell: points p (cell units, 0..1 inside the cell) with nx*u + ny*v > c are side 1 (solid),
	// the others side 0 (open); kind and height of each side. (nx, ny) has unit length and points into the solid side.
	struct Split { float nx, ny, c; uint8_t kind[2]; float h[2]; };
	struct Side { uint8_t kind; float h; };
	std::vector<float> corner_h;   // (w+1)*(h+1) floor heights at cell corners; NaN where no floor cell touches the corner
	std::vector<int32_t> split_of; // per cell: index into splits, or -1
	std::vector<Split> splits;
	static bool is_solid(uint8_t k) { return k == cliff || k == obstacle; }

	const Split* split(int cx, int cy) const {
		if (cx < 0 || cy < 0 || cx >= w || cy >= h || split_of.empty()) return nullptr;
		int32_t i = split_of[(size_t)cy * w + cx]; return i < 0 ? nullptr : &splits[i];
	}
	// Floor height inside cell (cx,cy) at (u,v) from its corners; corners without floor take `fallback`.
	double floor_at(int cx, int cy, double u, double v, double fallback) const {
		if (cx < 0 || cy < 0 || cx >= w || cy >= h || corner_h.empty()) return fallback;
		auto ch = [&](int X, int Y) { float f = corner_h[(size_t)Y * (w + 1) + X]; return std::isnan(f) ? fallback : (double)f; };
		double a = ch(cx, cy) + (ch(cx + 1, cy) - ch(cx, cy)) * u, b = ch(cx, cy + 1) + (ch(cx + 1, cy + 1) - ch(cx, cy + 1)) * u;
		return a + (b - a) * v;
	}
	// One side of a split (or the whole cell when side < 0) at (u,v): its kind and drawn height (floors slope).
	Side side_value(int cx, int cy, double u, double v, int side) const {
		const Cell& c = at(cx, cy); const Split* sp = side >= 0 ? split(cx, cy) : nullptr;
		Side r{sp ? sp->kind[side] : c.kind, sp ? sp->h[side] : c.h};
		if (r.kind == floor) r.h = (float)floor_at(cx, cy, u, v, r.h);
		return r;
	}
	// What is drawn at (u,v) of cell (cx,cy).
	Side side_at(int cx, int cy, double u, double v) const {
		const Split* sp = split(cx, cy);
		return side_value(cx, cy, u, v, sp ? (sp->nx * u + sp->ny * v > sp->c) : -1);
	}

	const Cell& at(int cx, int cy) const { static const Cell outside; return (cx < 0 || cy < 0 || cx >= w || cy >= h) ? outside : cells[(size_t)cy * w + cx]; }
	bool walkable(int cx, int cy) const { const Cell& c = at(cx, cy); return c.kind == floor; }
	// Height of the cell under a world position (px); solid cells report their top.
	double height_at(double x, double y) const { return at((int)std::floor(x / kCell), (int)std::floor(y / kCell)).h; }
	// Floor height for eyes and feet: the drawn floor surface (sloped on ramps). Always the floor, even where a wall's
	// contour cuts into a walkable cell, so standing next to a wall never lifts the eye onto it.
	double floor_height(double x, double y) const {
		double fx = x / kCell, fy = y / kCell; int cx = (int)std::floor(fx), cy = (int)std::floor(fy);
		const Cell& c = at(cx, cy);
		return c.kind == floor ? floor_at(cx, cy, fx - cx, fy - cy, c.h) : c.h;
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
		build_shape();
	}

	void build_shape() {
		auto idx = [&](int x, int y) { return (size_t)y * w + x; };
		// floor heights at corners
		corner_h.assign((size_t)(w + 1) * (h + 1), NAN);
		for (int Y = 0; Y <= h; ++Y) for (int X = 0; X <= w; ++X) {
			double sum = 0; int n = 0;
			for (int dy = -1; dy <= 0; ++dy) for (int dx = -1; dx <= 0; ++dx) { const Cell& c = at(X + dx, Y + dy); if (c.kind == floor) { sum += c.h; ++n; } }
			if (n) corner_h[(size_t)Y * (w + 1) + X] = (float)(sum / n);
		}
		// solidness smoothed at corners: binomial weights over the 6 cells on each axis around the corner (cells X-3..X+2),
		// enough to straighten a 2x1 staircase; the map border repeats its cells
		static const float k6[6] = {1 / 32.f, 5 / 32.f, 10 / 32.f, 10 / 32.f, 5 / 32.f, 1 / 32.f};
		auto solid_c = [&](int x, int y) { x = std::max(0, std::min(w - 1, x)); y = std::max(0, std::min(h - 1, y)); return is_solid(cells[idx(x, y)].kind) ? 1.f : 0.f; };
		std::vector<float> rows((size_t)(w + 1) * h), S((size_t)(w + 1) * (h + 1));
		for (int y = 0; y < h; ++y) for (int X = 0; X <= w; ++X) { float a = 0; for (int j = 0; j < 6; ++j) a += k6[j] * solid_c(X - 3 + j, y); rows[(size_t)y * (w + 1) + X] = a; }
		for (int Y = 0; Y <= h; ++Y) for (int X = 0; X <= w; ++X) { float a = 0; for (int j = 0; j < 6; ++j) a += k6[j] * rows[(size_t)std::max(0, std::min(h - 1, Y - 3 + j)) * (w + 1) + X]; S[(size_t)Y * (w + 1) + X] = a; }
		auto Sc = [&](int X, int Y) { return S[(size_t)Y * (w + 1) + X]; };
		// contour each cell at 0.5
		split_of.assign((size_t)w * h, -1); splits.clear();
		for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
			float s00 = Sc(x, y), s10 = Sc(x + 1, y), s01 = Sc(x, y + 1), s11 = Sc(x + 1, y + 1);
			bool b00 = s00 >= 0.5f, b10 = s10 >= 0.5f, b01 = s01 >= 0.5f, b11 = s11 >= 0.5f;
			if (b00 == b10 && b00 == b01 && b00 == b11) continue;
			double pu[4], pv[4]; int n = 0;
			auto cross = [&](float sa, float sb) { return (0.5 - sa) / (sb - sa); };
			if (b00 != b10) { pu[n] = cross(s00, s10); pv[n++] = 0; }
			if (b10 != b11) { pu[n] = 1; pv[n++] = cross(s10, s11); }
			if (b01 != b11) { pu[n] = cross(s01, s11); pv[n++] = 1; }
			if (b00 != b01) { pu[n] = 0; pv[n++] = cross(s00, s01); }
			if (n != 2) continue; // saddle: keep the cell as it is
			double du = pu[1] - pu[0], dv = pv[1] - pv[0], len = std::hypot(du, dv);
			if (len < 1e-6) continue;
			double nx = -dv / len, ny = du / len, c = nx * pu[0] + ny * pv[0];
			double qu = b00 ? 0 : b10 ? 1 : b01 ? 0 : 1, qv = b00 ? 0 : b10 ? 0 : b01 ? 1 : 1; // a solid corner
			if (nx * qu + ny * qv < c) { nx = -nx; ny = -ny; c = -c; }
			const Cell& self = cells[idx(x, y)];
			bool own_solid = is_solid(self.kind);
			if ((nx * 0.5 + ny * 0.5 > c) != own_solid) continue; // the contour would move the cell's centre: keep the cell
			// the other side: the nearest neighbour of the other kind whose centre lies on that side of the line
			int best = -1; double bd = 1e9;
			for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
				if (!dx && !dy) continue;
				const Cell& nb = at(x + dx, y + dy);
				if (nb.kind == edge || is_solid(nb.kind) == own_solid) continue;
				double f = nx * (dx + 0.5) + ny * (dy + 0.5) - c;
				if ((f > 0) != !own_solid) continue;
				double d = std::fabs(f) + 0.01 * (std::abs(dx) + std::abs(dy)); // ties: prefer edge neighbours
				if (d < bd) { bd = d; best = (int)idx(x + dx, y + dy); }
			}
			if (best < 0) continue;
			const Cell& other = cells[best];
			Split sp{(float)nx, (float)ny, (float)c, {}, {}};
			int so = own_solid ? 1 : 0;
			sp.kind[so] = self.kind; sp.h[so] = self.h; sp.kind[1 - so] = other.kind; sp.h[1 - so] = other.h;
			split_of[idx(x, y)] = (int32_t)splits.size(); splits.push_back(sp);
		}
	}
};

} // namespace fp
