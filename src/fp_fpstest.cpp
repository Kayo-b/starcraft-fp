// Headless behaviour test for the FPS layer (fp_fps.h). Exit code 0 only if every check passes.
#include "fp_fps.h"
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
		check(fps.range(p) >= 128 && fps.range(p) <= 256, "hero weapon range from OpenBW: " + std::to_string((int)fps.range(p)) + " px");
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
			check(n == Fps::wave_melee(1) + Fps::wave_ranged(1) && zl == n && reach == n, "wave 1 at frame " + std::to_string(g.frame()) + ": " + std::to_string(n) + " enemies, " + std::to_string(zl) + " Zerglings, " + std::to_string(reach) + " reachable from the player"); }
		if (verbose && wave_checked && !ai_checked && g.frame() % 6 == 0) { ai_checked = true; // one AI pass later
			int hunting = g.count([&](unit_t* u) { return u->owner == g.enemy() && u->order_target.unit == p && (u->order_type->id == Orders::AttackUnit || u->order_type->id == Orders::AttackMove); });
			int total = g.count([&](unit_t* u) { return u->owner == g.enemy(); });
			check(hunting == total && total > 0, "enemy AI: all " + std::to_string(total) + " enemies are ordered to attack the player (" + std::to_string(hunting) + " are)"); }
		if (verbose && !range_checked && wave_checked && ai_checked) { range_checked = true;
			unit_t* far = g.nearest(p->sprite->position.x, p->sprite->position.y, [&](unit_t* u) { return u->owner == g.enemy(); });
			if (!far) { check(false, "an enemy exists to test fire() range gating on (none found at frame " + std::to_string(g.frame()) + ")"); continue; }
			double d = std::hypot(far->sprite->position.x - p->sprite->position.x, far->sprite->position.y - p->sprite->position.y);
			check(d > fps.range(p) + 40 ? !fps.fire(far) : true, "fire() refuses a target outside weapon range (distance " + std::to_string((int)d) + " px)"); }
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
		// Duel: one Zergling inside weapon range; fire() must damage it and eventually kill it.
		{ Game d; d.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true); Fps f2(d); f2.start(); unit_t* p = f2.player();
		  xy at{p->sprite->position.x + 100, p->sprite->position.y}; unit_t* z = d.spawn(UnitTypes::Zerg_Zergling, at, d.enemy());
		  if (!z) z = d.spawn(UnitTypes::Zerg_Zergling, xy{p->sprite->position.x - 100, p->sprite->position.y}, d.enemy());
		  check(z != nullptr && f2.in_range(z), "duel setup: a Zergling is inside weapon range");
		  // Trigger up: an idle hero must not shoot an enemy that is right in front of it (StarCraft units normally would).
		  { int hpz = z ? (int)z->hp.integer_part() : 0; f2.set_trigger(false); for (int i = 0; i < 90; ++i) { d.step(); f2.tick(); }
		    check(z && (int)z->hp.integer_part() == hpz && f2.shots == 0, "trigger up: the hero does not auto-fire at an enemy in range (target HP " + std::to_string(hpz) + " -> " + std::to_string(z ? (int)z->hp.integer_part() : 0) + ", " + std::to_string(f2.shots) + " shots)"); }
		  f2.set_trigger(true);
		  int hp0 = z ? z->hp.integer_part() : 0; bool fired = z && f2.fire(z); int hp_after = hp0, killed_at = -1; unit_id zid = d.fn().get_unit_id(z);
		  for (int i = 0; i < 400 && fired; ++i) { d.step(); f2.tick(); unit_t* zz = d.fn().get_unit(zid); if (!zz) { killed_at = i; break; } hp_after = std::min(hp_after, (int)zz->hp.integer_part()); if (i % 10 == 0) f2.fire(zz); }
		  check(fired, "fire() accepted an in-range enemy");
		  check(killed_at >= 0 || hp_after < hp0, "shooting damages the target (HP " + std::to_string(hp0) + " -> " + (killed_at >= 0 ? std::string("dead after ") + std::to_string(killed_at) + " frames" : std::to_string(hp_after)) + ")");
		  check(f2.shots >= 1, "shots are counted from the weapon cooldown (" + std::to_string(f2.shots) + " shots fired in the duel)");
		  check(killed_at >= 0 && f2.alive(), "the hero wins the duel and survives (HP " + std::to_string(f2.alive() ? (int)f2.player()->hp.integer_part() : 0) + ")"); }
		// Death: with no one to defend, a large wave must kill the player and alive() must report it.
		Game g; g.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true); Fps fps(g); fps.start();
		unit_t* p = fps.player(); for (int i = 0; i < 40; ++i) g.spawn(UnitTypes::Zerg_Zergling, xy{p->sprite->position.x + 20 + (i % 8) * 6, p->sprite->position.y + 20 + (i / 8) * 6}, g.enemy());
		for (int i = 0; i < 600 && fps.alive(); ++i) g.step();
		check(!fps.alive(), "player death is detected (40 Zerglings on top of the player)");
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
	std::cout << (failures ? "FPSTEST FAILED" : "FPSTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
