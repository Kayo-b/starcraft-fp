// Live melee game over OpenBW: set up players on a stock melee map, step the simulation and issue orders
// for the human player through OpenBW's action API. No rendering or SDL here.
//
// Not covered: computer opponents (OpenBW has no AI), multiplayer, campaign/UMS map triggers.
#pragma once
#include "bwgame.h"
#include "actions.h"
#include "dir_loader.h"
#include <memory>
#include <string>

namespace fp {
using namespace bwgame;

class Game {
public:
	// human/enemy take the first two open or computer slots of the map. Both are ordinary human-style players
	// (OpenBW does not run AI); the enemy only acts if the caller issues orders for it.
	void open(const std::string& data_dir, const std::string& map, race_t human_race, race_t enemy_race, uint32_t seed = 1, bool fps = false) {
		act_.reset(); player_ = std::make_unique<game_player>();
		player_->init(dir_loader(data_dir));
		state& st = player_->st();
		game_load_functions load(st);
		int slots[2] = {-1, -1}; int found = 0;
		load.load_map_file(map, [&]() {
			if (fps) { // "use map settings": the map's own (neutral) units only, no bases or workers, no melee triggers
				load.setup_info.victory_condition = 0; load.setup_info.starting_units = 0; load.setup_info.tournament_mode = 0;
			} else {
				load.setup_info.victory_condition = 1;  // melee: runs the standard melee triggers
				load.setup_info.starting_units = 2;     // not "use map settings"; base + workers are created per slot
				load.setup_info.tournament_mode = 0;
				load.setup_info.resource_type = 1;      // fixed starting minerals
				load.setup_info.starting_minerals = 50;
			}
			for (int i = 0; i < 8; ++i) {
				int c = st.players[i].controller;
				if (found < 2 && (c == player_t::controller_open || c == player_t::controller_computer || c == player_t::controller_occupied || c == player_t::controller_computer_game)) {
					slots[found++] = i;
					st.players[i].controller = player_t::controller_occupied;
					st.players[i].race = found == 1 ? human_race : enemy_race;
					st.players[i].force = found;
				} else if (c != player_t::controller_neutral) {
					st.players[i].controller = player_t::controller_closed;
				}
			}
			for (int i = 8; i < 12; ++i) st.players[i].force = 0;
			st.lcg_rand_state = seed;
		}, true);
		if (found < 2) error("map has fewer than two player slots");
		owner_ = slots[0]; enemy_ = slots[1];
		action_st_ = std::make_unique<action_state>();
		act_ = std::make_unique<action_functions>(st, *action_st_);
	}

	void step() { static_cast<game_player&>(*player_).next_frame(); }
	int frame() { return player_->st().current_frame; }
	int owner() const { return owner_; }
	int enemy() const { return enemy_; }
	state& st() { return player_->st(); }
	state_functions& fn() { return player_->funcs(); }
	action_functions& act() { return *act_; }
	game_player& player() { return *player_; }

	int minerals() { return st().current_minerals[owner_]; }
	int gas() { return st().current_gas[owner_]; }
	int supply_used() { return st().supply_used[owner_][(int)st().players[owner_].race].integer_part(); }
	int supply_available() { return st().supply_available[owner_][(int)st().players[owner_].race].integer_part(); }

	// Orders for the human player. All return whether OpenBW accepted the action.
	bool select(unit_t* u) { return act_->action_select(owner_, u); }
	bool select_add(unit_t* u) { return act_->action_shift_select(owner_, u); }
	bool smart_order(xy pos, unit_t* target, bool queue = false) { return act_->action_default_order(owner_, pos, target, nullptr, queue); }
	bool stop() { return act_->action_stop(owner_, false); }
	bool train(UnitTypes t) { return act_->action_train(owner_, fn().get_unit_type(t)); }
	unit_t* selected() { return act_->get_first_selected_unit(owner_); }

	// Orders for any owner (used by the enemy AI): select the unit, then issue the order.
	bool attack_as(int owner, unit_t* u, unit_t* target) { return act_->action_select(owner, u) && act_->action_default_order(owner, target->sprite->position, target, nullptr, false); }
	bool attack_move_as(int owner, unit_t* u, xy pos) { return act_->action_select(owner, u) && act_->action_order(owner, fn().get_order_type(Orders::AttackMove), pos, nullptr, nullptr, false); }
	// Create, finish, place and complete a unit, as OpenBW does for starting units (plain create_unit leaves a mobile unit
	// hidden at 10% HP). Returns nullptr, with nothing left behind, if the spot cannot hold the unit.
	unit_t* spawn(UnitTypes t, xy pos, int owner) {
		unit_t* u = fn().create_unit(fn().get_unit_type(t), pos, owner);
		if (!u) return nullptr;
		fn().finish_building_unit(u);
		if (!fn().place_completed_unit(u)) { fn().destroy_unit(u); return nullptr; }
		fn().complete_unit(u);
		return u;
	}
	xy start_location(int owner) { return st().game->start_locations[owner]; }

	// First visible unit matching the predicate, or nullptr.
	template<typename F> unit_t* find(F&& pred) { for (auto u : ptr(st().visible_units)) if (pred(u)) return u; return nullptr; }
	template<typename F> int count(F&& pred) { int n = 0; for (auto u : ptr(st().visible_units)) if (pred(u)) ++n; return n; }
	// Nearest visible unit to (x,y) matching the predicate.
	template<typename F> unit_t* nearest(double x, double y, F&& pred) {
		unit_t* best = nullptr; double bd = 1e30;
		for (auto u : ptr(st().visible_units)) { if (!pred(u)) continue; double d = std::hypot(u->sprite->position.x - x, u->sprite->position.y - y); if (d < bd) { bd = d; best = u; } }
		return best;
	}

private:
	std::unique_ptr<game_player> player_;
	std::unique_ptr<action_state> action_st_;
	std::unique_ptr<action_functions> act_;
	int owner_ = 0, enemy_ = 1;
};

} // namespace fp
