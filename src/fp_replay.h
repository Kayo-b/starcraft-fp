// Recordings of a game (.fpr), to watch it again exactly: the setup as the lobby (or single player's setup) left it, then
// every command of both players with the frame it was applied in, and the state hash after every frame (playback stops
// where they differ). OpenBW's own replays cannot hold this game: they keep the standard commands only, not the heroes'
// free movement or shots, and the heroes are not made by commands. Same program version only (Game::kProtocol) and the
// same map file (CRC).
//
// Format, little-endian: "FPR1", u32 protocol, u32 map CRC, str map path, u8 mode (0 two-player, 1 single-player melee,
// 2 single-player FPS waves), u8 owner, u8 enemy, str name[2] (the recording player's, the other's), u32 random seed,
// 12 x player {i32 controller, i32 race, i32 force, i32 color, u8 initially active, i32 victory state}, u8 n, n x i32
// settings (the FPS layer's); then records: 'a' u32 frame, u8 slot, u16 n, n bytes (a command: Game's channel byte, then
// its data); 'h' u32 frame, u32 hash; 'e' u32 frame (the end). str = u16 length, bytes.
#pragma once
#include "bwgame.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace fp {
using namespace bwgame;

struct RecordingSetup {
	uint32_t protocol = 0, map_crc = 0; std::string map; int mode = 0, owner = 0, enemy = 1; std::string names[2];
	uint32_t seed = 0; std::array<player_t, 12> players{}; std::vector<int32_t> settings;
};

class RecordingWriter {
public:
	~RecordingWriter() { close(-1); }
	bool open(const std::string& path, const RecordingSetup& s) {
		f_ = fopen(path.c_str(), "wb"); if (!f_) return false;
		path_ = path; bytes("FPR1", 4); u32(s.protocol); u32(s.map_crc); str(s.map); u8(s.mode); u8(s.owner); u8(s.enemy); str(s.names[0]); str(s.names[1]); u32(s.seed);
		for (auto& p : s.players) { u32((uint32_t)p.controller); u32((uint32_t)p.race); u32((uint32_t)p.force); u32((uint32_t)p.color); u8(p.initially_active); u32((uint32_t)p.victory_state); }
		u8((int)s.settings.size()); for (int32_t v : s.settings) u32((uint32_t)v);
		return true;
	}
	void action(int frame, int slot, const uint8_t* d, size_t n) { if (!f_) return; u8('a'); u32((uint32_t)frame); u8(slot); u16((int)n); bytes(d, n); }
	void hash(int frame, uint32_t h) { if (!f_) return; u8('h'); u32((uint32_t)frame); u32(h); if (frame % 240 == 0) fflush(f_); }
	// frame < 0: the game ended without saying where (the program was closed): the hashes say how far it goes.
	void close(int frame) { if (!f_) return; if (frame >= 0) { u8('e'); u32((uint32_t)frame); } fclose(f_); f_ = nullptr; }
	bool is_open() const { return f_ != nullptr; }
	const std::string& path() const { return path_; }
private:
	void bytes(const void* d, size_t n) { fwrite(d, 1, n, f_); }
	void u8(int v) { uint8_t b = (uint8_t)v; bytes(&b, 1); }
	void u16(int v) { u8(v & 0xff); u8((v >> 8) & 0xff); }
	void u32(uint32_t v) { u16((int)(v & 0xffff)); u16((int)(v >> 16)); }
	void str(const std::string& s) { u16((int)std::min<size_t>(s.size(), 0xffff)); bytes(s.data(), std::min<size_t>(s.size(), 0xffff)); }
	FILE* f_ = nullptr; std::string path_;
};

class Recording {
public:
	struct Record { char kind; int frame, slot; uint32_t hash; std::vector<uint8_t> data; };
	// Throws (bwgame::error) on a file that is not a recording or is cut short in its header; a body cut short ends there.
	void load(const std::string& path) {
		FILE* f = fopen(path.c_str(), "rb"); if (!f) error("cannot open the recording %s", path.c_str());
		std::vector<uint8_t> d; uint8_t buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n); fclose(f);
		size_t p = 0;
		auto need = [&](size_t k) { if (p + k > d.size()) error("the recording %s is cut short", path.c_str()); };
		auto u8 = [&]() { need(1); return (int)d[p++]; };
		auto u16 = [&]() { int a = u8(); return a | u8() << 8; };
		auto u32 = [&]() { uint32_t a = (uint32_t)u16(); return a | (uint32_t)u16() << 16; };
		auto str = [&]() { int k = u16(); need((size_t)k); std::string s((const char*)d.data() + p, (size_t)k); p += (size_t)k; return s; };
		need(4); if (memcmp(d.data(), "FPR1", 4) != 0) error("%s is not a recording of this program", path.c_str()); p = 4;
		s_.protocol = u32(); s_.map_crc = u32(); s_.map = str(); s_.mode = u8(); s_.owner = u8(); s_.enemy = u8(); s_.names[0] = str(); s_.names[1] = str(); s_.seed = u32();
		for (auto& pl : s_.players) { pl.controller = (int)u32(); pl.race = (race_t)u32(); pl.force = (int)u32(); pl.color = (int)u32(); pl.initially_active = u8() != 0; pl.victory_state = (int)u32(); }
		s_.settings.resize((size_t)u8()); for (auto& v : s_.settings) v = (int32_t)u32();
		try {
			while (p < d.size()) {
				Record r{(char)u8(), 0, 0, 0, {}};
				if (r.kind == 'a') { r.frame = (int)u32(); r.slot = u8(); int k = u16(); need((size_t)k); r.data.assign(d.begin() + p, d.begin() + p + k); p += (size_t)k; }
				else if (r.kind == 'h') { r.frame = (int)u32(); r.hash = u32(); }
				else if (r.kind == 'e') { r.frame = (int)u32(); }
				else break;
				records_.push_back(std::move(r));
			}
		} catch (const std::exception&) {} // a game whose program stopped mid-record: what is there plays
		end_frame_ = 0; for (auto& r : records_) end_frame_ = std::max(end_frame_, r.frame);
	}
	const RecordingSetup& setup() const { return s_; }
	const std::vector<Record>& records() const { return records_; }
	int end_frame() const { return end_frame_; } // the last frame the recording knows of
private:
	RecordingSetup s_; std::vector<Record> records_; int end_frame_ = 0;
};

} // namespace fp
