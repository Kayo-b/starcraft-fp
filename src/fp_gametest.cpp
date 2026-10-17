// Headless behaviour test for the live game layer (fp_game.h). Exit code 0 only if every check passes.
#include "fp_game.h"
#include <cstdio>
#include <iostream>

using namespace fp;

static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }

static uint64_t checksum(Game& g) {
	uint64_t h = 1469598103934665603ull; auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
	mix(g.frame()); mix(g.minerals()); mix(g.gas()); mix(g.supply_used());
	for (auto u : ptr(g.st().visible_units)) { mix((uint64_t)u->unit_type->id); mix(u->owner); mix((uint64_t)u->sprite->position.x); mix((uint64_t)u->sprite->position.y); mix((uint64_t)u->hp.integer_part()); }
	return h;
}

// Scripted session used for both the behaviour checks and the determinism comparison.
static uint64_t session(const std::string& data, const std::string& map, bool verbose) {
	Game g; g.open(data, map, race_t::terran, race_t::protoss);
	auto mine = [&](UnitTypes t) { return g.count([&](unit_t* u) { return u->owner == g.owner() && u->unit_type->id == t; }); };
	if (verbose) std::cout << "setup (human slot " << g.owner() << ", enemy slot " << g.enemy() << ")\n";
	int cc = mine(UnitTypes::Terran_Command_Center), scv = mine(UnitTypes::Terran_SCV);
	if (verbose) {
		check(cc == 1 && scv == 4, "human starts with 1 Command Center and 4 SCVs (got " + std::to_string(cc) + " / " + std::to_string(scv) + ")");
		check(g.minerals() == 50, "starts with 50 minerals (got " + std::to_string(g.minerals()) + ")");
		check(g.supply_used() == 4 && g.supply_available() == 10, "supply 4/10 (got " + std::to_string(g.supply_used()) + "/" + std::to_string(g.supply_available()) + ")");
		int enemy_units = g.count([&](unit_t* u) { return u->owner == g.enemy(); });
		check(enemy_units >= 5, "enemy has a base and workers (" + std::to_string(enemy_units) + " units)");
	}
	// Train an SCV with the starting 50 minerals.
	unit_t* base = g.find([&](unit_t* u) { return u->owner == g.owner() && u->unit_type->id == UnitTypes::Terran_Command_Center; });
	bool sel = g.select(base), tr = g.train(UnitTypes::Terran_SCV);
	if (verbose) check(sel && tr && g.minerals() == 0, "train SCV accepted and spends 50 minerals (minerals now " + std::to_string(g.minerals()) + ")");
	// Move one SCV to a nearby walkable point.
	unit_t* w = g.find([&](unit_t* u) { return u->owner == g.owner() && u->unit_type->id == UnitTypes::Terran_SCV; });
	double mx = g.st().game->map_width / 2.0, my = g.st().game->map_height / 2.0;
	double ux = w->sprite->position.x, uy = w->sprite->position.y, d = std::hypot(mx - ux, my - uy);
	xy dest{(int)(ux + (mx - ux) / d * 160), (int)(uy + (my - uy) / d * 160)};
	bool walk = g.fn().is_walkable(dest);
	g.select(w); bool mv = g.smart_order(dest, nullptr);
	for (int i = 0; i < 300; ++i) g.step();
	double dd = std::hypot(w->sprite->position.x - dest.x, w->sprite->position.y - dest.y);
	if (verbose) check(walk && mv && dd < 24, "move order: SCV reaches a point 160 px away (walkable " + std::to_string(walk) + ", ended " + std::to_string((int)dd) + " px from it)");
	int trained_at = -1; for (int i = 0; i < 60 && trained_at < 0; ++i) { g.step(); if (mine(UnitTypes::Terran_SCV) == 5) trained_at = g.frame(); }
	if (verbose) check(trained_at > 0 && g.supply_used() == 5, "SCV finished training at frame " + std::to_string(trained_at) + ": 5 SCVs, supply " + std::to_string(g.supply_used()));
	// Gather: smart-order the same SCV on the nearest mineral field.
	unit_t* field = g.nearest(ux, uy, [&](unit_t* u) { int t = (int)u->unit_type->id; return t >= (int)UnitTypes::Resource_Mineral_Field && t <= (int)UnitTypes::Resource_Mineral_Field_Type_3; });
	int before = g.minerals();
	g.select(w); bool gt = g.smart_order(field->sprite->position, field);
	for (int i = 0; i < 2500; ++i) g.step();
	if (verbose) check(gt && g.minerals() > before, "gather: right-click on a mineral field raises minerals " + std::to_string(before) + " -> " + std::to_string(g.minerals()));
	// Order on an enemy unit becomes an attack order.
	unit_t* foe = g.find([&](unit_t* u) { return u->owner == g.enemy(); });
	auto dist = [&]() { return std::hypot(w->sprite->position.x - foe->sprite->position.x, w->sprite->position.y - foe->sprite->position.y); };
	double d0 = dist(); g.select(w); bool at = g.smart_order(foe->sprite->position, foe);
	for (int i = 0; i < 12; ++i) g.step(); // a worker mid-harvest passes through internal orders before the new one applies
	int oid = (int)w->order_type->id; double d1 = 0;
	for (int i = 0; i < 120; ++i) g.step();
	d1 = dist();
	if (verbose) check(at && (oid == (int)Orders::AttackUnit || oid == (int)Orders::AttackDefault || oid == (int)Orders::AttackMove) && d1 < d0 - 100,
		"order on an enemy unit becomes an attack and the SCV closes in (order id " + std::to_string(oid) + ", distance " + std::to_string((int)d0) + " -> " + std::to_string((int)d1) + ")");
	return checksum(g);
}

int main(int argc, char** argv) {
	if (argc < 3) { std::cerr << "usage: fp_gametest DATA_DIR MAP.scm\n"; return 2; }
	try {
		uint64_t a = session(argv[1], argv[2], true), b = session(argv[1], argv[2], false);
		check(a == b, "determinism: two identical sessions end in the same state (" + std::to_string(a) + " vs " + std::to_string(b) + ")");
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
	std::cout << (failures ? "GAMETEST FAILED" : "GAMETEST PASSED") << "\n";
	return failures ? 1 : 0;
}
