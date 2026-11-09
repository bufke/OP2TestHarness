// Harness scenario mission: an empty colony game on any map, for oracle runs. Units and orders come from the harness
// script (`spawn`, `move`, ...), which run.py generates from a scenario JSON (see README "Scenarios").
// The game reads MapName straight from the DLL file (MissionManager::GetModuleDesc maps the file, it doesn't load it),
// so run.py writes a copy of this DLL per map with the name patched into the marked MapName slot.
// Player 0 is human (Eden), player 1 an idle AI (Plymouth) for opposing units. Full daylight, no disasters triggered.
#include <windows.h>
#include "Tethys/API/API.h"

using namespace Tethys;
using namespace TethysAPI;

// EXPORT_OP2_MISSION_SCRIPT, spelled out so MapName has room for any map name and carries the patch marker.
extern "C" {
__declspec(dllexport) char      LevelDesc[]    = "Harness scenario";
__declspec(dllexport) char      MapName[64]    = "cm01.map\0" "@@OP2H_MAPNAME_SLOT@@";  // run.py patches this.
__declspec(dllexport) char      TechtreeName[] = "multitek.txt";
__declspec(dllexport) ModDesc   DescBlock      = { MissionType::Colony, 2, 12, false };
__declspec(dllexport) ModDescEx DescBlockEx    = { };
}

MISSION_API ibool InitProc() {
  Player[0].GoEden();
  Player[0].GoHuman();
  Player[1].GoPlymouth();
  Player[1].GoAI();
  Game::SetDaylightEverywhere(true);
  return 1;
}

MISSION_API void AIProc() { }

MISSION_API void GetSaveRegions(SaveRegion* pSave) {
  pSave->pData = nullptr;
  pSave->size  = 0;
}
