// Software first-person renderer over OpenBW simulation state (read-only).
// Rendering parameters (wall height, eye height, FOV) are design choices of this prototype, not StarCraft facts.
#pragma once
#include "bwgame.h"
#include "actions.h"
#include "replay.h"
#include "fp_assets.h"
#include "fp_terrain.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace fp {
using namespace bwgame;

struct Camera {
	double x = 0, y = 0;
	double heading = 0; // degrees; 0 = +x (east), 90 = +y (south)
	double pitch = 0;   // horizon shift in pixels of the render target (+ = look down)
};

struct UnitView {
	double x, y, r, hgt; // position (px), half-width, bounding box height
	int owner, type, image, frame; bool flip;
	int heading; bool directional; int frame_base;
	const unit_t* ptr;
	double hp_frac = 1, shield_frac = 0; // 0..1 of the unit type's maximum; shield_frac is 0 for units without shields
};

// What to highlight on top of the scene.
struct Overlay {
	std::vector<const unit_t*> selected;
	const unit_t* hover = nullptr;
	uint32_t crosshair = 0xFFFFFFFF; // 0xAARRGGBB
};

// Result of looking along the crosshair.
struct Pick {
	const unit_t* unit = nullptr;       // sprite under the crosshair (nearest, opaque pixel), if any
	bool ground = false; double gx = 0, gy = 0; // floor point under the crosshair, if the crosshair is on visible floor
};

// eye_h: eye above the floor under the camera; view_dist: how far rays go before the fog has closed in; level_h: height of
// one terrain level (all px). StarCraft draws one level's cliff face ~40-64 px tall onto the map (the south-facing cliff
// band is that much deeper than the north-facing one), so a level is 64 px rather than the tile size.
struct RenderParams { double fov = 66, eye_h = 12, view_dist = 3000, level_h = 64; };

// Default StarCraft player colours (red, blue, teal, purple, orange, brown, white, yellow); neutral/others grey.
static inline void player_rgb(int owner, double out[3]) {
	static const uint8_t c[8][3] = {{244, 4, 4}, {12, 72, 204}, {44, 180, 148}, {136, 64, 156}, {248, 140, 20}, {112, 48, 20}, {204, 224, 208}, {252, 252, 56}};
	if (owner >= 0 && owner < 8) { out[0] = c[owner][0]; out[1] = c[owner][1]; out[2] = c[owner][2]; } else { out[0] = out[1] = out[2] = 150; }
}

class Renderer {
public:
	static constexpr double kCell = 8.0; // minitile size in pixels

	Renderer(state& st, state_functions& fn, Assets& assets, int w, int h, RenderParams p = {})
		: st_(st), gs_(*st.game), fn_(fn), assets_(assets), W_(w), H_(h), p_(p), tiles_(assets.tiles(gs_.tileset_index)) {
		assets_.set_tileset(gs_.tileset_index);
		terrain_.level_h = p_.level_h;
		rebuild_terrain();
		{ // the map's average colour, for the ground plane beyond the mirrored strip
			double a[3] = {0, 0, 0}; int n = 0;
			for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) { uint8_t t[3]; tile_sample((x + 0.5) * gs_.map_width / 64.0, (y + 0.5) * gs_.map_height / 64.0, t); for (int k = 0; k < 3; ++k) a[k] += t[k]; ++n; }
			for (int k = 0; k < 3; ++k) avg_[k] = a[k] / n;
		}
	}
	int width() const { return W_; }
	int height() const { return H_; }
	const RenderParams& params() const { return p_; }
	const std::vector<UnitView>& units() const { return units_; }
	double map_w() const { return (double)gs_.map_width; }
	double map_h() const { return (double)gs_.map_height; }

	// The heightfield (fp_terrain.h), and from it the renderer's grid: 0 = wall (anything not walkable floor), 1..3 = floor
	// level + 1. Computed once per map load; creep-induced walkability changes during play are not tracked.
	void rebuild_terrain() {
		terrain_.build(st_, fn_);
		mw_ = terrain_.w; mh_ = terrain_.h;
		grid_.assign((size_t)mw_ * mh_, 0);
		for (int y = 0; y < mh_; ++y) for (int x = 0; x < mw_; ++x)
			if (terrain_.walkable(x, y)) grid_[(size_t)y * mw_ + x] = (uint8_t)(terrain_.at(x, y).level + 1);
	}
	const Terrain& terrain() const { return terrain_; }
	int cell_at(int cx, int cy) const { return (cx < 0 || cy < 0 || cx >= mw_ || cy >= mh_) ? 0 : grid_[(size_t)cy * mw_ + cx]; }
	bool can_stand(double x, double y, double r) const {
		for (int dy = -1; dy <= 1; dy += 2) for (int dx = -1; dx <= 1; dx += 2)
			if (cell_at((int)std::floor((x + dx * r) / kCell), (int)std::floor((y + dy * r) / kCell)) == 0) return false;
		return true;
	}

	// Snapshot visible units and preload their sprites (must run before render(); render() only reads).
	void update_units() {
		units_.clear();
		for (auto u : ptr(st_.visible_units)) {
			auto& d = u->unit_type->dimensions; auto* mi = u->sprite->main_image;
			units_.push_back({(double)u->sprite->position.x, (double)u->sprite->position.y, (d.from.x + d.to.x + 1) / 2.0, (double)(d.from.y + d.to.y + 1),
				u->owner, (int)u->unit_type->id, (int)mi->image_type->id, (int)mi->frame_index, (mi->flags & image_t::flag_horizontally_flipped) != 0,
				(int)fn_.direction_index(u->heading), (mi->flags & image_t::flag_has_directional_frames) != 0, (int)mi->frame_index_base, u});
			{ auto& b = units_.back(); double mh = u->unit_type->hitpoints.raw_value ? u->unit_type->hitpoints.raw_value / 256.0 : 0, ms = u->unit_type->shield_points;
			  if (mh > 0) b.hp_frac = std::max(0.0, std::min(1.0, (u->hp.raw_value / 256.0) / mh));
			  if (u->unit_type->has_shield && ms > 0) b.shield_frac = std::max(0.0, std::min(1.0, (u->shield_points.raw_value / 256.0) / ms)); }
			assets_.sprite((int)mi->image_type->id);
		}
	}

	// Beyond the map edge the world continues as the map's mirror image (darkened by the renderer), so the ground does
	// not stop at the border: things standing near it would otherwise stand against the sky.
	static int mirror_index(int c, int n) { int m = ((c % (2 * n)) + 2 * n) % (2 * n); return m < n ? m : 2 * n - 1 - m; }
	static double mirror_coord(double v, double n) { double m = std::fmod(v, 2 * n); if (m < 0) m += 2 * n; return m < n ? m : std::max(0.0, 2 * n - m - 1e-6); }
	bool outside_px(double x, double y) const { return x < 0 || y < 0 || x >= gs_.map_width || y >= gs_.map_height; }
	void world_sample(double fx, double fy, uint8_t out[3]) const {
		if (!outside_px(fx, fy)) tile_sample(fx, fy, out);
		else tile_sample(mirror_coord(fx, gs_.map_width), mirror_coord(fy, gs_.map_height), out);
	}
	void tile_sample(double fx, double fy, uint8_t out[3], bool clamp = false) const {
		int tx = (int)std::floor(fx / 32), ty = (int)std::floor(fy / 32);
		if (clamp) { tx = std::max(0, std::min((int)gs_.map_tile_width - 1, tx)); ty = std::max(0, std::min((int)gs_.map_tile_height - 1, ty)); }
		if (tx < 0 || ty < 0 || tx >= (int)gs_.map_tile_width || ty >= (int)gs_.map_tile_height) { out[0] = out[1] = out[2] = 0; return; }
		size_t mt = st_.tiles_mega_tile_index[(size_t)ty * gs_.map_tile_width + tx];
		tiles_->sample(mt, (int)((fx - tx * 32) * 4), (int)((fy - ty * 32) * 4), out);
	}

	// Render into fb (W*H, 0xFFRRGGBB). `hide` (may be null) is not drawn (the unit the camera sits in).
	void render(const Camera& cam, const unit_t* hide, uint32_t* fb, const Overlay* ov = nullptr, unsigned threads = 0) {
		View v = view(cam);
		zbuf_.assign((size_t)W_ * H_, kFar);
		std::vector<Vis> vis = visible(cam, hide, v);
		std::sort(vis.begin(), vis.end(), [](const Vis& a, const Vis& b) { return a.depth > b.depth; });

		auto band = [&](int xa, int xb) {
			for (int sx = xa; sx < xb; ++sx) column(cam, sx, v, fb);
			for (auto& vi : vis) sprite(cam, units_[vi.idx], vi.depth, vi.sx, v, fb, xa, xb);
		};
		unsigned n = threads ? threads : std::max(1u, std::min(12u, std::thread::hardware_concurrency()));
		if (n <= 1) { band(0, W_); }
		else {
			std::vector<std::thread> th;
			for (unsigned t = 0; t < n; ++t) { int xa = (int)((long)W_ * t / n), xb = (int)((long)W_ * (t + 1) / n); th.emplace_back(band, xa, xb); }
			for (auto& t : th) t.join();
		}
		if (ov) for (auto& vi : vis) {
			const UnitView& un = units_[vi.idx]; bool sel = std::find(ov->selected.begin(), ov->selected.end(), un.ptr) != ov->selected.end();
			if (sel || un.ptr == ov->hover) mark(cam, un, vi.depth, vi.sx, v, fb, sel);
		}
		const uint32_t xc = ov ? ov->crosshair : 0xFFFFFFFF;
		for (int d = -6; d <= 6; ++d) { if (std::abs(d) < 2) continue; fb[(size_t)(H_ / 2) * W_ + W_ / 2 + d] = xc; fb[(size_t)(H_ / 2 + d) * W_ + W_ / 2] = xc; }
	}

	// Aim assist: the nearest-to-the-crosshair unit passing `want` whose billboard centre is within max_px of the crosshair
	// and not hidden behind a wall (depth buffer of the most recent render()). Returns nullptr if none.
	template<typename F> const unit_t* nearest_to_crosshair(const Camera& cam, const unit_t* hide, double max_px, F&& want) const {
		View v = view(cam); const unit_t* best = nullptr; double bd = max_px;
		for (auto& vi : visible(cam, hide, v)) {
			const UnitView& un = units_[vi.idx]; if (!want(un.ptr)) continue;
			Draw d = geometry(cam, un, vi.depth, vi.sx, v);
			if (vi.depth > depth_at((int)vi.sx, (d.y0 + d.y1) / 2)) continue; // its centre is behind terrain
			double dx = vi.sx - W_ / 2.0, dy = (d.y0 + d.y1) / 2.0 - H_ / 2.0, dist = std::hypot(dx, dy);
			if (dist < bd) { bd = dist; best = un.ptr; }
		}
		return best;
	}

	// What the crosshair is on. Uses the depth buffer of the most recent render(), so call it right after render().
	Pick pick_center(const Camera& cam, const unit_t* hide) const {
		Pick pk; View v = view(cam); const int cx = W_ / 2, cy = H_ / 2;
		std::vector<Vis> vis = visible(cam, hide, v);
		std::sort(vis.begin(), vis.end(), [](const Vis& a, const Vis& b) { return a.depth < b.depth; }); // near -> far
		for (auto& vi : vis) {
			if (vi.depth > depth_at(cx, cy)) break; // everything further is behind the terrain at the crosshair
			Draw d = geometry(cam, units_[vi.idx], vi.depth, vi.sx, v);
			if (cx < d.x0 || cx >= d.x1 || cy < d.y0 || cy >= d.y1) continue;
			if (d.sp) { // ignore transparent pixels so you cannot select through gaps in a sprite
				const auto& fr = d.sp->frames[d.frame];
				double u = (cx - d.x0 + 0.5) / std::max(1.0, d.wpx); if (d.flip) u = 1.0 - u;
				double vv = (cy - d.y0 + 0.5) / std::max(1.0, d.hpx);
				uint8_t t[4]; d.sp->texel(fr.x + (int)(u * fr.w), fr.y + (int)(vv * fr.h), t);
				if (t[3] < 128) continue;
			}
			pk.unit = units_[vi.idx].ptr; break;
		}
		// the terrain point under the crosshair (the centre column's ray is the view direction); floor only
		double dc = depth_at(cx, cy);
		if (dc < kFar) {
			double gx = cam.x + v.dirx * dc, gy = cam.y + v.diry * dc;
			if (terrain_.walkable((int)std::floor(gx / kCell), (int)std::floor(gy / kCell))) { pk.ground = true; pk.gx = gx; pk.gy = gy; }
		}
		return pk;
	}

private:
	struct View { double dirx, diry, planex, planey, proj, horizon; double eye_h; }; // eye_h: eye height above level-0 ground (px)
	struct Vis { double depth, sx; int idx; };
	struct Draw { const AnimSprite* sp; int frame; bool flip; int x0, x1, y0, y1; double wpx, hpx; };
	static uint32_t pack(double r, double g, double b) { return 0xFF000000u | ((uint32_t)std::min(255.0, r) << 16) | ((uint32_t)std::min(255.0, g) << 8) | (uint32_t)std::min(255.0, b); }

	static constexpr float kFar = 1e30f; // depth of sky and fog pixels
	// The nearest of the isometric grid's normals: the axes and the normals of its 2:1 diagonals.
	static void iso_snap(double nx, double ny, double& sx, double& sy) {
		static const double k = 1 / 2.2360679774997896;
		static const double dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {k, 2 * k}, {-k, 2 * k}, {k, -2 * k}, {-k, -2 * k}};
		double best = -2; sx = nx; sy = ny;
		for (auto& d : dirs) { double dot = d[0] * nx + d[1] * ny; if (dot > best) { best = dot; sx = d[0]; sy = d[1]; } }
	}
	float depth_at(int x, int y) const { x = std::max(0, std::min(W_ - 1, x)); y = std::max(0, std::min(H_ - 1, y)); return zbuf_[(size_t)y * W_ + x]; }

	// One screen column: march the ray through the heightfield front to back. A cell's top is drawn where it lies below
	// the eye, its front face where it rises above the cell before; `top` is the highest row drawn so far, so nearer
	// terrain hides what is behind it. Rays go on past cliffs up to view_dist: high ground shows its faces at full
	// height from below, and low ground shows from above. Every pixel gets its depth (zbuf_) for sprites and picking.
	void column(const Camera& cam, int sx, const View& v, uint32_t* fb) {
		double camx = 2.0 * sx / W_ - 1.0, rdx = v.dirx + v.planex * camx, rdy = v.diry + v.planey * camx;
		double gx = cam.x / kCell, gy = cam.y / kCell;
		int mx = (int)std::floor(gx), my = (int)std::floor(gy);
		double ddx = rdx == 0 ? 1e30 : std::fabs(1.0 / rdx), ddy = rdy == 0 ? 1e30 : std::fabs(1.0 / rdy);
		int stx = rdx < 0 ? -1 : 1, sty = rdy < 0 ? -1 : 1;
		double sdx = (rdx < 0 ? gx - mx : mx + 1.0 - gx) * ddx, sdy = (rdy < 0 ? gy - my : my + 1.0 - gy) * ddy;
		const double eh = v.eye_h, proj = v.proj, hz = v.horizon, maxd = p_.view_dist;
		auto row_at = [&](double d, double h) { return hz + (eh - h) * proj / std::max(d, 0.5); };
		auto first_row = [](double r) { return (int)std::ceil(r - 0.5); }; // first pixel row whose centre is at or below r
		int top = H_;
		const double inv_maxd = 1.0 / maxd, fog_from = 0.4 * maxd, inv_fog = 1.0 / (0.6 * maxd);
		auto put = [&](int sy, double d, double r, double g, double b) { // distance shading, then fog toward the horizon colour
			double shade = 1.0 - 0.4 * std::min(1.0, d * inv_maxd);
			size_t i = (size_t)sy * W_ + sx;
			if (d <= fog_from) fb[i] = pack(r * shade, g * shade, b * shade);
			else {
				double f = std::min(1.0, (d - fog_from) * inv_fog), k = shade * (1 - f);
				fb[i] = pack(r * k + kFog[0] * f, g * k + kFog[1] * f, b * k + kFog[2] * f);
			}
			zbuf_[i] = (float)d;
		};
		// A cell's top between ray distances d0..d1, rising linearly h0 -> h1 (floors slope on ramps). Each row's distance
		// is solved exactly on that line; seen from behind (farther points lower on screen) it is not drawn.
		auto surface = [&](double h0, double h1, double d0, double d1) {
			d0 = std::max(d0, 0.5); if (d1 <= d0) return;
			double ra = row_at(d0, h0), rb = row_at(d1, h1);
			if (rb >= ra) return;
			double k = (h1 - h0) / (d1 - d0);
			int r0 = std::max(0, first_row(rb)), r1 = std::min(top, first_row(ra));
			for (int sy = r0; sy < r1; ++sy) {
				double den = sy + 0.5 - hz + k * proj; if (den <= 1e-9) continue;
				double d = std::max(d0, std::min(d1, (eh - h0 + k * d0) * proj / den));
				double px = cam.x + rdx * d, py = cam.y + rdy * d; uint8_t t[3]; world_sample(px, py, t);
				double k = outside_px(px, py) ? kOutside : 1.0;
				put(sy, d, t[0] * k, t[1] * k, t[2] * k);
			}
			if (r0 < r1) top = r0;
		};
		// A face rising h0 -> h1 at distance d; (nx, ny) is its outward normal (toward the camera).
		auto face = [&](double h0, double h1, double d, double nx, double ny) {
			double rt = row_at(d, h1), rb = row_at(d, h0);
			int r0 = std::max(0, first_row(rt)), r1 = std::min(top, first_row(rb));
			if (r0 >= r1) return;
			double hx = cam.x + rdx * d, hy = cam.y + rdy * d, light = 0.8 + 0.2 * ny + 0.05 * nx; // the art is lit for south-facing faces
			double snx, sny; iso_snap(nx, ny, snx, sny);
			for (int sy = r0; sy < r1; ++sy) {
				double hp = h0 + (h1 - h0) * (rb - (sy + 0.5)) / std::max(1e-6, rb - rt); // terrain height at this pixel
				// Stand-in until M3 maps the cliff band: the face's art is the solid side's art read inward, deeper for
				// higher points (StarCraft draws a cliff's face onto the ground beside its foot). Inward is the face normal
				// snapped to the isometric directions, so every cell along a straight wall reads the art the same way.
				double into = (hp - h0) + 0.5;
				uint8_t t[3]; world_sample(hx - snx * into, hy - sny * into, t);
				double k = (0.62 + 0.38 * (hp - h0) / std::max(1e-6, h1 - h0)) * light * (outside_px(hx, hy) ? kOutside : 1.0); // darker at the foot
				put(sy, d, t[0] * k, t[1] * k, t[2] * k);
			}
			top = r0;
		};
		// Draw cell (cx,cy) between ray parameters ta..tb (cell units); a split cell is two pieces with its wall between.
		// Returns the drawn height where the ray leaves the cell.
		auto mirrored = [&](int cx, int cy) -> const Terrain::Cell& { return terrain_.at(mirror_index(cx, terrain_.w), mirror_index(cy, terrain_.h)); };
		auto span = [&](int cx, int cy, double ta, double tb) -> double {
			bool out = cx < 0 || cy < 0 || cx >= terrain_.w || cy >= terrain_.h;
			const Terrain::Cell& cell = out ? mirrored(cx, cy) : terrain_.at(cx, cy);
			if (out || (cell.flags & Terrain::flat)) { surface(cell.h, cell.h, ta * kCell, tb * kCell); return cell.h; } // the mirror is drawn flat-topped
			double ua = gx + rdx * ta - cx, va = gy + rdy * ta - cy, ub = gx + rdx * tb - cx, vb = gy + rdy * tb - cy;
			const Terrain::Split* sp = terrain_.split(cx, cy);
			int sa = sp ? (sp->nx * ua + sp->ny * va > sp->c) : -1, sb = sp ? (sp->nx * ub + sp->ny * vb > sp->c) : -1;
			if (sa == sb) {
				double h0 = terrain_.side_value(cx, cy, ua, va, sa).h, h1 = terrain_.side_value(cx, cy, ub, vb, sa).h;
				surface(h0, h1, ta * kCell, tb * kCell);
				return h1;
			}
			double fa = sp->nx * ua + sp->ny * va - sp->c, fb = sp->nx * ub + sp->ny * vb - sp->c;
			double th = ta + (tb - ta) * fa / (fa - fb), uh = gx + rdx * th - cx, vh = gy + rdy * th - cy;
			double a0 = terrain_.side_value(cx, cy, ua, va, sa).h, a1 = terrain_.side_value(cx, cy, uh, vh, sa).h;
			double b0 = terrain_.side_value(cx, cy, uh, vh, sb).h, b1 = terrain_.side_value(cx, cy, ub, vb, sb).h;
			surface(a0, a1, ta * kCell, th * kCell);
			if (b0 > a1) { double s = sa == 1 ? 1 : -1; face(a1, b0, th * kCell, s * sp->nx, s * sp->ny); } // normal toward the side we came from
			surface(b0, b1, th * kCell, tb * kCell);
			return b1;
		};
		double t_in = 0, h_last = 0; int out_steps = 0; bool left = false;
		for (int step = 0; step < 16384 && top > 0; ++step) {
			double t_out = std::min(sdx, sdy);
			double h_exit = span(mx, my, t_in, std::min(t_out, maxd / kCell)); h_last = h_exit;
			if (t_out * kCell >= maxd) break;
			int pmx = mx, pmy = my;
			if (sdx < sdy) { sdx += ddx; mx += stx; } else { sdy += ddy; my += sty; }
			bool out = mx < 0 || my < 0 || mx >= terrain_.w || my >= terrain_.h;
			if (out && ++out_steps > kMirrorCells) { left = true; t_in = t_out; break; } // the plane below takes over
			const Terrain::Cell& nc = out ? mirrored(mx, my) : terrain_.at(mx, my);
			double h_entry = (out || (nc.flags & Terrain::flat)) ? nc.h : terrain_.side_at(mx, my, gx + rdx * t_out - mx, gy + rdy * t_out - my).h;
			if (h_entry > h_exit) face(h_exit, h_entry, t_out * kCell, pmx - mx, pmy - my);
			t_in = t_out;
		}
		// Past the mirrored strip: a plain ground plane at the last height, in the map's average colour, fogged.
		if (left) {
			int r0 = std::max(0, first_row(row_at(maxd, h_last))), r1 = std::min(top, first_row(row_at(t_in * kCell, h_last)));
			for (int sy = r0; sy < r1; ++sy) {
				double den = sy + 0.5 - hz; if (den <= 1e-9) continue;
				put(sy, std::max(t_in * kCell, (eh - h_last) * proj / den), avg_[0] * kOutside, avg_[1] * kOutside, avg_[2] * kOutside);
			}
			if (r0 < r1) top = r0;
		}
		// rows nothing covered: sky above the horizon, fog below it (beyond the view distance)
		for (int sy = 0; sy < top; ++sy) {
			double t = std::max(0.0, std::min(1.0, sy / std::max(1.0, hz)));
			size_t i = (size_t)sy * W_ + sx;
			fb[i] = sy < hz ? pack(40 + (kFog[0] - 40) * t, 50 + (kFog[1] - 50) * t, 90 + (kFog[2] - 90) * t) : pack(kFog[0], kFog[1], kFog[2]);
			zbuf_[i] = kFar;
		}
	}
	static constexpr double kFog[3] = {100, 120, 170}; // the sky's colour at the horizon
	static constexpr double kOutside = 0.55;           // brightness of the mirrored world beyond the map edge
	static constexpr int kMirrorCells = 96;            // how far rays draw the mirror past the edge (cells) before the plain plane


	View view(const Camera& cam) const {
		const double PI = 3.14159265358979323846, fov = p_.fov * PI / 180.0, hd = cam.heading * PI / 180.0;
		View v; v.dirx = std::cos(hd); v.diry = std::sin(hd);
		v.planex = -v.diry * std::tan(fov / 2); v.planey = v.dirx * std::tan(fov / 2);
		v.proj = (W_ / 2.0) / std::tan(fov / 2); v.horizon = H_ / 2.0 + cam.pitch;
		v.eye_h = p_.eye_h + terrain_.floor_height(cam.x, cam.y); // the eye follows the terrain under the camera (ramps too)
		return v;
	}
	std::vector<Vis> visible(const Camera& cam, const unit_t* hide, const View& v) const {
		std::vector<Vis> vis;
		for (size_t i = 0; i < units_.size(); ++i) {
			if (units_[i].ptr == hide) continue;
			double rx = units_[i].x - cam.x, ry = units_[i].y - cam.y;
			double depth = rx * v.dirx + ry * v.diry, lateral = rx * (-v.diry) + ry * v.dirx;
			if (depth < 4) continue;
			vis.push_back({depth, W_ / 2.0 + lateral / depth * v.proj, (int)i});
		}
		return vis;
	}
	// Screen rectangle and frame of a unit's billboard (shared by drawing, picking and overlays).
	Draw geometry(const Camera& cam, const UnitView& un, double depth, double sxc, const View& v) const {
		const double PI = 3.14159265358979323846;
		Draw d{}; d.sp = assets_.sprite_cached(un.image); d.frame = un.frame; d.flip = un.flip;
		if (d.sp && un.directional) { // OpenBW direction->frame rule, with heading relative to the camera bearing
			double bearing = std::atan2(un.x - cam.x, -(un.y - cam.y)) * 256.0 / (2 * PI);
			int rel = (((un.heading - (int)std::lround(bearing)) % 256) + 256) % 256;
			int off = (rel + 4) / 8; d.flip = false; if (off > 16) { off = 32 - off; d.flip = true; }
			d.frame = un.frame_base + off;
		}
		if (d.sp && (d.frame < 0 || d.frame >= (int)d.sp->frames.size())) d.sp = nullptr;
		double base = v.horizon + (v.eye_h - terrain_.floor_height(un.x, un.y)) * v.proj / depth; // the floor the unit stands on
		if (d.sp) {
			// The art stands on the floor at the unit's position: the sprite's median frame bottom is its ground line,
			// the same for all frames so animations do not bob. (The original renderer centres the box on the
			// position; drawn upright, that sinks the lower half of the art into the floor.)
			const auto& fr = d.sp->frames[d.frame];
			const double sf = 0.25; // HD pixels are 4x SD
			double cx = (fr.xoff + fr.w / 2.0 - d.sp->box_w / 2.0) * sf; // horizontal anchor offset (world px)
			double top = (fr.yoff - d.sp->ground) * sf;                  // frame top relative to the ground (negative = above)
			double bot = top + fr.h * sf;                                // frame bottom relative to ground
			d.wpx = fr.w * sf * v.proj / depth; d.hpx = fr.h * sf * v.proj / depth;
			d.x0 = (int)(sxc + cx * v.proj / depth - d.wpx / 2);
			d.x1 = (int)(sxc + cx * v.proj / depth + d.wpx / 2);
			d.y0 = (int)(base + top * v.proj / depth);
			d.y1 = (int)(base + bot * v.proj / depth);
		} else { d.wpx = un.r * 2 * v.proj / depth; d.hpx = un.hgt * v.proj / depth;
			d.x0 = (int)(sxc - d.wpx / 2); d.x1 = (int)(sxc + d.wpx / 2); d.y0 = (int)(base - d.hpx); d.y1 = (int)base; }
		return d;
	}


	void sprite(const Camera& cam, const UnitView& un, double depth, double sxc, const View& v, uint32_t* fb, int xa, int xb) {
		Draw d = geometry(cam, un, depth, sxc, v);
		if (d.sp) {
			const auto& fr = d.sp->frames[d.frame];
			double shade = std::max(0.4, 1.0 - depth / 1400.0), pc[3]; player_rgb(un.owner, pc);
			for (int x = std::max(xa, d.x0); x < std::min(xb, d.x1); ++x) {
				double u = (x - d.x0 + 0.5) / std::max(1.0, d.wpx); if (d.flip) u = 1.0 - u;
				for (int y = std::max(0, d.y0); y < std::min(H_, d.y1); ++y) {
					if (depth > zbuf_[(size_t)y * W_ + x]) continue; // behind terrain at this pixel
					double vv = (y - d.y0 + 0.5) / std::max(1.0, d.hpx);
					int tx = fr.x + (int)(u * fr.w), ty = fr.y + (int)(vv * fr.h);
					uint8_t t[4]; d.sp->texel(tx, ty, t);
					if (t[3] < 128) continue;
					double r = t[0], g = t[1], b = t[2];
					if (un.owner < 8) if (int m = d.sp->team_mask(tx, ty)) { // blend toward the player colour, keeping the diffuse shading
						double k = m / 255.0, lum = (0.3 * r + 0.59 * g + 0.11 * b) / 255.0 * 1.6;
						r = r * (1 - k) + pc[0] * lum * k; g = g * (1 - k) + pc[1] * lum * k; b = b * (1 - k) + pc[2] * lum * k;
					}
					fb[(size_t)y * W_ + x] = pack(r * shade, g * shade, b * shade);
				}
			}
		} else { // flat fallback in the player colour
			double pc[3]; player_rgb(un.owner, pc); uint32_t c = pack(pc[0], pc[1], pc[2]);
			for (int x = std::max(xa, d.x0); x <= std::min(xb - 1, d.x1); ++x) for (int y = std::max(0, d.y0); y < std::min(H_, d.y1); ++y) if (depth <= zbuf_[(size_t)y * W_ + x]) fb[(size_t)y * W_ + x] = c;
		}
	}

	// Selection brackets and a health/shield bar above a unit.
	void mark(const Camera& cam, const UnitView& un, double depth, double sxc, const View& v, uint32_t* fb, bool selected) {
		Draw d = geometry(cam, un, depth, sxc, v);
		if (depth > depth_at((int)sxc, (d.y0 + d.y1) / 2)) return; // its centre is behind terrain
		auto put = [&](int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < W_ && y < H_) fb[(size_t)y * W_ + x] = c; };
		uint32_t col = selected ? 0xFF40FF40 : 0xFFFF4040;
		int L = std::max(4, (d.x1 - d.x0) / 5);
		for (int i = 0; i < L; ++i) for (int t = 0; t < 2; ++t) { // four corner brackets
			put(d.x0 + i, d.y0 + t, col); put(d.x1 - 1 - i, d.y0 + t, col); put(d.x0 + i, d.y1 - 1 - t, col); put(d.x1 - 1 - i, d.y1 - 1 - t, col);
			put(d.x0 + t, d.y0 + i, col); put(d.x1 - 1 - t, d.y0 + i, col); put(d.x0 + t, d.y1 - 1 - i, col); put(d.x1 - 1 - t, d.y1 - 1 - i, col);
		}
		int bw = std::max(12, d.x1 - d.x0), bx = (int)sxc - bw / 2, by = d.y0 - 6;
		auto bar = [&](int y, double frac, uint32_t fg) { for (int x = 0; x < bw; ++x) for (int t = 0; t < 3; ++t) put(bx + x, y + t, (x < frac * bw) ? fg : 0xFF202020); };
		if (un.shield_frac > 0) { bar(by - 4, un.shield_frac, 0xFF4090FF); }
		bar(by, un.hp_frac, un.hp_frac > 0.66 ? 0xFF40D040 : un.hp_frac > 0.33 ? 0xFFE0C020 : 0xFFE03030);
	}

	state& st_; game_state& gs_; state_functions& fn_; Assets& assets_;
	int W_, H_; RenderParams p_; const HdTiles* tiles_;
	Terrain terrain_; int mw_ = 0, mh_ = 0; std::vector<uint8_t> grid_; double avg_[3] = {0, 0, 0};
	std::vector<UnitView> units_; std::vector<float> zbuf_; // per-pixel depth of the terrain (kFar for sky and fog)
};

} // namespace fp
