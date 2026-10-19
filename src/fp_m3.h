// StarCraft II .m3 models (MD34) for the first-person renderer: meshes in their rest pose with the diffuse textures of
// their opaque materials. Struct layouts from m3addon's structures.xml (github.com/SC2Mapster/m3addon); the model files
// come from a local SC2 mod (SC Evo Complete) and are never committed. Read-only.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fp {

// A texture decoded once at load (0xAARRGGBB), at the first mip level no larger than the requested size.
struct M3Texture {
	int w = 0, h = 0; bool has_alpha = false; std::vector<uint32_t> px;
	uint32_t sample(double u, double v) const {
		int x = (int)std::floor(u * w) % w, y = (int)std::floor(v * h) % h;
		if (x < 0) x += w; if (y < 0) y += h;
		return px[(size_t)y * w + x];
	}
};

namespace dds {
inline void rgb565(uint16_t c, int& r, int& g, int& b) { r = (c >> 11) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31; }
// One 4x4 colour block (BC1 layout); `dxt1` enables the 3-colour + transparent mode.
inline void color_block(const uint8_t* b, uint32_t out[16], bool dxt1) {
	uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
	int r[4], g[4], bl[4], a[4] = {255, 255, 255, 255};
	rgb565(c0, r[0], g[0], bl[0]); rgb565(c1, r[1], g[1], bl[1]);
	if (c0 > c1 || !dxt1) {
		r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; bl[2] = (2 * bl[0] + bl[1]) / 3;
		r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; bl[3] = (bl[0] + 2 * bl[1]) / 3;
	} else {
		r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; bl[2] = (bl[0] + bl[1]) / 2;
		r[3] = g[3] = bl[3] = 0; a[3] = 0;
	}
	uint32_t bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
	for (int i = 0; i < 16; ++i) { int k = (bits >> (2 * i)) & 3; out[i] = (uint32_t)a[k] << 24 | (uint32_t)r[k] << 16 | (uint32_t)g[k] << 8 | (uint32_t)bl[k]; }
}
inline void alpha_block_dxt5(const uint8_t* b, uint8_t out[16]) {
	int a[8]; a[0] = b[0]; a[1] = b[1];
	if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
	else { for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
	uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2 + i] << (8 * i);
	for (int i = 0; i < 16; ++i) out[i] = (uint8_t)a[(bits >> (3 * i)) & 7];
}
// DXT1/DXT3/DXT5 and uncompressed 32-bit DDS; false for anything else.
inline bool load(const std::vector<uint8_t>& f, int max_size, M3Texture& t) {
	if (f.size() < 128 || std::memcmp(f.data(), "DDS ", 4) != 0) return false;
	auto u32 = [&](size_t o) { return (uint32_t)f[o] | (uint32_t)f[o + 1] << 8 | (uint32_t)f[o + 2] << 16 | (uint32_t)f[o + 3] << 24; };
	int h = (int)u32(12), w = (int)u32(16), mips = (int)std::max(1u, u32(28));
	uint32_t pf = u32(80), four = u32(84), bits = u32(88);
	int kind = 0;
	if (pf & 4) { if (four == 0x31545844u) kind = 1; else if (four == 0x33545844u) kind = 3; else if (four == 0x35545844u) kind = 5; }
	else if ((pf & 0x40) && bits == 32) kind = 32;
	if (!kind || w <= 0 || h <= 0) return false;
	auto level_size = [&](int lw, int lh) -> size_t {
		if (kind == 32) return (size_t)lw * lh * 4;
		return (size_t)std::max(1, (lw + 3) / 4) * std::max(1, (lh + 3) / 4) * (kind == 1 ? 8 : 16);
	};
	size_t off = 128; int lw = w, lh = h;
	for (int level = 0; level + 1 < mips && std::max(lw, lh) > max_size; ++level) { off += level_size(lw, lh); lw = std::max(1, lw / 2); lh = std::max(1, lh / 2); }
	if (off + level_size(lw, lh) > f.size()) return false;
	t.w = lw; t.h = lh; t.has_alpha = kind != 1; t.px.assign((size_t)lw * lh, 0xFF000000u);
	if (kind == 32) {
		uint32_t m[4] = {u32(92), u32(96), u32(100), u32(104)}; int sh[4];
		for (int k = 0; k < 4; ++k) { sh[k] = 0; if (m[k]) while (!((m[k] >> sh[k]) & 1)) ++sh[k]; }
		t.has_alpha = m[3] != 0;
		for (size_t i = 0; i < (size_t)lw * lh; ++i) {
			uint32_t p = u32(off + i * 4), c[4];
			for (int k = 0; k < 4; ++k) c[k] = m[k] ? ((p & m[k]) >> sh[k]) * 255 / (m[k] >> sh[k]) : 255;
			t.px[i] = c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
		}
		return true;
	}
	const int bw = std::max(1, (lw + 3) / 4), bh = std::max(1, (lh + 3) / 4), bs = kind == 1 ? 8 : 16;
	for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
		const uint8_t* b = &f[off + ((size_t)by * bw + bx) * bs];
		uint32_t c[16]; uint8_t a[16];
		color_block(kind == 1 ? b : b + 8, c, kind == 1);
		if (kind == 5) alpha_block_dxt5(b, a);
		else if (kind == 3) for (int i = 0; i < 16; ++i) a[i] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
		for (int i = 0; i < 16; ++i) {
			int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2); if (x >= lw || y >= lh) continue;
			uint32_t p = c[i]; if (kind != 1) p = (p & 0x00FFFFFFu) | (uint32_t)a[i] << 24;
			t.px[(size_t)y * lw + x] = p;
		}
	}
	return true;
}
} // namespace dds

// Case-insensitive lookup of files under a mod's asset root ("Assets/Textures/X.dds" -> the file on disk).
class AssetIndex {
public:
	explicit AssetIndex(const std::filesystem::path& root) : root_(root) {
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec)) return;
		for (auto& e : std::filesystem::recursive_directory_iterator(root, ec)) if (e.is_regular_file()) idx_[norm(std::filesystem::relative(e.path(), root).generic_string())] = e.path();
	}
	bool empty() const { return idx_.empty(); }
	const std::filesystem::path* find(const std::string& rel) const { auto it = idx_.find(norm(rel)); return it == idx_.end() ? nullptr : &it->second; }
	static std::string norm(std::string s) { for (auto& c : s) { c = (char)std::tolower((unsigned char)c); if (c == '\\') c = '/'; } return s; }
private:
	std::filesystem::path root_; std::map<std::string, std::filesystem::path> idx_;
};

struct M3Model {
	struct Vertex { float x, y, z, nx, ny, nz, u, v; };
	struct Batch { uint32_t first = 0, count = 0; int tex = -1, emis = -1; float emis_mult = 1; bool two_sided = false, alpha_test = false; }; // emis: glow texture added on top
	std::vector<Vertex> verts; std::vector<uint32_t> idx; std::vector<Batch> batches; std::vector<M3Texture> tex;
	float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}; // bounds of the drawn (opaque) geometry, model units
	int skipped = 0;                            // batches not drawn (non-opaque materials: glows, effects)
	bool ok = false; std::string error;

	bool load(const std::filesystem::path& file, const AssetIndex& assets, int max_tex = 512) {
		std::ifstream in(file, std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		auto fail = [&](const std::string& e) { error = e; return false; };
		if (d.size() < 24 || std::memcmp(d.data(), "43DM", 4) != 0) return fail("not an MD34 file");
		auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= d.size() ? (uint32_t)(d[o] | d[o + 1] << 8) : 0; };
		auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? ((uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24) : 0; };
		auto f32 = [&](size_t o) { uint32_t v = u32(o); float f; std::memcpy(&f, &v, 4); return f; };
		struct Entry { char tag[5]; uint32_t off, reps, ver; };
		uint32_t ioff = u32(4), icnt = u32(8);
		if ((size_t)ioff + (size_t)icnt * 16 > d.size()) return fail("bad index");
		std::vector<Entry> E(icnt);
		for (uint32_t i = 0; i < icnt; ++i) { size_t o = ioff + (size_t)i * 16; for (int k = 0; k < 4; ++k) E[i].tag[k] = (char)d[o + 3 - k]; E[i].tag[4] = 0; E[i].off = u32(o + 4); E[i].reps = u32(o + 8); E[i].ver = u32(o + 12); }
		auto ref = [&](size_t o, const char* tag) -> const Entry* { // a Reference (entries, index, flags) at o
			uint32_t n = u32(o), i = u32(o + 4);
			if (!n || i >= E.size() || std::strcmp(E[i].tag, tag) != 0) return nullptr;
			return &E[i];
		};
		auto text = [&](size_t o) -> std::string { const Entry* c = ref(o, "CHAR"); if (!c || c->off + c->reps > d.size()) return {}; return std::string((const char*)&d[c->off], strnlen((const char*)&d[c->off], c->reps)); };
		const Entry* M = ref(12, "MODL");
		if (!M || M->ver < 23) return fail("unsupported MODL (need v23+)");
		const size_t m = M->off;
		const uint32_t vflags = u32(m + 96);
		const Entry* V = ref(m + 100, "U8__"); const Entry* DIV = ref(m + 112, "DIV_");
		const Entry* MATM = ref(m + 300, "MATM"); const Entry* MAT = ref(m + 312, "MAT_"); const Entry* CMP = ref(m + 336, "CMP_");
		if (!V || !DIV) return fail("no vertices or divisions");
		const size_t dv = DIV->off;
		const Entry* F = ref(dv, "U16_"); const Entry* R = ref(dv + 12, "REGN"); const Entry* B = ref(dv + 24, "BAT_");
		if (!F || !R || !B) return fail("division without faces, regions or batches");
		const size_t rsize = R->ver >= 5 ? 48 : R->ver == 4 ? 40 : 36;
		if (R->ver < 3) return fail("REGN v2 not supported");
		uint32_t nverts = 0;
		for (uint32_t i = 0; i < R->reps; ++i) nverts = std::max(nverts, u32(R->off + i * rsize + 8) + u32(R->off + i * rsize + 12));
		if (!nverts || V->reps % nverts != 0) return fail("cannot size the vertices");
		const size_t vs = V->reps / nverts, uvo = 24 + ((vflags & 0x200) ? 4 : 0);
		if (vs < 32 || uvo + 4 > vs) return fail("unexpected vertex size");
		verts.resize(nverts);
		for (uint32_t i = 0; i < nverts; ++i) {
			size_t o = V->off + i * vs; Vertex& v = verts[i];
			v.x = f32(o); v.y = f32(o + 4); v.z = f32(o + 8);
			v.nx = d[o + 20] / 255.f * 2 - 1; v.ny = d[o + 21] / 255.f * 2 - 1; v.nz = d[o + 22] / 255.f * 2 - 1;
			v.u = (int16_t)u16(o + uvo) / 2048.f; v.v = (int16_t)u16(o + uvo + 2) / 2048.f;
		}
		const size_t msize = !MAT ? 0 : MAT->ver >= 20 ? 352 : MAT->ver == 19 ? 340 : MAT->ver >= 16 ? 280 : 268;
		std::map<std::string, int> tex_of;
		std::vector<bool> uv_done(R->reps, false); // a region's UVs are scaled once, even if two batches draw it
		bool any = false;
		for (uint32_t bi = 0; bi < B->reps; ++bi) {
			size_t bo = B->off + bi * 14; uint32_t ri = u16(bo + 4), mi = u16(bo + 10);
			if (ri >= R->reps) continue;
			size_t ro = R->off + ri * rsize;
			uint32_t fv = u32(ro + 8), nv = u32(ro + 12), ff = u32(ro + 16), nf = u32(ro + 20);
			float mult = R->ver >= 5 ? f32(ro + 40) : 16.f, uoff = R->ver >= 5 ? f32(ro + 44) : 0.f;
			// material: only opaque standard materials are drawn (glows and effects are additive or blended)
			Batch b; std::string image, glow; bool opaque = false;
			// a composite material (type 3) layers others: draw it as its first opaque standard section
			if (MATM && CMP && mi < MATM->reps && u32(MATM->off + mi * 8) == 3) {
				uint32_t ci = u32(MATM->off + mi * 8 + 4);
				const Entry* S = ci < CMP->reps ? ref(CMP->off + ci * 28 + 16, "CMS_") : nullptr;
				for (uint32_t k = 0; S && k < S->reps; ++k) {
					uint32_t r2 = u32(S->off + k * 24);
					if (r2 < MATM->reps && u32(MATM->off + r2 * 8) == 1 && MAT && u32(MATM->off + r2 * 8 + 4) < MAT->reps) {
						size_t mo = MAT->off + u32(MATM->off + r2 * 8 + 4) * (MAT->ver >= 20 ? 352 : MAT->ver == 19 ? 340 : MAT->ver >= 16 ? 280 : 268);
						if (u32(mo + 20) == 0) { mi = r2; break; }
					}
				}
			}
			if (MATM && MAT && mi < MATM->reps && u32(MATM->off + mi * 8) == 1) {
				uint32_t si = u32(MATM->off + mi * 8 + 4);
				if (si < MAT->reps) {
					size_t mo = MAT->off + si * msize;
					opaque = u32(mo + 20) == 0;
					b.two_sided = (u32(mo + 16) & 0x8) != 0; b.alpha_test = d[mo + 40] > 0;
					const size_t layers = mo + (MAT->ver >= 20 ? 64 : 52); // diffuse, decal, specular, [gloss], emissive...
					if (const Entry* L = ref(layers, "LAYR")) image = text(L->off + 4);
					if (const Entry* L = ref(layers + (MAT->ver >= 16 ? 48 : 36), "LAYR")) glow = text(L->off + 4);
					b.emis_mult = f32(mo + 48);
				}
			}
			if (!opaque) { ++skipped; continue; }
			auto texture = [&](const std::string& path) -> int { // loaded once per path; -1 when missing (e.g. a base-game SC2 file)
				if (path.empty()) return -1;
				auto it = tex_of.find(path);
				if (it == tex_of.end()) {
					int id = -1; M3Texture t;
					if (const std::filesystem::path* p = assets.find(path)) {
						std::ifstream ti(*p, std::ios::binary); std::vector<uint8_t> td((std::istreambuf_iterator<char>(ti)), std::istreambuf_iterator<char>());
						if (dds::load(td, max_tex, t)) { id = (int)tex.size(); tex.push_back(std::move(t)); }
					}
					it = tex_of.emplace(path, id).first;
				}
				return it->second;
			};
			b.tex = texture(image); b.emis = texture(glow);
			if (!uv_done[ri]) { uv_done[ri] = true; for (uint32_t k = fv; k < fv + nv && k < nverts; ++k) { verts[k].u = verts[k].u * (mult / 16.f) + uoff; verts[k].v = verts[k].v * (mult / 16.f) + uoff; } }
			b.first = (uint32_t)idx.size();
			for (uint32_t k = 0; k + 2 < nf; k += 3) {
				uint32_t a = fv + u16(F->off + (ff + k) * 2), c = fv + u16(F->off + (ff + k + 1) * 2), e = fv + u16(F->off + (ff + k + 2) * 2);
				if (a >= nverts || c >= nverts || e >= nverts) continue;
				idx.push_back(a); idx.push_back(c); idx.push_back(e);
				for (uint32_t q : {a, c, e}) {
					const Vertex& v = verts[q]; float p[3] = {v.x, v.y, v.z};
					for (int ax = 0; ax < 3; ++ax) { if (!any || p[ax] < lo[ax]) lo[ax] = p[ax]; if (!any || p[ax] > hi[ax]) hi[ax] = p[ax]; }
					any = true;
				}
			}
			b.count = (uint32_t)idx.size() - b.first;
			if (b.count) batches.push_back(b);
		}
		if (batches.empty()) return fail("no opaque geometry");
		return ok = true;
	}
	size_t triangles() const { return idx.size() / 3; }
};

} // namespace fp
