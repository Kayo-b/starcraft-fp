// Weapon view-model and sound for the first-person view, loaded from an external mod directory.
//
// The art/sounds come from the user's local copy of a third-party Doom mod ("Boltgun Doom"); nothing from it is stored in this
// repository and its licence terms are not known to this project (see README). Everything here degrades gracefully: with no
// directory, a missing file or no audio device, the game still runs and just has no gun / no sound.
#pragma once
#include <SDL2/SDL.h>
#include <png.h>
#include <vorbis/vorbisfile.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fp {

// ---------- decoding ----------
struct Bitmap { int w = 0, h = 0, ox = 0, oy = 0; std::vector<uint8_t> rgba; bool ok = false; }; // ox/oy: Doom "grAb" sprite offset

inline bool file_exists(const std::string& p) { struct stat s; return stat(p.c_str(), &s) == 0 && S_ISREG(s.st_mode); }
// The mod stores some PNGs with a .png extension and some without; accept either.
inline std::string resolve(const std::string& base) {
	for (const char* ext : {"", ".png", ".ogg"}) if (file_exists(base + ext)) return base + ext;
	return "";
}

inline Bitmap load_png(const std::string& path) {
	Bitmap b; png_image img; memset(&img, 0, sizeof img); img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, path.c_str())) return b;
	img.format = PNG_FORMAT_RGBA; b.rgba.resize(PNG_IMAGE_SIZE(img));
	if (!png_image_finish_read(&img, nullptr, b.rgba.data(), 0, nullptr)) { png_image_free(&img); b.rgba.clear(); return b; }
	b.w = (int)img.width; b.h = (int)img.height; b.ok = true;
	std::ifstream f(path, std::ios::binary); std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
	for (size_t i = 8; i + 12 <= d.size();) { // chunk scan for grAb (two big-endian int32: x, y)
		uint32_t n = (d[i] << 24) | (d[i + 1] << 16) | (d[i + 2] << 8) | d[i + 3];
		if (!memcmp(&d[i + 4], "grAb", 4) && n >= 8 && i + 8 + 8 <= d.size()) {
			b.ox = (int32_t)((d[i + 8] << 24) | (d[i + 9] << 16) | (d[i + 10] << 8) | d[i + 11]);
			b.oy = (int32_t)((d[i + 12] << 24) | (d[i + 13] << 16) | (d[i + 14] << 8) | d[i + 15]); break; }
		i += 12 + n;
	}
	return b;
}

struct Pcm { std::vector<int16_t> s; int rate = 44100; bool ok = false; size_t frames() const { return s.size() / 2; } }; // interleaved stereo

inline Pcm load_ogg(const std::string& path) {
	Pcm p; OggVorbis_File vf; if (ov_fopen(path.c_str(), &vf) != 0) return p;
	vorbis_info* vi = ov_info(&vf, -1); if (!vi) { ov_clear(&vf); return p; }
	const int ch = vi->channels; p.rate = (int)vi->rate;
	std::vector<int16_t> raw; char buf[8192]; int bs = 0;
	for (;;) { long n = ov_read(&vf, buf, sizeof buf, 0, 2, 1, &bs); if (n <= 0) break; raw.insert(raw.end(), (int16_t*)buf, (int16_t*)(buf + n)); }
	ov_clear(&vf);
	for (size_t i = 0; i + ch <= raw.size(); i += ch) { int16_t l = raw[i], r = ch > 1 ? raw[i + 1] : raw[i]; p.s.push_back(l); p.s.push_back(r); }
	p.ok = !p.s.empty();
	return p;
}

// ---------- mixing (device independent, so it can be tested without audio hardware) ----------
class Mixer {
public:
	struct Voice { const Pcm* pcm; double pos; float vol; };
	int started = 0; // total voices started (for tests)
	void play(const Pcm* p, float vol) { if (!p || !p->ok) return; if (voices_.size() >= 32) voices_.erase(voices_.begin()); voices_.push_back({p, 0.0, vol}); ++started; }
	int active() const { return (int)voices_.size(); }
	// Mix `frames` stereo frames at out_rate into out (overwrites). Linear resampling; finished voices are dropped.
	void mix(int16_t* out, int frames, int out_rate) {
		std::vector<int32_t> acc((size_t)frames * 2, 0);
		for (auto& v : voices_) {
			const double step = (double)v.pcm->rate / out_rate; const size_t n = v.pcm->frames();
			for (int i = 0; i < frames; ++i) {
				size_t idx = (size_t)v.pos; if (idx + 1 >= n) { v.pos = (double)n; break; }
				double fr = v.pos - idx;
				for (int c = 0; c < 2; ++c) { double a = v.pcm->s[idx * 2 + c], b = v.pcm->s[(idx + 1) * 2 + c]; acc[(size_t)i * 2 + c] += (int32_t)((a + (b - a) * fr) * v.vol); }
				v.pos += step;
			}
		}
		voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.pos + 1 >= v.pcm->frames(); }), voices_.end());
		for (size_t i = 0; i < acc.size(); ++i) out[i] = (int16_t)std::max(-32768, std::min(32767, acc[i]));
	}
private:
	std::vector<Voice> voices_;
};

// SDL audio device driving a Mixer. If no device can be opened the game stays silent (ok() is false).
class Audio {
public:
	~Audio() { if (dev_) SDL_CloseAudioDevice(dev_); }
	bool open() {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
		SDL_AudioSpec want; SDL_zero(want); want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = &Audio::cb; want.userdata = this;
		SDL_AudioSpec got; dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
		if (!dev_ || got.format != AUDIO_S16SYS || got.channels != 2) { if (dev_) SDL_CloseAudioDevice(dev_); dev_ = 0; return false; }
		rate_ = got.freq; SDL_PauseAudioDevice(dev_, 0); return true;
	}
	bool ok() const { return dev_ != 0; }
	void play(const Pcm* p, float vol) { if (dev_) SDL_LockAudioDevice(dev_); mixer_.play(p, vol); if (dev_) SDL_UnlockAudioDevice(dev_); }
	int started() const { return mixer_.started; }
private:
	static void cb(void* u, Uint8* stream, int len) { auto* a = (Audio*)u; a->mixer_.mix((int16_t*)stream, len / 4, a->rate_); }
	Mixer mixer_; SDL_AudioDeviceID dev_ = 0; int rate_ = 44100;
};

// ---------- the weapon in the player's hands ----------
// Timeline of one shot, from the mod's Fire state (35 tics per second): A 2, B 1, B 1, C 1, C 1, A 1.
struct ShotFrame { char letter; int tics; };

class ViewModel {
public:
	int shots = 0; // shots started (for tests)

	// mod_dir: the unpacked mod directory (contains Patches/ and Sounds/). Returns false if the Boltgun frames are not found.
	bool load(SDL_Renderer* ren, const std::string& mod_dir) {
		const char* names[4] = {"BLTRGA0", "BLTRFA0", "BLTRFB0", "BLTRFC0"}; // ready, fire A/B/C
		for (int i = 0; i < 4; ++i) {
			std::string p = resolve(mod_dir + "/Patches/" + names[i]); if (p.empty()) return false;
			Bitmap b = load_png(p); if (!b.ok) return false;
			tex_[i] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, b.w, b.h);
			if (!tex_[i]) return false;
			SDL_UpdateTexture(tex_[i], nullptr, b.rgba.data(), b.w * 4); SDL_SetTextureBlendMode(tex_[i], SDL_BLENDMODE_BLEND);
			w_ = b.w; h_ = b.h;
		}
		for (int i = 1; i <= 5; ++i) { // fire sounds + the extra layer
			char n[16]; snprintf(n, sizeof n, "%02d", i);
			fire_.push_back(load_ogg(mod_dir + "/Sounds/Weapons/Boltgun/BoltgunFire" + n + ".ogg"));
			extra_.push_back(load_ogg(mod_dir + "/Sounds/Weapons/Boltgun/ExtraFire" + n + ".ogg"));
		}
		for (int i = 1; i <= 9; ++i) { char n[16]; snprintf(n, sizeof n, "%02d", i); pain_.push_back(load_ogg(mod_dir + "/Sounds/Player/Pain/SternguardCharacterPain" + n + ".ogg")); }
		for (int i = 2; i <= 6; ++i) { char n[16]; snprintf(n, sizeof n, "%02d", i); miss_.push_back(load_ogg(mod_dir + "/Sounds/Weapons/Boltgun/ProjectileImpact" + n + ".ogg")); }
		for (int i = 1; i <= 6; ++i) { char n[16]; snprintf(n, sizeof n, "%02d", i); death_.push_back(load_ogg(mod_dir + "/Sounds/Player/Death/SternguardDeathVO" + n + ".ogg")); }
		loaded_ = true; return true;
	}
	bool loaded() const { return loaded_; }
	~ViewModel() { for (auto* t : tex_) if (t) SDL_DestroyTexture(t); }

	void shoot(Audio* audio) {
		if (!loaded_) return; ++shots; t_ = 0; firing_ = true;
		if (audio) { audio->play(pick(fire_), 0.9f); audio->play(pick(extra_), 0.5f); }
	}
	void pain(Audio* audio) { if (loaded_ && audio) audio->play(pick(pain_), 0.9f); }
	void miss(Audio* audio) { if (loaded_ && audio) audio->play(pick(miss_), 0.7f); }
	void death(Audio* audio) { if (loaded_ && audio) audio->play(pick(death_), 1.0f); }

	void update(double dt, bool moving) {
		if (firing_) { t_ += dt; if (t_ >= total_tics() / 35.0) firing_ = false; }
		bob_t_ += moving ? dt : 0; bob_amp_ += ((moving ? 1.0 : 0.0) - bob_amp_) * std::min(1.0, dt * 8);
	}

	// Draw into the letterboxed viewport rectangle. The 512x256 canvas is scaled to 64% of the viewport width and sits on its bottom
	// edge, so the gun lands right of centre as in Doom.
	void draw(SDL_Renderer* ren, const SDL_Rect& vp) {
		if (!loaded_) return;
		static const ShotFrame seq[] = {{'A', 2}, {'B', 1}, {'B', 1}, {'C', 1}, {'C', 1}, {'A', 1}};
		int idx = 0; double tics = t_ * 35.0, kick = 0;
		if (firing_) { double acc = 0; for (auto& f : seq) { if (tics < acc + f.tics) { idx = f.letter - 'A' + 1; break; } acc += f.tics; }
			kick = (tics >= 2 && tics < 4) ? 1.0 : 0.0; } // the mod nudges the weapon (+2,+1) for two tics
		const double cw = vp.w * 0.64, sc = cw / w_, ch = h_ * sc, unit = vp.w / 320.0;
		double bx = std::sin(bob_t_ * 7.0) * vp.w * 0.010 * bob_amp_, by = std::fabs(std::cos(bob_t_ * 7.0)) * vp.h * 0.014 * bob_amp_;
		SDL_Rect d{(int)(vp.x + (vp.w - cw) / 2 + bx + kick * 2 * unit), (int)(vp.y + vp.h - ch + by + 6 + kick * 1 * unit), (int)cw, (int)ch};
		SDL_RenderCopy(ren, tex_[idx], nullptr, &d);
	}

private:
	static int total_tics() { return 2 + 1 + 1 + 1 + 1 + 1; }
	const Pcm* pick(const std::vector<Pcm>& v) { return v.empty() ? nullptr : &v[rng_() % v.size()]; }
	SDL_Texture* tex_[4] = {}; int w_ = 512, h_ = 256; bool loaded_ = false, firing_ = false; double t_ = 0, bob_t_ = 0, bob_amp_ = 0;
	std::vector<Pcm> fire_, extra_, pain_, death_, miss_; std::mt19937 rng_{12345};
};

} // namespace fp
