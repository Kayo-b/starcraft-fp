// Headless two-player test (fp_net.h, Game::open_net, Fps versus mode): two games, one per thread, connected over TCP
// on localhost, each driven by its own scripted commands. Exit code 0 only if every check passes.
//   1. a full game: both games hold the same state on every frame; commands (select, train, attack orders, trigger)
//      take effect on the same frame in both, `latency` frames after they were given; heroes fight, die and come back.
//   2. one game changes its state outside the command stream: both notice that they went out of sync.
//   3. a player leaves: the other game goes on alone and its player wins.
//   4. free movement (--fps-move-shoot): the heroes walk on input commands and shoot while walking; both games agree on
//      every frame, and the camera's view of each hero, run ahead of the simulation, is exactly where the hero ends up.
#include "fp_fps.h"
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <thread>
#include <unistd.h>

using namespace fp;
static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }

struct Peer {
	const char* name; bool host; race_t race;
	Game g; std::unique_ptr<Fps> f;
	std::vector<uint32_t> hashes;     // state hash after every frame
	int mineral_drop = -1;            // frame on which the host's 50 starting minerals were spent (as seen by this game)
	std::string error;
	bool direct = false;              // free movement (session 4)
	// session 4: OpenBW walks the hero to the other one (trigger free), free movement once they are close
	bool free = false; int direct_since = 0, compared = 0, mispredicted = 0; double walked = 0; std::pair<double, double> last_at{-1, -1};
	struct Prediction { double x, y; int id; };
	std::map<int, Prediction> pred[2]; // the camera's view of the hero for a frame, made one and two frames before it
};

// Opens the game in the lobby, then steps `frames` frames, calling script(peer) before each step.
static void run(Peer& p, const std::string& data, const std::string& map, int port, int frames, const std::function<void(Peer&)>& script) {
	try {
		NetOptions o; o.host = p.host; o.port = port; o.latency = 2; o.wait_seconds = 20; o.name = p.name;
		p.g.open_net(data, map, p.race, o, [&](const std::string&) {}, Fps::settings_hash(200, 200, 90, p.direct));
		p.f = std::make_unique<Fps>(p.g, 7, 200, 200, 90, p.direct, true);
		if (!p.f->start()) { p.error = "no hero"; return; }
		auto t0 = std::chrono::steady_clock::now();
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
	std::cout << "session 3: the joiner leaves\n";
	{
		Peer h{"host", true, race_t::protoss}, j{"joiner", false, race_t::zerg};
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

	std::cout << (failures ? "NETTEST FAILED" : "NETTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
