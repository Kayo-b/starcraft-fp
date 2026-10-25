// First-person shooter layer over the live game. Two modes:
//  - waves (single player): a hero for the human player, waves of enemies that hunt it. OpenBW has no AI, so the enemy
//    "brain" here is deliberately simple: spawn near, attack the player.
//  - versus (two-player game, fp_net.h): a hero for each player next to their base; a dead hero comes back at its start
//    location after kRespawnFrames. No waves.
// Combat itself (range, cooldown, damage, projectiles) is OpenBW's own. Everything here that changes the simulation runs
// in start() and tick() (both games, at the same point) or goes out as a command (Game::command/custom), so a two-player
// game stays in sync. That includes free movement (direct mode): integer physics per frame, the input as commands.
#pragma once
#include "fp_game.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <utility>
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
		g_.set_fps_handler([this](int slot, data_loading::data_reader_le& r) { on_message(slot, r); });
		g_.set_before_step([this] { flush_input(); });
		g_.set_hash_extra([this] { return hash(); });
	}
	~Fps() { g_.set_fps_handler(nullptr); g_.set_before_step(nullptr); g_.set_hash_extra(nullptr); }
	Fps(const Fps&) = delete; Fps& operator=(const Fps&) = delete;
	bool direct() const { return direct_; }
	bool versus() const { return versus_; }
	// Everything here that both players must agree on (Game::open_net checks it).
	static uint32_t settings_hash(int fire_rate_percent, int range_percent, int speed_percent, bool direct = false) { return ((uint32_t)fire_rate_percent * 1000003u ^ (uint32_t)range_percent * 7919u ^ (uint32_t)speed_percent) + (direct ? 0x9e3779b9u : 0); }

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
		unit_t* u = spawn_hero(g_.owner());
		if (!u) return false;
		heroes_[g_.owner()].id = g_.fn().get_unit_id(u); slots_.push_back(g_.owner()); g_.select(u); roster_ = fps_enemy_roster(er);
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
	bool in_range(const unit_t* target) { unit_t* p = player(); return p && targetable(target) && reaches(p, target); }
	bool is_enemy(const unit_t* u) const { return targetable(u) && u->owner == g_.enemy(); }

	enum class ShotResult { none, hit, miss };

	// Fire at `target`. In range: direct mode sends the shot as a command, which applies the damage straight through
	// OpenBW's weapon-hit resolution (no orders ever); order mode issues an attack order (StarCraft rules, the hero stands
	// still). Out of range: the shot is always a miss either way. The hero's own cooldown gates the rate of fire. In a
	// two-player game the shot, attack or miss takes effect `latency` frames later; until then it is not sent again.
	ShotResult fire(unit_t* target) {
		// The target may come from the previous rendered frame; the simulation can have killed it since, and OpenBW rejects orders
		// on dead targets (it throws), so check at the moment of use.
		if (!target || g_.fn().unit_dead(target) || g_.fn().unit_dying(target)) return ShotResult::none;
		unit_t* p = player(); if (!p || !is_enemy(target)) return ShotResult::none;
		if (p->ground_weapon_cooldown > kHold) return ShotResult::none; // kHold is our artificial no-auto-fire floor, not a real cooldown
		if (g_.frame() < busy_until_) return target == pending_target_ ? ShotResult::hit : ShotResult::none; // a command is on its way
		const weapon_type_t* w = p->unit_type->ground_weapon.operator->();
		if (!w) return ShotResult::none;
		if (!in_range(target)) { // out of weapon range: the bolt cannot reach; take the shot and miss
			ActionWriter m; m.u8(Game::kChanFps).u8(kMsgMiss); g_.custom(m);
			busy(nullptr);
			return ShotResult::miss;
		}
		if (direct_) { // counted (gun animation, sound) when taken, not when it lands
			ActionWriter m; m.u8(Game::kChanFps).u8(kMsgShoot).u16((int)g_.fn().get_unit_id(target).raw_value); g_.custom(m);
			busy(target);
			++shots;
			return ShotResult::hit;
		}
		if (p->order_target.unit == target && p->order_type->id == Orders::AttackUnit) return ShotResult::hit;
		g_.select(p); g_.smart_order(target->sprite->position, target);
		busy(target);
		++shots;
		return ShotResult::hit;
	}

	// ---- free movement (direct mode only). The hero takes no movement or attack orders; its position is integer physics
	// run once per simulation frame for every hero (move_step), from its player's input sent as commands, so both games
	// of a two-player game move it alike. The local camera runs the same physics ahead (view_position).
	// Input: walk toward (ix,iy) in map space (any length; (0,0) = no input). Sent before the next step if it changed.
	void drive(double ix, double iy) { want_dir_ = (ix == 0 && iy == 0) ? -1 : direction_of(ix, iy); }
	// Walk to (x,y) until there is walking input again (RMB).
	void go_to(double x, double y) { send_input(kMsgGoal, (int)x, (int)y); }
	// Stop at once: no input, no goal, no speed.
	void halt() { want_dir_ = sent_dir_ = -1; send_input(kMsgHalt, 0, 0); }
	// Single player (tests): put the hero at (x,y), at rest; it is moved there by the next tick().
	void place(double x, double y) { Mover& m = heroes_[g_.owner()].m; m.x = (int)x * 256 + 128; m.y = (int)y * 256 + 128; m.vx = m.vy = 0; m.goal = false; }
	// The local hero's speed in px per second.
	double speed() const { const Mover& m = heroes_[g_.owner()].m; return std::hypot(m.vx, m.vy) / 256.0 * kFramesPerSecond; }
	// A hero's exact free-movement position (px).
	std::pair<double, double> mover_position(int slot) const { const Mover& m = heroes_[slot].m; return {m.x / 256.0, m.y / 256.0}; }
	// Where the camera shows the local hero: the simulation's position, run ahead over the input this player has sent
	// that the simulation has not applied yet, then `ahead` (0..1) of the way into the frame that input given now lands in
	// (single player: the next frame). The simulation runs the same code with the same input and only terrain stops the
	// hero, so it ends up exactly there unless the hero dies first. False when not in free movement.
	bool view_position(double ahead, double& x, double& y) {
		const Hero& h = heroes_[g_.owner()]; unit_t* p = g_.fn().get_unit(h.id);
		if (!direct_ || !p || h.trigger == kTriggerFree || h.was_free || g_.fn().unit_dying(p)) return false;
		Mover m = h.m; int now = g_.frame(), last = now + (g_.net() ? g_.latency() : 1);
		int px = m.x, py = m.y;
		for (int f = now + 1; f <= last; ++f) {
			for (const Input& e : pending_) if (e.frame == f) apply(m, e);
			if (f == g_.command_frame() && want_dir_ != sent_dir_) m.dir = want_dir_; // the input as it is now goes out before the next step
			px = m.x; py = m.y;
			move_step(m, p);
		}
		ahead = std::max(0.0, std::min(1.0, ahead));
		x = (px + (m.x - px) * ahead) / 256.0; y = (py + (m.y - py) * ahead) / 256.0;
		return true;
	}

	// The weapon only fires while the trigger is held. StarCraft units attack anything in range on their own, which is wrong for a
	// shooter, so while the trigger is up the weapon cooldown is kept above zero (the attack logic needs it to be 0).
	// The trigger is part of the simulation: a change goes out as a command (two-player game: applied `latency` frames later).
	// Direct mode shoots only through fire(): its trigger stays up (or free while commanding the base).
	void set_trigger(bool held) { if (!direct_) set_trigger_mode(held ? kTriggerDown : kTriggerUp); }
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

	// Free movement per simulation frame (23.81 per second), in 1/256 px: top speed 192 px/s, acceleration 2000 px/s^2,
	// friction e^(-8 t) without input, at rest below 2 px/s.
	static constexpr double kFramesPerSecond = 23.81;
	static constexpr int kTopSpeed = 2064, kAccel = 903, kFriction = 183, kRest = 22;

private:
	enum : uint8_t { kMsgTrigger = 1, kMsgMiss = 2, kMsgMove = 3, kMsgGoal = 4, kMsgHalt = 5, kMsgShoot = 6 };
	// Free movement state of one hero: position (1/256 px), velocity (1/256 px per frame), input direction (OpenBW
	// direction units, 0 = north, 64 = east; -1 = none), walk target (px).
	struct Mover { int x = 0, y = 0, vx = 0, vy = 0, dir = -1; bool goal = false; int gx = 0, gy = 0; };
	struct Hero { unit_id id{}; int trigger = kTriggerUp; int respawn_at = -1; Mover m; bool walking = false, was_free = false; int shot_frame = -100; };
	// A movement command of the local player, sent but maybe not applied yet (frame: the one whose step applies it).
	struct Input { int frame; int kind, a, b; };

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
		if (direct_) steer(h, p);
		else if (s == g_.owner() && (int)p->ground_weapon_cooldown > last_cooldown_ && (int)p->ground_weapon_cooldown > kHold) ++shots; // a real attack, not our hold
		if (direct_ ? h.trigger != kTriggerFree : h.trigger == kTriggerUp) { if (p->ground_weapon_cooldown < kHold) p->ground_weapon_cooldown = kHold; if (p->air_weapon_cooldown < kHold) p->air_weapon_cooldown = kHold; }
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
		else if (kind == kMsgMove || kind == kMsgGoal || kind == kMsgHalt) {
			Input e{0, kind, 0, 0};
			if (kind == kMsgMove) { int d = r.get<uint16_t>(); e.a = d > 255 ? -1 : d; }
			else if (kind == kMsgGoal) { e.a = r.get<uint16_t>(); e.b = r.get<uint16_t>(); }
			apply(h.m, e);
			if (slot == g_.owner() && !pending_.empty()) pending_.erase(pending_.begin());
		}
		else if (kind == kMsgShoot) { // direct mode: the shot lands now; the target may have died or walked out of range since
			unit_t* t = g_.fn().get_unit(unit_id((uint16_t)r.get<uint16_t>()));
			unit_t* p = g_.fn().get_unit(h.id); const weapon_type_t* w = p ? p->unit_type->ground_weapon.operator->() : nullptr;
			if (!p || !w || h.trigger == kTriggerFree || g_.fn().unit_dying(p) || !targetable(t) || t->owner == slot || t->owner >= 8) return;
			if (p->ground_weapon_cooldown > kHold) return;
			p->ground_weapon_cooldown = w->cooldown;
			if (!reaches(p, t)) { if (slot == g_.owner()) ++misses; return; }
			direction_t d = g_.fn().xy_direction(t->sprite->position - p->sprite->position);
			g_.fn().set_unit_heading(p, d); h.shot_frame = g_.frame();
			g_.fn().weapon_deal_damage(w, fp8::integer(w->damage_amount), 1, t, d, p, slot);
		}
	}
	// Weapon range check in integers (both games of a two-player game must agree on it).
	bool reaches(const unit_t* p, const unit_t* t) {
		int64_t dx = t->sprite->position.x - p->sprite->position.x, dy = t->sprite->position.y - p->sprite->position.y;
		int64_t r = (int64_t)range(p) + t->unit_type->dimensions.to.x;
		return dx * dx + dy * dy <= r * r;
	}

	// ---- free movement ----
	static int direction_of(double ix, double iy) { // map-space vector -> OpenBW direction (local input only)
		return (int)std::lround((std::atan2(iy, ix) + 1.5707963267948966) / 6.283185307179586 * 256) & 255;
	}
	static int64_t isqrt(int64_t v) {
		if (v <= 0) return 0;
		int64_t r = (int64_t)std::sqrt((double)v);
		while (r * r > v) --r;
		while ((r + 1) * (r + 1) <= v) ++r;
		return r;
	}
	static void apply(Mover& m, const Input& e) {
		if (e.kind == kMsgMove) m.dir = e.a;
		else if (e.kind == kMsgGoal) { m.goal = true; m.gx = e.a; m.gy = e.b; }
		else if (e.kind == kMsgHalt) { m.dir = -1; m.goal = false; m.vx = m.vy = 0; }
	}
	void send_input(int kind, int a, int b) {
		if (!direct_) return;
		pending_.push_back({g_.command_frame(), kind, a, b}); // first: single player applies it (and drops it) at once
		ActionWriter w; w.u8(Game::kChanFps).u8(kind);
		if (kind == kMsgMove) w.u16(a < 0 ? 0xffff : a); else if (kind == kMsgGoal) w.u16(a).u16(b); // a direction uses all of 0..255
		g_.custom(w);
	}
	void flush_input() {
		pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Input& e) { return e.frame <= g_.frame(); }), pending_.end()); // never applied (connection lost)
		if (direct_ && want_dir_ != sent_dir_) { sent_dir_ = want_dir_; send_input(kMsgMove, want_dir_, 0); }
	}
	// One frame of free movement for hero unit u: accelerate toward the input direction (or the walk target), friction
	// without input, speed cap, then terrain collision per axis.
	void move_step(Mover& m, const unit_t* u) {
		int64_t ax = 0, ay = 0; bool push = false;
		if (m.dir >= 0) { m.goal = false; const xy_fp8& d = direction_table[m.dir & 255]; ax = (int64_t)d.x.raw_value * kAccel / 256; ay = (int64_t)d.y.raw_value * kAccel / 256; push = true; }
		else if (m.goal) {
			int64_t dx = (int64_t)m.gx * 256 + 128 - m.x, dy = (int64_t)m.gy * 256 + 128 - m.y, dist = isqrt(dx * dx + dy * dy);
			if (dist < 12 * 256) m.goal = false;
			else { ax = dx * kAccel / dist; ay = dy * kAccel / dist; push = true; }
		}
		if (push) {
			m.vx += (int)ax; m.vy += (int)ay;
			int64_t sp = isqrt((int64_t)m.vx * m.vx + (int64_t)m.vy * m.vy);
			if (sp > kTopSpeed) { m.vx = (int)((int64_t)m.vx * kTopSpeed / sp); m.vy = (int)((int64_t)m.vy * kTopSpeed / sp); }
		} else {
			m.vx = m.vx * kFriction / 256; m.vy = m.vy * kFriction / 256;
			if (std::abs(m.vx) < kRest) m.vx = 0;
			if (std::abs(m.vy) < kRest) m.vy = 0;
		}
		if (!slide(u, m.x, m.y, m.vx, 0)) m.vx = 0;
		if (!slide(u, m.x, m.y, 0, m.vy)) m.vy = 0;
	}
	// Move by (dx,dy) in steps of at most 4 px (no tunnelling through thin walls); false if a step was blocked (the hero
	// stays at the last free one).
	bool slide(const unit_t* u, int& x, int& y, int dx, int dy) {
		int n = (std::max(std::abs(dx), std::abs(dy)) + 1023) / 1024, x0 = x, y0 = y;
		for (int i = 1; i <= n; ++i) {
			int nx = x0 + (int)((int64_t)dx * i / n), ny = y0 + (int)((int64_t)dy * i / n);
			if (!stands(u, nx, ny)) return false;
			x = nx; y = ny;
		}
		return true;
	}
	// Whether hero unit u can stand at (x,y) (1/256 px): the corners of a box half its width around the whole pixel are
	// walkable, and OpenBW keeps the position as it is (map edges).
	bool stands(const unit_t* u, int x, int y) {
		int r = (u->unit_type->dimensions.from.x + u->unit_type->dimensions.to.x + 1) * 64, px = x >> 8, py = y >> 8;
		if (x < 0 || y < 0) return false;
		for (int sy = -1; sy <= 1; sy += 2) for (int sx = -1; sx <= 1; sx += 2) {
			int cx = px * 256 + sx * r, cy = py * 256 + sy * r;
			if (cx < 0 || cy < 0) return false;
			cx >>= 8; cy >>= 8;
			if (cx >= (int)g_.st().game->map_width || cy >= (int)g_.st().game->map_height || !g_.fn().is_walkable(xy{cx, cy})) return false;
		}
		return g_.fn().restrict_move_target_to_valid_bounds(u, xy{px, py}) == xy{px, py};
	}
	// The mover at rest on the unit (a new hero, or back from OpenBW's control). The input stays: it is what was last sent.
	void reset_mover(Hero& h, const unit_t* p, bool keep_goal = false) {
		h.m.x = p->sprite->position.x * 256 + 128; h.m.y = p->sprite->position.y * 256 + 128; h.m.vx = h.m.vy = 0; h.walking = false;
		if (!keep_goal) h.m.goal = false;
	}
	// Once per frame and hero (direct mode). While its player commands the base (trigger free) OpenBW drives the hero.
	void steer(Hero& h, unit_t* p) {
		if (g_.fn().unit_dying(p)) return;
		if (h.trigger == kTriggerFree) { h.was_free = true; return; }
		if (h.was_free) { h.was_free = false; reset_mover(h, p, true); } // a walk target given on the way back stays
		// OpenBW's idle behaviour picks targets on its own and walks to them; this hero shoots only through fire()
		Orders o = p->order_type->id;
		if (o != Orders::PlayerGuard && o != Orders::Guard && o != Orders::Stop && o != Orders::Nothing) g_.fn().issue_order(p, false, g_.fn().get_order_type(Orders::Stop), {});
		move_step(h.m, p);
		xy at{h.m.x >> 8, h.m.y >> 8};
		if (p->sprite->position != at) g_.fn().move_unit(p, at); // also when OpenBW pushed it: the position is ours
		bool walking = h.m.vx != 0 || h.m.vy != 0;
		if (walking && g_.frame() - h.shot_frame > 8) { // face the way it walks (after a shot: the target, for a moment)
			direction_t d = g_.fn().xy_direction(xy{h.m.vx, h.m.vy});
			if (d != p->heading) g_.fn().set_unit_heading(p, d);
		}
		if (walking != h.walking) { // the walk animation, as OpenBW plays it for units moving on orders
			h.walking = walking;
			if (walking) g_.fn().sprite_run_anim(p->sprite, iscript_anims::Walking);
			else if (!g_.fn().s_flag(p->sprite, sprite_t::flag_iscript_nobrk)) g_.fn().sprite_run_anim(p->sprite, iscript_anims::WalkingToIdle);
		}
	}
	uint32_t hash() const {
		uint32_t v = 2166136261u; auto add = [&](uint32_t x) { v ^= x; v *= 16777619u; };
		for (int s : slots_) { const Hero& h = heroes_[s]; const Mover& m = h.m;
			add(h.id.raw_value); add(h.trigger); add(h.respawn_at); add(m.x); add(m.y); add(m.vx); add(m.vy); add(m.dir); add(m.goal); add(m.gx); add(m.gy); add(h.walking); }
		return v;
	}
	// Two-player game: hold this shot until its command has been applied (single player: applied already).
	void busy(const unit_t* target) { pending_target_ = target; busy_until_ = g_.net() ? g_.frame() + g_.latency() + 1 : 0; }
	unit_t* spawn_hero(int s) {
		unit_t* u = g_.spawn(fps_player_unit(g_.st().players[s].race), g_.start_location(s), s);
		if (!u) return nullptr;
		reset_mover(heroes_[s], u);
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
	int want_dir_ = -1, sent_dir_ = -1; std::vector<Input> pending_; FpsKinds roster_{};
	std::vector<unit_id> enemy_ids_; int next_wave_frame_ = 0, last_cooldown_ = 0;
};

} // namespace fp
