// First-person shooter layer over the live game. Two modes:
//  - waves (single player): a hero for the human player, waves of enemies that hunt it. OpenBW has no AI, so the enemy
//    "brain" here is deliberately simple: spawn near, attack the player.
//  - versus (two-player game, fp_net.h): a hero for each player next to their base; a dead hero comes back at its start
//    location after kRespawnFrames. No waves.
// Combat itself (range, cooldown, damage, projectiles) is OpenBW's own. Everything here that changes the simulation runs
// in start() and tick() (both games, at the same point) or goes out as a command (Game::command/custom), so a two-player
// game stays in sync. Free movement (direct mode) does not yet: single player only.
#pragma once
#include "fp_game.h"
#include <array>
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
	// Trigger modes. up: the hero holds fire; down: it fires; free: it fights like any StarCraft unit (versus: while its
	// player is away commanding the base).
	enum : int { kTriggerUp = 0, kTriggerDown = 1, kTriggerFree = 2 };
	static constexpr int kRespawnFrames = 24 * 10;
	// Local player's numbers. waves: kills = enemies killed; versus: kills = enemy hero deaths, deaths = own hero deaths.
	// shots/misses: attacks the local hero has started.
	int kills = 0, deaths = 0, wave = 0, shots = 0, misses = 0;
	// Enemies in wave w: melee from the start, ranged attackers join from wave 2. Capped and growing slowly so a lone hero stays winnable.
	static int wave_melee(int w) { return std::min(6, 1 + (w + 1) / 2); }
	static int wave_ranged(int w) { return w < 2 ? 0 : std::min(3, (w - 1) / 2); }
	// fire_rate_percent / range_percent / speed_percent: hero weapon and movement tuning relative to the StarCraft data
	// (100 = unchanged). Weapon entries and the hero's flingy are unique to the hero (or unused by anyone else on an FPS
	// map), so mutating the loaded data only affects the hero.
	// direct: the hero never takes movement/attack orders; WASD drives the position with our own velocity physics and
	// fire() applies damage through OpenBW's hit resolution. This allows moving and shooting at the same time.
	// versus: one hero per player, no waves (two-player games; see the header comment).
	explicit Fps(Game& g, uint32_t seed = 7, int fire_rate_percent = 200, int range_percent = 200, int speed_percent = 90, bool direct = false, bool versus = false)
		: g_(g), rng_(seed), fire_rate_(std::max(10, fire_rate_percent)), range_pct_(std::max(10, range_percent)),
		  speed_pct_(std::max(10, speed_percent)), direct_(direct), versus_(versus) {
		if (direct_ && g_.net()) error("free movement (--fps-move-shoot) is single player only");
		g_.set_fps_handler([this](int slot, data_loading::data_reader_le& r) { on_message(slot, r); });
	}
	~Fps() { g_.set_fps_handler(nullptr); }
	Fps(const Fps&) = delete; Fps& operator=(const Fps&) = delete;
	bool direct() const { return direct_; }
	bool versus() const { return versus_; }
	// Everything here that both players must agree on (Game::open_net checks it).
	static uint32_t settings_hash(int fire_rate_percent, int range_percent, int speed_percent) { return (uint32_t)fire_rate_percent * 1000003u ^ (uint32_t)range_percent * 7919u ^ (uint32_t)speed_percent; }

	// Spawn the heroes on their start locations. Returns false if the map gave no usable start.
	bool start() {
		if (versus_) { // both games create the heroes in the same order (by slot), so they get the same unit ids
			for (int s : {std::min(g_.owner(), g_.enemy()), std::max(g_.owner(), g_.enemy())}) {
				unit_t* u = spawn_hero(s); if (!u) return false;
				heroes_[s].id = g_.fn().get_unit_id(u); slots_.push_back(s);
			}
			g_.select(player());
			return true;
		}
		race_t er = g_.st().players[g_.enemy()].race;
		xy at = g_.start_location(g_.owner());
		unit_t* u = spawn_hero(g_.owner());
		if (!u) return false;
		heroes_[g_.owner()].id = g_.fn().get_unit_id(u); slots_.push_back(g_.owner()); g_.select(u); roster_ = fps_enemy_roster(er);
		hx_ = at.x; hy_ = at.y;
		next_wave_frame_ = g_.frame() + 48; // short grace period
		return true;
	}
	unit_t* player() { return g_.fn().get_unit(heroes_[g_.owner()].id); }
	unit_t* opponent() { return versus_ ? g_.fn().get_unit(heroes_[g_.enemy()].id) : nullptr; }
	bool alive() { return player() != nullptr; }
	// Versus: frames until the local hero comes back (0 while it lives).
	int respawn_in() { const Hero& h = heroes_[g_.owner()]; return alive() || h.respawn_at < 0 ? 0 : std::max(0, h.respawn_at - g_.frame()); }
	int enemies_alive() { return (int)enemy_ids_.size(); }
	int player_hp() { unit_t* p = player(); return p ? (int)p->hp.integer_part() : 0; }
	int player_max_hp() { unit_t* p = player(); return p ? (int)p->unit_type->hitpoints.integer_part() : 0; }

	// Weapon range in pixels against ground targets (OpenBW's own value, including upgrades).
	double range(const unit_t* u) {
		const weapon_type_t* w = u->unit_type->ground_weapon.operator->(); return w ? g_.fn().weapon_max_range(u, w) : 0;
	}
	// Units we may still target. Callers can hold a pointer from an earlier frame (the crosshair pick is made before the
	// next simulation step); a unit killed since keeps its slot but loses its sprite, so check at the moment of use.
	bool targetable(const unit_t* u) const { return u && u->sprite && !g_.fn().unit_dying(u); }
	// Enemy unit the crosshair is on, if it is in weapon range.
	bool in_range(const unit_t* target) { unit_t* p = player(); if (!p || !targetable(target)) return false;
		double d = std::hypot(p->sprite->position.x - target->sprite->position.x, p->sprite->position.y - target->sprite->position.y);
		return d <= range(p) + target->unit_type->dimensions.to.x; }
	bool is_enemy(const unit_t* u) const { return targetable(u) && u->owner == g_.enemy(); }

	enum class ShotResult { none, hit, miss };

	// Fire at `target`. In range: direct mode applies the damage straight through OpenBW's weapon-hit resolution (no orders
	// ever); order mode issues an attack order (StarCraft rules, the hero stands still). Out of range: the shot is always a
	// miss either way. The hero's own cooldown gates the rate of fire. In a two-player game the attack or miss takes
	// effect `latency` frames later; until then the same shot is not sent again.
	ShotResult fire(unit_t* target) {
		// The target may come from the previous rendered frame; the simulation can have killed it since, and OpenBW rejects orders
		// on dead targets (it throws), so check at the moment of use.
		if (!target || g_.fn().unit_dead(target) || g_.fn().unit_dying(target)) return ShotResult::none;
		unit_t* p = player(); if (!p || !is_enemy(target)) return ShotResult::none;
		if (p->ground_weapon_cooldown > kHold) return ShotResult::none; // kHold is our artificial no-auto-fire floor, not a real cooldown
		if (g_.frame() < busy_until_) return target == pending_target_ ? ShotResult::hit : ShotResult::none; // a command is on its way
		const weapon_type_t* w = p->unit_type->ground_weapon.operator->();
		if (!w) return ShotResult::none;
		double ang = std::atan2(target->sprite->position.y - p->sprite->position.y, target->sprite->position.x - p->sprite->position.x);
		int units = (int)std::lround(ang / 6.283185307179586 * 256) & 255;
		if (!in_range(target)) { // out of weapon range: the bolt cannot reach; take the shot and miss
			ActionWriter m; m.u8(Game::kChanFps).u8(kMsgMiss); g_.custom(m);
			busy(nullptr);
			return ShotResult::miss;
		}
		if (direct_) {
			// In direct mode we own the unit completely: face the target ourselves (order mode lets OpenBW turn the unit;
			// overwriting its heading there breaks the attack sequence).
			p->heading = direction_t::from_raw(units);
			g_.fn().weapon_deal_damage(w, fp8::integer(w->damage_amount), 1, target, p->heading, p, g_.owner());
			p->ground_weapon_cooldown = w->cooldown;
			++shots;
			return ShotResult::hit;
		}
		if (p->order_target.unit == target && p->order_type->id == Orders::AttackUnit) return ShotResult::hit;
		g_.select(p); g_.smart_order(target->sprite->position, target);
		busy(target);
		++shots;
		return ShotResult::hit;
	}

	// ---- direct movement (only used when direct() is true): velocity physics, any direction, no orders ----
	void go_to(double x, double y) { gx_ = x; gy_ = y; goal_ = true; }
	void drive(double ix, double iy, double dt) {
		unit_t* u = player(); if (!u) return;
		if (ix != 0 || iy != 0) goal_ = false;
		if (goal_ && ix == 0 && iy == 0) { // auto-walk toward the RMB point
			double dx = gx_ - hx_, dy = gy_ - hy_;
			double d = std::hypot(dx, dy);
			if (d < 12) goal_ = false;
			else { ix = dx / d; iy = dy / d; }
		}
		const double accel = 2000.0, max_speed = 192.0, friction = 8.0; // 40% slower than the original 320 px/s cap
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
		double nx = hx_ + vx_ * dt, ny = hy_ + vy_ * dt;
		if (can(nx, hy_)) hx_ = nx; else vx_ = 0;
		if (can(hx_, ny)) hy_ = ny; else vy_ = 0;
	}
	void halt() { vx_ = vy_ = 0; goal_ = false; }
	// Direct mode: put the hero at (x,y) (tests, spawning); takes effect at the next tick().
	void place(double x, double y) { hx_ = x; hy_ = y; halt(); }
	double speed() { return std::hypot(vx_, vy_); }

	// The weapon only fires while the trigger is held. StarCraft units attack anything in range on their own, which is wrong for a
	// shooter, so while the trigger is up the weapon cooldown is kept above zero (the attack logic needs it to be 0).
	// The trigger is part of the simulation: a change goes out as a command (two-player game: applied `latency` frames later).
	void set_trigger(bool held) { set_trigger_mode(held ? kTriggerDown : kTriggerUp); }
	void set_trigger_mode(int mode) {
		if (mode == sent_trigger_) return;
		sent_trigger_ = mode;
		ActionWriter m; m.u8(Game::kChanFps).u8(kMsgTrigger).u8(mode); g_.custom(m);
	}

	// Call once per simulation frame after Game::step().
	void tick() {
		for (int s : slots_) tick_hero(s);
		if (versus_) return;
		unit_t* p = player(); if (!p) return;
		// kills: enemies we were tracking that are gone
		for (size_t i = 0; i < enemy_ids_.size();) { if (!g_.fn().get_unit(enemy_ids_[i])) { ++kills; enemy_ids_.erase(enemy_ids_.begin() + i); } else ++i; }
		if (g_.frame() >= next_wave_frame_ && enemy_ids_.empty()) spawn_wave(p);
		if (g_.frame() % 6 != 0) return; // enemy thinking is cheap but needn't run every frame
		for (auto id : enemy_ids_) { unit_t* e = g_.fn().get_unit(id); if (!e) continue;
			if (e->order_target.unit != p || (e->order_type->id != Orders::AttackUnit && e->order_type->id != Orders::AttackMove)) g_.attack_as(g_.enemy(), e, p); }
	}

private:
	enum : uint8_t { kMsgTrigger = 1, kMsgMiss = 2 };
	struct Hero { unit_id id{}; int trigger = kTriggerUp; int respawn_at = -1; };

	void tick_hero(int s) {
		Hero& h = heroes_[s]; unit_t* p = g_.fn().get_unit(h.id);
		if (!p) {
			if (!versus_ || !g_.fn().player_slot_active(s)) return; // waves: death ends the run; a player who left keeps no hero
			if (h.respawn_at < 0) { h.respawn_at = g_.frame() + kRespawnFrames; if (s == g_.owner()) ++deaths; else ++kills; }
			else if (g_.frame() >= h.respawn_at) {
				if (unit_t* u = spawn_hero(s)) { h.id = g_.fn().get_unit_id(u); h.respawn_at = -1; }
				else h.respawn_at = g_.frame() + 24; // no room at the start location: try again in a second
			}
			return;
		}
		if (direct_ && s == g_.owner()) { // the hero's position is ours: re-apply it in case anything in the step moved it (e.g. unit collision)
			p->sprite->position.x = (int)std::lround(hx_);
			p->sprite->position.y = (int)std::lround(hy_);
		}
		if (s == g_.owner() && (int)p->ground_weapon_cooldown > last_cooldown_ && (int)p->ground_weapon_cooldown > kHold) ++shots; // a real attack, not our hold
		if (h.trigger == kTriggerUp) { if (p->ground_weapon_cooldown < kHold) p->ground_weapon_cooldown = kHold; if (p->air_weapon_cooldown < kHold) p->air_weapon_cooldown = kHold; }
		if (s == g_.owner()) last_cooldown_ = (int)p->ground_weapon_cooldown;
	}
	// A command from player `slot` (single player: at once; two-player game: inside the frame, in both games).
	void on_message(int slot, data_loading::data_reader_le& r) {
		int kind = r.get<uint8_t>();
		if (slot < 0 || slot >= 8) return;
		Hero& h = heroes_[slot];
		if (kind == kMsgTrigger) { int m = r.get<uint8_t>(); h.trigger = m <= kTriggerFree ? m : kTriggerUp; }
		else if (kind == kMsgMiss) { // the out-of-range shot spends the weapon's cooldown like a real one
			unit_t* p = g_.fn().get_unit(h.id); const weapon_type_t* w = p ? p->unit_type->ground_weapon.operator->() : nullptr;
			if (p && w && p->ground_weapon_cooldown <= kHold) { p->ground_weapon_cooldown = w->cooldown; if (slot == g_.owner()) ++misses; }
		}
	}
	// Two-player game: hold this shot until its command has been applied (single player: applied already).
	void busy(const unit_t* target) { pending_target_ = target; busy_until_ = g_.net() ? g_.frame() + g_.latency() + 1 : 0; }
	unit_t* spawn_hero(int s) {
		unit_t* u = g_.spawn(fps_player_unit(g_.st().players[s].race), g_.start_location(s), s);
		if (!u) return nullptr;
		// Hero weapon tuning: fire rate and range are scaled on the hero's own weapon entry (used by no other unit), once per
		// weapon. OpenBW's attack logic and weapon_max_range() read these values, so everything downstream follows.
		if (weapon_type_t* w = const_cast<weapon_type_t*>(u->unit_type->ground_weapon.operator->())) {
			if (!tuned_[(size_t)w->id]) {
				tuned_[(size_t)w->id] = true;
				w->cooldown = std::max(1, w->cooldown * 100 / fire_rate_);
				w->max_range = std::max(32u, (unsigned)((uint64_t)w->max_range * range_pct_ / 100));
			}
		}
		// Movement speed: scale the hero's runtime flingy_top_speed (what movement actually reads). Hero flingies have a
		// ~0 top_speed in the data; their real speed is set by their iscript at spawn, so the runtime value is authoritative.
		{ int base = (int)u->flingy_top_speed.integer_part();
		  if (base > 0) u->flingy_top_speed = fp8::integer(std::max(1, base * speed_pct_ / 100)); }
		return u;
	}
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
	Game& g_; std::mt19937 rng_; int fire_rate_, range_pct_, speed_pct_; bool direct_, versus_;
	std::array<Hero, 8> heroes_{}; std::vector<int> slots_; std::array<bool, 130> tuned_{};
	int sent_trigger_ = kTriggerUp; const unit_t* pending_target_ = nullptr; int busy_until_ = 0;
	double vx_ = 0, vy_ = 0, hx_ = 0, hy_ = 0; bool goal_ = false; double gx_ = 0, gy_ = 0; FpsKinds roster_{};
	std::vector<unit_id> enemy_ids_; int next_wave_frame_ = 0, last_cooldown_ = 0;
};

} // namespace fp
