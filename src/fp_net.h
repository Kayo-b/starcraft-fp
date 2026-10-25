// Two-player games over OpenBW's lockstep sync (sync.h) and its TCP server. Each player runs the whole simulation;
// only player commands travel. A command is applied by both players on the same future frame (`latency` frames after
// it was given), so the two games stay identical as long as the simulation only changes through commands, or through
// code both players run on the same frame from the same state.
//
// Lobby: the host listens, the joiner connects (retrying until the host is up); each takes a map slot and sends its
// race, then the host starts the game. It runs inside the map load (OpenBW's setup callback): after the map's slots are
// read, before any starting unit exists. OpenBW shuffles the two players over the map's start locations (melee rules).
//
// Latency: the host picks it from the round trip it measured in the lobby (pick_latency), unless given one, and tells
// the joiner just before it starts the game. A game schedules every command it receives at the sender's frame plus its
// own latency of that moment, so both must switch while no scheduled message is on its way: after both have taken
// their slots and races (those messages have arrived and been applied in the host's game), before the start (sent
// after the latency, so the joiner has switched by the time it reads it). Until then the lobby runs at kLobbyLatency.
// The latency stays fixed for the game: changed later, commands already scheduled could land out of order.
//
// Not covered: more than two players (sync.h accepts one peer), NAT traversal (the joiner must reach the host's port),
// reconnecting, observers.
#pragma once
#include "bwgame.h"
#include "actions.h"
#include "sync.h"
#include "sync_server_asio_tcp.h"
#include <algorithm>
#include <chrono>
#include <cmath>
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
	int latency = 0;          // host: frames between a command and its execution (1..8), 0 = picked from the round trip; the joiner takes the host's
	double wait_seconds = 0;  // give up in the lobby after this long without a game (0: wait forever)
	std::string name = "player";
};

// Frames of latency for a measured round trip (ms). Frame N of one game can be stepped once the other game has
// announced frame N - latency + 1, which it does when it is ready to step it; so the latency less one frame must cover
// the one-way trip plus the wait until the other game's next look at the network (once per rendered frame, ~17 ms at
// 60 Hz), with some room for jitter: kNetMarginMs in all. At least 2 (localhost's value; 1 makes every frame wait for
// the other game's last one).
static constexpr double kNetMarginMs = 25, kFrameMs = 1000.0 / 23.81;
static inline int pick_latency(double rtt_ms) { return std::max(2, std::min(8, 1 + (int)std::ceil((std::max(0.0, rtt_ms) / 2 + kNetMarginMs) / kFrameMs))); }

// The "waiting for the other player" line. It comes on once the game has stood still for kShowAfter, or has spent
// kBusy of about the last second waiting (a slow connection: short stops, often; the game then runs at 60% speed or
// less), and goes off only after kHoldFor without either, counted while the game runs; so a game that stops and goes
// does not make it blink.
struct WaitIndicator {
	static constexpr double kShowAfter = 0.3, kBusy = 0.4, kHoldFor = 1.0;
	double stalled = 0, busy = 0, hold = 0; // seconds of the current stop; share of recent time spent waiting; seconds left on
	void update(bool waiting, double dt) {
		stalled = waiting ? stalled + dt : 0;
		busy += ((waiting ? 1.0 : 0.0) - busy) * std::min(1.0, dt); // about the last second
		if (stalled >= kShowAfter || busy >= kBusy) hold = kHoldFor;
		else if (!waiting) hold = std::max(0.0, hold - dt);
	}
	bool shown() const { return hold > 0; }
};

// OpenBW's TCP server, with our own messages (round-trip pings, the host's latency) taken out of the stream before the
// sync layer sees them: they start with a tag no sync message starts with (OpenBW's message ids are small, its
// in-game escape is 0xdc, replay actions stay below 0x60).
struct NetServer : sync_server_asio_tcp {
	static constexpr uint8_t kTag0 = 0xfe, kTag1 = 0xf9;
	std::function<void(const void* h, data_loading::data_reader_le&)> own;
	template<typename F> void set_on_message(const void* h, F&& f) {
		sync_server_asio_tcp::set_on_message(h, [this, h, f = std::forward<F>(f)](const void* data, size_t n) mutable {
			const uint8_t* d = (const uint8_t*)data;
			if (n >= 3 && d[0] == kTag0 && d[1] == kTag1) { if (own) { data_loading::data_reader_le r(d + 2, d + n); own(h, r); } return; }
			f(data, n);
		});
	}
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
	using syncer_t = sync_functions::syncer_t<NetServer>;
	static constexpr int kLobbyLatency = 2;
	// Lobby: pings while the players take their slots, until kPingSamples round trips are in (the host then picks the
	// latency from the 90th percentile; it gives up waiting after kPingWait). In the game: one ping every half second.
	static constexpr int kPingSamples = 12; static constexpr double kPingWait = 3.0;

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
		sync_st_.latency = kLobbyLatency;
		funcs_->set_local_client_name(o.name.c_str());
		server_.own = [this](const void* h, data_loading::data_reader_le& r) { on_own(h, r); };
		std::vector<int> open; // computer slots become open ones (OpenBW has no AI); sync.h only lets players take open slots
		for (int i = 0; i < 8; ++i) {
			auto& c = st.players[i].controller;
			if (c == player_t::controller_computer) c = player_t::controller_open;
			if (c == player_t::controller_open) open.push_back(i);
		}
		if (open.size() < 2) { error_ = "the map has fewer than two player slots"; return false; }
		if (o.host) { server_.bind("0.0.0.0", o.port); status(format("Hosting on port %d: waiting for the other player...", o.port).c_str()); }
		else status(format("Joining %s:%d...", o.address.c_str(), o.port).c_str());
		auto t0 = std::chrono::steady_clock::now(), last_try = t0 - std::chrono::seconds(10), connected_at = t0;
		bool asked_slot = false, asked_start = false, said_connected = false;
		while (!sync_st_.game_started) {
			auto now = std::chrono::steady_clock::now();
			if (o.wait_seconds > 0 && now - t0 > std::chrono::duration<double>(o.wait_seconds)) { error_ = o.host ? "nobody joined" : "could not reach the host"; return false; }
			if (!o.host && peers() < 2 && now - last_try > std::chrono::seconds(1)) { server_.connect(o.address.c_str(), o.port); last_try = now; }
			funcs_->sync(server_); // one lobby frame (waits up to ~50 ms for messages)
			no_delay();
			if (peers() < 2) { if (said_connected) { error_ = "the other player left the lobby"; return false; } continue; }
			if (!said_connected) { said_connected = true; connected_at = now; status("Connected: " + peer_name()); }
			ping_if_due(0.04);
			if (!asked_slot) { // host takes the first open slot, joiner the second (OpenBW shuffles start locations at the start)
				funcs_->switch_to_slot(server_, open[o.host ? 0 : 1]);
				funcs_->set_local_client_race(server_, race);
				asked_slot = true;
			}
			if (o.host && !asked_start) {
				bool all = true; for (auto& c : sync_st_.clients) if (c.player_slot < 0) all = false;
				if (!all) continue;
				if (o.latency <= 0 && (int)lobby_rtts_.size() < kPingSamples && now - connected_at < std::chrono::duration<double>(kPingWait)) continue; // still measuring
				// The quiet moment (see the header comment): switch, tell the joiner, then start.
				int lat = o.latency > 0 ? o.latency : lobby_rtts_.empty() ? kLobbyLatency : pick_latency(lobby_rtt_ms());
				sync_st_.latency = lat;
				auto m = own_message('l'); m.put<uint8_t>((uint8_t)lat); server_.send_message(m, nullptr);
				funcs_->start_game(server_); asked_start = true;
				status(o.latency > 0 ? format("Starting (latency %d frames)...", lat).c_str() : lobby_rtts_.empty() ? format("Starting (latency %d frames: no round trip measured)...", lat).c_str()
					: format("Starting (round trip %.0f ms: latency %d frames)...", lobby_rtt_ms(), lat).c_str());
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
		if (peers() >= 2) ping_if_due(0.5);
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
	// Round trips (ms): the lobby's (90th percentile of its samples; 0 if none came back), and the game's (smoothed; 0
	// before the first). The game's includes the wait until the other game reads the network (once per rendered frame).
	double lobby_rtt_ms() const {
		if (lobby_rtts_.empty()) return 0;
		std::vector<double> v = lobby_rtts_; std::sort(v.begin(), v.end()); return v[(v.size() - 1) * 9 / 10];
	}
	int lobby_samples() const { return (int)lobby_rtts_.size(); }
	double rtt_ms() const { return rtt_ms_; }
	bool latency_from_host() const { return latency_from_host_; } // joiner: the host's latency arrived
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

	// Our own messages (NetServer): 'p' ping (sequence, send time), answered at once by 'q' with the same payload; 'l' the
	// latency the host picked (joiner only; see the header comment for why it must come in between the lobby and the start).
	NetServer::message_t own_message(char kind) { auto m = server_.new_message(); m.put<uint8_t>(NetServer::kTag0); m.put<uint8_t>(NetServer::kTag1); m.put<uint8_t>((uint8_t)kind); return m; }
	static uint64_t now_ns() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	void ping_if_due(double interval) {
		auto now = std::chrono::steady_clock::now();
		if (now < next_ping_) return;
		next_ping_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(interval));
		auto m = own_message('p'); m.put<uint32_t>(++ping_seq_); m.put<uint64_t>(now_ns()); server_.send_message(m, nullptr);
	}
	void on_own(const void* h, data_loading::data_reader_le& r) {
		int kind = r.get<uint8_t>();
		if (kind == 'p') { uint32_t seq = r.get<uint32_t>(); uint64_t t = r.get<uint64_t>(); auto m = own_message('q'); m.put<uint32_t>(seq); m.put<uint64_t>(t); server_.send_message(m, h); }
		else if (kind == 'q') {
			r.get<uint32_t>(); double ms = (double)(now_ns() - r.get<uint64_t>()) / 1e6;
			if (!sync_st_.game_started) { if (lobby_rtts_.size() < 64) lobby_rtts_.push_back(ms); }
			else rtt_ms_ = rtt_ms_ > 0 ? rtt_ms_ + (ms - rtt_ms_) * 0.25 : ms;
		}
		else if (kind == 'l' && !options_.host && !sync_st_.game_started) { int l = r.get<uint8_t>(); if (l >= 1 && l <= 8) { sync_st_.latency = l; latency_from_host_ = true; } }
	}

	NetServer server_;
	sync_state sync_st_;
	std::unique_ptr<sync_functions> funcs_;
	NetOptions options_;
	std::string error_;
	bool announced_ = false, left_ = false, latency_from_host_ = false;
	std::vector<double> lobby_rtts_; double rtt_ms_ = 0;
	uint32_t ping_seq_ = 0; std::chrono::steady_clock::time_point next_ping_{};
};

} // namespace fp
