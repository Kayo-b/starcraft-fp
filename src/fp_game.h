// Live melee game over OpenBW: set up players on a stock melee map, step the simulation and issue orders
// for the human player through OpenBW's action API. No rendering or SDL here.
//
// Every command of the human player goes through command()/custom() as OpenBW replay-format bytes: a single-player
// game applies it at once; a two-player game (open_net, fp_net.h) sends it to both games, which apply it on the same
// frame, `latency` frames later. Code that changes the simulation any other way must run inside a frame step, in
// both players' games alike (Fps::tick does).
//
// Not covered: computer opponents (OpenBW has no AI), campaign/UMS map triggers.
#pragma once
#include "bwgame.h"
#include "actions.h"
#include "dir_loader.h"
#include "fp_net.h"
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fp {
using namespace bwgame;

class Game {
public:
	// Custom message channels (first byte of a custom command).
	enum : uint8_t { kChanGame = 1, kChanFps = 2 };
	enum : uint8_t { kHello = 1, kHash = 2 };
	static constexpr int kProtocol = 3;   // bump when commands, the lobby or the simulation-side code change incompatibly
	// Every frame each game sends a hash of its state; the other compares it with its own for that frame (kept for the
	// last kHashRing frames). OpenBW's own check (sync.h, every 32 frames) drops the connection on a mismatch; this one
	// comes first and says why.
	static constexpr int kHashRing = 64;

	~Game() { leave(); }

	// human/enemy take the first two open or computer slots of the map. Both are ordinary human-style players
	// (OpenBW does not run AI); the enemy only acts if the caller issues orders for it.
	void open(const std::string& data_dir, const std::string& map, race_t human_race, race_t enemy_race, uint32_t seed = 1, bool fps = false) {
		reset(); player_ = std::make_unique<game_player>();
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
		own_act_ = std::make_unique<action_functions>(st, *action_st_); act_ = own_act_.get();
	}

	// Two-player melee game (fp_net.h): blocks in the lobby until the host starts it; throws if the lobby gives up.
	// `settings`: a hash of everything else that changes the simulation (e.g. FPS tuning); both players must agree.
	void open_net(const std::string& data_dir, const std::string& map, race_t race, const NetOptions& o, const std::function<void(const std::string&)>& status, uint32_t settings = 0) {
		reset(); player_ = std::make_unique<game_player>();
		player_->init(dir_loader(data_dir));
		state& st = player_->st();
		action_st_ = std::make_unique<action_state>();
		net_ = std::make_unique<Net>();
		sync_functions& sf = net_->attach(st, *action_st_); act_ = &sf;
		sf.on_custom_action = [this](int slot, data_loading::data_reader_le& r) { on_custom(slot, r); };
		game_load_functions load(st);
		load.load_map_file(map, [&]() {
			if (!net_->lobby(load, race, o, status)) error("%s", net_->error().c_str());
			int f = 0; // each player in a force of their own (no shared vision); the slots are the same in both games here
			for (int i = 0; i < 12; ++i) st.players[i].force = i < 8 && st.players[i].controller == player_t::controller_occupied ? ++f : 0;
		}, true);
		owner_ = net_->local_slot(); enemy_ = net_->peer_slot();
		if (owner_ < 0 || enemy_ < 0) error("the game started without two players");
		settings_ = settings; map_crc_ = file_crc(map);
		ActionWriter w; w.u8(kChanGame).u8(kHello).u8(kProtocol).u8(net_->latency()).u32(map_crc_).u32(settings_);
		custom(w);
	}

	// One simulation frame. Two-player game: false, with nothing changed, while the other player has not caught up,
	// and for good once the games disagree (failure()).
	bool step() {
		if (before_step_) before_step_();
		if (net_) {
			if (!failure_.empty()) return false;
			try { if (!net_->poll_frame()) return false; }
			catch (const std::exception& e) { failure_ = std::string("network: ") + e.what(); return false; }
			if (!failure_.empty()) return false; // a mismatch found by this frame's messages
		}
		static_cast<game_player&>(*player_).next_frame();
		if (net_) send_hash();
		return true;
	}
	int frame() { return player_->st().current_frame; }
	// The frame whose step applies a command given now: the next one in single player, `latency` frames on in a
	// two-player game (one more while this game waits for the other player's: its next frame is already announced).
	int command_frame() { return frame() + (net_ ? net_->latency() + (net_->announced() ? 1 : 0) : 1); }
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

	// ---- commands of the human player. Return whether OpenBW accepted them (two-player game: whether they were sent) ----
	bool select(unit_t* u) { return select_units(&u, 1, false); }
	bool select_add(unit_t* u) { return select_units(&u, 1, true); }
	bool smart_order(xy pos, unit_t* target, bool queue = false) {
		ActionWriter w; w.u8(20).u16(pos.x).u16(pos.y).u16(uid(target)).u16((int)UnitTypes::None).u8(queue); return command(w);
	}
	bool order(Orders o, xy pos, unit_t* target, bool queue = false) {
		ActionWriter w; w.u8(21).u16(pos.x).u16(pos.y).u16(uid(target)).u16((int)UnitTypes::None).u8((int)o).u8(queue); return command(w);
	}
	bool stop() { ActionWriter w; w.u8(26).u8(0); return command(w); }
	bool train(UnitTypes t) { ActionWriter w; w.u8(31).u16((int)t); return command(w); }
	bool build(Orders o, UnitTypes t, xy_t<size_t> tile) { ActionWriter w; w.u8(12).u8((int)o).u16((int)tile.x).u16((int)tile.y).u16((int)t); return command(w); }

	// The human player's selection. Two-player game: what this player last selected, until that command has been applied.
	std::vector<unit_t*> selection() {
		std::vector<unit_t*> r;
		if (net_ && frame() <= sel_frame_ + net_->latency() + 1) { for (unit_id id : sel_) if (unit_t* u = fn().get_unit(id)) r.push_back(u); return r; }
		for (unit_t* u : act_->selected_units(owner_)) r.push_back(u);
		return r;
	}
	unit_t* selected() { auto s = selection(); return s.empty() ? nullptr : s.front(); }

	// Raw command streams: OpenBW actions and our own custom messages (first byte: channel).
	bool command(const ActionWriter& w) {
		if (net_) { net_->send_action(w); return true; }
		return act_->read_action(owner_, w.buf, w.n);
	}
	void custom(const ActionWriter& w) {
		if (net_) { net_->send_custom(w); return; }
		data_loading::data_reader_le r(w.buf, w.buf + w.n); on_custom(owner_, r);
	}
	// Receiver of kChanFps messages (slot = the player who sent it); runs inside the frame, in both games.
	void set_fps_handler(std::function<void(int slot, data_loading::data_reader_le&)> f) { fps_handler_ = std::move(f); }
	// Called at the start of every step() (two-player game: every attempt), to send input gathered since the last one.
	void set_before_step(std::function<void()> f) { before_step_ = std::move(f); }
	// More simulation state to cover in state_hash() (state kept outside OpenBW, e.g. the heroes' free movement).
	void set_hash_extra(std::function<uint32_t()> f) { hash_extra_ = std::move(f); }

	// ---- two-player status ----
	bool net() const { return net_ != nullptr; }
	int latency() const { return net_ ? net_->latency() : 0; }
	double rtt_ms() const { return net_ ? net_->rtt_ms() : 0; }             // round trip during the game (0 before the first)
	double lobby_rtt_ms() const { return net_ ? net_->lobby_rtt_ms() : 0; } // what the latency was picked from (0: none measured)
	Net* network() { return net_.get(); }
	bool opponent_left() const { return net_ && net_->peers() < 2; }
	// The other player's game went away without leaving (connection lost, or OpenBW found the games out of sync).
	bool opponent_dropped() { return opponent_left() && st().players[enemy_].victory_state == 1; }
	std::string opponent_name() const { return net_ ? net_->peer_name() : std::string(); }
	int desync_frame() const { return desync_frame_; } // first frame whose state hashes differed, -1 while in sync
	const std::string& failure() const { return failure_; } // why this game stopped (settings differ, out of sync, network)
	bool won() { return fn().player_won(owner_); }
	bool lost() { return fn().player_defeated(owner_); }
	void leave() { if (net_) net_->leave(); }
	uint32_t state_hash() {
		uint32_t h = 2166136261u; auto add = [&](uint32_t v) { h ^= v; h *= 16777619u; };
		add(st().current_frame); add(st().lcg_rand_state);
		for (int i = 0; i < 12; ++i) { add(st().current_minerals[i]); add(st().current_gas[i]); }
		for (unit_t* u : ptr(st().visible_units)) {
			add((uint32_t)u->unit_type->id); add(u->owner); add(u->exact_position.x.raw_value); add(u->exact_position.y.raw_value);
			add((u->hp + u->shield_points).raw_value); add((uint32_t)u->order_type->id); add(u->ground_weapon_cooldown);
		}
		if (hash_extra_) add(hash_extra_());
		return h;
	}

	// Orders for any owner (used by the enemy AI): select the unit, then issue the order. Single player only.
	bool attack_as(int owner, unit_t* u, unit_t* target) { return act_->action_select(owner, u) && act_->action_default_order(owner, target->sprite->position, target, nullptr, false); }
	bool attack_move_as(int owner, unit_t* u, xy pos) { return act_->action_select(owner, u) && act_->action_order(owner, fn().get_order_type(Orders::AttackMove), pos, nullptr, nullptr, false); }
	// Create, finish, place and complete a unit, as OpenBW does for starting units (plain create_unit leaves a mobile unit
	// hidden at 10% HP). Returns nullptr, with nothing left behind, if the spot cannot hold the unit.
	// Changes the simulation directly: in a two-player game only from code both games run on the same frame.
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
	void reset() { leave(); net_.reset(); own_act_.reset(); act_ = nullptr; action_st_.reset(); player_.reset(); failure_.clear(); desync_frame_ = -1; sel_.clear(); sel_frame_ = -1000; hashes_ = {}; }
	int uid(unit_t* u) { return u ? (int)fn().get_unit_id(u).raw_value : 0; }
	bool select_units(unit_t* const* us, size_t n, bool add) {
		ActionWriter w; w.u8(add ? 10 : 9).u8((int)n);
		for (size_t i = 0; i < n; ++i) w.u16(uid(us[i]));
		if (net_) {
			if (!add) sel_.clear();
			for (size_t i = 0; i < n; ++i) { if (!us[i]) continue; unit_id id = fn().get_unit_id(us[i]); if (std::find(sel_.begin(), sel_.end(), id) == sel_.end()) sel_.push_back(id); }
			sel_frame_ = frame();
		}
		return command(w);
	}
	static uint32_t file_crc(const std::string& path) {
		std::ifstream f(path, std::ios::binary); std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
		return d.empty() ? 0 : data_loading::crc32_t()(d.data(), d.size());
	}
	void send_hash() {
		uint32_t h = state_hash(); hashes_[frame() % kHashRing] = {frame(), h};
		ActionWriter w; w.u8(kChanGame).u8(kHash).u32((uint32_t)frame()).u32(h); custom(w);
	}
	void on_custom(int slot, data_loading::data_reader_le& r) {
		int chan = r.get<uint8_t>();
		if (chan == kChanFps) { if (fps_handler_) fps_handler_(slot, r); return; }
		if (chan != kChanGame) { failure_ = "unknown message from the other player (different program versions?)"; return; }
		int kind = r.get<uint8_t>();
		if (slot == owner_) return; // our own hello/hash, echoed back
		if (kind == kHello) {
			int proto = r.get<uint8_t>(), lat = r.get<uint8_t>(); uint32_t crc = r.get<uint32_t>(), set = r.get<uint32_t>();
			if (proto != kProtocol) failure_ = "the other player runs a different version of this program";
			else if (lat != latency()) failure_ = format("the games disagree on the latency (%d and %d frames): the host's choice did not arrive", lat, latency()).c_str();
			else if (crc != map_crc_) failure_ = "the other player loaded a different map file";
			else if (set != settings_) failure_ = "the other player uses different game settings (--fire-rate / --range / --speed / --fps-move-shoot)";
		} else if (kind == kHash) {
			int f = (int)r.get<uint32_t>(); uint32_t h = r.get<uint32_t>();
			auto& mine = hashes_[f % kHashRing];
			if (mine.first == f && mine.second != h && desync_frame_ < 0) { desync_frame_ = f; failure_ = format("the games went out of sync at frame %d", f).c_str(); }
		}
	}

	std::unique_ptr<game_player> player_;
	std::unique_ptr<action_state> action_st_;
	std::unique_ptr<action_functions> own_act_; // single player
	std::unique_ptr<Net> net_;                  // two-player game: its sync functions are act_
	action_functions* act_ = nullptr;
	int owner_ = 0, enemy_ = 1;
	std::function<void(int, data_loading::data_reader_le&)> fps_handler_;
	std::function<void()> before_step_;
	std::function<uint32_t()> hash_extra_;
	std::vector<unit_id> sel_; int sel_frame_ = -1000;
	std::array<std::pair<int, uint32_t>, kHashRing> hashes_{};
	uint32_t settings_ = 0, map_crc_ = 0;
	int desync_frame_ = -1;
	std::string failure_;
};

} // namespace fp
