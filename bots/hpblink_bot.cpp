// Red health bar blink probe (notes/findings-polish.md "Blinking red health bar"). MapUnit::DrawUIOverlay (0x43EE21)
// draws a bar at a quarter health or less in RGB555 red masked with bit 1 of the game tick (0x43EE56..0x43EE62), so it
// alternates two ticks red, two ticks black. OPU's "always show damaged units' HP bars" option (ShowUnitHPBars=1, the
// default; OPUPatch UserInterface.cpp) also hooks 0x43EE62 to force the colour to red, which is why harness frames
// show it steady. This bot puts the original `and ecx, 0x7C00` back on the first tick, so real-time frames show what
// the unpatched game draws (the bars stay visible on every damaged unit, as with OPU).
//   ./run.sh --scenario $ORACLE/input/hp_blink.json --no-turbo --screenshot-every 1
//     --bot build/hpblink_bot.dll --out DIR
#include <windows.h>
#include <cstring>

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int) {
  static bool done = false;
  if (done) return;
  done = true;
  static const unsigned char original[6] = { 0x81, 0xE1, 0x00, 0x7C, 0x00, 0x00 };  // and ecx, 0x7C00
  void* at = reinterpret_cast<void*>(0x43EE62);
  DWORD old;
  if (VirtualProtect(at, sizeof(original), PAGE_EXECUTE_READWRITE, &old)) {
    memcpy(at, original, sizeof(original));
    VirtualProtect(at, sizeof(original), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(original));
  }
}
