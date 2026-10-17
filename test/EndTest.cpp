// Harness self-test mission: a small Eden base that is declared victorious at tick 800, used to check that the
// harness notices the end of a mission. Installed as zHarnessEnd.dll so it stays out of the in-game mission lists.
#include <windows.h>
#include "Tethys/API/API.h"

using namespace Tethys;
using namespace TethysAPI;

EXPORT_OP2_MISSION_SCRIPT("Harness self-test: instant win", MissionType::Colony, 1, "cm01.map", "multitek.txt", 12, false);

MISSION_API ibool InitProc() {
  Player[0].GoEden();
  Player[0].GoHuman();
  const Location base(71, 63);
  Game::CreateUnit(MapID::CommandCenter, base, 0);
  Game::CreateUnit(MapID::Lynx, base + Location(5, 3), 0, MapID::Laser);
  Game::CreateUnit(MapID::Lynx, base + Location(7, 3), 0, MapID::Laser);
  Player[0].CenterViewOn(base);
  CreateVictoryCondition(CreateTimeTrigger(800, "NoResponseToTrigger"), "Survive until tick 800");
  return 1;
}

MISSION_API void AIProc() {}
MISSION_API void NoResponseToTrigger() {}
MISSION_API void GetSaveRegions(SaveRegion* pSave) { pSave->pData = nullptr;  pSave->size = 0; }
