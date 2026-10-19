// m3_probe: load SC2 .m3 models with fp_m3.h and print what the renderer would draw. Read-only.
//   m3_probe ASSET_ROOT MODEL.m3...   (ASSET_ROOT: the mod's Base.SC2Assets directory; model paths relative to it)
#include "fp_m3.h"
#include <cstdio>
#include <iostream>
using namespace fp;
int main(int argc, char** argv) {
	if (argc < 3) { std::cerr << "usage: m3_probe ASSET_ROOT MODEL.m3...\n"; return 2; }
	AssetIndex assets(argv[1]); int bad = 0;
	for (int i = 2; i < argc; ++i) {
		M3Model m; const std::filesystem::path* p = assets.find(argv[i]);
		if (!p || !m.load(*p, assets)) { std::printf("%-60s FAILED: %s\n", argv[i], p ? m.error.c_str() : "not found"); ++bad; continue; }
		int textured = 0; for (auto& b : m.batches) textured += b.tex >= 0;
		std::printf("%-60s %6zu tris, %zu verts, %zu batches (%d textured, %d skipped), %zu textures; x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", argv[i], m.triangles(), m.verts.size(),
			m.batches.size(), textured, m.skipped, m.tex.size(), m.lo[0], m.hi[0], m.lo[1], m.hi[1], m.lo[2], m.hi[2]);
	}
	return bad ? 1 : 0;
}
