// Prints the HD sprite (anim/main_NNN.anim) image id of unit types, so you can extract exactly the files the FPS mode needs.
// usage: fp_images DATA_DIR MAP.scm   (prints a regex for tools/casc_get.cjs)
#include "fp_fps.h"
#include <iostream>
#include <set>
using namespace fp;
int main(int argc, char** argv) {
	if (argc < 3) { std::cerr << "usage: fp_images DATA_DIR MAP.scm\n"; return 2; }
	try {
		Game g; g.open(argv[1], argv[2], race_t::terran, race_t::zerg, 1, true);
		std::set<int> ids;
		for (UnitTypes t : {UnitTypes::Hero_Jim_Raynor_Marine, UnitTypes::Hero_Fenix_Dragoon, UnitTypes::Hero_Hunter_Killer, UnitTypes::Zerg_Zergling, UnitTypes::Zerg_Hydralisk,
		                    UnitTypes::Terran_Marine, UnitTypes::Terran_Firebat, UnitTypes::Protoss_Zealot, UnitTypes::Protoss_Dragoon}) {
			unit_t* u = g.fn().create_unit(g.fn().get_unit_type(t), xy{512, 512}, g.enemy()); if (!u) { std::cerr << "cannot create type " << (int)t << "\n"; continue; }
			int id = (int)u->sprite->main_image->image_type->id; ids.insert(id);
			std::cout << (int)t << " -> anim/main_" << id << ".anim\n"; g.fn().destroy_unit(u);
		}
		std::cout << "regex: ^anim\\\\\\\\main_0*("; bool first = true; for (int id : ids) { std::cout << (first ? "" : "|") << id; first = false; } std::cout << ")\\\\.anim$\n";
	} catch (const std::exception& e) { std::cout << "ERROR: " << e.what() << "\n"; return 1; }
}
