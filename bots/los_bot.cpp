// Line-of-sight probe bot (harness/oracle/FINDINGS.md "Combat"). At tick LOS_TICK (default 3) it asks the game's own
// line-of-sight test (0x436FA0, thiscall on the map at 0x54F7F8, args x1 y1 x2 y2 in internal tiles; the target
// search 0x4A6EA0 calls it with the candidate's tile first) between each tile listed in LOS_FROM ("x,y;x,y", file
// tiles) and every tile within LOS_R (default 6), both ways, and logs each tile's TileData bit 31 (the flag the test
// stops at besides sight-blocking cell types).
//
// Run: harness/run.sh --scenario S.json --bot harness/build/los_bot.dll --out DIR   (LOS_FROM in the environment)
// Log: $OP2H_OUT\los.jsonl, one line per source tile: {"from":[x,y],"to":[[x,y,from->to,to->from,bit31],...]}
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int PadTiles() {
  // GameMap: padded width minus the real width over 2 is the padding; TethysAPI keeps it at 0x54F7F8 + 0x10 (tile
  // x mask) etc. The scenarios here use non-wrapping maps, padded by 32.
  return 32;
}

unsigned TileData(int x, int y) {
  const int mask = *reinterpret_cast<int*>(0x54F804);
  const int shift = *reinterpret_cast<unsigned char*>(0x54F82C);
  const char* tiles = *reinterpret_cast<char**>(0x54FC5C);
  const int xm = x & mask;
  const int off = ((((xm >> 5) << shift) + y) << 7) + (xm & 31) * 4;
  return *reinterpret_cast<const unsigned*>(tiles + off);
}

int Los(int x1, int y1, int x2, int y2) {
  using Fn = int(__thiscall*)(void*, int, int, int, int);
  return reinterpret_cast<Fn>(0x436FA0)(reinterpret_cast<void*>(0x54F7F8), x1, y1, x2, y2);
}

}  // namespace

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int tick) {
  const char* t = getenv("LOS_TICK");
  if (tick != (t ? atoi(t) : 3)) return;
  const char* from = getenv("LOS_FROM");
  const char* r = getenv("LOS_R");
  const int radius = r ? atoi(r) : 6;
  if (!from) return;
  char path[MAX_PATH];
  const char* out = getenv("OP2H_OUT");
  snprintf(path, sizeof(path), "%s\\los.jsonl", out ? out : ".");
  FILE* f = fopen(path, "w");
  if (!f) return;
  const int pad = PadTiles();
  const char* p = from;
  while (*p) {
    int fx = 0, fy = 0;
    if (sscanf(p, "%d,%d", &fx, &fy) != 2) break;
    fprintf(f, "{\"from\":[%d,%d],\"to\":[", fx, fy);
    bool first = true;
    for (int y = fy - radius; y <= fy + radius; ++y) {
      for (int x = fx - radius; x <= fx + radius; ++x) {
        if (y < 0) continue;
        const int a = Los(fx + pad, fy, x + pad, y), b = Los(x + pad, y, fx + pad, fy);
        const unsigned bit = TileData(x + pad, y) >> 31;
        fprintf(f, "%s[%d,%d,%d,%d,%u]", first ? "" : ",", x, y, a, b, bit);
        first = false;
      }
    }
    fprintf(f, "]}\n");
    const char* semi = strchr(p, ';');
    if (!semi) break;
    p = semi + 1;
  }
  fclose(f);
}
