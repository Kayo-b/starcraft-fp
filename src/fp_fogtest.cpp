// Headless test of the fog of war as the renderer draws it for one player (fp_render.h with a viewer): the terrain's
// brightness by what the player sees, has seen and has never seen, faded; enemy buildings remembered in the fog until
// their spot is in sight again; cloaked units (a shimmer while undetected, see-through once detected, burrowed ones not
// at all), and the FPS layer not targeting what is not detected. Exit code 0 only if every check passes.
// A third argument (a directory) writes PPM frames of the checks to look at.
#include "fp_fps.h"
#include "fp_render.h"
#include <chrono>
#include <cstdio>
#include <iostream>

using namespace fp;
static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }

int main(int argc, char** argv) {
	if (argc < 3) { std::cerr << "usage: fp_fogtest DATA_DIR MAP.scm [FRAMES_DIR]\n"; return 2; }
	std::cout << std::unitbuf;
	const std::string out = argc > 3 ? argv[3] : "";
	try {
		Game g; g.open(argv[1], argv[2], race_t::terran, race_t::protoss); // a melee start: both bases
		Assets assets(argv[1]); const int W = 320, H = 200; Renderer r(g.st(), g.fn(), assets, W, H); r.set_viewer(g.owner());
		Fps f(g, 7, 200, 200, 90, true); // targeting rules only (no heroes)
		std::vector<uint32_t> fb((size_t)W * H);
		auto step = [&](int n) { for (int i = 0; i < n; ++i) { g.step(); r.update_units(1, 1.0 / 24); } };
		auto drawn = [&](UnitTypes t, int owner) -> const UnitView* { for (auto& v : r.units()) if (v.type == (int)t && v.owner == owner) return &v; return nullptr; };
		// A camera `dist` px from (x,y) on standing ground, looking at it; returns false if there is no such spot.
		auto aim = [&](Camera& cam, double x, double y, double dist) {
			for (int k = 0; k < 16; ++k) { double a = k * 3.14159265358979 / 8, cx = x + std::cos(a) * dist, cy = y + std::sin(a) * dist;
				if (!r.can_stand(cx, cy, 6)) continue;
				cam.x = cx; cam.y = cy; cam.heading = std::atan2(y - cy, x - cx) * 180 / 3.14159265358979; cam.pitch = 0; return true; }
			return false; };
		auto frame = [&](const Camera& cam, const char* name) {
			r.render(cam, nullptr, fb.data());
			if (out.empty()) return;
			std::string p = out + "/fog_" + name + ".ppm"; FILE* fp = fopen(p.c_str(), "wb"); if (!fp) return;
			fprintf(fp, "P6\n%d %d\n255\n", W, H); for (uint32_t c : fb) { uint8_t b[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; fwrite(b, 1, 3, fp); } fclose(fp);
		};
		auto brightness = [&]() { double s = 0; for (int y = H / 2; y < H; ++y) for (int x = 0; x < W; ++x) { uint32_t c = fb[(size_t)y * W + x]; s += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255); } return s / (W * H / 2 * 3); };
		auto spawn_near = [&](UnitTypes t, xy at, int owner, int from = 80) -> unit_t* {
			for (int d = from; d < from + 200; d += 16) for (int k = 0; k < 8; ++k) {
				unit_t* u = g.spawn(t, xy{at.x + (int)(std::cos(k * 0.785398) * d), at.y + (int)(std::sin(k * 0.785398) * d)}, owner); if (u) return u; }
			return nullptr; };
		step(2);
		xy mine = g.start_location(g.owner()), theirs = g.start_location(g.enemy());
		unit_t* nexus = g.find([&](unit_t* u) { return u->owner == g.enemy() && u->unit_type->id == UnitTypes::Protoss_Nexus; });

		std::cout << "terrain\n";
		check(r.fog_at(mine.x, mine.y) == Renderer::kLit && r.fog_at(theirs.x, theirs.y) == Renderer::kUnexplored,
		      bwgame::format("lit around your base (%.2f), dark at the other base, never seen (%.2f)", r.fog_at(mine.x, mine.y), r.fog_at(theirs.x, theirs.y)).c_str());
		check(nexus && !drawn(UnitTypes::Protoss_Nexus, g.enemy()) && r.remembered() == 0, "the other base's Nexus is not drawn and not remembered: never seen");
		{ Camera out_of_base; double ox = g.st().game->map_width / 2.0 - mine.x, oy = g.st().game->map_height / 2.0 - mine.y, ol = std::hypot(ox, oy);
		  out_of_base.x = mine.x + ox / ol * 140; out_of_base.y = mine.y + oy / ol * 140; out_of_base.heading = std::atan2(oy, ox) * 180 / 3.14159265358979; frame(out_of_base, "from_base"); }
		Camera cam; bool aimed = aim(cam, theirs.x, theirs.y, 260);
		frame(cam, "unexplored"); double dark = brightness();
		r.set_viewer(-1); r.update_units(); frame(cam, "unexplored_without_fog"); double plain = brightness();
		r.set_viewer(g.owner()); r.update_units();
		check(aimed && dark < plain * 0.45, bwgame::format("a view of it is drawn darker than without the fog of war (mean %.0f vs %.0f)", dark, plain).c_str());

		std::cout << "a remembered building\n";
		unit_t* scout = spawn_near(UnitTypes::Terran_Marine, theirs, g.owner(), 120);
		int f0 = g.frame(); while (g.frame() - f0 < 150 && r.fog_at(theirs.x, theirs.y) != Renderer::kLit) step(1);
		const UnitView* nv = drawn(UnitTypes::Protoss_Nexus, g.enemy());
		check(scout && r.fog_at(theirs.x, theirs.y) == Renderer::kLit && nv && nv->ptr == nexus && r.remembered() == 1,
		      bwgame::format("a Marine sent there lights it up (%.2f after %d frames) and the Nexus is drawn as itself", r.fog_at(theirs.x, theirs.y), g.frame() - f0).c_str());
		frame(cam, "scouted");
		g.fn().kill_unit(scout);
		f0 = g.frame(); float lo = 1, mid = -1; // the fog closes at OpenBW's next vision refresh (every 100 frames), faded over a few frames
		while (g.frame() - f0 < 200 && r.fog_at(theirs.x, theirs.y) != Renderer::kFogged) { step(1); float v = r.fog_at(theirs.x, theirs.y); lo = std::min(lo, v); if (mid < 0 && v > Renderer::kFogged && v < Renderer::kLit) mid = v; }
		check(r.fog_at(theirs.x, theirs.y) == Renderer::kFogged && mid > 0, bwgame::format("the Marine gone, the base is fogged (%.2f after %d frames), fading through %.2f", r.fog_at(theirs.x, theirs.y), g.frame() - f0, mid).c_str());
		nv = drawn(UnitTypes::Protoss_Nexus, g.enemy());
		check(!(nexus->sprite->visibility_flags & (1 << g.owner())) && nv && !nv->ptr && nv->x == nexus->sprite->position.x && nv->y == nexus->sprite->position.y && std::fabs(nv->dim - Renderer::kFogged) < 0.01,
		      "the Nexus, out of sight, is still drawn where it was seen, dimmed, and cannot be targeted");
		frame(cam, "remembered");
		Camera at_nexus; aim(at_nexus, nexus->sprite->position.x, nexus->sprite->position.y, 200); at_nexus.pitch = 20; r.render(at_nexus, nullptr, fb.data());
		const unit_t* picked = r.pick_center(at_nexus, nullptr).unit;
		check(!picked || picked->owner != g.enemy(), "the crosshair on the remembered Nexus picks nothing of the enemy's (what is behind it, if anything)");
		g.fn().kill_unit(nexus); step(60);
		check(drawn(UnitTypes::Protoss_Nexus, g.enemy()) && r.remembered() == 1, "destroyed out of sight, it is still remembered: nobody saw it go");
		scout = spawn_near(UnitTypes::Terran_Marine, theirs, g.owner(), 120);
		f0 = g.frame(); while (g.frame() - f0 < 150 && r.fog_at(theirs.x, theirs.y) != Renderer::kLit) step(1);
		check(scout && !drawn(UnitTypes::Protoss_Nexus, g.enemy()) && r.remembered() == 0, "seen again, the spot is empty and the memory goes");

		std::cout << "cloaked units\n";
		// In the open toward the map centre, lit by a Supply Depot (a Dark Templar kills a Marine at one blow), the camera on
		// the Templar with open ground behind it; the turret comes later, behind the Templar as the camera sees it.
		double cx = g.st().game->map_width / 2.0 - mine.x, cy = g.st().game->map_height / 2.0 - mine.y, cl = std::hypot(cx, cy);
		unit_t* depot = spawn_near(UnitTypes::Terran_Supply_Depot, xy{mine.x + (int)(cx / cl * 420), mine.y + (int)(cy / cl * 420)}, g.owner(), 0);
		step(24);
		unit_t* dt = depot ? spawn_near(UnitTypes::Protoss_Dark_Templar, depot->sprite->position, g.enemy(), 80) : nullptr;
		step(24);
		const UnitView* dv = drawn(UnitTypes::Protoss_Dark_Templar, g.enemy());
		check(dt && g.fn().unit_is_undetected(dt, g.owner()) && dv && dv->look == UnitView::kShimmer && !dv->ptr, "an enemy Dark Templar where you see, undetected, is drawn as a shimmer that cannot be targeted");
		check(dt && !f.is_enemy(dt), "and the FPS layer does not take it as a target");
		Camera at_dt; bool dt_aimed = dt && aim(at_dt, dt->sprite->position.x, dt->sprite->position.y, 90); at_dt.pitch = 10;
		if (dt_aimed) { frame(at_dt, "shimmer"); const unit_t* pk = r.pick_center(at_dt, nullptr).unit;
			check(pk != dt, "the crosshair on the shimmer does not pick the Templar"); }
		unit_t* turret = nullptr;
		if (dt && dt_aimed) { double bx = dt->sprite->position.x - at_dt.x, by = dt->sprite->position.y - at_dt.y, bl = std::hypot(bx, by);
			turret = spawn_near(UnitTypes::Terran_Missile_Turret, xy{dt->sprite->position.x + (int)(bx / bl * 90), dt->sprite->position.y + (int)(by / bl * 90)}, g.owner(), 0); }
		step(24); dv = drawn(UnitTypes::Protoss_Dark_Templar, g.enemy());
		check(turret && !g.fn().unit_is_undetected(dt, g.owner()) && dv && dv->look == UnitView::kSeeThrough && dv->ptr == dt && f.is_enemy(dt),
		      "a Missile Turret next to it detects it: drawn see-through, a target");
		if (dt_aimed) frame(at_dt, "detected");
		if (turret) g.fn().kill_unit(turret); // no detector left
		g.st().tech_researched[g.enemy()][TechTypes::Burrowing] = true;
		unit_t* z = spawn_near(UnitTypes::Zerg_Zergling, mine, g.enemy(), 160);
		if (z) g.fn().issue_order(z, false, g.fn().get_order_type(Orders::Burrowing), {});
		step(48);
		check(z && g.fn().u_burrowed(z) && g.fn().unit_is_undetected(z, g.owner()) && !drawn(UnitTypes::Zerg_Zergling, g.enemy()), "an enemy Zergling burrowed in your base, undetected, is not drawn at all");

		std::cout << "cost\n";
		{ Renderer big(g.st(), g.fn(), assets, 640, 400); std::vector<uint32_t> bfb(640 * 400); Camera c; aim(c, theirs.x, theirs.y, 400);
		  // one render thread, the two alternated and the fastest of each taken: other work on the machine only slows a run down
		  auto ms = [&](int viewer) { big.set_viewer(viewer); big.update_units(); auto t0 = std::chrono::steady_clock::now(); big.render(c, nullptr, bfb.data(), nullptr, 1);
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
		  double without = 1e9, with = 1e9; for (int i = 0; i < 15; ++i) { without = std::min(without, ms(-1)); with = std::min(with, ms(g.owner())); }
		  check(with < without * 1.4, bwgame::format("a 640x400 frame on one thread takes %.1f ms with the fog of war, %.1f ms without", with, without).c_str()); }
		std::cout << "without a viewer (replays, single player)\n";
		Renderer all(g.st(), g.fn(), assets, W, H); all.update_units();
		const UnitView* av = nullptr; for (auto& v : all.units()) if (v.ptr == dt) av = &v;
		check(all.fog_at(theirs.x, theirs.y) == 1 && all.remembered() == 0 && av && av->look == UnitView::kSeeThrough, "no fog, nothing remembered, a cloaked unit drawn see-through for all to see");
		std::string miss; for (int id : assets.missing_images()) miss += " " + std::to_string(id);
		check(miss.empty(), "every unit drawn here has HD art (the Command Center and the Dark Templar through the GRP they share)" + (miss.empty() ? std::string() : ": missing" + miss));
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
	std::cout << (failures ? "FOGTEST FAILED" : "FOGTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
