// Headless test of the weapon asset layer (PNG/OGG decoding and the mixer). Needs the mod directory; no window, no audio device.
#include "fp_weapon.h"
#include <iostream>

using namespace fp;
static int failures = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n"; if (!ok) ++failures; }

int main(int argc, char** argv) {
	if (argc < 2) { std::cerr << "usage: fp_assettest MOD_DIR\n"; return 2; }
	std::string mod = argv[1];
	// ---- PNG ----
	std::string p = resolve(mod + "/Patches/BLTRGA0");
	check(!p.empty(), "resolve() finds Patches/BLTRGA0 with or without an extension (" + p.substr(p.rfind('/') + 1) + ")");
	Bitmap b = load_png(p);
	check(b.ok && b.w == 512 && b.h == 256, "PNG decodes to 512x256 RGBA (" + std::to_string(b.w) + "x" + std::to_string(b.h) + ")");
	check(b.ox == 56 && b.oy == 30, "Doom grAb offset read: (" + std::to_string(b.ox) + "," + std::to_string(b.oy) + ")");
	int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
	for (int y = 0; y < b.h; ++y) for (int x = 0; x < b.w; ++x) if (b.rgba[((size_t)y * b.w + x) * 4 + 3] > 8) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1); }
	check(x0 == 161 && y0 == 65 && x1 == 480 && y1 == 256, "alpha bounding box matches an independent decode (161,65)-(480,256): (" + std::to_string(x0) + "," + std::to_string(y0) + ")-(" + std::to_string(x1) + "," + std::to_string(y1) + ")");
	Bitmap fire = load_png(resolve(mod + "/Patches/BLTRFB0"));
	size_t diff = 0; if (fire.ok && fire.rgba.size() == b.rgba.size()) for (size_t i = 0; i < b.rgba.size(); ++i) diff += b.rgba[i] != fire.rgba[i];
	check(fire.ok && diff > 1000, "the lit fire frame differs from the ready frame (" + std::to_string(diff) + " bytes differ)");
	check(!load_png(mod + "/Patches/does-not-exist").ok, "a missing PNG is reported, not a crash");
	// ---- OGG ----
	Pcm f1 = load_ogg(mod + "/Sounds/Weapons/Boltgun/BoltgunFire01.ogg");
	double secs = f1.ok ? (double)f1.frames() / f1.rate : 0;
	{ OggVorbis_File vf; double total = 0; if (ov_fopen((mod + "/Sounds/Weapons/Boltgun/BoltgunFire01.ogg").c_str(), &vf) == 0) { total = ov_time_total(&vf, -1); ov_clear(&vf); }
	  check(f1.ok && std::fabs(secs - total) < 0.01 && secs > 0.05 && secs < 5, "OGG decodes: " + std::to_string(f1.frames()) + " frames at " + std::to_string(f1.rate) + " Hz = " + std::to_string(secs) + " s (libvorbis reports " + std::to_string(total) + " s)"); }
	int16_t peak = 0; for (int16_t v : f1.s) peak = std::max<int16_t>(peak, std::abs(v));
	check(peak > 1000, "the decoded fire sound is not silent (peak " + std::to_string(peak) + ")");
	check(!load_ogg(mod + "/Sounds/nope.ogg").ok, "a missing OGG is reported, not a crash");
	// ---- mixer, with synthetic voices ----
	{ Pcm c; c.rate = 22050; c.ok = true; c.s.assign(2000 * 2, 1000); // 2000 frames of constant 1000 at half the output rate
	  Mixer m; m.play(&c, 0.5f); int16_t out[256 * 2]; m.mix(out, 256, 44100);
	  bool flat = true; for (int i = 0; i < 256 * 2; ++i) flat &= (out[i] == 500);
	  check(flat && m.active() == 1, "mixer applies volume: 1000 x 0.5 = 500 on every sample");
	  int mixed = 256; while (m.active() && mixed < 10000) { m.mix(out, 256, 44100); mixed += 256; }
	  check(mixed >= 3800 && mixed <= 4400, "a 2000-frame voice at 22.05 kHz lasts ~4000 output frames at 44.1 kHz (finished after " + std::to_string(mixed) + ")"); }
	{ Pcm loud; loud.rate = 44100; loud.ok = true; loud.s.assign(100 * 2, 30000); Mixer m; m.play(&loud, 1.0f); m.play(&loud, 1.0f);
	  int16_t out[32 * 2]; m.mix(out, 32, 44100); check(out[0] == 32767 && out[1] == 32767, "two loud voices clip to 32767 instead of wrapping"); }
	{ Mixer m; Pcm bad; m.play(&bad, 1.0f); m.play(nullptr, 1.0f); check(m.active() == 0 && m.started == 0, "invalid or null voices are ignored"); }
	{ Mixer m; m.play(&f1, 1.0f); int16_t out[1024 * 2]; m.mix(out, 1024, 44100); double e = 0; for (int i = 0; i < 2048; ++i) e += std::fabs(out[i]);
	  check(e > 1000, "mixing the real fire sound produces audible output"); }
	std::cout << (failures ? "ASSETTEST FAILED" : "ASSETTEST PASSED") << "\n";
	return failures ? 1 : 0;
}
