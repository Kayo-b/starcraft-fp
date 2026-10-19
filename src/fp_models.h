// Which StarCraft units are drawn as 3D models, and the loaded models. The models are SC2 .m3 files from a local copy
// of the SC Evo Complete mod (its SCEvo_Assets.SC2Mod/Base.SC2Assets directory, passed as --models DIR); without it every
// unit stays a sprite. Buildings and mineral fields first: upright billboards of them fit a first-person view worst.
#pragma once
#include "bwgame.h"
#include "fp_m3.h"
#include <map>
#include <memory>
#include <string>

namespace fp {

// Unit type -> model file under the asset root. Buildings the mod has no live model for (Starport, Physics Lab, Armory,
// Lair, Hive, Hydralisk Den, Spire, Greater Spire, Ultralisk Cavern, Nydus Canal) and the Vespene Geyser keep sprites.
inline const char* model_file(bwgame::UnitTypes t) {
	using bwgame::UnitTypes;
	static const std::map<UnitTypes, const char*> files = {
		{UnitTypes::Terran_Command_Center, "Assets/Buildings/Terran/CommandCenterSCBW/CommandCenterSCBW.m3"},
		{UnitTypes::Terran_Comsat_Station, "Assets/Buildings/Terran/ComsatStationSCBW/ComsatStationSCBW.m3"},
		{UnitTypes::Terran_Nuclear_Silo, "Assets/Buildings/Terran/NuclearSiloSCBW/NuclearSiloSCBW.m3"},
		{UnitTypes::Terran_Supply_Depot, "Assets/Buildings/Terran/SupplyDepotSCBW/SupplyDepotSCBW.m3"},
		{UnitTypes::Terran_Refinery, "Assets/Buildings/Terran/RefinerySCBW/RefinerySCBW.m3"},
		{UnitTypes::Terran_Barracks, "Assets/Buildings/Terran/BarracksSCBW/BarracksSCBW.m3"},
		{UnitTypes::Terran_Academy, "Assets/Buildings/Terran/AcademySCBW/AcademySCBW.m3"},
		{UnitTypes::Terran_Factory, "Assets/Buildings/Terran/FactorySCBW/FactorySCBW.m3"},
		{UnitTypes::Terran_Control_Tower, "Assets/Buildings/Terran/ControlTowerSCBW/ControlTowerSCBW.m3"},
		{UnitTypes::Terran_Science_Facility, "Assets/Buildings/Terran/ScienceFacilitySCBW/ScienceFacilitySCBW.m3"},
		{UnitTypes::Terran_Covert_Ops, "Assets/Buildings/Terran/CovertOpsSCBW/CovertOpsSCBW.m3"},
		{UnitTypes::Terran_Machine_Shop, "Assets/Buildings/Terran/MachineShopSCBW/MachineShopSCBWFix.m3"},
		{UnitTypes::Terran_Engineering_Bay, "Assets/Buildings/Terran/EngineeringBaySCBW/EngineeringBaySCBW.m3"},
		{UnitTypes::Terran_Missile_Turret, "Assets/Buildings/Terran/MissileTurretSCBW/MissileTurretSCBW.m3"},
		{UnitTypes::Terran_Bunker, "Assets/Buildings/Terran/BunkerSCBW/BunkerSCBW.m3"},
		{UnitTypes::Zerg_Infested_Command_Center, "Assets/Buildings/Zerg/InfestedCommandCenterSCBW/InfestedCommandCenterSCBW.m3"},
		{UnitTypes::Zerg_Hatchery, "Assets/Buildings/Zerg/HatcherySCBW/HatcherySCBW.m3"},
		{UnitTypes::Zerg_Defiler_Mound, "Assets/Buildings/Zerg/DefilerMoundSCBW/DefilerMoundSCBW.m3"},
		{UnitTypes::Zerg_Queens_Nest, "Assets/Buildings/Zerg/QueensNestSCBW/QueensNestSCBW.m3"},
		{UnitTypes::Zerg_Evolution_Chamber, "Assets/Buildings/Zerg/EvolutionChamberSCBW/EvolutionChamberSCBW.m3"},
		{UnitTypes::Zerg_Spawning_Pool, "Assets/Buildings/Zerg/SpawningPoolSCBW/SpawningPoolSCBW.m3"},
		{UnitTypes::Zerg_Creep_Colony, "Assets/Buildings/Zerg/CreepColonySCBW/CreepColonySCBW.m3"},
		{UnitTypes::Zerg_Spore_Colony, "Assets/Buildings/Zerg/SporeColonySCBW/SporeColonySCBW.m3"},
		{UnitTypes::Zerg_Sunken_Colony, "Assets/Buildings/Zerg/SunkenColonySCBW/SunkenColonySCBW.m3"},
		{UnitTypes::Zerg_Extractor, "Assets/Buildings/Zerg/ExtractorSCBW/ExtractorSCBW.m3"},
		{UnitTypes::Protoss_Nexus, "Assets/Buildings/Protoss/NexusSCBW/NexusSCBW.m3"},
		{UnitTypes::Protoss_Robotics_Facility, "Assets/Buildings/Protoss/RoboticsFacilitySCBW/RoboticsFacilitySCBW.m3"},
		{UnitTypes::Protoss_Pylon, "Assets/Buildings/Protoss/PylonSCBW/PylonSCBW.m3"},
		{UnitTypes::Protoss_Assimilator, "Assets/Buildings/Protoss/AssimilatorSCBW/AssimilatorSCBW.m3"},
		{UnitTypes::Protoss_Observatory, "Assets/Buildings/Protoss/ObservatorySCBW/ObservatorySCBW.m3"},
		{UnitTypes::Protoss_Gateway, "Assets/Buildings/Protoss/GatewaySCBW/GatewaySCBW.m3"},
		{UnitTypes::Protoss_Photon_Cannon, "Assets/Buildings/Protoss/PhotonCannonSCBW/PhotonCannonSCBW.m3"},
		{UnitTypes::Protoss_Citadel_of_Adun, "Assets/Buildings/Protoss/CitadelOfAdunSCBW/CitadelofAdunSCBW.m3"},
		{UnitTypes::Protoss_Cybernetics_Core, "Assets/Buildings/Protoss/CyberneticsCoreSCBW/CyberneticsCoreSCBW.m3"},
		{UnitTypes::Protoss_Templar_Archives, "Assets/Buildings/Protoss/TemplarArchivesSCBW/TemplarArchivesSCBW.m3"},
		{UnitTypes::Protoss_Forge, "Assets/Buildings/Protoss/ForgeSCBW/ForgeSCBW.m3"},
		{UnitTypes::Protoss_Stargate, "Assets/Buildings/Protoss/StargateSCBW/StargateSCBW.m3"},
		{UnitTypes::Protoss_Fleet_Beacon, "Assets/Buildings/Protoss/FleetBeaconSCBW/FleetBeaconSCBW.m3"},
		{UnitTypes::Protoss_Arbiter_Tribunal, "Assets/Buildings/Protoss/ArbiterTribunalSCBW/ArbiterTribunalSCBW.m3"},
		{UnitTypes::Protoss_Robotics_Support_Bay, "Assets/Buildings/Protoss/RoboticsSupportBaySCBW/RoboticsSupportBaySCBW.m3"},
		{UnitTypes::Protoss_Shield_Battery, "Assets/Buildings/Protoss/ShieldBatterySCBW/ShieldBatterySCBW.m3"},
		{UnitTypes::Resource_Mineral_Field, "Assets/Buildings/Resources/ResourceMineralClassicSCBW/ResourceMineralClassicSCBW_00.m3"},
		{UnitTypes::Resource_Mineral_Field_Type_2, "Assets/Buildings/Resources/ResourceMineralClassicSCBW/ResourceMineralClassicSCBW_01.m3"},
		{UnitTypes::Resource_Mineral_Field_Type_3, "Assets/Buildings/Resources/ResourceMineralClassicSCBW/ResourceMineralClassicSCBW_02.m3"},
	};
	auto it = files.find(t); return it == files.end() ? nullptr : it->second;
}

class ModelLibrary {
public:
	explicit ModelLibrary(const std::string& root) : assets_(root) {}
	bool ok() const { return !assets_.empty(); }
	// Loads once; nullptr when the file is missing or unreadable (reported once on stderr).
	const M3Model* load(const std::string& rel) {
		auto it = cache_.find(rel);
		if (it == cache_.end()) {
			auto m = std::make_unique<M3Model>(); const std::filesystem::path* p = assets_.find(rel);
			if (!p || !m->load(*p, assets_)) { std::fprintf(stderr, "models: %s: %s; drawn as a sprite\n", rel.c_str(), p ? m->error.c_str() : "not found"); m.reset(); }
			it = cache_.emplace(rel, std::move(m)).first;
		}
		return it->second.get();
	}
	const M3Model* for_unit(bwgame::UnitTypes t) { const char* f = model_file(t); return f ? load(f) : nullptr; }
private:
	AssetIndex assets_; std::map<std::string, std::unique_ptr<M3Model>> cache_;
};

} // namespace fp
