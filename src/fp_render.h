// Software first-person renderer over OpenBW simulation state (read-only).
// Rendering parameters (wall height, eye height, FOV) are design choices of this prototype, not StarCraft facts.
#pragma once
#include "bwgame.h"
#include "actions.h"
#include "replay.h"
#include "fp_assets.h"
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

struct RenderParams { double fov = 66, wall_h = 48, eye_h = 12; };

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
		rebuild_terrain();
	}
	int width() const { return W_; }
	int height() const { return H_; }
	const RenderParams& params() const { return p_; }
	const std::vector<UnitView>& units() const { return units_; }
	double map_w() const { return (double)gs_.map_width; }
	double map_h() const { return (double)gs_.map_height; }

	// 0 = wall (unwalkable), 1..4 = height level (low, middle, high, very high). Computed once per map load;
	// creep-induced walkability changes during play are not tracked.
	void rebuild_terrain() {
		mw_ = (int)gs_.map_walk_width; mh_ = (int)gs_.map_walk_height;
		grid_.assign((size_t)mw_ * mh_, 0);
		for (int y = 0; y < mh_; ++y) for (int x = 0; x < mw_; ++x) {
			xy p{x * 8 + 4, y * 8 + 4};
			if (!fn_.is_walkable(p)) continue;
			auto& t = st_.tiles[(size_t)(p.y / 32) * gs_.map_tile_width + (p.x / 32)];
			int lvl = 1;
			if (t.flags & tile_t::flag_very_high) lvl = 4; else if (t.flags & tile_t::flag_high) lvl = 3; else if (t.flags & tile_t::flag_middle) lvl = 2;
			grid_[(size_t)y * mw_ + x] = (uint8_t)lvl;
		}
	}
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
		zbuf_.assign(W_, 1e9);
		std::vector<Vis> vis = visible(cam, hide, v);
		std::sort(vis.begin(), vis.end(), [](const Vis& a, const Vis& b) { return a.depth > b.depth; });

		auto band = [&](int xa, int xb) {
			for (int sx = xa; sx < xb; ++sx) column(cam, sx, v.dirx, v.diry, v.planex, v.planey, v.proj, v.horizon, v.eye_level, fb);
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
			int col = std::max(0, std::min(W_ - 1, (int)vi.sx)); if (vi.depth > zbuf_[col]) continue;
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
			if (vi.depth > zbuf_[cx]) break; // everything further is behind the wall at the crosshair
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
		double below = cy - v.horizon; // rows below the horizon line look at the floor
		if (below >= 1.0) { double rowd = (p_.eye_h * v.proj) / below; if (rowd < zbuf_[cx]) { pk.ground = true; pk.gx = cam.x + v.dirx * rowd; pk.gy = cam.y + v.diry * rowd; } }
		return pk;
	}

private:
	static uint32_t pack(double r, double g, double b) { return 0xFF000000u | ((uint32_t)std::min(255.0, r) << 16) | ((uint32_t)std::min(255.0, g) << 8) | (uint32_t)std::min(255.0, b); }

	void column(const Camera& cam, int sx, double dirx, double diry, double planex, double planey, double proj, double horizon, double eye_level, uint32_t* fb) {
		double camx = 2.0 * sx / W_ - 1.0, rdx = dirx + planex * camx, rdy = diry + planey * camx;
		double gx = cam.x / kCell, gy = cam.y / kCell;
		int mx = (int)std::floor(gx), my = (int)std::floor(gy);
		double ddx = rdx == 0 ? 1e30 : std::fabs(1.0 / rdx), ddy = rdy == 0 ? 1e30 : std::fabs(1.0 / rdy);
		int stx = rdx < 0 ? -1 : 1, sty = rdy < 0 ? -1 : 1;
		double sdx = (rdx < 0 ? gx - mx : mx + 1.0 - gx) * ddx, sdy = (rdy < 0 ? gy - my : my + 1.0 - gy) * ddy;
		int side = 0; bool hit = false; double dist = 0;
		int cur = cell_at(mx, my); int cur_level = cur > 0 ? cur - 1 : 0;
		double seg_start = 0;
		struct Seg { double d0, d1; int level; };
		std::vector<Seg> segs;
		int wall_mx = 0, wall_my = 0, wall_side = 0, level_before = cur_level, prev_mx = mx, prev_my = my;
		for (int step = 0; step < 4096; ++step) {
			prev_mx = mx; prev_my = my;
			if (sdx < sdy) { dist = sdx; sdx += ddx; mx += stx; side = 0; } else { dist = sdy; sdy += ddy; my += sty; side = 1; }
			int c = cell_at(mx, my);
			if (c == 0) { // a wall: the floor segment ends here
				segs.push_back({seg_start, dist, cur_level});
				hit = true; wall_mx = mx; wall_my = my; wall_side = side; level_before = cur_level; break;
			}
			int nl = c - 1;
			if (nl != cur_level) { segs.push_back({seg_start, dist, cur_level}); seg_start = dist; cur_level = nl; }
		}
		if (!hit) segs.push_back({seg_start, 1e9, cur_level});
		double perp = hit ? dist * kCell : 1e9;
		zbuf_[sx] = perp;
		const double eh = p_.eye_h + eye_level * 32.0; // eye height above level-0 ground
		auto row_at = [&](double d, double h) { return horizon + (eh - h * 32.0) * proj / std::max(d, 1.0); };
		// sky first: any row above the horizon not later covered by a floor segment above the eye
		for (int sy = 0; sy < H_; ++sy) {
			double t = std::max(0.0, std::min(1.0, sy / std::max(1.0, horizon)));
			fb[(size_t)sy * W_ + sx] = pack(40 + 60 * t, 50 + 70 * t, 90 + 80 * t);
		}
		// floor segments: for each segment, the rows whose cast distance falls inside it
		for (auto& seg : segs) {
			double d0 = std::max(0.5, seg.d0 * kCell), d1 = std::min(seg.d1 * kCell, 1e9);
			int ra = (int)std::round(row_at(d0, seg.level)), rb = (int)std::round(row_at(d1, seg.level));
			int r0 = std::max(0, std::min(ra, rb)), r1 = std::min(H_ - 1, std::max(ra, rb));
			for (int sy = r0; sy <= r1; ++sy) {
				double d = (eh - seg.level * 32.0) * proj / (sy - horizon);
				if (!(d >= d0 - 1e-6 && d <= d1 + 1e-6)) continue;
				if (d > perp + 1e-6) continue; // beyond the wall
				uint8_t t[3]; tile_sample(cam.x + rdx * d, cam.y + rdy * d, t);
				double shade = std::max(0.35, 1.0 - d / 1400.0);
				fb[(size_t)sy * W_ + sx] = pack(t[0] * shade, t[1] * shade, t[2] * shade);
			}
		}
		// wall / ramp face: one vertical strip from the solid tile's edge art
		if (hit) {
			int wc = cell_at(wall_mx, wall_my);
			bool step_up = wc > 0;                 // walkable higher ground: a ramp step
			int top_level = step_up ? wc - 1 : level_before + 1; // cliffs show one 32px step from below
			double hx = cam.x + rdx * perp, hy = cam.y + rdy * perp;
			double u = wall_side ? hx : hy;        // coordinate along the face
			int r_bot = (int)std::round(row_at(perp, level_before));
			int r_top = (int)std::round(row_at(perp, top_level));
			int span = std::max(1, std::abs(r_bot - r_top));
			for (int sy = std::max(0, std::min(r_top, r_bot)); sy <= std::min(H_ - 1, std::max(r_top, r_bot)); ++sy) {
				// The wall face shows the solid tile's art at the boundary, mapped vertically across the whole tile:
				// the top of the face shows the tile's surface rows, the bottom its edge/shadow rows. That keeps the
				// face structured instead of one stretched colour.
				double v = (double)(sy - std::min(r_top, r_bot)) / span; // 0 = face top, 1 = face bottom
				uint8_t t[3];
				if (wall_side == 0) { // vertical face at x = wall_mx*8
					double fx = step_up ? wall_mx * kCell + 0.5 : wall_mx * kCell - 0.5;
					double tile_y = std::floor(u / 32.0) * 32.0;
					tile_sample(fx, tile_y + v * 31.0, t, true);
				} else {
					double fy = step_up ? wall_my * kCell + 0.5 : wall_my * kCell - 0.5;
					double tile_x = std::floor(u / 32.0) * 32.0;
					tile_sample(tile_x + v * 31.0, fy, t, true);
				}
				// vertical shading: faces darken toward their base; side faces are darker than front-facing ones
				double vshade = 0.62 + 0.38 * (1.0 - v);
				double shade = std::max(0.3, 1.0 - perp / 1000.0) * (wall_side ? 0.75 : 1.0) * vshade;
				fb[(size_t)sy * W_ + sx] = pack(t[0] * shade, t[1] * shade, t[2] * shade);
			}
		}
	}

	struct View { double dirx, diry, planex, planey, proj, horizon; double eye_level; };
	struct Vis { double depth, sx; int idx; };
	struct Draw { const AnimSprite* sp; int frame; bool flip; int x0, x1, y0, y1; double wpx, hpx; };

	View view(const Camera& cam) const {
		const double PI = 3.14159265358979323846, fov = p_.fov * PI / 180.0, hd = cam.heading * PI / 180.0;
		View v; v.dirx = std::cos(hd); v.diry = std::sin(hd);
		v.planex = -v.diry * std::tan(fov / 2); v.planey = v.dirx * std::tan(fov / 2);
		v.proj = (W_ / 2.0) / std::tan(fov / 2); v.horizon = H_ / 2.0 + cam.pitch;
		int lc = cell_at((int)std::floor(cam.x / kCell), (int)std::floor(cam.y / kCell)); // eye height follows the terrain level
		v.eye_level = lc > 0 ? lc - 1 : 0;
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
		int ul = cell_at((int)std::floor(un.x / kCell), (int)std::floor(un.y / kCell)); ul = ul > 0 ? ul - 1 : 0; // the floor level the unit stands on
		double eh = p_.eye_h + v.eye_level * 32.0;
		double base = v.horizon + (eh - ul * 32.0) * v.proj / depth;
		if (d.sp) {
			// The unit position anchors the CENTER of the sprite box (same convention as the community renderers):
			// a frame at (xoff,yoff) spans below the anchor, so most of the art sits lower than a bottom-aligned billboard.
			const auto& fr = d.sp->frames[d.frame];
			const double sf = 0.25; // HD pixels are 4x SD
			double cx = (fr.xoff + fr.w / 2.0 - d.sp->box_w / 2.0) * sf; // horizontal anchor offset (world px)
			double top = (fr.yoff - d.sp->box_h / 2.0) * sf;             // frame top relative to ground (negative = above)
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
				if (depth > zbuf_[x]) continue;
				double u = (x - d.x0 + 0.5) / std::max(1.0, d.wpx); if (d.flip) u = 1.0 - u;
				for (int y = std::max(0, d.y0); y < std::min(H_, d.y1); ++y) {
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
			for (int x = std::max(xa, d.x0); x <= std::min(xb - 1, d.x1); ++x) { if (depth > zbuf_[x]) continue; for (int y = std::max(0, d.y0); y < std::min(H_, d.y1); ++y) fb[(size_t)y * W_ + x] = c; }
		}
	}

	// Selection brackets and a health/shield bar above a unit.
	void mark(const Camera& cam, const UnitView& un, double depth, double sxc, const View& v, uint32_t* fb, bool selected) {
		Draw d = geometry(cam, un, depth, sxc, v);
		int cxm = std::max(0, std::min(W_ - 1, (int)sxc)); if (depth > zbuf_[cxm]) return;
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
	int mw_ = 0, mh_ = 0; std::vector<uint8_t> grid_;
	std::vector<UnitView> units_; std::vector<double> zbuf_;
};

} // namespace fp
