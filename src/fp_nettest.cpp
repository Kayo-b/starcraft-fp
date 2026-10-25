// Headless two-player test (fp_net.h, Game::open_net, Fps versus mode): two games, one per thread, connected over TCP
// on localhost, each driven by its own scripted commands. Exit code 0 only if every check passes.
//   1. a full game: both games hold the same state on every frame; commands (select, train, attack orders, trigger)
//      take effect on the same frame in both, `latency` frames after they were given; heroes fight, die and come back.
//   2. one game changes its state outside the command stream: both notice that they went out of sync.
//   3. a player leaves: the other game goes on alone and its player wins.
//   4. free movement (--fps-move-shoot): the heroes walk on input commands and shoot while walking; both games agree on
//      every frame, and the camera's view of each hero, run ahead of the simulation, is exactly where the hero ends up.
//   5. a slow network (a relay holding everything for 50 ms each way): the host measures the round trip in the lobby,
//      picks the latency from it and the joiner takes it; both games, paced as the window paces them, keep up with the
//      clock. 6. the same network with a latency too short for it: the games wait on each other and fall behind.
// Before them, without a network: the latency table and the waiting indicator's timing.
#include "fp_fps.h"
#include <arpa/inet.h>
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace fp;
static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }
using Clock = std::chrono::steady_clock;

// A TCP relay on localhost that holds everything it passes on for `delay_ms`, both ways: a slow network.
struct SlowLink {
	int lfd = -1, to_port, delay_ms; std::atomic<bool> stop{false};
	std::thread acceptor; std::vector<std::thread> pumps; // pumps: added by the acceptor only, joined after it
	SlowLink(int port, int to_port, int delay_ms) : to_port(to_port), delay_ms(delay_ms) {
		lfd = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in a = addr(port);
		if (bind(lfd, (sockaddr*)&a, sizeof a) != 0 || listen(lfd, 4) != 0) throw std::runtime_error("SlowLink: cannot listen on the relay port");
		acceptor = std::thread([this] { accept_loop(); });
	}
	~SlowLink() { stop = true; acceptor.join(); for (auto& t : pumps) t.join(); close(lfd); }
	static sockaddr_in addr(int port) { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); return a; }
	static void no_delay(int fd) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
	void accept_loop() {
		while (!stop) {
			pollfd pf{lfd, POLLIN, 0}; if (poll(&pf, 1, 20) <= 0) continue;
			int c = accept(lfd, nullptr, nullptr); if (c < 0) continue;
			int s = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a = addr(to_port);
			if (connect(s, (sockaddr*)&a, sizeof a) != 0) { close(c); close(s); continue; } // the host is not up yet: the joiner tries again
			no_delay(c); no_delay(s);
			auto open = std::make_shared<std::atomic<int>>(2); // the second pump to finish closes both sockets
			pumps.emplace_back([=] { pump(c, s, open); }); pumps.emplace_back([=] { pump(s, c, open); });
		}
	}
	void pump(int from, int to, std::shared_ptr<std::atomic<int>> open) {
		std::deque<std::pair<Clock::time_point, std::vector<uint8_t>>> q; bool eof = false;
		while (!stop && !(eof && q.empty())) {
			auto now = Clock::now();
			while (!q.empty() && q.front().first <= now) {
				const auto& b = q.front().second; size_t sent = 0;
				while (sent < b.size()) { ssize_t n = send(to, b.data() + sent, b.size() - sent, MSG_NOSIGNAL); if (n <= 0) { eof = true; break; } sent += (size_t)n; }
				if (eof) { q.clear(); break; }
				q.pop_front();
			}
			int wait = q.empty() ? 5 : (int)std::max<int64_t>(0, std::min<int64_t>(5, std::chrono::duration_cast<std::chrono::milliseconds>(q.front().first - now).count()));
			if (eof) { std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, wait))); continue; }
			pollfd pf{from, POLLIN, 0};
			if (poll(&pf, 1, wait) > 0) {
				uint8_t buf[4096]; ssize_t n = recv(from, buf, sizeof buf, 0);
				if (n <= 0) eof = true; else q.push_back({Clock::now() + std::chrono::milliseconds(delay_ms), std::vector<uint8_t>(buf, buf + n)});
			}
		}
		shutdown(to, SHUT_WR);
		if (--*open == 0) { close(from); close(to); }
	}
};

struct Peer {
	const char* name; bool host; race_t race;
	Game g; std::unique_ptr<Fps> f;
	std::vector<uint32_t> hashes;     // state hash after every frame
	int mineral_drop = -1;            // frame on which the host's 50 starting minerals were spent (as seen by this game)
	std::string error;
	int latency = 2;                  // --latency (0: auto); the joiner's is ignored
	int via = 0;                      // joiner: connect to this port instead of the host's (a SlowLink)
	bool paced = false;               // step as fp_play's window does: on the clock, a look at the network per rendered frame
	double elapsed = 0, waited = 0;   // paced: seconds from the first step to the last, and spent with a step due but blocked
	bool direct = false;              // free movement (session 4)
	// session 4: OpenBW walks the hero to the other one (trigger free), free movement once they are close
	bool free = false; int direct_since = 0, compared = 0, mispredicted = 0; double walked = 0; std::pair<double, double> last_at{-1, -1};
	struct Prediction { double x, y; int id; };
	std::map<int, Prediction> pred[2]; // the camera's view of the hero for a frame, made one and two frames before it
};

// Opens the game in the lobby, then steps `frames` frames, calling script(peer) before each step.
static void run(Peer& p, const std::string& data, const std::string& map, int port, int frames, const std::function<void(Peer&)>& script) {
	try {
		NetOptions o; o.host = p.host; o.port = p.via && !p.host ? p.via : port; o.latency = p.host ? p.latency : 0; o.wait_seconds = 20; o.name = p.name;
		p.g.open_net(data, map, p.race, o, [&](const std::string&) {}, Fps::settings_hash(200, 200, 90, p.direct));
		p.f = std::make_unique<Fps>(p.g, 7, 200, 200, 90, p.direct, true);
		if (!p.f->start()) { p.error = "no hero"; return; }
		auto t0 = std::chrono::steady_clock::now();
		if (p.paced) { // fp_play's loop: every rendered frame (60 Hz) as much simulation as the clock asks for, unless blocked
			const double sim_dt = 1.0 / 23.81, render_dt = 1.0 / 60; double acc = 0; int scripted = -1;
			auto last = Clock::now(), first = last;
			while ((int)p.hashes.size() < frames) {
				std::this_thread::sleep_until(last + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(render_dt)));
				auto now = Clock::now(); double dt = std::chrono::duration<double>(now - last).count(); last = now; acc += dt;
				if (now - t0 > std::chrono::seconds(120)) { p.error = "timed out waiting for the other game"; return; }
				while (acc >= sim_dt) {
					if (scripted != p.g.frame()) { scripted = p.g.frame(); script(p); }
					if (!p.g.step()) { if (!p.g.failure().empty()) return; p.waited += dt; acc = sim_dt; break; }
					if (p.hashes.empty()) first = now;
					acc -= sim_dt; p.f->tick(); p.hashes.push_back(p.g.state_hash());
				}
			}
			p.elapsed = std::chrono::duration<double>(Clock::now() - first).count();
			return;
		}
		while ((int)p.hashes.size() < frames) {
			script(p);
			while (!p.g.step()) {
				if (!p.g.failure().empty()) return;
				if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) { p.error = "timed out waiting for the other game"; return; }
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
			p.f->tick();
			p.hashes.push_back(p.g.state_hash());
		}
	} catch (const std::exception& e) { p.error = e.what(); }
}

// Both games of one session, the host's thread started first.
static void session(Peer& a, Peer& b, const std::string& data, const std::string& map, int port, int frames, const std::function<void(Peer&)>& script) {
	std::thread ta([&] { run(a, data, map, port, frames, script); });
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::thread tb([&] { run(b, data, map, port, frames, script); });
	ta.join(); tb.join();
}

static int first_difference(const Peer& a, const Peer& b) {
	size_t n = std::min(a.hashes.size(), b.hashes.size());
	for (size_t i = 0; i < n; ++i) if (a.hashes[i] != b.hashes[i]) return (int)i;
	return -1;
}

int main(int argc, char** argv) {
	if (argc < 3) { std::cerr << "usage: fp_nettest DATA_DIR MAP.scm [PORT]\n"; return 2; }
	std::cout << std::unitbuf;
	const std::string data = argv[1], map = argv[2];
	int port = argc > 3 ? atoi(argv[3]) : 47000 + (int)(getpid() % 2000);

	// 0. Without a network.
	std::cout << "latency and waiting indicator\n";
	{
		std::string t; for (double ms : {0.0, 1.0, 40.0, 100.0, 200.0, 400.0, 1000.0}) t += bwgame::format(" %.0f ms: %d", ms, pick_latency(ms)).c_str();
		check(pick_latency(0) == 2 && pick_latency(40) == 3 && pick_latency(100) == 3 && pick_latency(200) == 4 && pick_latency(1000) == 8, "latency for a round trip of" + t + " frames");
		// The window's frames (60 Hz) with a pattern of waiting (w) and running (r) seconds, repeated; returns the
		// seconds the line was shown and how many times it came on, after the first 2 s.
		auto run_pattern = [](std::vector<std::pair<bool, double>> pattern, double total) {
			WaitIndicator w; double t = 0, shown = 0; int ons = 0; bool was = false; size_t k = 0; double left = pattern[0].second;
			for (; t < total; t += 1.0 / 60) {
				w.update(pattern[k].first, 1.0 / 60);
				if ((left -= 1.0 / 60) <= 1e-9) { k = (k + 1) % pattern.size(); left += pattern[k].second; }
				if (t >= 2) { if (w.shown()) shown += 1.0 / 60; if (w.shown() && !was) ++ons; }
				was = w.shown();
			}
			return std::make_pair(shown, ons);
		};
		auto r1 = run_pattern({{true, 0.2}, {false, 2.0}}, 12);      // a short stop now and then
		auto r2 = run_pattern({{true, 0.4}, {false, 0.6}}, 12);      // long stops, often
		auto r3 = run_pattern({{true, 0.05}, {false, 0.05}}, 12);    // half the time waiting, in tiny stops (half speed)
		auto r4 = run_pattern({{true, 0.03}, {false, 0.12}}, 12);    // a fifth of the time waiting
		auto r5 = run_pattern({{true, 0.5}, {false, 3.0}}, 12.1);    // a stop of half a second every 3.5 s
		check(r1.second == 0, bwgame::format("waiting indicator: off for short stops (0.2 s every 2.2 s: on %d times)", r1.second).c_str());
		check(r2.second <= 1 && r2.first > 9.5, bwgame::format("waiting indicator: on, without blinking, through long stops often (0.4 s every second: on %d time(s), %.1f of 10 s)", r2.second, r2.first).c_str());
		check(r3.second <= 1 && r3.first > 9.5, bwgame::format("waiting indicator: on when half the time goes to waiting in tiny stops (%d time(s), %.1f of 10 s)", r3.second, r3.first).c_str());
		check(r4.second == 0, bwgame::format("waiting indicator: off when a fifth of the time goes to waiting (on %d times)", r4.second).c_str());
		check(r5.second == 3 && r5.first > 3 * 1.1 && r5.first < 3 * 1.4, bwgame::format("waiting indicator: a half-second stop shows it once, for about a second after the stop (%d times, %.2f s each)", r5.second, r5.second ? r5.first / r5.second : 0.0).c_str());
	}

	// 1. A full game.
	std::cout << "session 1: a full game (port " << port << ")\n";
	{
		Peer h{"host", true, race_t::terran}, j{"joiner", false, race_t::protoss};
		const int kOrderFrame = 30, kFightFrame = 60, kFrames = 2400;
		std::atomic<int> train_frame_h{-1}, train_frame_j{-1};
		auto script = [&](Peer& p) {
			Game& g = p.g; Fps& f = *p.f; int fr = g.frame();
			if (fr == kOrderFrame) { // the base trains a worker: select, then train, in one frame
				UnitTypes base = p.race == race_t::terran ? UnitTypes::Terran_Command_Center : UnitTypes::Protoss_Nexus;
				UnitTypes worker = p.race == race_t::terran ? UnitTypes::Terran_SCV : UnitTypes::Protoss_Probe;
				unit_t* b = g.find([&](unit_t* u) { return u->owner == g.owner() && u->unit_type->id == base; });
				if (b) { g.select(b); g.train(worker); (p.host ? train_frame_h : train_frame_j) = fr; }
			}
			if (fr == kFightFrame) { f.set_trigger(true); } // weapons free: the heroes fire whenever they can
			if (fr >= kFightFrame && fr % 24 == 0) { // keep each hero attacking the other one
				unit_t* me = f.player(); unit_t* foe = f.opponent();
				if (me && foe && (me->order_target.unit != foe || me->order_type->id != Orders::AttackUnit)) { g.select(me); g.smart_order(foe->sprite->position, foe); }
			}
			// the host's starting minerals as this game sees them (both games must see them spent on the same frame)
			int host_slot = p.host ? g.owner() : g.enemy();
			if (p.mineral_drop < 0 && g.st().current_minerals[host_slot] < 50) p.mineral_drop = fr;
		};
		auto t0 = std::chrono::steady_clock::now();
		session(h, j, data, map, port, kFrames, script);
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		check(h.error.empty() && j.error.empty(), "both games ran (host: " + (h.error.empty() ? std::string("ok") : h.error) + ", joiner: " + (j.error.empty() ? std::string("ok") : j.error) + ")");
		check(h.g.failure().empty() && j.g.failure().empty(), "no failure reported (host: \"" + h.g.failure() + "\", joiner: \"" + j.g.failure() + "\")");
		check(h.g.owner() == j.g.enemy() && h.g.enemy() == j.g.owner() && h.g.owner() != h.g.enemy(), "the games agree on the slots (host " + std::to_string(h.g.owner()) + ", joiner " + std::to_string(j.g.owner()) + ")");
		check(h.g.opponent_name() == "joiner" && j.g.opponent_name() == "host", "each game knows the other player's name (" + h.g.opponent_name() + ", " + j.g.opponent_name() + ")");
		int d = first_difference(h, j);
		check((int)h.hashes.size() == kFrames && (int)j.hashes.size() == kFrames && d < 0, "the two games hold the same state on all " + std::to_string(std::min(h.hashes.size(), j.hashes.size())) + " frames" + (d >= 0 ? " (first difference after step " + std::to_string(d) + ")" : ""));
		check(h.mineral_drop > 0 && h.mineral_drop == j.mineral_drop, "the host's train command takes effect on the same frame in both games (" + std::to_string(h.mineral_drop) + " / " + std::to_string(j.mineral_drop) + ")");
		check(h.mineral_drop >= train_frame_h + 2, "... and not before the command latency has passed (given on frame " + std::to_string(train_frame_h.load()) + ", latency 2)");
		auto workers = [](Peer& p, int slot) { return p.g.count([&](unit_t* u) { return u->owner == slot && (u->unit_type->id == UnitTypes::Terran_SCV || u->unit_type->id == UnitTypes::Protoss_Probe); }); };
		check(workers(h, h.g.owner()) == 5 && workers(h, h.g.enemy()) == 5 && workers(j, j.g.owner()) == 5 && workers(j, j.g.enemy()) == 5, "both players trained a fifth worker, in both games");
		Fps& fh = *h.f; Fps& fj = *j.f;
		check(fh.kills + fh.deaths > 0, "the heroes fought: host hero deaths " + std::to_string(fh.deaths) + ", joiner hero deaths " + std::to_string(fh.kills));
		check(fh.kills == fj.deaths && fh.deaths == fj.kills, "both games count the same hero deaths (" + std::to_string(fh.deaths) + ":" + std::to_string(fh.kills) + " / " + std::to_string(fj.kills) + ":" + std::to_string(fj.deaths) + ")");
		check(fh.alive() || fh.respawn_in() > 0, "the host's hero is alive or about to come back (" + std::to_string(fh.respawn_in()) + " frames)");
		bool came_back = (fh.deaths == 0 || fh.alive() || fh.respawn_in() < Fps::kRespawnFrames) && (fj.deaths == 0 || fj.alive() || fj.respawn_in() < Fps::kRespawnFrames);
		check(came_back, "dead heroes come back after " + std::to_string(Fps::kRespawnFrames) + " frames");
		// The game runs at 24 frames per second; over localhost the lockstep alone must be far faster than that.
		check(kFrames / secs > 240, bwgame::format("lockstep over localhost runs at %.0f frames per second (lobby included; the game needs 24)", kFrames / secs).c_str());
	}

	// 2. A change outside the command stream.
	std::cout << "session 2: one game changes its state on its own\n";
	{
		Peer h{"host", true, race_t::zerg}, j{"joiner", false, race_t::terran};
		const int kCheat = 100;
		auto script = [&](Peer& p) { if (!p.host && p.g.frame() == kCheat) p.g.st().current_minerals[p.g.owner()] += 8; };
		session(h, j, data, map, port + 1, 400, script);
		check(h.error.empty() && j.error.empty(), "both games ran (" + h.error + j.error + ")");
		check(h.g.desync_frame() > kCheat && h.g.desync_frame() <= kCheat + 2 && j.g.desync_frame() == h.g.desync_frame(), "both games find the first frame whose states differ (host: frame " + std::to_string(h.g.desync_frame()) + ", joiner: frame " + std::to_string(j.g.desync_frame()) + ", the change was made before step " + std::to_string(kCheat) + ")");
		check(!h.g.failure().empty() && h.g.failure().find("out of sync") != std::string::npos, "and stop with a message: \"" + h.g.failure() + "\"");
	}

	// 3. A player leaves.
	std::cout << "session 3: the joiner leaves (latency picked from the round trip)\n";
	{
		Peer h{"host", true, race_t::protoss}, j{"joiner", false, race_t::zerg}; h.latency = 0;
		std::atomic<bool> joiner_left{false};
		auto script = [&](Peer& p) {
			if (!p.host && p.g.frame() == 120 && !joiner_left) { p.g.leave(); joiner_left = true; throw std::runtime_error("left"); }
		};
		std::thread ta([&] { run(h, data, map, port + 2, 600, script); });
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::thread tb([&] { run(j, data, map, port + 2, 600, script); });
		ta.join(); tb.join();
		check(joiner_left && j.error == "left", "the joiner left on frame 120");
		check(h.error.empty() && (int)h.hashes.size() == 600, "the host's game goes on alone (" + std::to_string(h.hashes.size()) + " frames" + (h.error.empty() ? "" : ", " + h.error) + ")");
		check(h.g.opponent_left() && !h.g.opponent_dropped(), "the host's game knows the other player left (and did not just drop)");
		check(h.g.won(), "the host wins (melee rules)");
		Net& hn = *h.g.network();
		check(hn.lobby_samples() >= Net::kPingSamples && hn.lobby_rtt_ms() < 5 && h.g.latency() == 2 && j.g.latency() == 2 && j.g.network()->latency_from_host(),
		      bwgame::format("over localhost the host measures a round trip of %.2f ms (%d pings) and picks latency 2; the joiner takes it (%d, %d)", hn.lobby_rtt_ms(), hn.lobby_samples(), h.g.latency(), j.g.latency()).c_str());
	}

	// 4. Free movement.
	std::cout << "session 4: free movement (walk and shoot)\n";
	{
		Peer h{"host", true, race_t::terran}, j{"joiner", false, race_t::protoss}; h.direct = j.direct = true;
		const int kFrames = 2400;
		auto script = [&](Peer& p) {
			Game& g = p.g; Fps& f = *p.f; int fr = g.frame(); unit_t* me = f.player(); unit_t* foe = f.opponent();
			int id = me ? (int)g.fn().get_unit_id(me).raw_value : 0; bool live = me && !g.fn().unit_dying(me);
			for (auto& pr : p.pred) { auto it = pr.find(fr); if (it == pr.end()) continue; // the predictions for this frame
				if (live && it->second.id == id) { ++p.compared; auto at = f.mover_position(g.owner()); if (at.first != it->second.x || at.second != it->second.y) ++p.mispredicted; }
				pr.erase(it); }
			if (live && !p.free) { auto at = f.mover_position(g.owner()); if (p.last_at.first >= 0) p.walked += std::hypot(at.first - p.last_at.first, at.second - p.last_at.second); p.last_at = at; }
			else p.last_at = {-1, -1};
			if (me && foe) {
				double dx = foe->sprite->position.x - me->sprite->position.x, dy = foe->sprite->position.y - me->sprite->position.y, d = std::hypot(dx, dy);
				if (!p.free && d > 420) { f.halt(); f.set_trigger_mode(Fps::kTriggerFree); p.free = true; } // far: OpenBW walks it there
				else if (p.free && d < 260) { f.set_trigger_mode(Fps::kTriggerUp); p.free = false; p.direct_since = fr; f.go_to(foe->sprite->position.x, foe->sprite->position.y); }
				if (p.free && fr % 24 == 0 && (me->order_target.unit != foe || me->order_type->id != Orders::AttackUnit)) { g.select(me); g.smart_order(foe->sprite->position, foe); }
				if (!p.free && fr - p.direct_since > 12) { // circle-strafe, changing sides, and fire whenever the gun is ready
					double side = (fr / 20) % 2 ? 1 : -1; f.drive(-dy * side + dx * 0.25, dx * side + dy * 0.25);
					if (f.in_range(foe)) f.fire(foe);
				}
			} else f.drive(0, 0);
			double x, y; // what the camera would show now, for the next frame and the one after
			if (f.view_position(0, x, y)) p.pred[0][fr + g.latency() - 1] = {x, y, id};
			if (f.view_position(1, x, y)) p.pred[1][fr + g.latency()] = {x, y, id};
		};
		session(h, j, data, map, port + 3, kFrames, script);
		check(h.error.empty() && j.error.empty(), "both games ran (" + h.error + j.error + ")");
		check(h.g.failure().empty() && j.g.failure().empty(), "no failure reported (\"" + h.g.failure() + j.g.failure() + "\")");
		int d = first_difference(h, j);
		check((int)h.hashes.size() == kFrames && (int)j.hashes.size() == kFrames && d < 0, "the two games hold the same state on all " + std::to_string(std::min(h.hashes.size(), j.hashes.size())) + " frames, the heroes' free movement included" + (d >= 0 ? " (first difference after step " + std::to_string(d) + ")" : ""));
		check(h.walked > 150 && j.walked > 150, bwgame::format("both heroes walked in free movement (%.0f px, %.0f px)", h.walked, j.walked).c_str());
		check(h.compared > 200 && j.compared > 200 && h.mispredicted == 0 && j.mispredicted == 0, bwgame::format("the camera's view, ahead of the simulation by the latency, is exactly where the hero ends up (%d and %d frames compared, %d and %d off)", h.compared, j.compared, h.mispredicted, j.mispredicted).c_str());
		Fps& fh = *h.f; Fps& fj = *j.f;
		check(fh.shots > 0 && fj.shots > 0, "both heroes shot while walking (" + std::to_string(fh.shots) + " and " + std::to_string(fj.shots) + " shots)");
		check(fh.kills + fh.deaths > 0 && fh.kills == fj.deaths && fh.deaths == fj.kills, "the heroes fought and both games count the same deaths (" + std::to_string(fh.deaths) + ":" + std::to_string(fh.kills) + " / " + std::to_string(fj.kills) + ":" + std::to_string(fj.deaths) + ")");
	}

	// 5-6. A slow network: 50 ms each way, plus the wait for the other game's next look (the window's 60 Hz).
	const int kPaced = 168; const double sim_dt = 1.0 / 23.81;
	auto slowdown = [&](const Peer& p) { return p.elapsed / ((kPaced - 1) * sim_dt); };
	auto slow_session = [&](Peer& h, Peer& j, int latency, int p0) {
		h.latency = latency; j.latency = 7; h.paced = j.paced = true; j.via = p0 + 1; // the joiner's own --latency is left out
		session(h, j, data, map, p0, kPaced, [](Peer&) {});
		check(h.error.empty() && j.error.empty() && h.g.failure().empty() && j.g.failure().empty(), "both games ran (" + h.error + j.error + h.g.failure() + j.g.failure() + ")");
		int d = first_difference(h, j);
		check((int)h.hashes.size() == kPaced && (int)j.hashes.size() == kPaced && d < 0, "the two games hold the same state on all " + std::to_string(std::min(h.hashes.size(), j.hashes.size())) + " frames");
	};
	std::cout << "session 5: a slow network (100 ms round trip), latency picked from it\n";
	{
		SlowLink link(port + 5, port + 4, 50); // declared first: the games leave through it before it closes
		Peer h{"host", true, race_t::terran}, j{"joiner", false, race_t::zerg};
		slow_session(h, j, 0, port + 4);
		Net& hn = *h.g.network(); double rtt = hn.lobby_rtt_ms();
		check(hn.lobby_samples() >= Net::kPingSamples && rtt >= 99 && rtt < 140, bwgame::format("the host measures the round trip in the lobby: %.1f ms (%d pings)", rtt, hn.lobby_samples()).c_str());
		check(h.g.latency() == pick_latency(rtt) && h.g.latency() == 3 && j.g.latency() == h.g.latency() && j.g.network()->latency_from_host(),
		      bwgame::format("the host picks latency %d from it, the joiner takes it (%d; it asked for 7)", h.g.latency(), j.g.latency()).c_str());
		check(slowdown(h) < 1.08 && slowdown(j) < 1.08, bwgame::format("both games keep up with the clock: %d frames in %.2f s and %.2f s (%.2f s due)", kPaced, h.elapsed, j.elapsed, (kPaced - 1) * sim_dt).c_str());
		check(h.waited < 0.1 * h.elapsed && j.waited < 0.1 * j.elapsed, bwgame::format("and seldom wait for each other: %.2f s and %.2f s with a step due but blocked", h.waited, j.waited).c_str());
		check(h.g.rtt_ms() >= 99 && h.g.rtt_ms() < 170 && j.g.rtt_ms() >= 99 && j.g.rtt_ms() < 170, bwgame::format("the round trip during the game, for the HUD: %.0f ms and %.0f ms", h.g.rtt_ms(), j.g.rtt_ms()).c_str());
	}
	std::cout << "session 6: the same network with latency 2\n";
	{
		SlowLink link(port + 7, port + 6, 50);
		Peer h{"host", true, race_t::terran}, j{"joiner", false, race_t::zerg};
		slow_session(h, j, 2, port + 6);
		check(h.g.latency() == 2 && j.g.latency() == 2, "the host's --latency 2 holds for both games");
		check(slowdown(h) > 1.04 && slowdown(j) > 1.04 && h.waited > 0.1 * h.elapsed && j.waited > 0.1 * j.elapsed, bwgame::format("the games wait on each other and fall behind the clock: %d frames in %.2f s and %.2f s (%.2f s due; waited %.2f s and %.2f s)", kPaced, h.elapsed, j.elapsed, (kPaced - 1) * sim_dt, h.waited, j.waited).c_str());
	}

	std::cout << (failures ? "NETTEST FAILED" : "NETTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
