// Example harness bot: plays the human side by sending every idle player-0 combat vehicle to attack the nearest
// enemy unit. Build with `make build/sample_bot.dll`, run with `run.sh --bot harness/build/sample_bot.dll`.
// Orders go through Unit::Do*(), i.e. the same command packets local player input produces.
#include <windows.h>
#include <climits>
#include "Tethys/API/API.h"

using namespace Tethys;
using namespace TethysAPI;

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int tick) {
  if (tick % 40 != 0) return;  // Re-plan every 10 game "times".
  constexpr int me = 0;
  for (Unit tank : PlayerVehicleEnum(me)) {
    if (!tank.IsLive() || !tank.IsOffensive() || tank.GetCommand() == CommandType::Attack) continue;
    const Location at = tank.GetLocation();
    Unit best;
    int bestDist = INT_MAX;
    for (int p = 0; p < Game::NumPlayers(); ++p) {
      if (p == me) continue;
      for (Unit enemy : PlayerVehicleEnum(p)) {
        if (!enemy.IsLive()) continue;
        const Location e = enemy.GetLocation();
        const int d = (e.x - at.x) * (e.x - at.x) + (e.y - at.y) * (e.y - at.y);
        if (d < bestDist) { bestDist = d;  best = enemy; }
      }
    }
    if (best.IsLive()) tank.DoAttack(best);
  }
}
