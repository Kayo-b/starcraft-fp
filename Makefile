# OPENBW_DIR: checkout of OpenBW headers. DATA_DIR / MAP / MOD_DIR: see the `test` target (data is extracted from your own installs, never committed).
OPENBW_DIR ?= ../starcraft-data/openbw
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -w -I$(OPENBW_DIR) -Isrc
HDRS := src/dir_loader.h src/fp_assets.h src/fp_terrain.h src/fp_render.h src/fp_game.h src/fp_fps.h src/fp_weapon.h
SDL_LIBS = $(shell pkg-config --cflags --libs sdl2 SDL2_ttf libpng vorbisfile)

all: build/fp_view build/fp_play build/fp_gametest build/fp_fpstest build/fp_assettest build/fp_terraintest build/terrain_survey

build/fp_view: src/fp_view.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

build/fp_play: src/fp_play.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread $(SDL_LIBS)

build/fp_gametest: src/fp_gametest.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

build/fp_fpstest: src/fp_fpstest.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

build/fp_assettest: src/fp_assettest.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread $(SDL_LIBS)

build/fp_terraintest: src/fp_terraintest.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

build/terrain_survey: tools/terrain_survey.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

# Headless tests. Usage: make test DATA_DIR=... MAP=".../(2)Bottleneck.scm" MOD_DIR=.../W40K_Boltgun_Doom_a0.9.1
# (TERRAIN_MAPS: more quoted map paths for fp_terraintest, e.g. one per tileset)
test: build/fp_gametest build/fp_fpstest build/fp_assettest build/fp_terraintest
	./build/fp_terraintest "$(DATA_DIR)" "$(MAP)" $(TERRAIN_MAPS)
	./build/fp_gametest "$(DATA_DIR)" "$(MAP)"
	./build/fp_fpstest "$(DATA_DIR)" "$(MAP)"
	./build/fp_assettest "$(MOD_DIR)"

clean:
	rm -rf build
