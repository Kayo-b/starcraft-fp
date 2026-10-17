// First-person shooter layer over the live game: one player unit, waves of enemies that hunt it.
// OpenBW has no AI, so the enemy "brain" here is deliberately simple: spawn near, attack the player.
// Combat itself (range, cooldown, damage, projectiles) is OpenBW's own.
#pragma once
#include "fp_game.h"
#include <random>
#include <vector>

namespace fp {
using namespace bwgame;

struct FpsKinds { UnitTypes player; UnitTypes melee, ranged; };
// Player unit and enemy roster per race: a ranged player, enemies of the opposing race.
// The player is a hero unit (more HP and damage than the rank and file): a lone StarCraft unit cannot kite, so a plain Marine loses a
// one-on-one against a Zergling.
static inline UnitTypes fps_player_unit(race_t r) { return r == race_t::terran ? UnitTypes::Hero_Jim_Raynor_Marine : r == race_t::protoss ? UnitTypes::Hero_Fenix_Dragoon : UnitTypes::Hero_Hunter_Killer; }
static inline FpsKinds fps_enemy_roster(race_t r) {
	if (r == race_t::zerg) return {UnitTypes::Zerg_Hydralisk, UnitTypes::Zerg_Zergling, UnitTypes::Zerg_Hydralisk};
	if (r == race_t::terran) return {UnitTypes::Terran_Marine, UnitTypes::Terran_Firebat, UnitTypes::Terran_Marine};
	return {UnitTypes::Protoss_Dragoon, UnitTypes::Protoss_Zealot, UnitTypes::Protoss_Dragoon};
}

class Fps {
public:
	int kills = 0, wave = 0, shots = 0; // shots: attacks the player has started (the weapon cooldown jumping up)
	// Enemies in wave w: melee from the start, ranged attackers join from wave 2. Capped and growing slowly so a lone hero stays winnable.
	static int wave_melee(int w) { return std::min(6, 1 + (w + 1) / 2); }
	static int wave_ranged(int w) { return w < 2 ? 0 : std::min(3, (w - 1) / 2); }
	// fire_rate_percent: how fast the hero's weapon fires relative to the StarCraft data (100 = unchanged, 200 = twice as fast).
	// Applied to the hero's own weapon entry only (hero weapons are used by no other unit).
	explicit Fps(Game& g, uint32_t seed = 7, int fire_rate_percent = 200) : g_(g), rng_(seed), fire_rate_(std::max(10, fire_rate_percent)) {}

	// Spawn the player's unit on their start location. Returns false if the map gave no usable start.
	bool start() {
		race_t pr = g_.st().players[g_.owner()].race, er = g_.st().players[g_.enemy()].race; (void)pr;
		xy at = g_.start_location(g_.owner());
		unit_t* u = g_.spawn(fps_player_unit(g_.st().players[g_.owner()].race), at, g_.owner());
		if (!u) return false;
		player_id_ = g_.fn().get_unit_id(u); g_.select(u); roster_ = fps_enemy_roster(er);
		// Faster hero fire rate (gameplay tuning): scale the hero's weapon cooldown down. Hero weapon types are used by
		// no other unit, so mutating the loaded data only affects the hero. OpenBW's attack logic reads it every shot.
		if (weapon_type_t* w = const_cast<weapon_type_t*>(u->unit_type->ground_weapon.operator->())) {
			w->cooldown = std::max(1, w->cooldown * 100 / fire_rate_);
		}
		next_wave_frame_ = g_.frame() + 48; // short grace period
		return true;
	}
	unit_t* player() { return g_.fn().get_unit(player_id_); }
	bool alive() { return player() != nullptr; }
	int enemies_alive() { return (int)enemy_ids_.size(); }
	int player_hp() { unit_t* p = player(); return p ? (int)p->hp.integer_part() : 0; }
	int player_max_hp() { unit_t* p = player(); return p ? (int)p->unit_type->hitpoints.integer_part() : 0; }

	// Weapon range in pixels against ground targets (OpenBW's own value, including upgrades).
	double range(const unit_t* u) {
		const weapon_type_t* w = u->unit_type->ground_weapon.operator->(); return w ? g_.fn().weapon_max_range(u, w) : 0;
	}
	// Enemy unit the crosshair is on, if it is in weapon range.
	bool in_range(const unit_t* target) { unit_t* p = player(); if (!p || !target) return false;
		double d = std::hypot(p->sprite->position.x - target->sprite->position.x, p->sprite->position.y - target->sprite->position.y);
		return d <= range(p) + target->unit_type->dimensions.to.x; }
	bool is_enemy(const unit_t* u) const { return u && u->owner == g_.enemy(); }

	// Fire at `target` (an enemy in range): an attack order, re-issued only when the target changes.
	bool fire(unit_t* target) {
		// The target may come from the previous rendered frame; the simulation can have killed it since, and OpenBW rejects orders
		// on dead targets (it throws), so check at the moment of use.
		if (!target || g_.fn().unit_dead(target) || g_.fn().unit_dying(target)) return false;
		unit_t* p = player(); if (!p || !is_enemy(target) || !in_range(target)) return false;
		if (p->order_target.unit == target && (p->order_type->id == Orders::AttackUnit)) return true;
		g_.select(p); return g_.smart_order(target->sprite->position, target);
	}

	// The weapon only fires while the trigger is held. StarCraft units attack anything in range on their own, which is wrong for a
	// shooter, so while the trigger is up the weapon cooldown is kept above zero (the attack logic needs it to be 0).
	void set_trigger(bool held) { trigger_ = held; }

	// Call once per simulation frame after Game::step().
	void tick() {
		unit_t* p = player(); if (!p) return;
		if ((int)p->ground_weapon_cooldown > last_cooldown_ && (int)p->ground_weapon_cooldown > kHold) ++shots; // a real attack, not our hold
		if (!trigger_) { if (p->ground_weapon_cooldown < kHold) p->ground_weapon_cooldown = kHold; if (p->air_weapon_cooldown < kHold) p->air_weapon_cooldown = kHold; }
		last_cooldown_ = (int)p->ground_weapon_cooldown;
		// kills: enemies we were tracking that are gone
		for (size_t i = 0; i < enemy_ids_.size();) { if (!g_.fn().get_unit(enemy_ids_[i])) { ++kills; enemy_ids_.erase(enemy_ids_.begin() + i); } else ++i; }
		if (g_.frame() >= next_wave_frame_ && enemy_ids_.empty()) spawn_wave(p);
		if (g_.frame() % 6 != 0) return; // enemy thinking is cheap but needn't run every frame
		for (auto id : enemy_ids_) { unit_t* e = g_.fn().get_unit(id); if (!e) continue;
			if (e->order_target.unit != p || (e->order_type->id != Orders::AttackUnit && e->order_type->id != Orders::AttackMove)) g_.attack_as(g_.enemy(), e, p); }
	}

private:
	void spawn_wave(unit_t* p) {
		++wave; int melee = wave_melee(wave), ranged = wave_ranged(wave); xy pp = p->sprite->position;
		std::uniform_real_distribution<double> ang(0, 6.283185307179586), dist(420, 760);
		std::vector<xy> placed; // keep spawns apart so enemies do not clump into one ball
		int made_melee = 0, made_ranged = 0;
		for (int tries = 0; (made_melee < melee || made_ranged < ranged) && tries < 600; ++tries) {
			double a = ang(rng_), d = dist(rng_); xy pos{(int)(pp.x + std::cos(a) * d), (int)(pp.y + std::sin(a) * d)};
			if (pos.x < 32 || pos.y < 32 || pos.x >= (int)g_.st().game->map_width - 32 || pos.y >= (int)g_.st().game->map_height - 32) continue;
			if (!g_.fn().is_walkable(pos) || !g_.fn().is_reachable(pp, pos)) continue;
			bool too_close = false; for (auto& q : placed) if (std::hypot(q.x - pos.x, q.y - pos.y) < 72) { too_close = true; break; }
			if (too_close) continue;
			bool want_ranged = made_melee >= melee || (made_ranged < ranged && made_melee * 3 > melee * 2); // sprinkle ranged through the wave
			UnitTypes t = want_ranged ? roster_.ranged : roster_.melee;
			unit_t* e = g_.spawn(t, pos, g_.enemy()); if (!e) continue;
			enemy_ids_.push_back(g_.fn().get_unit_id(e));
			if (want_ranged) ++made_ranged; else ++made_melee;
			placed.push_back(pos);
		}
		next_wave_frame_ = g_.frame() + 24 * 8; // next wave only after this one is cleared AND 8 s have passed
	}
	static constexpr int kHold = 2; // after the per-frame decrement it is still > 0
	bool trigger_ = false;
	Game& g_; std::mt19937 rng_; int fire_rate_; unit_id player_id_{}; FpsKinds roster_{};
	std::vector<unit_id> enemy_ids_; int next_wave_frame_ = 0, last_cooldown_ = 0;
};

} // namespace fp
