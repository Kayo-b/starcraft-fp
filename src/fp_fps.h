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
	int kills = 0, wave = 0, shots = 0; // shots: attacks the player has started (counted in fire())
	// Enemies in wave w: melee from the start, ranged attackers join from wave 2. Capped and growing slowly so a lone hero stays winnable.
	static int wave_melee(int w) { return std::min(6, 1 + (w + 1) / 2); }
	static int wave_ranged(int w) { return w < 2 ? 0 : std::min(3, (w - 1) / 2); }
	explicit Fps(Game& g, uint32_t seed = 7) : g_(g), rng_(seed) {}

	// Spawn the player's unit on their start location. Returns false if the map gave no usable start.
	bool start() {
		race_t pr = g_.st().players[g_.owner()].race, er = g_.st().players[g_.enemy()].race; (void)pr;
		xy at = g_.start_location(g_.owner());
		unit_t* u = g_.spawn(fps_player_unit(g_.st().players[g_.owner()].race), at, g_.owner());
		if (!u) return false;
		player_id_ = g_.fn().get_unit_id(u); g_.select(u); roster_ = fps_enemy_roster(er);
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

	// Fire at `target` (an enemy in weapon range). The hero NEVER takes orders in FPS mode: OpenBW attack orders include
	// movement ("close into range"), which fights the direct position writes and makes the hero lurch. Damage is applied
	// directly through OpenBW's weapon-hit resolution, and the hero's own cooldown gates the rate of fire.
	bool fire(unit_t* target) {
		// The target may come from the previous rendered frame; the simulation can have killed it since.
		if (!target || g_.fn().unit_dead(target) || g_.fn().unit_dying(target)) return false;
		unit_t* p = player(); if (!p || !is_enemy(target) || !in_range(target)) return false;
		if (p->ground_weapon_cooldown > kHold) return false; // kHold is our artificial no-auto-fire floor, not a real cooldown
		const weapon_type_t* w = p->unit_type->ground_weapon.operator->();
		if (!w) return false;
		// face the target (sprite frame selection uses the unit heading)
		double ang = std::atan2(target->sprite->position.y - p->sprite->position.y, target->sprite->position.x - p->sprite->position.x);
		int units = (int)std::lround(ang / 6.283185307179586 * 256) & 255;
		p->heading = direction_t::from_raw(units);
		g_.fn().weapon_deal_damage(w, fp8::integer(w->damage_amount), 1, target, p->heading, p, g_.owner());
		p->ground_weapon_cooldown = w->cooldown;
		++shots;
		return true;
	}

	// Doom-like direct movement: velocity with acceleration and friction, written straight onto the hero's position
	// (no move orders, so the hero can walk and shoot at the same time). Collision is checked against OpenBW's own
	// walkability per axis (slide); enemy body-blocking is not handled yet.
	void go_to(double x, double y) { gx_ = x; gy_ = y; goal_ = true; }
	void drive(double ix, double iy, double dt) {
		unit_t* u = player(); if (!u) return;
		if (ix != 0 || iy != 0) goal_ = false;
		if (goal_ && ix == 0 && iy == 0) { // auto-walk toward the RMB point
			double dx = gx_ - u->sprite->position.x, dy = gy_ - u->sprite->position.y;
			double d = std::hypot(dx, dy);
			if (d < 12) { goal_ = false; }
			else { ix = dx / d; iy = dy / d; }
		}
		const double accel = 2000.0, max_speed = 320.0, friction = 8.0;
		if (ix != 0 || iy != 0) {
			double len = std::hypot(ix, iy); ix /= len; iy /= len;
			vx_ += ix * accel * dt; vy_ += iy * accel * dt;
			double sp = std::hypot(vx_, vy_);
			if (sp > max_speed) { vx_ *= max_speed / sp; vy_ *= max_speed / sp; }
		} else {
			vx_ *= std::exp(-friction * dt); vy_ *= std::exp(-friction * dt);
			if (std::fabs(vx_) < 2) vx_ = 0; if (std::fabs(vy_) < 2) vy_ = 0;
		}
		double r = (u->unit_type->dimensions.from.x + u->unit_type->dimensions.to.x + 1) / 4.0;
		auto can = [&](double nx, double ny) {
			for (int dy = -1; dy <= 1; dy += 2) for (int dx = -1; dx <= 1; dx += 2) {
				int px = (int)std::floor(nx + dx * r), py = (int)std::floor(ny + dy * r);
				if (px < 0 || py < 0 || px >= (int)g_.st().game->map_width || py >= (int)g_.st().game->map_height) return false;
				if (!g_.fn().is_walkable(xy{px, py})) return false;
			}
			return true;
		};
		double nx = u->sprite->position.x + vx_ * dt, ny = u->sprite->position.y + vy_ * dt;
		if (can(nx, u->sprite->position.y)) u->sprite->position.x = (int)std::lround(nx); else vx_ = 0;
		if (can(u->sprite->position.x, ny)) u->sprite->position.y = (int)std::lround(ny); else vy_ = 0;
	}
	void halt() { vx_ = vy_ = 0; }
	double speed() { return std::hypot(vx_, vy_); }

	// The weapon only fires while the trigger is held. StarCraft units attack anything in range on their own, which is wrong for a
	// shooter, so while the trigger is up the weapon cooldown is kept above zero (the attack logic needs it to be 0).
	void set_trigger(bool held) { trigger_ = held; }

	// Call once per simulation frame after Game::step().
	void tick() {
		unit_t* p = player(); if (!p) return;
		if (!trigger_) { // trigger up: keep the cooldown off zero so the idle hero never auto-attacks
			if (p->ground_weapon_cooldown < kHold) p->ground_weapon_cooldown = kHold;
			if (p->air_weapon_cooldown < kHold) p->air_weapon_cooldown = kHold;
		}
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
	Game& g_; std::mt19937 rng_; unit_id player_id_{}; FpsKinds roster_{};
	std::vector<unit_id> enemy_ids_; int next_wave_frame_ = 0, last_cooldown_ = 0; double vx_ = 0, vy_ = 0; bool goal_ = false; double gx_ = 0, gy_ = 0;
};

} // namespace fp
