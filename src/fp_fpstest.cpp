// Headless behaviour test for the FPS layer (fp_fps.h). Exit code 0 only if every check passes.
#include "fp_fps.h"
#include "fp_render.h"
#include <iostream>

using namespace fp;
static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }

struct Outcome { int kills, wave, frames; bool alive; int min_hp; uint64_t sum; };
static int p0hp = 0; // the player's HP at the start of the first fight

// A fight where the player holds fire on the nearest enemy that is in weapon range.
static Outcome fight(const std::string& data, const std::string& map, int max_frames, bool verbose) {
	Game g; g.open(data, map, race_t::terran, race_t::zerg, 1, true);
	Fps fps(g); bool started = fps.start();
	if (verbose) {
		int mine = g.count([&](unit_t* u) { return u->owner == g.owner(); }), foes = g.count([&](unit_t* u) { return u->owner == g.enemy(); });
		int neutral = g.count([&](unit_t* u) { return u->owner >= 8; });
		check(started && mine == 1, "FPS arena: the player has exactly one unit (" + std::to_string(mine) + "), enemy has none (" + std::to_string(foes) + ")");
		check(neutral > 0, "map scenery is kept (" + std::to_string(neutral) + " neutral units, e.g. mineral fields)");
		unit_t* p = fps.player();
		check(p && p->unit_type->id == UnitTypes::Hero_Jim_Raynor_Marine, "player is Jim Raynor's Marine (hero) with " + std::to_string(p ? (int)p->hp.integer_part() : 0) + " HP");
		check(fps.range(p) >= 300 && fps.range(p) <= 360, "hero weapon range from OpenBW (scaled 200%): " + std::to_string((int)fps.range(p)) + " px");
	}
	int min_hp = 999; p0hp = (int)fps.player()->hp.integer_part(); bool wave_checked = false, ai_checked = false, range_checked = false; uint64_t sum = 1469598103934665603ull;
	int f = 0;
	for (; f < max_frames && fps.alive(); ++f) {
		g.step(); fps.tick();
		unit_t* p = fps.player(); if (!p) break;
		min_hp = std::min(min_hp, (int)p->hp.integer_part());
		if (verbose && !wave_checked && fps.wave == 1) { wave_checked = true;
			int n = g.count([&](unit_t* u) { return u->owner == g.enemy(); });
			int zl = g.count([&](unit_t* u) { return u->owner == g.enemy() && u->unit_type->id == UnitTypes::Zerg_Zergling; });
			int reach = g.count([&](unit_t* u) { return u->owner == g.enemy() && g.fn().is_reachable(p->sprite->position, u->sprite->position); });
			check(n == Fps::wave_lead(1) + Fps::wave_extra(1) && zl == n && reach == n, "wave 1 at frame " + std::to_string(g.frame()) + ": " + std::to_string(n) + " enemies, " + std::to_string(zl) + " Zerglings, " + std::to_string(reach) + " reachable from the player"); }
		if (verbose && wave_checked && !ai_checked && g.frame() % 6 == 0) { ai_checked = true; // one AI pass later
			int hunting = g.count([&](unit_t* u) { return u->owner == g.enemy() && u->order_target.unit == p && (u->order_type->id == Orders::AttackUnit || u->order_type->id == Orders::AttackMove); });
			int total = g.count([&](unit_t* u) { return u->owner == g.enemy(); });
			check(hunting == total && total > 0, "enemy AI: all " + std::to_string(total) + " enemies are ordered to attack the player (" + std::to_string(hunting) + " are)"); }
		if (verbose && !range_checked && wave_checked && ai_checked) { range_checked = true;
			unit_t* far = g.nearest(p->sprite->position.x, p->sprite->position.y, [&](unit_t* u) { return u->owner == g.enemy(); });
			if (!far) { check(false, "an enemy exists to test fire() range gating on (none found at frame " + std::to_string(g.frame()) + ")"); continue; }
			double d = std::hypot(far->sprite->position.x - p->sprite->position.x, far->sprite->position.y - p->sprite->position.y);
			if (d > fps.range(p) + 40) {
				int hp_before = (int)far->hp.integer_part();
				check(fps.fire(far) == Fps::ShotResult::miss, "fire() out of range takes the shot and reports a miss (distance " + std::to_string((int)d) + " px)");
				check(fps.misses == 1 && (int)far->hp.integer_part() == hp_before, "the miss deals no damage (target HP " + std::to_string(hp_before) + " -> " + std::to_string((int)far->hp.integer_part()) + ")");
				check(p->order_target.unit != far && p->order_type->id != Orders::AttackUnit, "the miss issues no attack order (hero order " + std::to_string((int)p->order_type->id) + ")");
			} }
		// shoot the nearest enemy in range
		unit_t* tgt = g.nearest(p->sprite->position.x, p->sprite->position.y, [&](unit_t* u) { return u->owner == g.enemy() && fps.in_range(u); });
		fps.set_trigger(tgt != nullptr); if (tgt) fps.fire(tgt);
		sum ^= (uint64_t)(g.frame() * 1000003 + p->sprite->position.x * 31 + p->sprite->position.y * 17 + p->hp.integer_part() + fps.kills * 7919); sum *= 1099511628211ull;
	}
	return {fps.kills, fps.wave, f, fps.alive(), min_hp, sum};
}

int main(int argc, char** argv) {
	std::cout << std::unitbuf;
	if (argc < 3) { std::cerr << "usage: fp_fpstest DATA_DIR MAP.scm\n"; return 2; }
	try {
		Outcome a = fight(argv[1], argv[2], 2400, true), b = fight(argv[1], argv[2], 2400, false);
		std::cout << "fight: " << a.frames << " frames, waves " << a.wave << ", kills " << a.kills << ", lowest HP " << a.min_hp << ", player " << (a.alive ? "alive" : "dead") << "\n";
		check(a.kills >= 1, "shooting kills enemies (" + std::to_string(a.kills) + " kills)");
		check(a.min_hp < 999 && a.min_hp < p0hp, "enemies hurt the player (lowest HP " + std::to_string(a.min_hp) + ")");
		check(a.sum == b.sum && a.kills == b.kills && a.frames == b.frames, "determinism: two identical fights end the same way");
		// A Terran enemy leads with Marines from wave 1; Firebats only join from wave 3.
		{ Game t; t.open(argv[1], argv[2], race_t::terran, race_t::terran, 1, true); Fps ft(t); ft.start();
		  for (int i = 0; i < 400 && ft.wave < 1; ++i) { t.step(); ft.tick(); }
		  int marines = t.count([&](unit_t* u) { return u->owner == t.enemy() && u->unit_type->id == UnitTypes::Terran_Marine; });
		  int firebats = t.count([&](unit_t* u) { return u->owner == t.enemy() && u->unit_type->id == UnitTypes::Terran_Firebat; });
		  check(ft.wave == 1 && marines == Fps::wave_lead(1) && marines == 2 && firebats == 0,
		        "Terran wave 1: " + std::to_string(marines) + " Marines and " + std::to_string(firebats) + " Firebats"); }
		// Duel: one Zergling inside weapon range; fire() must damage it and eventually kill it.
		{ Game d; d.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true); Fps f2(d); f2.start(); unit_t* p = f2.player();
		  xy at{p->sprite->position.x + 100, p->sprite->position.y}; unit_t* z = d.spawn(UnitTypes::Zerg_Zergling, at, d.enemy());
		  if (!z) z = d.spawn(UnitTypes::Zerg_Zergling, xy{p->sprite->position.x - 100, p->sprite->position.y}, d.enemy());
		  check(z != nullptr && f2.in_range(z), "duel setup: a Zergling is inside weapon range");
		  // Trigger up: an idle hero must not shoot an enemy that is right in front of it (StarCraft units normally would).
		  { int hpz = z ? (int)z->hp.integer_part() : 0; f2.set_trigger(false); for (int i = 0; i < 90; ++i) { d.step(); f2.tick(); }
		    check(z && (int)z->hp.integer_part() == hpz && f2.shots == 0, "trigger up: the hero does not auto-fire at an enemy in range (target HP " + std::to_string(hpz) + " -> " + std::to_string(z ? (int)z->hp.integer_part() : 0) + ", " + std::to_string(f2.shots) + " shots)"); }
		  f2.set_trigger(true);
		  int hp0 = z ? z->hp.integer_part() : 0; bool fired = z && f2.fire(z) == Fps::ShotResult::hit; int hp_after = hp0, killed_at = -1; unit_id zid = d.fn().get_unit_id(z);
		  for (int i = 0; i < 400 && fired; ++i) { d.step(); f2.tick(); unit_t* zz = d.fn().get_unit(zid); if (!zz) { killed_at = i; break; } hp_after = std::min(hp_after, (int)zz->hp.integer_part()); if (i % 10 == 0) f2.fire(zz); }
		  check(fired, "fire() accepted an in-range enemy");
		  { const weapon_type_t* w = f2.player()->unit_type->ground_weapon.operator->(); check(w && w->cooldown <= 10, "hero fire rate: the weapon cooldown is scaled down (" + std::to_string(w ? w->cooldown : -1) + " frames)"); }
		  check(f2.range(f2.player()) >= 300, "hero range: the weapon reaches at least 300 px (" + std::to_string((int)f2.range(f2.player())) + " px)");
		  check(killed_at >= 0 || hp_after < hp0, "shooting damages the target (HP " + std::to_string(hp0) + " -> " + (killed_at >= 0 ? std::string("dead after ") + std::to_string(killed_at) + " frames" : std::to_string(hp_after)) + ")");
		  check(f2.shots >= 1, "shots are counted from the weapon cooldown (" + std::to_string(f2.shots) + " shots fired in the duel)");
		  check(killed_at >= 0 && f2.alive(), "the hero wins the duel and survives (HP " + std::to_string(f2.alive() ? (int)f2.player()->hp.integer_part() : 0) + ")");
		  // The game keeps last frame's crosshair pick across a simulation step: a pointer to the dead Zergling must be safe and no target.
		  check(killed_at >= 0 && !f2.is_enemy(z) && !f2.in_range(z) && f2.fire(z) == Fps::ShotResult::none, "a dead enemy's stale pointer is no target (is_enemy, in_range, fire)"); }
		// Direct-control mode: velocity movement in any direction, walk-and-shoot, goal steering, position ownership.
		{ Game m; m.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true); Fps f3(m, 7, 200, 200, 90, true); f3.start(); unit_t* p = f3.player();
		  // speed scaling: the hero's flingy top speed is 150% of the stock value
		  { Game m2; m2.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true);
			unit_t* stock = m2.spawn(UnitTypes::Hero_Jim_Raynor_Marine, m2.start_location(m2.owner()), m2.owner());
			check(stock && p, "direct mode: both heroes spawn for the speed comparison");
			if (stock && p) { int base = (int)stock->flingy_top_speed.integer_part();
				int scaled = (int)p->flingy_top_speed.integer_part();
				check(base > 0 && scaled == std::max(1, base * 90 / 100), "direct mode: hero movement speed is scaled 90% (" + std::to_string(base) + " -> " + std::to_string(scaled) + ")"); } }
		  // the camera's view of the hero runs ahead of the simulation by the input not applied yet: exactly where it ends up
		  { double x0, y0, x1, y1; f3.drive(1, 0);
			bool ok = f3.view_position(0, x0, y0) && f3.view_position(1, x1, y1);
			auto at0 = f3.mover_position(m.owner());
			m.step(); f3.tick(); auto at1 = f3.mover_position(m.owner());
			check(ok && x0 == at0.first && y0 == at0.second && x1 == at1.first && y1 == at1.second && x1 > x0,
			      "direct mode: the view predicts the next frame exactly (" + std::to_string(x0) + " -> " + std::to_string(x1) + ", simulation " + std::to_string(at1.first) + ")"); }
		  check(f3.speed() > 0 && f3.speed() <= 200, "direct mode: velocity ramps toward the cap (" + std::to_string((int)f3.speed()) + ")");
		  double path = 0; int bad = 0; double lx = p->sprite->position.x, ly = p->sprite->position.y;
		  for (int d = 0; d < 4; ++d) for (int i = 0; i < 60; ++i) {
			double ix = (d == 0) - (d == 2), iy = (d == 1) - (d == 3);
			f3.drive(ix, iy); m.step(); f3.tick();
			path += std::hypot(p->sprite->position.x - lx, p->sprite->position.y - ly);
			lx = p->sprite->position.x; ly = p->sprite->position.y;
			double r = (p->unit_type->dimensions.from.x + p->unit_type->dimensions.to.x + 1) / 4.0; bool ok = true;
			for (int dy = -1; dy <= 1 && ok; dy += 2) for (int dx = -1; dx <= 1; dx += 2) {
				int px = (int)std::floor(p->sprite->position.x + dx * r), py = (int)std::floor(p->sprite->position.y + dy * r);
				if (px < 0 || py < 0 || px >= (int)m.st().game->map_width || py >= (int)m.st().game->map_height || !m.fn().is_walkable(xy{px, py})) ok = false; }
			if (!ok) ++bad; }
		  check(path > 60, "direct mode: the hero moves in every direction (" + std::to_string((int)path) + " px of path)");
		  check(bad == 0, "direct mode: never leaves walkable ground (0 violations)");
		  // goal steering
		  { double gx = p->sprite->position.x, gy = p->sprite->position.y; double gxx = 0, gyy = 0; bool found = false;
			for (int d = 0; d < 4 && !found; ++d) { double ix = (d == 0) - (d == 2), iy = (d == 1) - (d == 3), tx = gx + ix * 80, ty = gy + iy * 80;
				bool clear = true; for (double t = 0; t <= 80 && clear; t += 8) { int px = (int)(gx + ix * t), py = (int)(gy + iy * t);
					if (px < 0 || py < 0 || px >= (int)m.st().game->map_width || py >= (int)m.st().game->map_height || !m.fn().is_walkable(xy{px, py})) clear = false; }
				if (clear) { gxx = tx; gyy = ty; found = true; } }
			if (found) { f3.go_to(gxx, gyy);
				for (int i = 0; i < 120; ++i) { f3.drive(0, 0); m.step(); f3.tick(); }
				double dg = std::hypot(p->sprite->position.x - gxx, p->sprite->position.y - gyy);
				check(dg < 40, "direct mode: goal steering stops close to the point (" + std::to_string((int)dg) + " px away)"); } }
		  // walk-and-shoot
		  { unit_t* z = m.spawn(UnitTypes::Zerg_Zergling, xy{p->sprite->position.x + 90, p->sprite->position.y}, m.enemy());
			if (z) { f3.set_trigger(true); int hp0 = (int)z->hp.integer_part(), hits = 0, shots0 = f3.shots;
				for (int i = 0; i < 60; ++i) { f3.drive(0, 1); m.step(); f3.tick(); if (f3.in_range(z) && f3.fire(z) == Fps::ShotResult::hit) ++hits; }
				check((int)z->hp.integer_part() < hp0 || m.fn().unit_dead(z), "direct mode: the hero shoots while moving (target HP " + std::to_string(hp0) + " -> " + std::to_string((int)z->hp.integer_part()) + ")");
				check(hits > 0 && f3.shots - shots0 == hits, "direct mode: every shot is counted once (" + std::to_string(hits) + " shots, " + std::to_string(f3.shots - shots0) + " counted)"); } }
		  // position ownership: enemies running over the hero must not displace it
		  { f3.halt(); double hx = p->sprite->position.x, hy = p->sprite->position.y;
			for (int i = 0; i < 3; ++i) { unit_t* z = m.spawn(UnitTypes::Zerg_Zergling, xy{(int)hx + 2 + i * 6, (int)hy + 2}, m.enemy()); if (z) m.attack_as(m.enemy(), z, p); }
			for (int i = 0; i < 120; ++i) { m.step(); f3.tick(); f3.drive(0, 0); }
			check(p->sprite->position.x == (int)std::lround(hx) && p->sprite->position.y == (int)std::lround(hy), "direct mode: enemies cannot shove the hero (position unchanged)"); }
		}
		// Levels: on Bottleneck's (3284,500) a plateau edge looks south down a cliff into a low trench (y 548-588).
		// (Positions are Bottleneck's; on other maps this part is skipped.) The hero must see and
		// target an enemy below from the edge, and one on the edge from below, within the look-up/down range (rendered).
		if (std::string(argv[2]).find("Bottleneck") != std::string::npos) {
			auto aim_check = [&](xy hero_at, xy enemy_at, double aim_above, const char* what) {
				Game g2; g2.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true); Fps f(g2, 7, 200, 200, 90, true); f.start();
				f.place(hero_at.x, hero_at.y); g2.step(); f.tick();
				unit_t* h = f.player(); unit_t* z = g2.spawn(UnitTypes::Zerg_Zergling, enemy_at, g2.enemy());
				if (!h || !z) { check(false, std::string(what) + ": hero and enemy placed"); return; }
				Assets assets(argv[1]); Renderer r(g2.st(), g2.fn(), assets, 640, 400); r.update_units();
				const double PI = 3.14159265358979323846, dx = z->sprite->position.x - h->sprite->position.x, dy = z->sprite->position.y - h->sprite->position.y, dist = std::hypot(dx, dy);
				const Terrain& T = r.terrain(); double proj = 320.0 / std::tan(r.params().fov * PI / 360.0);
				double eye = r.params().eye_h + T.floor_height(h->sprite->position.x, h->sprite->position.y), target = T.floor_height(z->sprite->position.x, z->sprite->position.y) + aim_above;
				Camera cam; cam.x = h->sprite->position.x; cam.y = h->sprite->position.y; cam.heading = std::atan2(dy, dx) * 180 / PI; cam.pitch = -(eye - target) * proj / dist;
				std::vector<uint32_t> fb(640 * 400); r.render(cam, h, fb.data());
				Pick pk = r.pick_center(cam, h); const unit_t* near = r.nearest_to_crosshair(cam, h, 0.045 * 640, [&](const unit_t* u) { return f.is_enemy(u); });
				check(std::fabs(cam.pitch) <= kMaxPitch * 400 && (pk.unit == z || near == z) && f.in_range(z),
				      std::string(what) + " (" + std::to_string((int)(eye - target)) + " px of height over " + std::to_string((int)dist) + " px, pitch " + std::to_string((int)cam.pitch) + ")");
			};
			// The hero walks up to the walkable edge (y 507 is the last walkable row; the hero is about 4 px wide); an enemy below
			// is seen past the edge. From below (the high platform's edge at (2036,2204) over open main-level ground), an enemy
			// as near the edge as OpenBW lets it stand shows its upper body over it.
			aim_check(xy{3284, 503}, xy{3284, 572}, 6, "from a cliff edge the hero sees and targets an enemy below");
			aim_check(xy{2036, 2464}, xy{2036, 2190}, 12, "from below the hero sees and targets an enemy at the cliff edge above");
		}
		// Death: with no one to defend, a large wave must kill the player and alive() must report it.
		Game g; g.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true); Fps fps(g); fps.start();
		unit_t* p = fps.player(); for (int i = 0; i < 40; ++i) g.spawn(UnitTypes::Zerg_Zergling, xy{p->sprite->position.x + 20 + (i % 8) * 6, p->sprite->position.y + 20 + (i / 8) * 6}, g.enemy());
		for (int i = 0; i < 600 && fps.alive(); ++i) g.step();
		check(!fps.alive(), "player death is detected (40 Zerglings on top of the player)");
		// Drawing between simulation frames (Renderer::update_units): a walking unit is drawn on its way from the previous
		// frame's position to the current one; a new unit, and one put somewhere else, are drawn where they are.
		{ Game g3; g3.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true); Fps f(g3, 7, 200, 200, 90, true); f.start(); unit_t* h = f.player();
		  Assets assets(argv[1]); Renderer r(g3.st(), g3.fn(), assets, 64, 40);
		  auto drawn = [&](const unit_t* u) { for (auto& v : r.units()) if (v.ptr == u) return std::make_pair(v.x, v.y); return std::make_pair(-1.0, -1.0); };
		  const double PI = 3.14159265358979323846; xy s = h->sprite->position; int best = 0; double bx = 1, by = 0; // the longest clear walk from the start
		  auto clear = [&](double x, double y) { return x >= 16 && y >= 16 && x < (int)g3.st().game->map_width - 16 && y < (int)g3.st().game->map_height - 16 && g3.fn().is_walkable(xy{(int)x, (int)y}); };
		  for (int d = 0; d < 8; ++d) { double ix = std::cos(d * PI / 4), iy = std::sin(d * PI / 4); int n = 0;
			while (n < 400 && clear(s.x + ix * n, s.y + iy * n)) n += 4;
			if (n > best) { best = n; bx = ix; by = iy; } }
		  for (int i = 0; i < 30; ++i) { f.drive(bx, by); g3.step(); f.tick(); r.update_units(); }
		  double x0 = h->sprite->position.x, y0 = h->sprite->position.y; g3.step(); f.tick(); double x1 = h->sprite->position.x, y1 = h->sprite->position.y;
		  r.update_units(0); auto a0 = drawn(h); r.update_units(0.5); auto a5 = drawn(h); r.update_units(1); auto a1 = drawn(h);
		  check((x1 != x0 || y1 != y0) && a0.first == x0 && a0.second == y0 && a5.first == (x0 + x1) / 2 && a5.second == (y0 + y1) / 2 && a1.first == x1 && a1.second == y1,
		        bwgame::format("drawn between frames: a walking hero at 0, 1/2 and 1 of the way from (%.0f,%.0f) to (%.0f,%.0f) is drawn at (%.1f,%.1f), (%.1f,%.1f), (%.1f,%.1f)", x0, y0, x1, y1, a0.first, a0.second, a5.first, a5.second, a1.first, a1.second).c_str());
		  unit_t* z = g3.spawn(UnitTypes::Zerg_Zergling, xy{(int)(x1 - by * 60), (int)(y1 + bx * 60)}, g3.enemy());
		  if (!z) z = g3.spawn(UnitTypes::Zerg_Zergling, xy{(int)(x1 + by * 60), (int)(y1 - bx * 60)}, g3.enemy());
		  f.drive(0, 0); f.place(s.x, s.y); g3.step(); f.tick(); r.update_units(0); auto zn = drawn(z), hp = drawn(h);
		  check(z && zn.first == z->sprite->position.x && zn.second == z->sprite->position.y, "drawn between frames: a new unit is drawn where it is, not on its way from somewhere");
		  check(std::hypot(x1 - s.x, y1 - s.y) > 64 && h->sprite->position == s && hp.first == s.x && hp.second == s.y, bwgame::format("drawn between frames: a hero put back %.0f px is drawn there at once, not sliding (drawn at %.0f,%.0f)", std::hypot(x1 - s.x, y1 - s.y), hp.first, hp.second).c_str());
		  // Direction 255 (just west of north) is a direction like any other: the walking input once sent it as "no input".
		  xy at{-1, -1}; for (int dy = -320; dy <= 320 && at.x < 0; dy += 16) for (int dx = -320; dx <= 320 && at.x < 0; dx += 16) {
			bool ok = true; for (int k = 0; k <= 120 && ok; k += 4) for (int sx : {-10, 10}) ok = ok && clear(s.x + dx + sx - k / 40.0, s.y + dy - k);
			if (ok) at = xy{s.x + dx, s.y + dy}; }
		  if (at.x >= 0) { f.drive(0, 0); f.place(at.x, at.y); for (int i = 0; i < 2; ++i) { g3.step(); f.tick(); }
			f.drive(-std::sin(PI / 128), -std::cos(PI / 128)); for (int i = 0; i < 8; ++i) { g3.step(); f.tick(); }
			check(at.y - h->sprite->position.y > 30 && f.speed() > 150, bwgame::format("direct mode: walking in direction 255 (just west of north) moves the hero (%d px north, %.0f px/s)", at.y - h->sprite->position.y, f.speed()).c_str()); }
		  else check(false, "direct mode: found clear ground north of a spot near the start for the direction-255 walk"); }
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
	std::cout << (failures ? "FPSTEST FAILED" : "FPSTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
