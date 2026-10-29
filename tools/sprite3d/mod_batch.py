"""Every building the SC Evo Complete mod has a model for, as the mod's model textured with the Remastered sprite's own
pixels (the user's choice for all buildings, after the Command Center and the Hatchery/Lair/Hive): the model in its
"Stand" pose ("Stand Attached" for the Control Tower; `m3_pose.py`; the rest pose where it has none), plus the few
non-opaque parts the sprite shows as solid (`m3_fx.py`), fitted to the sprite and repainted
(`trellis_compose.py ... sprite-texture`), then rendered and saved as .blend (`blender_views.py`). Steps whose output
exists are skipped, so a run can be resumed; JOBS buildings run at once.

Image ids are the units' main images from the game data (openbw: unit type -> flingy -> sprite -> image), which is also
the HD anim file (each is the first image using its GRP); several GRP names mislead (the Engineering Bay is weaponpl,
the Sunken Colony Lurker, the Fleet Beacon warp, the Arbiter Tribunal prism, the Robotics Support Bay stasis).

    M3_OBJ=path/to/m3_obj mod_batch.py ASSET_ROOT OUT_DIR [NAME ...] [--jobs N] [--recolour]
    (ASSET_ROOT: the mod's SCEvo_Assets.SC2Mod/Base.SC2Assets; OUT_DIR gets sc2/ and NAME.glb/.blend/.png)
With --recolour, the mod's own textures recoloured to the sprite's colours instead (`trellis_compose.py ... recolour`,
the Protoss with their gold deepened), into OUT_DIR/recolour/.
Run with the venv python."""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable

# name: (image id = anim number, model under the asset root)
BUILDINGS = {
    'command_center': (63, 'Assets/Buildings/Terran/CommandCenterSCBW/CommandCenterSCBW.m3'),
    'comsat_station': (271, 'Assets/Buildings/Terran/ComsatStationSCBW/ComsatStationSCBW.m3'),
    'nuclear_silo': (312, 'Assets/Buildings/Terran/NuclearSiloSCBW/NuclearSiloSCBW.m3'),
    'supply_depot': (278, 'Assets/Buildings/Terran/SupplyDepotSCBW/SupplyDepotSCBW.m3'),
    'refinery': (307, 'Assets/Buildings/Terran/RefinerySCBW/RefinerySCBW.m3'),
    'barracks': (266, 'Assets/Buildings/Terran/BarracksSCBW/BarracksSCBW.m3'),
    'academy': (263, 'Assets/Buildings/Terran/AcademySCBW/AcademySCBW.m3'),
    'factory': (285, 'Assets/Buildings/Terran/FactorySCBW/FactorySCBW.m3'),
    'control_tower': (281, 'Assets/Buildings/Terran/ControlTowerSCBW/ControlTowerSCBW.m3'),
    'science_facility': (309, 'Assets/Buildings/Terran/ScienceFacilitySCBW/ScienceFacilitySCBW.m3'),
    'covert_ops': (288, 'Assets/Buildings/Terran/CovertOpsSCBW/CovertOpsSCBW.m3'),
    'machine_shop': (293, 'Assets/Buildings/Terran/MachineShopSCBW/MachineShopSCBWFix.m3'),
    'engineering_bay': (322, 'Assets/Buildings/Terran/EngineeringBaySCBW/EngineeringBaySCBW.m3'),
    'missile_turret': (296, 'Assets/Buildings/Terran/MissileTurretSCBW/MissileTurretSCBW.m3'),
    'bunker': (304, 'Assets/Buildings/Terran/BunkerSCBW/BunkerSCBW.m3'),
    'defiler_mound': (82, 'Assets/Buildings/Zerg/DefilerMoundSCBW/DefilerMoundSCBW.m3'),
    'queens_nest': (84, 'Assets/Buildings/Zerg/QueensNestSCBW/QueensNestSCBW.m3'),
    'evolution_chamber': (66, 'Assets/Buildings/Zerg/EvolutionChamberSCBW/EvolutionChamberSCBW.m3'),
    'spawning_pool': (64, 'Assets/Buildings/Zerg/SpawningPoolSCBW/SpawningPoolSCBW.m3'),
    'creep_colony': (68, 'Assets/Buildings/Zerg/CreepColonySCBW/CreepColonySCBW.m3'),
    'spore_colony': (99, 'Assets/Buildings/Zerg/SporeColonySCBW/SporeColonySCBW.m3'),
    'sunken_colony': (76, 'Assets/Buildings/Zerg/SunkenColonySCBW/SunkenColonySCBW.m3'),
    'extractor': (93, 'Assets/Buildings/Zerg/ExtractorSCBW/ExtractorSCBW.m3'),
    'nexus': (179, 'Assets/Buildings/Protoss/NexusSCBW/NexusSCBW.m3'),
    'robotics_facility': (192, 'Assets/Buildings/Protoss/RoboticsFacilitySCBW/RoboticsFacilitySCBW.m3'),
    'pylon': (189, 'Assets/Buildings/Protoss/PylonSCBW/PylonSCBW.m3'),
    'assimilator': (158, 'Assets/Buildings/Protoss/AssimilatorSCBW/AssimilatorSCBW.m3'),
    'observatory': (161, 'Assets/Buildings/Protoss/ObservatorySCBW/ObservatorySCBW.m3'),
    'gateway': (171, 'Assets/Buildings/Protoss/GatewaySCBW/GatewaySCBW.m3'),
    'photon_cannon': (183, 'Assets/Buildings/Protoss/PhotonCannonSCBW/PhotonCannonSCBW.m3'),
    'citadel_of_adun': (164, 'Assets/Buildings/Protoss/CitadelOfAdunSCBW/CitadelofAdunSCBW.m3'),
    'cybernetics_core': (174, 'Assets/Buildings/Protoss/CyberneticsCoreSCBW/CyberneticsCoreSCBW.m3'),
    'templar_archives': (155, 'Assets/Buildings/Protoss/TemplarArchivesSCBW/TemplarArchivesSCBW.m3'),
    'forge': (167, 'Assets/Buildings/Protoss/ForgeSCBW/ForgeSCBW.m3'),
    'stargate': (199, 'Assets/Buildings/Protoss/StargateSCBW/StargateSCBW.m3'),
    'fleet_beacon': (208, 'Assets/Buildings/Protoss/FleetBeaconSCBW/FleetBeaconSCBW.m3'),
    'arbiter_tribunal': (186, 'Assets/Buildings/Protoss/ArbiterTribunalSCBW/ArbiterTribunalSCBW.m3'),
    'robotics_support_bay': (204, 'Assets/Buildings/Protoss/RoboticsSupportBaySCBW/RoboticsSupportBaySCBW.m3'),
    'shield_battery': (195, 'Assets/Buildings/Protoss/ShieldBatterySCBW/ShieldBatterySCBW.m3'),
}

# the pose shown, where it is not "Stand". Add-ons also have "Stand Attached", which only reads as its sprite for the
# Control Tower (the Comsat Station, Nuclear Silo, Covert Ops and Machine Shop fit and look better in "Stand")
SEQUENCE = {'control_tower': 'Stand Attached'}

# `m3_fx.py` changes: left-out (non-opaque) parts the sprite shows as solid, added back (its numbers): the Fleet Beacon's
# energy sphere, the Gateway's crystal, the Robotics Facility's glass dome; and drop:R, a region in the same place as
# another: the Assimilator's depleted dome (it fought with the full one)
SOLID_FX = {'fleet_beacon': ['0'], 'gateway': ['2'], 'robotics_facility': ['3'], 'assimilator': ['drop:2']}

# `trellis_compose.py` squash:Z0:K: the Creep Colony's tentacle stands twice as tall as the sprite's bulb
SQUASH = {'creep_colony': 'squash:0.5:0.5'}

# `trellis_compose.py` size:K: the finished model scaled, the user's calls from first person: the Nexus's pyramid read
# small beside the Command Center's body (1.4x, then 1.4x again: 1.96), the Pylon too big beside the Gateway (to 2/3 of
# the Gateway's height: 0.85, 77 -> 65 px)
SIZE = {'nexus': 'size:1.96', 'pylon': 'size:0.85'}

# recoloured Protoss gold deepened (`colour_map`; the user found it washed out)
GOLD = 'gold:1.6:6'
PROTOSS = {n for n, (_, m) in BUILDINGS.items() if '/Protoss/' in m}


def run(cmd, log):
    log.write('$ %s\n' % ' '.join(cmd)); log.flush()
    r = subprocess.run(cmd, cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write(r.stdout); log.flush()
    if r.returncode:
        raise RuntimeError('%s failed (%d)' % (os.path.basename(cmd[0 if cmd[0] != PY else 1]), r.returncode))
    return r.stdout


def building(name, assets, out, recolour=False):
    image_id, model = BUILDINGS[name]
    seq = SEQUENCE.get(name, 'Stand')
    sc2 = os.path.join(out, 'sc2')
    base, dst = os.path.join(sc2, name), os.path.join(out, 'recolour' if recolour else '', name)
    posed = os.path.join(sc2, name + '_' + seq.lower().replace(' ', '_'))
    with open(dst + '.log', 'a') as log:
        try:
            if not os.path.exists(base + '.obj'):
                run([os.environ.get('M3_OBJ', 'm3_obj'), assets, model, base], log)
            if not os.path.exists(posed + '.obj') and not os.path.exists(base + '.rest'):
                seqs = run([PY, 'm3_pose.py', os.path.join(assets, model), base, '-', '-', '--list'], log).split('\n')
                try:  # a model without the sequence (or without bones to pose) stays in its rest pose
                    if not any(s[4:] == seq for s in seqs):
                        raise RuntimeError('no "%s"' % seq)
                    run([PY, 'm3_pose.py', os.path.join(assets, model), base, seq, posed], log)
                except RuntimeError as e:
                    log.write('rest pose: %s\n' % e)
                    open(base + '.rest', 'w').write(str(e))
            if os.path.exists(base + '.rest'):
                posed = base
            if name in SOLID_FX:
                if not os.path.exists(posed + '_fx.obj'):
                    run([PY, 'm3_fx.py', os.path.join(assets, model), posed + '.obj', posed + '_fx.obj'] + SOLID_FX[name], log)
                posed += '_fx'
            if not os.path.exists(dst + '.glb'):
                run([PY, 'trellis_compose.py', str(image_id), '0', posed + '.obj', '850', dst,
                     'recolour' if recolour else 'sprite-texture'] + ([SQUASH[name]] if name in SQUASH else [])
                    + ([SIZE[name]] if name in SIZE else [])
                    + ([GOLD] if recolour and name in PROTOSS else []), log)
            if not os.path.exists(dst + '.blend'):
                run(['blender', '-b', '-P', 'blender_views.py', '--', dst + '.glb', dst, '35'], log)
            fit = [l for l in open(dst + '.log') if l.startswith('body:')]
            return '%-22s %s' % (name, ' | '.join(l.strip()[6:] for l in fit[-2:]))
        except Exception as e:  # one building failing must not stop the others
            return '%-22s FAILED: %s (see %s.log)' % (name, e, dst)


if __name__ == '__main__':
    args = sys.argv[1:]
    jobs, recolour = 2, '--recolour' in args
    args = [a for a in args if a != '--recolour']
    if '--jobs' in args:
        k = args.index('--jobs'); jobs = int(args[k + 1]); del args[k:k + 2]
    assets, out = os.path.abspath(args[0]), os.path.abspath(args[1])
    names = args[2:] or list(BUILDINGS)
    os.makedirs(os.path.join(out, 'sc2'), exist_ok=True)
    os.makedirs(os.path.join(out, 'recolour'), exist_ok=True)
    with ThreadPoolExecutor(jobs) as pool:
        for line in pool.map(lambda n: building(n, assets, out, recolour), names):
            print(line, flush=True)
