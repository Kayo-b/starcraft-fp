// Top-down view (the classic RTS camera) over OpenBW simulation state, drawn from the Remastered HD art: the map's terrain
// tiles, creep and its edges, and every image of every sprite (units with their shadows, turrets and overlays, bullets,
// explosions, remnants) in the original's drawing order. What a viewer sees comes from the first-person renderer's
// snapshot (Renderer::update_units): units drawn between simulation frames, the fog of war, remembered buildings and cloaked
// units, so the two views always agree.
#pragma once
#include "fp_render.h"
#include <array>
#include <set>
#include <unordered_map>

namespace fp {

// The camera: the map point at the centre of the screen (px) and the screen px per map px. StarCraft draws 1; the HD art
// has 4 texels per map px, so 4 shows all of it.
struct TopView { double x = 0, y = 0, zoom = 2; };

// What to draw on top: selection circles (under the unit, as StarCraft draws them) and health bars, the unit under the
// mouse, a selection box being dragged (screen px).
struct TopOverlay {
	std::vector<const unit_t*> selected; const unit_t* hover = nullptr;
	int me = -1; // whose circles are green (others red, neutral yellow)
	bool box = false; int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
};

class TopDown {
public:
	static constexpr double kZooms[5] = {1, 1.5, 2, 3, 4}; // the mouse wheel's steps; 1, 2 and 4 copy the art 1:1
	static constexpr size_t kCacheBytes = 256u << 20;     // decoded art kept; past it everything is decoded again as needed

	TopDown(state& st, state_functions& fn, Assets& assets, int w, int h)
		: st_(st), gs_(*st.game), fn_(fn), assets_(assets), W_(w), H_(h), tiles_(assets.tiles(gs_.tileset_index)) {
		assets_.set_tileset(gs_.tileset_index); creep_ = assets_.creep_edges();
	}
	int width() const { return W_; }
	int height() const { return H_; }
	bool creep_edges() const { return creep_ != nullptr; }

	void to_map(const TopView& v, double sx, double sy, double& mx, double& my) const { mx = v.x + (sx - W_ / 2.0) / v.zoom; my = v.y + (sy - H_ / 2.0) / v.zoom; }
	void to_screen(const TopView& v, double mx, double my, double& sx, double& sy) const { sx = W_ / 2.0 + (mx - v.x) * v.zoom; sy = H_ / 2.0 + (my - v.y) * v.zoom; }
	// The centre the camera may take: the map fills the screen where it is big enough (else centred on that axis).
	void clamp(TopView& v) const {
		double hw = W_ / 2.0 / v.zoom, hh = H_ / 2.0 / v.zoom, mw = gs_.map_width, mh = gs_.map_height;
		v.x = mw <= 2 * hw ? mw / 2 : std::max(hw, std::min(mw - hw, v.x)); v.y = mh <= 2 * hh ? mh / 2 : std::max(hh, std::min(mh - hh, v.y));
	}

	// Render into fb (W*H, 0xFFRRGGBB). Call r.update_units() first (as for a first-person frame); r's viewer decides what is
	// drawn. threads: 0 = all cores; the picture is the same for any number.
	void render(const Renderer& r, const TopView& v, uint32_t* fb, const TopOverlay* ov = nullptr, unsigned threads = 0) {
		const double z = std::max(0.25, v.zoom); const int m = mip(z);
		if (bytes_ > kCacheBytes) { pics_.clear(); bytes_ = 0; }
		jobs_.clear(); items_.clear(); bars_.clear(); view_ = v; view_.zoom = z; r_ = &r;
		prepare_tiles(v, z, m);
		collect(r, v, z, m, ov);
		decode();
		for (auto& t : vis_tiles_) { t.base = &pics_.at(t.base_key); t.edge = t.edge_key ? &pics_.at(t.edge_key) : nullptr; }
		for (auto& it : items_) it.pic = &pics_.at(it.key);
		auto band = [&](int ya, int yb) { terrain(r, fb, ya, yb, z, m); for (const Item& it : items_) draw(it, fb, ya, yb); };
		unsigned n = threads ? threads : std::max(1u, std::min(12u, std::thread::hardware_concurrency()));
		if (n <= 1) band(0, H_);
		else { std::vector<std::thread> th; for (unsigned t = 0; t < n; ++t) th.emplace_back(band, (int)((long)H_ * t / n), (int)((long)H_ * (t + 1) / n)); for (auto& t : th) t.join(); }
		for (const Bar& b : bars_) bar(b, fb);
		if (ov && ov->box) box(*ov, fb);
	}

	// The minimap: the whole map in a square of `size` px with its top-left at (x0, y0) on the frame (render() first, for
	// the viewer's snapshot): each tile's colour (its art's average; creep's for creep), dimmed by the fog of war, the units
	// the viewer sees as dots in their players' colours (minerals and geysers cyan), the view's outline.
	void minimap(const Renderer& r, uint32_t* fb, int x0, int y0, int size) {
		const int tw = (int)gs_.map_tile_width, th = (int)gs_.map_tile_height; const double k = (double)size / std::max(tw, th); // px per tile
		const int w = (int)std::lround(tw * k), h = (int)std::lround(th * k); mm_ = {x0, y0, w, h};
		auto put = [&](int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < W_ && y < H_) fb[(size_t)y * W_ + x] = c; };
		if (mm_tiles_.size() != (size_t)tw * th || st_.current_frame - mm_frame_ >= 24 || st_.current_frame < mm_frame_) { // the tiles' colours, again every second (creep spreads)
			mm_tiles_.resize((size_t)tw * th); mm_frame_ = st_.current_frame;
			for (size_t i = 0; i < mm_tiles_.size(); ++i) { size_t mt = st_.tiles_mega_tile_index[i]; if ((st_.tiles[i].flags & tile_t::flag_has_creep) && gs_.cv5.size() > 1) mt = gs_.cv5[1].mega_tile_index[0]; mm_tiles_[i] = tile_colour(mt); }
		}
		std::vector<uint32_t>& lit = mm_lit_; lit = mm_tiles_; // dimmed by the fog of war, per tile
		if (r.viewer() >= 0) for (int ty = 0; ty < th; ++ty) for (int tx = 0; tx < tw; ++tx) { uint32_t& c = lit[(size_t)ty * tw + tx]; c = scale(c, r.fog_at(tx * 32 + 16, ty * 32 + 16)); }
		for (int y = 0; y < h; ++y) { const uint32_t* row = &lit[(size_t)std::min(th - 1, (int)(y / k)) * tw]; for (int x = 0; x < w; ++x) put(x0 + x, y0 + y, row[std::min(tw - 1, (int)(x / k))] | 0xFF000000u); }
		for (const UnitView& u : r.units()) {
			if (u.turret_of >= 0) continue; // drawn with its hull
			bool res = u.owner >= 8 && (u.type == (int)UnitTypes::Resource_Mineral_Field || u.type == (int)UnitTypes::Resource_Mineral_Field_Type_2 || u.type == (int)UnitTypes::Resource_Mineral_Field_Type_3 || u.type == (int)UnitTypes::Resource_Vespene_Geyser);
			if (u.owner >= 8 && !res) continue; // critters and the like
			double pc[3]; player_rgb(u.owner, pc); uint32_t c = res ? 0xFF50C8F0u : 0xFF000000u | (uint32_t)pc[0] << 16 | (uint32_t)pc[1] << 8 | (uint32_t)pc[2];
			int d = std::max(2, (int)std::lround(2 * u.r * k / 32)), cx = x0 + (int)(u.x / 32 * k), cy = y0 + (int)(u.y / 32 * k);
			for (int yy = cy - d / 2; yy < cy - d / 2 + d; ++yy) for (int xx = cx - d / 2; xx < cx - d / 2 + d; ++xx) if (xx >= x0 && yy >= y0 && xx < x0 + w && yy < y0 + h) put(xx, yy, c);
		}
		auto at = [&](double mx, double my, int& x, int& y) { x = x0 + (int)std::lround(std::max(0.0, std::min((double)tw * 32, mx)) / 32 * k); y = y0 + (int)std::lround(std::max(0.0, std::min((double)th * 32, my)) / 32 * k); };
		int ax, ay, bx, by; at(view_.x - W_ / 2.0 / view_.zoom, view_.y - H_ / 2.0 / view_.zoom, ax, ay); at(view_.x + W_ / 2.0 / view_.zoom, view_.y + H_ / 2.0 / view_.zoom, bx, by);
		for (int x = ax; x <= bx; ++x) { put(x, ay, 0xFFFFFFFFu); put(x, by, 0xFFFFFFFFu); }
		for (int y = ay; y <= by; ++y) { put(ax, y, 0xFFFFFFFFu); put(bx, y, 0xFFFFFFFFu); }
		for (int x = x0 - 1; x <= x0 + w; ++x) { put(x, y0 - 1, 0xFF606060u); put(x, y0 + h, 0xFF606060u); }
		for (int y = y0 - 1; y <= y0 + h; ++y) { put(x0 - 1, y, 0xFF606060u); put(x0 + w, y, 0xFF606060u); }
	}
	// The map point under frame px (x, y) if it is on the last minimap drawn.
	bool on_minimap(double x, double y, double& mx, double& my) const {
		if (mm_[2] <= 0 || x < mm_[0] || y < mm_[1] || x >= mm_[0] + mm_[2] || y >= mm_[1] + mm_[3]) return false;
		mx = (x - mm_[0]) / mm_[2] * gs_.map_width; my = (y - mm_[1]) / mm_[3] * gs_.map_height; return true;
	}

	// After render(): the unit drawn topmost with art under screen point (sx, sy), else the nearest one whose footprint is
	// within a few px of it (small units), else nullptr. Only units the viewer can target.
	const unit_t* unit_at(double sx, double sy) const {
		for (size_t k = items_.size(); k-- > 0;) {
			const Item& it = items_[k]; if (!it.unit || it.blend == kShadow || it.blend == kCircle) continue;
			int x = (int)std::floor(sx), y = (int)std::floor(sy);
			if (x < it.x0 || y < it.y0 || x >= it.x0 + it.dw || y >= it.y0 + it.dh) continue;
			int u = (int)((x - it.x0 + 0.5f) * it.inv_s), vv = (int)((y - it.y0 + 0.5f) * it.inv_s);
			if (u >= it.pic->w || vv >= it.pic->h) continue; if (it.flip) u = it.pic->w - 1 - u;
			if ((it.pic->px[(size_t)vv * it.pic->w + u] >> 24) >= 128) return it.unit;
		}
		const unit_t* best = nullptr; double bd = 1e9;
		if (r_) for (const UnitView& un : r_->units()) {
			if (!un.ptr || un.turret_of >= 0) continue; double ux, uy; to_screen(view_, un.x, un.y, ux, uy);
			double d = std::hypot(ux - sx, uy - sy) - un.r * view_.zoom; if (d < 4 && d < bd) { bd = d; best = un.ptr; }
		}
		return best;
	}
	// After render(): the units the viewer can target whose position is inside the screen rectangle (any corner order).
	std::vector<const unit_t*> units_in(double x0, double y0, double x1, double y1) const {
		std::vector<const unit_t*> out; if (!r_) return out;
		if (x0 > x1) std::swap(x0, x1); if (y0 > y1) std::swap(y0, y1);
		for (const UnitView& un : r_->units()) { if (!un.ptr || un.turret_of >= 0) continue; double ux, uy; to_screen(view_, un.x, un.y, ux, uy); if (ux >= x0 && ux <= x1 && uy >= y0 && uy <= y1) out.push_back(un.ptr); }
		return out;
	}
	// Last render(): images drawn (on screen); of them the ones of a given player's sprites, shadows, shimmers (undetected
	// cloaked units) and selection circles.
	int drawn() const { return (int)items_.size(); }
	int drawn_of(int owner) const { int n = 0; for (auto& it : items_) n += it.owner == owner && it.blend != kCircle; return n; }
	int shadows() const { int n = 0; for (auto& it : items_) n += it.blend == kShadow; return n; }
	int shimmers() const { int n = 0; for (auto& it : items_) n += it.blend == kShimmer; return n; }
	int circles() const { int n = 0; for (auto& it : items_) n += it.blend == kCircle; return n; }
	int bars() const { return (int)bars_.size(); }
	int order_of(const unit_t* u) const { for (size_t k = 0; k < items_.size(); ++k) if (items_[k].unit == u && items_[k].blend != kShadow) return (int)k; return -1; } // its first image drawn
	size_t cache_bytes() const { return bytes_; }

	// Which creep edge frame a tile without creep shows, from which of its 8 neighbours have creep (bits: 1 N, 2 S, 4 E, 8 W,
	// 16 NE, 32 NW, 64 SE, 128 SW); -1 for none. The creep spills over from the neighbours: a side marks both corners next to
	// it, a diagonal neighbour marks its corner only together with a side next to it, and opposite corners both marked add one
	// more case; the frames are numbered in the order of the patterns that occur (OpenBW's ui works out the same table).
	static int creep_edge_frame(int nb) {
		static const std::array<int, 256> table = [] {
			auto pattern = [](int b) {
				bool n = b & 1, s = b & 2, e = b & 4, w = b & 8, ne = b & 16, nw = b & 32, se = b & 64, sw = b & 128;
				int p = (w || (n && nw)) | n << 1 | (e || (n && ne)) << 2 | (w || (s && sw)) << 3 | s << 4 | (e || (s && se)) << 5;
				if (((p & 0x21) == 0x21) || ((p & 0x0c) == 0x0c)) p |= 0x40;
				return p;
			};
			std::array<int, 128> rank{}; std::array<bool, 128> seen{};
			for (int b = 0; b < 256; ++b) seen[pattern(b)] = true;
			for (int p = 0, k = 0; p < 128; ++p) if (seen[p]) rank[p] = k++;
			std::array<int, 256> t{}; for (int b = 0; b < 256; ++b) t[b] = rank[pattern(b)] - 1;
			return t;
		}();
		return table[nb & 255];
	}

private:
	// Decoded art at one mip level: premultiplied 0xAARRGGBB, and the player-colour mask (empty for art without one).
	struct Pic { int w = 0, h = 0; std::vector<uint32_t> px; std::vector<uint8_t> team; };
	enum Blend : uint8_t { kNormal, kShadow, kShimmer, kWarp, kCircle };
	struct Item {
		uint64_t key; const Pic* pic; int x0, y0, dw, dh; float inv_s; bool flip; uint8_t blend, alpha; float dim;
		int owner; uint32_t tint; const unit_t* unit; // owner < 8: player colour from the mask; tint: circles; unit: for picking
	};
	struct Bar { int x, y, w; double hp, shield; };
	struct TileArt { uint64_t base_key, edge_key; const Pic* base; const Pic* edge; int ex, ey; };
	struct Job { uint64_t key; int kind; const AnimSprite* sp; int index, m; };

	// The mip level for a zoom: 4 >> m texels per map px, the first one at or below the screen's px (never magnified less
	// than 1:1, so art is copied or picked, not averaged, while drawing).
	static int mip(double z) { int m = 0; while (m < 3 && 4.0 / (2 << m) >= z - 1e-9) ++m; return m; }

	// ---- the art cache ----
	enum Kind { kTile = 1, kEdge = 2, kFrame = 3 };
	uint64_t want(int kind, const AnimSprite* sp, int index, int m) {
		uint64_t id = 0;
		if (kind == kFrame) { auto it = anim_ids_.find(sp); if (it == anim_ids_.end()) it = anim_ids_.emplace(sp, (uint32_t)anim_ids_.size() + 1).first; id = it->second; }
		uint64_t key = (uint64_t)kind << 60 | id << 32 | (uint64_t)index << 4 | (uint64_t)m;
		if (!pics_.count(key) && !queued_.count(key)) { queued_.insert(key); jobs_.push_back({key, kind, sp, index, m}); }
		return key;
	}
	// Average k x k texels (k = 2^m) of a w x h source, premultiplied; texel(x, y, rgba) -> player-colour mask (0..255).
	template<typename F> static void shrink(Pic& p, int w, int h, int m, bool team, F&& texel) {
		const int k = 1 << m, n = k * k; p.w = (w + k - 1) / k; p.h = (h + k - 1) / k;
		p.px.assign((size_t)p.w * p.h, 0); if (team) p.team.assign(p.px.size(), 0);
		for (int y = 0; y < p.h; ++y) for (int x = 0; x < p.w; ++x) {
			uint32_t r = 0, g = 0, b = 0, a = 0, t = 0;
			for (int dy = 0; dy < k; ++dy) for (int dx = 0; dx < k; ++dx) {
				int sx = x * k + dx, sy = y * k + dy; if (sx >= w || sy >= h) continue;
				uint8_t c[4]; int tm = texel(sx, sy, c); r += c[0] * c[3]; g += c[1] * c[3]; b += c[2] * c[3]; a += c[3]; t += tm * c[3];
			}
			size_t i = (size_t)y * p.w + x;
			p.px[i] = (a / n) << 24 | (r / (255 * n)) << 16 | (g / (255 * n)) << 8 | b / (255 * n);
			if (team) p.team[i] = (uint8_t)(a ? t / a : 0);
		}
	}
	void decode() {
		std::vector<Pic> out(jobs_.size());
		auto work = [&](size_t j) {
			const Job& jb = jobs_[j]; Pic& p = out[j];
			if (jb.kind == kTile) shrink(p, 128, 128, jb.m, false, [&](int x, int y, uint8_t c[4]) { tiles_->sample((size_t)jb.index, x, y, c); c[3] = 255; return 0; });
			else if (jb.kind == kEdge) { const auto& f = creep_->frames[jb.index]; shrink(p, f.w, f.h, jb.m, false, [&](int x, int y, uint8_t c[4]) { creep_->texel(jb.index, x, y, c); return 0; }); }
			else { const auto& f = jb.sp->frames[jb.index]; const bool team = jb.sp->has_teamcolor();
				shrink(p, f.w, f.h, jb.m, team, [&](int x, int y, uint8_t c[4]) { jb.sp->texel(f.x + x, f.y + y, c); return team ? jb.sp->team_mask(f.x + x, f.y + y) : 0; }); }
		};
		unsigned n = std::max(1u, std::min<unsigned>((unsigned)jobs_.size(), std::min(12u, std::thread::hardware_concurrency())));
		if (n <= 1) { for (size_t j = 0; j < jobs_.size(); ++j) work(j); }
		else { std::vector<std::thread> th; for (unsigned t = 0; t < n; ++t) th.emplace_back([&, t] { for (size_t j = t; j < jobs_.size(); j += n) work(j); }); for (auto& t : th) t.join(); }
		for (size_t j = 0; j < jobs_.size(); ++j) { bytes_ += out[j].px.size() * 4 + out[j].team.size() + 64; pics_.emplace(jobs_[j].key, std::move(out[j])); }
		jobs_.clear(); queued_.clear();
	}

	// ---- terrain ----
	// The visible tiles' art: their own megatile, or for creep one of the tileset's creep tiles (picked per tile, fixed),
	// and on a tile beside creep the edge spilling over onto it.
	void prepare_tiles(const TopView& v, double z, int m) {
		const int tw = (int)gs_.map_tile_width, th = (int)gs_.map_tile_height;
		tx0_ = std::max(0, (int)std::floor((v.x - W_ / 2.0 / z) / 32)); ty0_ = std::max(0, (int)std::floor((v.y - H_ / 2.0 / z) / 32));
		int tx1 = std::min(tw - 1, (int)std::floor((v.x + W_ / 2.0 / z) / 32)), ty1 = std::min(th - 1, (int)std::floor((v.y + H_ / 2.0 / z) / 32));
		tcols_ = std::max(0, tx1 - tx0_ + 1); trows_ = std::max(0, ty1 - ty0_ + 1); vis_tiles_.assign((size_t)tcols_ * trows_, TileArt{});
		auto creep = [&](int x, int y) { return x >= 0 && y >= 0 && x < tw && y < th && (st_.tiles[(size_t)y * tw + x].flags & tile_t::flag_has_creep); };
		for (int ty = ty0_; ty < ty0_ + trows_; ++ty) for (int tx = tx0_; tx < tx0_ + tcols_; ++tx) {
			TileArt& a = vis_tiles_[(size_t)(ty - ty0_) * tcols_ + (tx - tx0_)];
			size_t mt = st_.tiles_mega_tile_index[(size_t)ty * tw + tx];
			if (creep(tx, ty)) {
				uint32_t h = (uint32_t)tx * 73856093u ^ (uint32_t)ty * 19349663u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
				int pick = h % 100 < 4 ? 6 + (int)(h / 100 % 7) : (int)(h / 100 % 6); // mostly the 6 plain creep tiles, now and then a rarer one
				if (gs_.cv5.size() > 1) mt = gs_.cv5[1].mega_tile_index[pick];
			} else if (creep_) {
				int nb = creep(tx, ty - 1) | creep(tx, ty + 1) << 1 | creep(tx + 1, ty) << 2 | creep(tx - 1, ty) << 3
					| creep(tx + 1, ty - 1) << 4 | creep(tx - 1, ty - 1) << 5 | creep(tx + 1, ty + 1) << 6 | creep(tx - 1, ty + 1) << 7;
				int f = nb ? creep_edge_frame(nb) : -1;
				if (f >= 0 && f < (int)creep_->frames.size()) { a.edge_key = want(kEdge, nullptr, f, m); a.ex = creep_->frames[f].x >> m; a.ey = creep_->frames[f].y >> m; }
			}
			a.base_key = want(kTile, nullptr, (int)mt, m);
		}
	}
	void terrain(const Renderer& r, uint32_t* fb, int ya, int yb, double z, int m) {
		const double mw = gs_.map_width, mh = gs_.map_height, k = 4.0 / (1 << m); // texels per map px
		const bool fog = r.viewer() >= 0;
		for (int y = ya; y < yb; ++y) {
			uint32_t* row = fb + (size_t)y * W_; const double my = view_.y + (y + 0.5 - H_ / 2.0) / z;
			int ty = (int)std::floor(my / 32);
			if (my < 0 || my >= mh || ty < ty0_ || ty >= ty0_ + trows_) { std::fill(row, row + W_, 0xFF000000u); continue; }
			const int tv = (int)((my - ty * 32) * k);
			for (int x = 0; x < W_; ++x) {
				const double mx = view_.x + (x + 0.5 - W_ / 2.0) / z; int tx = (int)std::floor(mx / 32);
				if (mx < 0 || mx >= mw || tx < tx0_ || tx >= tx0_ + tcols_) { row[x] = 0xFF000000u; continue; }
				const TileArt& a = vis_tiles_[(size_t)(ty - ty0_) * tcols_ + (tx - tx0_)];
				const int tu = (int)((mx - tx * 32) * k);
				uint32_t c = a.base->px[(size_t)std::min(tv, a.base->h - 1) * a.base->w + std::min(tu, a.base->w - 1)];
				if (a.edge) { int eu = tu - a.ex, ev = tv - a.ey; if (eu >= 0 && ev >= 0 && eu < a.edge->w && ev < a.edge->h) c = over(a.edge->px[(size_t)ev * a.edge->w + eu], c); }
				if (fog) c = scale(c, r.fog_at(mx, my));
				row[x] = c | 0xFF000000u;
			}
		}
	}

	// ---- sprites ----
	// Every sprite near the view, in StarCraft's order (elevation, then from north to south up to the ground units' level,
	// turrets over their base), each sprite's images from the bottom one up; selection circles under the unit's first image
	// that is not a shadow, as the original draws them.
	void collect(const Renderer& r, const TopView& v, double z, int m, const TopOverlay* ov) {
		const int viewer = r.viewer(), bit = viewer >= 0 ? 1 << viewer : 0;
		const auto& us = r.units();
		by_sprite_.assign(4096, -1); unit_sprite_.assign(4096, 0);
		auto mark = [&](const unit_t* u, int i) { for (int k = 0; u && k < 2; ++k, u = u->subunit) { size_t s = u->sprite->index; if (s < by_sprite_.size()) { unit_sprite_[s] = 1; if (i >= 0) by_sprite_[s] = i; } } };
		for (auto u : ptr(st_.visible_units)) mark(u, -1);
		for (size_t i = 0; i < us.size(); ++i) if (us[i].src && us[i].turret_of < 0) mark(us[i].src, (int)i); // a turret entry: its hull's (and the turret sprite is the hull's subunit)
		struct S { uint32_t order; size_t n; const sprite_t* sp; int unit; }; std::vector<S> list;
		const int lines = (int)gs_.map_tile_height, ly0 = std::max(0, (int)std::floor((v.y - H_ / 2.0 / z) / 32) - 8), ly1 = std::min(lines - 1, (int)std::floor((v.y + H_ / 2.0 / z) / 32) + 8);
		for (int ly = ly0; ly <= ly1; ++ly) for (auto* sp : ptr(st_.sprites_on_tile_line[ly])) {
			if (fn_.s_hidden(sp)) continue;
			size_t si = sp->index; int ui = si < by_sprite_.size() ? by_sprite_[si] : -1;
			if (ui < 0 && si < unit_sprite_.size() && unit_sprite_[si]) continue; // a unit the viewer does not see
			if (ui < 0 && viewer >= 0 && sp->owner < 8 && sp->owner != viewer && !(sp->visibility_flags & bit)) continue; // an effect in their fog
			uint32_t order = (uint32_t)sp->elevation_level << 14 | (uint32_t)(sp->elevation_level <= 4 ? std::max(0, sp->position.y) & 0x1fff : 0) << 1 | (fn_.s_flag(sp, sprite_t::flag_turret) ? 1 : 0);
			list.push_back({order, list.size(), sp, ui});
		}
		for (size_t i = 0; i < us.size(); ++i) if (!us[i].src && !us[i].ptr) // remembered buildings (an undetected unit has its src)
			list.push_back({4u << 14 | (uint32_t)std::max(0.0, us[i].y) << 1, list.size(), nullptr, (int)i});
		std::sort(list.begin(), list.end(), [](const S& a, const S& b) { return a.order != b.order ? a.order < b.order : a.n < b.n; });
		auto selected = [&](const unit_t* u) { return u && ov && std::find(ov->selected.begin(), ov->selected.end(), u) != ov->selected.end(); };
		for (const S& s : list) {
			const UnitView* un = s.unit >= 0 ? &us[s.unit] : nullptr;
			if (!s.sp) { // remembered: its main image as last seen
				if (const AnimSprite* a = assets_.sprite(un->image)) if (un->frame >= 0 && un->frame < (int)a->frames.size())
					add(a, un->frame, un->flip, un->x, un->y, z, m, kNormal, 255, un->dim, un->owner, 0, nullptr);
				continue;
			}
			const sprite_t* sp = s.sp;
			double dx = 0, dy = 0, dim = un ? un->dim : (viewer >= 0 ? r.fog_at(sp->position.x, sp->position.y) : 1);
			if (un && un->src) { const sprite_t* own = un->src->sprite; dx = un->x - own->position.x; dy = un->y - own->position.y; } // drawn between frames, as the unit is
			int look = un ? un->look : UnitView::kPlain; const unit_t* target = un ? un->ptr : nullptr;
			bool circle = un && un->ptr && (selected(un->ptr) || (ov && ov->hover == un->ptr)) && !fn_.s_flag(sp, sprite_t::flag_turret);
			for (auto* im : ptr(reverse(sp->images))) {
				if (fn_.i_flag(im, image_t::flag_hidden)) continue;
				int mod = im->modifier; uint8_t blend = kNormal, alpha = 255; int owner = sp->owner;
				if (mod == 10) { if (look == UnitView::kShimmer) continue; blend = kShadow; }
				else if (circle) { circle_for(sp, *un, selected(un->ptr), ov, dx, dy, z, m); circle = false; }
				if (mod == 9 || mod == 17) owner = -1;                         // effects: their own colours
				else if (mod == 12) blend = kWarp;                            // a Protoss building warping in
				else if (mod == 2 || mod == 4) alpha = (uint8_t)(255 - 16 * std::max(0, std::min(8, im->modifier_data1))); // cloaking, decloaking
				else if (mod == 3) alpha = 128;                               // cloaked, seen
				else if (mod == 8) blend = kShimmer;
				if (look == UnitView::kShimmer && blend != kShadow) blend = kShimmer;
				else if (look == UnitView::kSeeThrough) alpha = std::min<uint8_t>(alpha, 128);
				const AnimSprite* a = assets_.sprite((int)im->image_type->id);
				if (!a || im->frame_index >= a->frames.size()) continue;
				double cx = sp->position.x + im->offset.x + dx, cy = sp->position.y + im->offset.y + dy;
				if (fn_.i_flag(im, image_t::flag_y_frozen) && im->grp && im->frame_index < im->grp->frames.size()) // held at a height (lifting off, landing)
					cy = im->frozen_y_value - ((int)im->grp->frames[im->frame_index].offset.y - (int)im->grp->height / 2);
				add(a, (int)im->frame_index, fn_.i_flag(im, image_t::flag_horizontally_flipped), cx, cy, z, m, blend, alpha, (float)dim, owner, 0, target);
			}
			if (un && un->ptr && selected(un->ptr) && !fn_.s_flag(sp, sprite_t::flag_turret)) health_bar(sp, *un, dx, dy, z);
		}
	}
	// One image's frame at map centre (cx, cy): HD frames lie in the art's box as SD frames lie in the GRP's, at 4x.
	void add(const AnimSprite* a, int frame, bool flip, double cx, double cy, double z, int m, uint8_t blend, uint8_t alpha, float dim, int owner, uint32_t tint, const unit_t* unit) {
		const auto& f = a->frames[frame];
		double X0 = flip ? cx - (f.xoff + f.w - a->box_w / 2.0) / 4 : cx + (f.xoff - a->box_w / 2.0) / 4, Y0 = cy + (f.yoff - a->box_h / 2.0) / 4;
		const int k = 1 << m; const int pw = (f.w + k - 1) / k, ph = (f.h + k - 1) / k; const double s = z * k / 4;
		Item it{}; it.x0 = (int)std::floor(W_ / 2.0 + (X0 - view_.x) * z + 0.5); it.y0 = (int)std::floor(H_ / 2.0 + (Y0 - view_.y) * z + 0.5);
		it.dw = (int)std::ceil(pw * s - 1e-9); it.dh = (int)std::ceil(ph * s - 1e-9);
		if (it.x0 >= W_ || it.y0 >= H_ || it.x0 + it.dw <= 0 || it.y0 + it.dh <= 0) return;
		it.key = want(kFrame, a, frame, m); it.inv_s = (float)(1 / s); it.flip = flip; it.blend = blend; it.alpha = alpha; it.dim = dim; it.owner = owner; it.tint = tint; it.unit = unit;
		items_.push_back(it);
	}
	// StarCraft's selection circle for the sprite (its size and height from the sprite type), coloured by whose it is.
	void circle_for(const sprite_t* sp, const UnitView& un, bool sel, const TopOverlay* ov, double dx, double dy, double z, int m) {
		int id = (int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sp->sprite_type->selection_circle;
		const AnimSprite* a = assets_.sprite(id); if (!a || a->frames.empty()) return;
		uint32_t c = un.owner >= 8 ? 0xFFE8E040u : (!ov || ov->me < 0 || un.owner == ov->me) ? 0xFF30F030u : 0xFFF03030u; // no player (a replay): all green
		add(a, 0, false, sp->position.x + dx, sp->position.y + sp->sprite_type->selection_circle_vpos + dy, z, m, kCircle, sel ? 255 : 120, 1, -1, c, nullptr);
	}
	void health_bar(const sprite_t* sp, const UnitView& un, double dx, double dy, double z) {
		int id = (int)ImageTypes::IMAGEID_Selection_Circle_22pixels + sp->sprite_type->selection_circle; const AnimSprite* a = assets_.sprite(id);
		double ch = a && !a->frames.empty() ? a->frames[0].h / 4.0 : 16, w = std::max(19, sp->sprite_type->health_bar_size);
		double sx, sy; to_screen(view_, sp->position.x + dx, sp->position.y + sp->sprite_type->selection_circle_vpos + ch / 2 + 6 + dy, sx, sy);
		bars_.push_back({(int)std::lround(sx - w * z / 2), (int)std::lround(sy), (int)std::lround(w * z), un.hp_frac, un.shield_frac});
	}

	// A megatile's colour for the minimap: the average of 8 x 8 of its texels (kept per megatile).
	uint32_t tile_colour(size_t mt) {
		auto it = tile_colours_.find(mt); if (it != tile_colours_.end()) return it->second;
		unsigned r = 0, g = 0, b = 0; for (int y = 8; y < 128; y += 16) for (int x = 8; x < 128; x += 16) { uint8_t c[3]; tiles_->sample(mt, x, y, c); r += c[0]; g += c[1]; b += c[2]; }
		return tile_colours_[mt] = 0xFF000000u | (r / 64) << 16 | (g / 64) << 8 | b / 64;
	}

	// ---- pixels ----
	static uint32_t over(uint32_t s, uint32_t d) { // premultiplied s over d
		uint32_t ia = 255 - (s >> 24);
		uint32_t r = ((s >> 16) & 255) + ((d >> 16) & 255) * ia / 255, g = ((s >> 8) & 255) + ((d >> 8) & 255) * ia / 255, b = (s & 255) + (d & 255) * ia / 255;
		return 0xFF000000u | std::min(255u, r) << 16 | std::min(255u, g) << 8 | std::min(255u, b);
	}
	static uint32_t scale(uint32_t c, float k) {
		return (c & 0xFF000000u) | (uint32_t)(((c >> 16) & 255) * k) << 16 | (uint32_t)(((c >> 8) & 255) * k) << 8 | (uint32_t)((c & 255) * k);
	}
	void draw(const Item& it, uint32_t* fb, int ya, int yb) const {
		const Pic& p = *it.pic; double pc[3]; player_rgb(it.owner, pc);
		const int x0 = std::max(0, it.x0), x1 = std::min(W_, it.x0 + it.dw), y0 = std::max(ya, it.y0), y1 = std::min(yb, it.y0 + it.dh);
		if (x0 >= x1 || y0 >= y1) return;
		thread_local std::vector<uint32_t> behind; const double phase = st_.current_frame * 0.45;
		for (int y = y0; y < y1; ++y) {
			int v = (int)((y - it.y0 + 0.5f) * it.inv_s); if (v >= p.h) continue;
			uint32_t* row = fb + (size_t)y * W_;
			if (it.blend == kShimmer) behind.assign(row + std::max(0, x0 - 4), row + std::min(W_, x1 + 4));
			for (int x = x0; x < x1; ++x) {
				int u = (int)((x - it.x0 + 0.5f) * it.inv_s); if (u >= p.w) continue; if (it.flip) u = p.w - 1 - u;
				size_t i = (size_t)v * p.w + u; uint32_t s = p.px[i], a = s >> 24; if (!a) continue;
				uint32_t& d = row[x];
				switch (it.blend) {
				case kShadow: { uint32_t k = 255 - a * 128 / 255; d = 0xFF000000u | (((d >> 16) & 255) * k / 255) << 16 | (((d >> 8) & 255) * k / 255) << 8 | (d & 255) * k / 255; break; }
				case kShimmer: { // what is behind, shifted sideways in waves (a copy of the row: the band's own pixels only)
					if (a < 128) break; int sx = x + (int)std::lround(3 * std::sin(x * 0.35 + y * 0.15 + phase)) - std::max(0, x0 - 4);
					uint32_t c = behind[std::max(0, std::min((int)behind.size() - 1, sx))];
					d = 0xFF000000u | std::min(255u, ((c >> 16) & 255) + 14) << 16 | std::min(255u, ((c >> 8) & 255) + 14) << 8 | std::min(255u, (c & 255) + 18); break; }
				case kCircle: { uint32_t k = a * it.alpha / 255; d = over(k << 24 | (((it.tint >> 16) & 255) * k / 255) << 16 | (((it.tint >> 8) & 255) * k / 255) << 8 | (it.tint & 255) * k / 255, d); break; }
				default: {
					double r = (s >> 16) & 255, g = (s >> 8) & 255, b = s & 255;
					if (it.owner >= 0 && it.owner < 8 && !p.team.empty() && p.team[i]) { // toward the player colour, keeping the art's shading
						double k = p.team[i] / 255.0, lum = (0.3 * r + 0.59 * g + 0.11 * b) / 255.0 * 1.6;
						r = r * (1 - k) + pc[0] * lum * k; g = g * (1 - k) + pc[1] * lum * k; b = b * (1 - k) + pc[2] * lum * k;
					}
					if (it.blend == kWarp) { r = r * 0.5 + 60 * a / 255.0; g = g * 0.6 + 90 * a / 255.0; b = b * 0.6 + 150 * a / 255.0; }
					double k = it.dim * it.alpha / 255.0, ka = it.alpha / 255.0 * (it.blend == kWarp ? 0.75 : 1.0);
					uint32_t sa = (uint32_t)(a * ka);
					d = over(sa << 24 | (uint32_t)std::min(255.0, r * k * (it.blend == kWarp ? 0.75 : 1.0)) << 16 | (uint32_t)std::min(255.0, g * k * (it.blend == kWarp ? 0.75 : 1.0)) << 8 | (uint32_t)std::min(255.0, b * k * (it.blend == kWarp ? 0.75 : 1.0)), d);
				}
				}
			}
		}
	}
	void bar(const Bar& b, uint32_t* fb) const {
		const int h = std::max(3, (int)std::lround(2.5 * view_.zoom)), gap = std::max(1, h / 3);
		auto fill = [&](int y, double frac, uint32_t fg) {
			int filled = (int)std::lround(b.w * std::max(0.0, std::min(1.0, frac)));
			for (int yy = y; yy < y + h; ++yy) if (yy >= 0 && yy < H_) for (int x = b.x - 1; x <= b.x + b.w; ++x) if (x >= 0 && x < W_)
				fb[(size_t)yy * W_ + x] = (x < b.x || x >= b.x + b.w || yy == y || yy == y + h - 1) ? 0xFF101010u : (x - b.x < filled ? fg : 0xFF303030u);
		};
		int y = b.y; if (b.shield > 0) { fill(y, b.shield, 0xFF4090FFu); y += h + gap; }
		fill(y, b.hp, b.hp > 0.66 ? 0xFF40D040u : b.hp > 0.33 ? 0xFFE0C020u : 0xFFE03030u);
	}
	void box(const TopOverlay& ov, uint32_t* fb) const {
		int x0 = std::min(ov.bx0, ov.bx1), x1 = std::max(ov.bx0, ov.bx1), y0 = std::min(ov.by0, ov.by1), y1 = std::max(ov.by0, ov.by1);
		auto put = [&](int x, int y) { if (x >= 0 && y >= 0 && x < W_ && y < H_) fb[(size_t)y * W_ + x] = 0xFF30F030u; };
		for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
		for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
	}

	state& st_; game_state& gs_; state_functions& fn_; Assets& assets_;
	int W_, H_; const HdTiles* tiles_; const CreepEdges* creep_ = nullptr;
	std::unordered_map<uint64_t, Pic> pics_; size_t bytes_ = 0; std::unordered_map<const AnimSprite*, uint32_t> anim_ids_;
	std::vector<Job> jobs_; std::set<uint64_t> queued_;
	std::vector<TileArt> vis_tiles_; int tx0_ = 0, ty0_ = 0, tcols_ = 0, trows_ = 0;
	std::vector<Item> items_; std::vector<Bar> bars_; std::vector<int> by_sprite_; std::vector<uint8_t> unit_sprite_;
	TopView view_; const Renderer* r_ = nullptr;
	std::unordered_map<size_t, uint32_t> tile_colours_; std::array<int, 4> mm_{}; // the minimap's last place (x, y, w, h)
	std::vector<uint32_t> mm_tiles_, mm_lit_; int mm_frame_ = 0;
};

} // namespace fp
