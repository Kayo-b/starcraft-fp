// Asset readers for Remastered HD data extracted from the user's own CASC install. See README for the verified layouts.
#pragma once
#include "bwgame.h"
#include "dir_loader.h"
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace fp {
using bwgame::a_vector;

// DXT1 (BC1) texel at (x,y) of a width-w texture whose block data starts at `blocks`.
static inline void dxt1_texel(const uint8_t* blocks, int w, int x, int y, uint8_t out[3]) {
	const uint8_t* blk = blocks + ((size_t)(y / 4) * (w / 4) + (x / 4)) * 8;
	uint16_t c0 = blk[0] | (blk[1] << 8), c1 = blk[2] | (blk[3] << 8);
	uint32_t bits = blk[4] | (blk[5] << 8) | (blk[6] << 16) | ((uint32_t)blk[7] << 24);
	auto rgb = [](uint16_t c, int& r, int& g, int& b) { r = ((c >> 11) & 31) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31; };
	int r0, g0, b0, r1, g1, b1; rgb(c0, r0, g0, b0); rgb(c1, r1, g1, b1);
	int idx = (bits >> (2 * ((y & 3) * 4 + (x & 3)))) & 3, r, g, b;
	if (c0 > c1) {
		if (idx == 0) { r = r0; g = g0; b = b0; } else if (idx == 1) { r = r1; g = g1; b = b1; }
		else if (idx == 2) { r = (2 * r0 + r1) / 3; g = (2 * g0 + g1) / 3; b = (2 * b0 + b1) / 3; }
		else { r = (r0 + 2 * r1) / 3; g = (g0 + 2 * g1) / 3; b = (b0 + 2 * b1) / 3; }
	} else {
		if (idx == 0) { r = r0; g = g0; b = b0; } else if (idx == 1) { r = r1; g = g1; b = b1; }
		else if (idx == 2) { r = (r0 + r1) / 2; g = (g0 + g1) / 2; b = (b0 + b1) / 2; }
		else { r = g = b = 0; }
	}
	out[0] = r; out[1] = g; out[2] = b;
}

// ---- HD terrain tiles: <tileset>.dds.vr4 = container of one 128x128 DXT1 DDS per megatile ----
// Layout verified on real files: u16 frame count at offset 4; frames from offset 8, each
// [u16 ?, u16 ?, u16 w, u16 h, u32 size, DDS(size bytes)]; DDS = 128-byte header + DXT1 blocks.
struct HdTiles {
	std::vector<uint8_t> raw;
	std::vector<size_t> dds_off; // offset of each DDS (header start)
	bool ok = false;
	bool load(const std::vector<uint8_t>& data) {
		raw = data;
		if (raw.size() < 8) return false;
		unsigned n = raw[4] | (raw[5] << 8);
		size_t pos = 8;
		for (unsigned i = 0; i < n; ++i) {
			if (pos + 12 > raw.size()) return false;
			unsigned w = raw[pos + 4] | (raw[pos + 5] << 8), h = raw[pos + 6] | (raw[pos + 7] << 8);
			uint32_t size = raw[pos + 8] | (raw[pos + 9] << 8) | (raw[pos + 10] << 16) | ((uint32_t)raw[pos + 11] << 24);
			if (w != 128 || h != 128 || size != 8320 || pos + 12 + size > raw.size()) return false;
			dds_off.push_back(pos + 12);
			pos += 12 + size;
		}
		ok = pos == raw.size();
		return ok;
	}
	static void rgb565(uint16_t c, int& r, int& g, int& b) {
		r = ((c >> 11) & 31) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31;
	}
	// texel (x,y) in [0,128) of megatile `mt`
	void sample(size_t mt, int x, int y, uint8_t out[3]) const {
		if (mt >= dds_off.size()) { out[0] = out[1] = out[2] = 255; return; }
		dxt1_texel(&raw[dds_off[mt] + 128], 128, x & 127, y & 127, out);
	}
};

// ---- HD sprites: anim/main_NNN.anim (NNN = image id). Layout verified on real files (see README). ----
// Header: "ANIM", u16 version@4, u16 layers@8, u16 entries@10, 10 x 32-byte layer names @12.
// HD files have one sprite record at 0x14c: u16 frames@0, u16 w@4, u16 h@6, u32 frame_table@8, then per layer
// {u32 dds_offset, u32 dds_size, u16 tex_w, u16 tex_h}. Frame record (16 B): u16 x, u16 y, i16 xoff, i16 yoff, u16 w, u16 h.
// Layer 0 ("diffuse") is DXT5 (128-byte DDS header + blocks).
struct AnimSprite {
	struct Frame { int x, y, xoff, yoff, w, h; };
	std::vector<uint8_t> raw; std::vector<Frame> frames;
	size_t diffuse = 0, teamcolor_off = 0; int tex_w = 0, tex_h = 0, box_w = 0, box_h = 0; bool ok = false;
	static unsigned u16(const std::vector<uint8_t>& b, size_t o) { return b[o] | (b[o + 1] << 8); }
	static uint32_t u32(const std::vector<uint8_t>& b, size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); }
	bool load(std::vector<uint8_t> data) {
		raw = std::move(data);
		const size_t H = 0x14c;
		if (raw.size() < H + 12 + 12 || memcmp(raw.data(), "ANIM", 4) != 0) return false;
		unsigned layers = u16(raw, 8), entries = u16(raw, 10);
		if (entries != 1 || layers < 1 || layers > 10) return false;
		unsigned nf = u16(raw, H); box_w = u16(raw, H + 4); box_h = u16(raw, H + 6); uint32_t ft = u32(raw, H + 8);
		if (nf == 0 || (size_t)ft + (size_t)nf * 16 > raw.size()) return false;
		size_t dds = u32(raw, H + 12), dsz = u32(raw, H + 16);
		tex_w = u16(raw, H + 20); tex_h = u16(raw, H + 22);
		if (dds + dsz > raw.size() || dsz != 128 + (size_t)tex_w * tex_h || memcmp(&raw[dds], "DDS ", 4) != 0 || memcmp(&raw[dds + 84], "DXT5", 4) != 0) return false;
		diffuse = dds + 128;
		// Optional "teamcolor" layer: a DXT1 mask over the same atlas; its red channel says how much player colour to apply.
		for (unsigned L = 1; L < layers; ++L) {
			if (strncmp((const char*)&raw[12 + L * 32], "teamcolor", 9) != 0) continue;
			size_t o = H + 12 + (size_t)L * 12; if (o + 12 > raw.size()) break;
			size_t toff = u32(raw, o), tsz = u32(raw, o + 4);
			if (u16(raw, o + 8) == (unsigned)tex_w && u16(raw, o + 10) == (unsigned)tex_h && toff + tsz <= raw.size() && tsz == 128 + (size_t)tex_w * tex_h / 2 && memcmp(&raw[toff], "DDS ", 4) == 0 && memcmp(&raw[toff + 84], "DXT1", 4) == 0)
				teamcolor_off = toff + 128;
		}
		for (unsigned i = 0; i < nf; ++i) { size_t o = ft + i * 16;
			frames.push_back({(int)u16(raw, o), (int)u16(raw, o + 2), (int16_t)u16(raw, o + 4), (int16_t)u16(raw, o + 6), (int)u16(raw, o + 8), (int)u16(raw, o + 10)}); }
		return ok = true;
	}
	bool has_teamcolor() const { return teamcolor_off != 0; }
	// Player-colour mask strength 0..255 at atlas texel (x,y) (0 when the sprite has no teamcolor layer).
	int team_mask(int x, int y) const {
		if (!teamcolor_off || x < 0 || y < 0 || x >= tex_w || y >= tex_h) return 0;
		uint8_t t[3]; dxt1_texel(&raw[teamcolor_off], tex_w, x, y, t); return t[0];
	}
	// RGBA of texel (x,y) in the diffuse DXT5 atlas
	void texel(int x, int y, uint8_t out[4]) const {
		if (x < 0 || y < 0 || x >= tex_w || y >= tex_h) { out[0] = out[1] = out[2] = out[3] = 0; return; }
		const uint8_t* blk = &raw[diffuse + ((size_t)(y / 4) * (tex_w / 4) + (x / 4)) * 16];
		int a0 = blk[0], a1 = blk[1];
		uint64_t ab = 0; for (int i = 0; i < 6; ++i) ab |= (uint64_t)blk[2 + i] << (8 * i);
		int ai = (ab >> (3 * ((y & 3) * 4 + (x & 3)))) & 7, a;
		if (ai == 0) a = a0; else if (ai == 1) a = a1;
		else if (a0 > a1) a = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
		else if (ai == 6) a = 0; else if (ai == 7) a = 255; else a = ((6 - ai) * a0 + (ai - 1) * a1) / 5;
		uint16_t c0 = blk[8] | (blk[9] << 8), c1 = blk[10] | (blk[11] << 8);
		uint32_t bits = blk[12] | (blk[13] << 8) | (blk[14] << 16) | ((uint32_t)blk[15] << 24);
		int r0, g0, b0, r1, g1, b1; HdTiles::rgb565(c0, r0, g0, b0); HdTiles::rgb565(c1, r1, g1, b1);
		int ci = (bits >> (2 * ((y & 3) * 4 + (x & 3)))) & 3, r, g, b; // DXT5 colour block is always 4-colour mode
		if (ci == 0) { r = r0; g = g0; b = b0; } else if (ci == 1) { r = r1; g = g1; b = b1; }
		else if (ci == 2) { r = (2 * r0 + r1) / 3; g = (2 * g0 + g1) / 3; b = (2 * b0 + b1) / 3; }
		else { r = (r0 + 2 * r1) / 3; g = (g0 + 2 * g1) / 3; b = (b0 + 2 * b1) / 3; }
		out[0] = r; out[1] = g; out[2] = b; out[3] = a;
	}
};

// Unit names from rez/stat_txt.tbl (verified: 0-based entry == unit type id, e.g. 7 = "Terran SCV").
struct NameTable {
	std::vector<uint8_t> raw; std::vector<uint16_t> offs;
	void load(std::vector<uint8_t> data) {
		raw = std::move(data); offs.clear();
		if (raw.size() < 2) return;
		size_t n = raw[0] | (raw[1] << 8);
		if (2 + n * 2 > raw.size()) return;
		for (size_t i = 0; i < n; ++i) offs.push_back(raw[2 + i * 2] | (raw[3 + i * 2] << 8));
	}
	std::string unit(int id) const {
		if (id < 0 || (size_t)id >= offs.size() || offs[id] >= raw.size()) return "Unit " + std::to_string(id);
		std::string s; for (size_t o = offs[id]; o < raw.size() && raw[o] >= 0x20; ++o) s += (char)raw[o];
		return s.empty() ? "Unit " + std::to_string(id) : s;
	}
};

// Lazily loads and caches assets by name from the extracted-data directory.
class Assets {
public:
	explicit Assets(const std::string& data_dir) : files_(data_dir) {}
	const HdTiles* tiles(size_t tileset_index) {
		static const char* names[8] = {"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"};
		auto it = tiles_.find(tileset_index);
		if (it == tiles_.end()) {
			auto t = std::make_unique<HdTiles>();
			a_vector<uint8_t> buf; files_(buf, bwgame::format("TileSet/%s.dds.vr4", names[tileset_index % 8]));
			if (!t->load(std::vector<uint8_t>(buf.begin(), buf.end()))) bwgame::error("HD tile file did not match the verified layout");
			it = tiles_.emplace(tileset_index, std::move(t)).first;
		}
		return it->second.get();
	}
	// nullptr when the file is missing or does not match the verified layout.
	const AnimSprite* sprite(int image_id) {
		auto it = sprites_.find(image_id);
		if (it == sprites_.end()) {
			AnimSprite sp;
			try { a_vector<uint8_t> buf; files_(buf, bwgame::format("anim/main_%03d.anim", image_id)); sp.load(std::vector<uint8_t>(buf.begin(), buf.end())); } catch (const std::exception&) {}
			if (!sp.ok) missing_.insert(image_id);
			it = sprites_.emplace(image_id, std::move(sp)).first;
		}
		return it->second.ok ? &it->second : nullptr;
	}
	// Lookup only (never loads), so it is safe to call from render threads after update_units() preloaded the sprites.
	const AnimSprite* sprite_cached(int image_id) const {
		auto it = sprites_.find(image_id);
		return (it != sprites_.end() && it->second.ok) ? &it->second : nullptr;
	}
	// Unit names; falls back to "Unit N" when rez/stat_txt.tbl is not in the data directory.
	const NameTable& names() {
		if (!names_loaded_) { names_loaded_ = true; try { a_vector<uint8_t> buf; files_(buf, "rez/stat_txt.tbl"); names_.load(std::vector<uint8_t>(buf.begin(), buf.end())); } catch (const std::exception&) {} }
		return names_;
	}
	dir_loader& files() { return files_; }
private:
	dir_loader files_;
	std::map<size_t, std::unique_ptr<HdTiles>> tiles_;
	std::map<int, AnimSprite> sprites_;
	NameTable names_; bool names_loaded_ = false; std::set<int> missing_;
public:
	// Image ids whose anim/main_NNN.anim was missing or unreadable (those units draw as flat player-coloured boxes).
	const std::set<int>& missing_images() const { return missing_; }
};

} // namespace fp
