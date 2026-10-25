// Two-player games over OpenBW's lockstep sync (sync.h) and its TCP server. Each player runs the whole simulation;
// only player commands travel. A command is applied by both players on the same future frame (`latency` frames after
// it was given), so the two games stay identical as long as the simulation only changes through commands, or through
// code both players run on the same frame from the same state.
//
// Lobby: the host listens, the joiner connects (retrying until the host is up); each takes a map slot and sends its
// race, then the host starts the game. It runs inside the map load (OpenBW's setup callback): after the map's slots are
// read, before any starting unit exists. OpenBW shuffles the two players over the map's start locations (melee rules).
//
// Not covered: more than two players (sync.h accepts one peer), NAT traversal (the joiner must reach the host's port),
// reconnecting, observers.
#pragma once
#include "bwgame.h"
#include "actions.h"
#include "sync.h"
#include "sync_server_asio_tcp.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fp {
using namespace bwgame;

struct NetOptions {
	bool host = false;
	std::string address = "127.0.0.1"; // joiner: where the host is
	int port = 6112;
	int latency = 2;          // frames between a command and its execution; both players must use the same value
	double wait_seconds = 0;  // give up in the lobby after this long without a game (0: wait forever)
	std::string name = "player";
};

// The command stream of one player: OpenBW replay-format actions (applied with read_action) and our own custom
// messages. Single player applies each at once; multiplayer sends it to both games, which apply it on the same frame.
struct ActionWriter {
	uint8_t buf[64]; size_t n = 0;
	ActionWriter& u8(int v) { buf[n++] = (uint8_t)v; return *this; }
	ActionWriter& u16(int v) { u8(v & 0xff); return u8((v >> 8) & 0xff); }
	ActionWriter& u32(uint32_t v) { u16((int)(v & 0xffff)); return u16((int)(v >> 16)); }
};

class Net {
public:
	using syncer_t = sync_functions::syncer_t<sync_server_asio_tcp>;

	Net() = default;
	Net(const Net&) = delete; Net& operator=(const Net&) = delete;
	// OpenBW's server frees its client list before its io_service, and a socket read still pending then releases its
	// client into the freed list (double free on exit). Close the sockets and run the aborted handlers first.
	~Net() {
		for (auto& c : server_.clients) { c.on_kill = {}; c.on_message = {}; asio::error_code ec; c.socket.close(ec); }
		asio::error_code ec; server_.timer.cancel(ec);
		server_.io_service.poll();
	}

	// Creates the sync layer over a game's state. The returned functions replace the plain action_functions.
	sync_functions& attach(state& st, action_state& action_st) {
		funcs_ = std::make_unique<sync_functions>(st, action_st, sync_st_);
		return *funcs_;
	}

	// Called from the map load's setup callback (before starting units exist). Blocks until the host has started the
	// game (true) or the lobby gave up (false, reason in error()). `status` gets progress lines.
	bool lobby(game_load_functions& load, race_t race, const NetOptions& o, const std::function<void(const std::string&)>& status) {
		state& st = funcs_->st; options_ = o;
		load.setup_info.victory_condition = 1;  // melee: the standard melee triggers decide victory and defeat
		load.setup_info.starting_units = 2;     // a base and workers per player
		load.setup_info.tournament_mode = 0;
		load.setup_info.resource_type = 1;
		load.setup_info.starting_minerals = 50;
		sync_st_.setup_info = &load.setup_info;
		sync_st_.game_type_melee = true;
		sync_st_.latency = o.latency;
		funcs_->set_local_client_name(o.name.c_str());
		std::vector<int> open; // computer slots become open ones (OpenBW has no AI); sync.h only lets players take open slots
		for (int i = 0; i < 8; ++i) {
			auto& c = st.players[i].controller;
			if (c == player_t::controller_computer) c = player_t::controller_open;
			if (c == player_t::controller_open) open.push_back(i);
		}
		if (open.size() < 2) { error_ = "the map has fewer than two player slots"; return false; }
		if (o.host) { server_.bind("0.0.0.0", o.port); status(format("Hosting on port %d: waiting for the other player...", o.port).c_str()); }
		else status(format("Joining %s:%d...", o.address.c_str(), o.port).c_str());
		auto t0 = std::chrono::steady_clock::now(), last_try = t0 - std::chrono::seconds(10);
		bool asked_slot = false, asked_start = false, said_connected = false;
		while (!sync_st_.game_started) {
			auto now = std::chrono::steady_clock::now();
			if (o.wait_seconds > 0 && now - t0 > std::chrono::duration<double>(o.wait_seconds)) { error_ = o.host ? "nobody joined" : "could not reach the host"; return false; }
			if (!o.host && peers() < 2 && now - last_try > std::chrono::seconds(1)) { server_.connect(o.address.c_str(), o.port); last_try = now; }
			funcs_->sync(server_); // one lobby frame (waits up to ~50 ms for messages)
			no_delay();
			if (peers() < 2) { if (said_connected) { error_ = "the other player left the lobby"; return false; } continue; }
			if (!said_connected) { said_connected = true; status("Connected: " + peer_name()); }
			if (!asked_slot) { // host takes the first open slot, joiner the second (OpenBW shuffles start locations at the start)
				funcs_->switch_to_slot(server_, open[o.host ? 0 : 1]);
				funcs_->set_local_client_race(server_, race);
				asked_slot = true;
			}
			if (o.host && !asked_start) {
				bool all = true; for (auto& c : sync_st_.clients) if (c.player_slot < 0) all = false;
				if (all) { funcs_->start_game(server_); asked_start = true; status("Starting..."); }
			}
		}
		return true;
	}

	// Lockstep, without blocking: announces this player's next frame once, then reports whether every player has
	// caught up. When it returns true the frame's commands have been applied and the caller steps the state.
	bool poll_frame() {
		syncer_t& sy = funcs_->get_syncer(server_);
		if (!announced_) {
			sy.sync_next_frame(); announced_ = true;
			server_.set_timeout(std::chrono::seconds(1), std::bind(&syncer_t::timeout_func, &sy));
		}
		server_.poll(std::bind(&syncer_t::on_new_client, &sy, std::placeholders::_1));
		if (!sy.all_clients_in_sync()) return false;
		auto now = std::chrono::steady_clock::now(); for (auto& c : sync_st_.clients) c.last_synced = now;
		sy.process_messages();
		announced_ = false;
		return true;
	}

	void send_action(const ActionWriter& w) { funcs_->input_action(server_, w.buf, w.n); }
	void send_custom(const ActionWriter& w) { funcs_->send_custom_action(server_, w.buf, w.n); }
	// Leave politely: the other game sees this player leave on a known frame.
	void leave() { if (funcs_ && sync_st_.game_started && !left_) { left_ = true; try { funcs_->leave_game(server_); } catch (const std::exception&) {} } }

	int peers() const { int n = 0; for (auto& c : sync_st_.clients) if (c.has_uid) ++n; return n; }
	int local_slot() const { return sync_st_.local_client->player_slot; }
	int peer_slot() const { for (auto& c : sync_st_.clients) if (&c != sync_st_.local_client && c.player_slot >= 0) return c.player_slot; return -1; }
	std::string peer_name() const { for (auto& c : sync_st_.clients) if (&c != sync_st_.local_client) return c.name.c_str(); return ""; }
	int latency() const { return sync_st_.latency; }
	// Whether this player's next frame is announced but not yet stepped (waiting for the other player): a command given
	// now lands one frame later than right after a step.
	bool announced() const { return announced_; }
	const NetOptions& options() const { return options_; }
	const std::string& error() const { return error_; }
	sync_functions& funcs() { return *funcs_; }

private:
	// Every frame sends a few tiny messages; with Nagle's algorithm each waits for the previous one's ACK (delayed up to
	// 40 ms), which held localhost lockstep to ~50 frames per second. OpenBW's server does not set this itself.
	void no_delay() {
		for (auto& c : server_.clients) if (c.socket.is_open()) { asio::error_code ec; c.socket.set_option(asio::ip::tcp::no_delay(true), ec); }
	}

	sync_server_asio_tcp server_;
	sync_state sync_st_;
	std::unique_ptr<sync_functions> funcs_;
	NetOptions options_;
	std::string error_;
	bool announced_ = false, left_ = false;
};

} // namespace fp
