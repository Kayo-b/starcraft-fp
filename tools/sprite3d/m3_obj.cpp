// m3_obj: write an SC2 .m3 model (rest pose, opaque batches, as fp_m3.h reads it) as a Wavefront OBJ plus one PPM per
// texture, for the Python tools (`sc2_structure.py`). Model units, z up, as in the .m3. A batch with a glow (emissive)
// texture gets "# glow tN MULT" after its usemtl.
//   g++ -std=c++17 -O2 -I../../src m3_obj.cpp -o m3_obj
//   m3_obj ASSET_ROOT MODEL.m3 OUT_PREFIX     -> OUT_PREFIX.obj, OUT_PREFIX_tN.ppm (N = texture index)
#include "fp_m3.h"
#include <cstdio>
#include <iostream>
using namespace fp;
int main(int argc, char** argv) {
	if (argc != 4) { std::cerr << "usage: m3_obj ASSET_ROOT MODEL.m3 OUT_PREFIX\n"; return 2; }
	AssetIndex assets(argv[1]); M3Model m; const std::filesystem::path* p = assets.find(argv[2]);
	if (!p || !m.load(*p, assets, 2048)) { std::cerr << argv[2] << ": " << (p ? m.error : "not found") << "\n"; return 1; }
	const std::string out = argv[3];
	FILE* f = std::fopen((out + ".obj").c_str(), "w");
	for (auto& v : m.verts) std::fprintf(f, "v %.6f %.6f %.6f\n", v.x, v.y, v.z);
	for (auto& v : m.verts) std::fprintf(f, "vt %.6f %.6f\n", v.u, v.v); // v counts down the image, as the renderer samples it
	for (auto& b : m.batches) {
		std::fprintf(f, "usemtl t%d\n", b.tex);
		if (b.emis >= 0) std::fprintf(f, "# glow t%d %.3f\n", b.emis, b.emis_mult); // added on top, as fp_render.h does
		for (uint32_t k = b.first; k < b.first + b.count; k += 3)
			std::fprintf(f, "f %u/%u %u/%u %u/%u\n", m.idx[k] + 1, m.idx[k] + 1, m.idx[k + 1] + 1, m.idx[k + 1] + 1, m.idx[k + 2] + 1, m.idx[k + 2] + 1);
	}
	std::fclose(f);
	for (size_t t = 0; t < m.tex.size(); ++t) {
		const M3Texture& T = m.tex[t];
		FILE* g = std::fopen((out + "_t" + std::to_string(t) + ".ppm").c_str(), "wb");
		std::fprintf(g, "P6\n%d %d\n255\n", T.w, T.h);
		for (uint32_t c : T.px) { uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; std::fwrite(rgb, 1, 3, g); }
		std::fclose(g);
	}
	std::printf("%s: %zu verts, %zu tris, %zu textures\n", argv[2], m.verts.size(), m.triangles(), m.tex.size());
	return 0;
}
