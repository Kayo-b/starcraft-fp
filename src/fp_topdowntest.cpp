// Headless test of the top-down view (fp_topdown.h): the terrain is the map's HD art where nothing stands, creep is drawn
// with the tileset's creep tiles and edges, every pixel is written, sprites come in StarCraft's order (picking finds the
// one drawn on top), a selection box finds the units in it, the fog of war and remembered buildings follow the
// first-person renderer's snapshot, the picture does not depend on the number of threads, and the cost per frame.
// Exit code 0 only if every check passes. A third argument (a directory) writes PPM frames of the checks to look at.
#include "fp_game.h"
#include "fp_topdown.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <set>

using namespace fp;
static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }

int main(int argc, char** argv) {
	if (argc < 3) { std::cerr << "usage: fp_topdowntest DATA_DIR MAP.scm [FRAMES_DIR]\n"; return 2; }
	std::cout << std::unitbuf;
	const std::string out = argc > 3 ? argv[3] : "";
	try {
		Game g; g.open(argv[1], argv[2], race_t::zerg, race_t::protoss); // a melee start: a Hatchery with its creep, a Nexus
		Assets assets(argv[1]); Renderer r(g.st(), g.fn(), assets, 320, 200);
		const int W = 640, H = 400; TopDown td(g.st(), g.fn(), assets, W, H);
		std::vector<uint32_t> fb((size_t)W * H);
		auto& gs = *g.st().game; const int tw = (int)gs.map_tile_width;
		auto step = [&](int n) { for (int i = 0; i < n; ++i) { g.step(); r.update_units(1, 1.0 / 24); } };
		auto save = [&](const char* name) {
			if (out.empty()) return; std::string p = out + "/topdown_" + name + ".ppm"; FILE* fp = fopen(p.c_str(), "wb"); if (!fp) return;
			fprintf(fp, "P6\n%d %d\n255\n", W, H); for (uint32_t c : fb) { uint8_t b[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; fwrite(b, 1, 3, fp); } fclose(fp);
		};
		auto render = [&](TopView v, const char* name = nullptr, const TopOverlay* ov = nullptr) { r.update_units(); td.render(r, v, fb.data(), ov); if (name) save(name); return v; };
		auto creep = [&](int tx, int ty) { return tx >= 0 && ty >= 0 && tx < tw && ty < (int)gs.map_tile_height && (g.st().tiles[(size_t)ty * tw + tx].flags & tile_t::flag_has_creep); };
		step(2);
		xy mine = g.start_location(g.owner()), theirs = g.start_location(g.enemy());
		const HdTiles* hd = assets.tiles(gs.tileset_index);
		// the screen pixel's map point at a view, and whether a unit stands within `clear` px of it
		auto map_at = [&](const TopView& v, int x, int y, double& mx, double& my) { td.to_map(v, x + 0.5, y + 0.5, mx, my); };
		auto near_unit = [&](double mx, double my, double clear) { for (auto& u : r.units()) if (std::hypot(u.x - mx, u.y - my) < clear + u.r) return true; return false; };
		auto texel = [&](size_t mt, double mx, double my) { int tx = (int)(mx / 32), ty = (int)(my / 32); uint8_t c[3]; hd->sample(mt, (int)((mx - tx * 32) * 4), (int)((my - ty * 32) * 4), c); return 0xFF000000u | c[0] << 16 | c[1] << 8 | c[2]; };

		std::cout << "art\n";
		const CreepEdges* edges = assets.creep_edges();
		check(edges && edges->frames.size() == 37, "the HD creep edges load (TileSet/<tileset>.dds.grp: 37 frames)");
		{ std::set<int> used; for (int b = 0; b < 256; ++b) used.insert(TopDown::creep_edge_frame(b));
		  check(used.size() == 38 && *used.begin() == -1 && *used.rbegin() == 36, bwgame::format("the 256 neighbour patterns choose among exactly the 37 edge frames and none (%d)", (int)used.size()).c_str()); }

		std::cout << "terrain\n";
		// pixel checks at zoom 4 (the HD art 1:1) on a wider frame; points within reach of a unit's art or shadow are skipped
		const int BW = 1280, BH = 800; TopDown wide(g.st(), g.fn(), assets, BW, BH); std::vector<uint32_t> wfb((size_t)BW * BH);
		auto save_wide = [&](const char* name) { if (out.empty()) return; std::string p = out + "/topdown_" + name + ".ppm"; FILE* fp = fopen(p.c_str(), "wb"); if (!fp) return;
			fprintf(fp, "P6\n%d %d\n255\n", BW, BH); for (uint32_t c : wfb) { uint8_t b[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; fwrite(b, 1, 3, fp); } fclose(fp); };
		TopView v{gs.map_width / 2.0, gs.map_height / 2.0, 4}; wide.clamp(v);
		std::fill(wfb.begin(), wfb.end(), 0x00123456u); r.update_units(); wide.render(r, v, wfb.data()); save_wide("terrain_zoom4");
		check(std::count(wfb.begin(), wfb.end(), 0x00123456u) == 0, "every pixel is written");
		auto clear_of = [&](double mx, double my) { for (auto& u : r.units()) if (std::hypot(u.x - mx, u.y - my) < u.r + 64) return false; return true; };
		int tried = 0, same = 0;
		for (int y = 5; y < BH; y += 23) for (int x = 7; x < BW; x += 29) { double mx, my; wide.to_map(v, x + 0.5, y + 0.5, mx, my); int tx = (int)(mx / 32), ty = (int)(my / 32);
			bool beside = false; for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) beside |= creep(tx + dx, ty + dy);
			if (beside || !clear_of(mx, my)) continue; ++tried; same += wfb[(size_t)y * BW + x] == texel(g.st().tiles_mega_tile_index[(size_t)ty * tw + tx], mx, my); }
		check(tried >= 200 && same >= tried * 9 / 10, bwgame::format("at zoom 4 the ground away from units and creep is the map's HD tile art, texel for texel (%d of %d points)", same, tried).c_str());
		TopView corner{0, 0, 2}; td.clamp(corner); double cx0, cy0; td.to_map(corner, 0, 0, cx0, cy0);
		check(cx0 == 0 && cy0 == 0, "the camera stops at the map's edge (the corner view starts at map 0,0)");

		std::cout << "creep\n";
		// the view on the creep's edge nearest to the Hatchery: creep, its edge and plain ground in one frame
		double best = 1e9; int etx = 0, ety = 0;
		for (int ty = 0; ty < (int)gs.map_tile_height; ++ty) for (int tx = 0; tx < tw; ++tx) if (!creep(tx, ty) && (creep(tx - 1, ty) || creep(tx + 1, ty) || creep(tx, ty - 1) || creep(tx, ty + 1))) {
			double d = std::hypot(tx * 32 + 16 - mine.x, ty * 32 + 16 - mine.y); if (d < best) { best = d; etx = tx; ety = ty; } }
		v = TopView{etx * 32 + 16.0, ety * 32 + 16.0, 4}; wide.clamp(v); wide.render(r, v, wfb.data()); save_wide("creep_edge");
		int ctried = 0, ccreep = 0, cown = 0, etried = 0, eover = 0;
		std::vector<size_t> creep_tiles; for (int k = 0; k < 13; ++k) creep_tiles.push_back(gs.cv5[1].mega_tile_index[k]);
		for (int y = 3; y < BH; y += 7) for (int x = 3; x < BW; x += 7) { double mx, my; wide.to_map(v, x + 0.5, y + 0.5, mx, my); int tx = (int)(mx / 32), ty = (int)(my / 32);
			if (!clear_of(mx, my)) continue; uint32_t c = wfb[(size_t)y * BW + x]; size_t own = g.st().tiles_mega_tile_index[(size_t)ty * tw + tx];
			if (creep(tx, ty)) { ++ctried; bool any = false; for (size_t mt : creep_tiles) any |= c == texel(mt, mx, my); ccreep += any; cown += c == texel(own, mx, my) && !any; }
			else { int nb = creep(tx, ty - 1) | creep(tx, ty + 1) << 1 | creep(tx + 1, ty) << 2 | creep(tx - 1, ty) << 3 | creep(tx + 1, ty - 1) << 4 | creep(tx - 1, ty - 1) << 5 | creep(tx + 1, ty + 1) << 6 | creep(tx - 1, ty + 1) << 7;
				if (nb) { ++etried; eover += c != texel(own, mx, my); } } }
		check(ctried >= 100 && ccreep >= ctried * 9 / 10, bwgame::format("the Hatchery's creep is drawn with the tileset's creep tiles (%d of %d points; %d still the map's own art)", ccreep, ctried, cown).c_str());
		check(etried >= 100 && eover >= etried / 5 && eover < etried, bwgame::format("tiles beside the creep get its edge spilling over (%d of %d points covered)", eover, etried).c_str());

		std::cout << "sprites\n";
		v = TopView{(double)mine.x, (double)mine.y, 2}; render(v, "base");
		unit_t* hatch = g.find([&](unit_t* u) { return u->owner == g.owner() && u->unit_type->id == UnitTypes::Zerg_Hatchery; });
		unit_t* drone = g.find([&](unit_t* u) { return u->owner == g.owner() && u->unit_type->id == UnitTypes::Zerg_Drone; });
		check(td.drawn_of(g.owner()) >= 5 && td.shadows() >= 4, bwgame::format("your base is drawn: %d images of your sprites, %d shadows", td.drawn_of(g.owner()), td.shadows()).c_str());
		double hx, hy; td.to_screen(v, hatch->sprite->position.x, hatch->sprite->position.y, hx, hy);
		check(td.unit_at(hx, hy) == hatch, "a click on the Hatchery's art picks it");
		double ex, ey; bool empty_found = false;
		for (int k = 0; k < 64 && !empty_found; ++k) { ex = 40 + (k * 97) % (W - 80); ey = 40 + (k * 61) % (H - 80); double mx, my; td.to_map(v, ex, ey, mx, my); empty_found = !near_unit(mx, my, 40); }
		check(empty_found && td.unit_at(ex, ey) == nullptr, "a click on open ground picks nothing");
		double bx0, by0, bx1, by1; td.to_screen(v, mine.x - 260, mine.y - 200, bx0, by0); td.to_screen(v, mine.x + 260, mine.y + 200, bx1, by1);
		auto in_box = td.units_in(bx0, by0, bx1, by1); int own_in = 0; for (auto* u : in_box) own_in += u->owner == g.owner();
		int own_near = g.count([&](unit_t* u) { return u->owner == g.owner() && std::abs(u->sprite->position.x - mine.x) <= 260 && std::abs(u->sprite->position.y - mine.y) <= 200; });
		check(own_in == own_near && own_in >= 5, bwgame::format("a selection box around the base finds your units in it (%d of %d)", own_in, own_near).c_str());
		// StarCraft's order: ground units from north to south, then what flies over them (an Overlord drawn last though further
		// north); a click where the Overlord covers a Drone picks the Overlord
		unit_t* a = g.spawn(UnitTypes::Zerg_Zergling, xy{mine.x + 90, mine.y + 60}, g.owner());
		unit_t* b = a ? g.spawn(UnitTypes::Zerg_Zergling, xy{a->sprite->position.x, a->sprite->position.y + 30}, g.owner()) : nullptr;
		unit_t* over = drone ? g.spawn(UnitTypes::Zerg_Overlord, xy{drone->sprite->position.x, drone->sprite->position.y - 2}, g.owner()) : nullptr;
		render(v, "order"); double dx0 = 0, dy0 = 0; if (drone) td.to_screen(v, drone->sprite->position.x, drone->sprite->position.y, dx0, dy0);
		check(a && b && b->sprite->position.y > a->sprite->position.y && td.order_of(a) >= 0 && td.order_of(b) > td.order_of(a), bwgame::format("of two Zerglings the one further south is drawn later (%d after %d)", b ? td.order_of(b) : -1, a ? td.order_of(a) : -1).c_str());
		check(over && td.order_of(over) > td.order_of(b) && td.order_of(over) > td.order_of(drone), "an Overlord is drawn over the ground units, wherever it is");
		check(over && td.unit_at(dx0, dy0) == over, "a click where it covers a Drone picks the Overlord");
		const unit_t* other = g.find([&](unit_t* u) { return u->owner == g.owner() && u->unit_type->id == UnitTypes::Zerg_Drone && u != drone; });
		TopOverlay ov; ov.me = g.owner(); ov.selected = {hatch, drone}; ov.hover = other;
		render(v, "selected", &ov);
		check(td.circles() == 3 && td.bars() == 2, bwgame::format("selection circles under the two selected units and the one under the mouse, health bars for the selected (%d, %d)", td.circles(), td.bars()).c_str());

		std::cout << "same picture on any number of threads\n";
		{ std::vector<uint32_t> one(fb.size()), many(fb.size()); r.update_units(); td.render(r, v, one.data(), &ov, 1); td.render(r, v, many.data(), &ov, 7);
		  check(one == many, "one thread and seven draw the same pixels"); }

		std::cout << "fog of war (as player " << g.owner() + 1 << " sees it)\n";
		r.set_viewer(g.owner()); step(1);
		TopView at_them{(double)theirs.x, (double)theirs.y, 2}; td.clamp(at_them);
		render(at_them, "fog_unexplored"); double dark = 0; for (uint32_t c : fb) dark += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
		int their_seen = td.drawn_of(g.enemy());
		r.set_viewer(-1); render(at_them); double plain = 0; for (uint32_t c : fb) plain += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
		int their_all = td.drawn_of(g.enemy()); r.set_viewer(g.owner()); r.update_units();
		check(their_seen == 0 && their_all >= 3, bwgame::format("the other base, never seen, shows none of its sprites (%d drawn; %d without the fog)", their_seen, their_all).c_str());
		check(dark < plain * 0.4, bwgame::format("and its ground is dark (mean %.0f vs %.0f without the fog of war)", dark / fb.size() / 3, plain / fb.size() / 3).c_str());
		unit_t* nexus = g.find([&](unit_t* u) { return u->owner == g.enemy() && u->unit_type->id == UnitTypes::Protoss_Nexus; });
		unit_t* scout = nullptr; for (int d = 120; d < 320 && !scout; d += 16) for (int k = 0; k < 8 && !scout; ++k) scout = g.spawn(UnitTypes::Zerg_Overlord, xy{theirs.x + (int)(std::cos(k * 0.785) * d), theirs.y + (int)(std::sin(k * 0.785) * d)}, g.owner());
		int f0 = g.frame(); while (g.frame() - f0 < 150 && r.fog_at(theirs.x, theirs.y) != Renderer::kLit) step(1);
		render(at_them, "fog_scouted"); int seen = td.drawn_of(g.enemy());
		g.fn().kill_unit(scout); f0 = g.frame(); while (g.frame() - f0 < 200 && r.fog_at(theirs.x, theirs.y) != Renderer::kFogged) step(1);
		render(at_them, "fog_remembered"); double nx, ny; td.to_screen(at_them, nexus->sprite->position.x, nexus->sprite->position.y, nx, ny);
		check(seen >= 3 && r.remembered() >= 1 && td.drawn_of(g.enemy()) >= 1 && td.drawn_of(g.enemy()) < seen && td.unit_at(nx, ny) == nullptr,
		      bwgame::format("scouted, their base is drawn (%d images); in the fog again only the remembered buildings stay (%d), and they cannot be picked", seen, td.drawn_of(g.enemy())).c_str());
		unit_t* dt = nullptr; for (int d = 120; d < 320 && !dt; d += 16) for (int k = 0; k < 8 && !dt; ++k) dt = g.spawn(UnitTypes::Protoss_Dark_Templar, xy{mine.x + (int)(std::cos(k * 0.785) * d), mine.y + (int)(std::sin(k * 0.785) * d)}, g.enemy());
		step(24); TopView at_dt{(double)dt->sprite->position.x, (double)dt->sprite->position.y, 2}; td.clamp(at_dt); render(at_dt, "shimmer"); double dx, dy; td.to_screen(at_dt, dt->sprite->position.x, dt->sprite->position.y, dx, dy);
		check(dt && g.fn().unit_is_undetected(dt, g.owner()) && td.shimmers() >= 1 && td.unit_at(dx, dy) != dt, bwgame::format("an undetected Dark Templar in your base is a shimmer (%d images) that cannot be picked", td.shimmers()).c_str());

		std::cout << "cost\n";
		{ TopDown big(g.st(), g.fn(), assets, 1280, 800); std::vector<uint32_t> bfb(1280 * 800); r.set_viewer(-1); r.update_units();
		  TopView bv{(double)mine.x, (double)mine.y, 2}; big.clamp(bv);
		  auto ms = [&](unsigned threads) { auto t0 = std::chrono::steady_clock::now(); big.render(r, bv, bfb.data(), nullptr, threads); return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
		  double first = ms(0), one = 1e9, all = 1e9; for (int i = 0; i < 9; ++i) { one = std::min(one, ms(1)); all = std::min(all, ms(0)); }
		  check(one < 40 && all < 12, bwgame::format("a 1280x800 frame takes %.1f ms on one thread, %.1f ms on all cores (the first, decoding the art: %.0f ms)", one, all, first).c_str());
		  r.set_viewer(g.owner()); r.update_units(); big.render(r, bv, bfb.data()); double mm = 1e9;
		  for (int i = 0; i < 9; ++i) { auto t0 = std::chrono::steady_clock::now(); big.minimap(r, bfb.data(), 8, 592, 200); mm = std::min(mm, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
		  double mx, my; bool on = big.on_minimap(108, 692, mx, my);
		  check(mm < 2 && on && std::fabs(mx - gs.map_width / 2.0) < 40 && std::fabs(my - gs.map_height / 2.0) < 40, bwgame::format("the minimap (200 px, with the fog of war) takes %.2f ms; its middle is the map's", mm).c_str()); }
		std::string miss; for (int id : assets.missing_images()) miss += " " + std::to_string(id);
		check(miss.empty(), "every image drawn has HD art" + (miss.empty() ? std::string() : ": missing" + miss));
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
	std::cout << (failures ? "TOPDOWNTEST FAILED" : "TOPDOWNTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
