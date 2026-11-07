// Mission-API boundary probe (notes/findings-dll.md "Mission-API boundary sweep"). At tick 2 it calls the exe's exported
// mission API directly, by address, with arguments at and past the edges of the map and the tile array, and writes what
// came back: return values, the objects created (their index, internal pixel position, heading, type code) and the
// tile words the writes touched. op2-missionhost's tests/edges.rs replays every probe against its own implementation.
//
// Run: harness/run.sh --scenario harness/oracle/scenarios/api_edges.json --bot harness/build/edge_bot.dll --out DIR
// Output: $OP2H_OUT\edges.jsonl, one probe per line: {"p": name, "a": [args], "r": result, ...}. Coordinates are
// internal tiles (file x + 32 on cm01.map, 128x128: map columns 32..159, tile array 0..255).
//
// Calling conventions: the static TethysGame/GameMap exports are __fastcall; a LOCATION by value goes on the stack, so
// the first int after it takes ecx (SetTile's tile, 0x476D80). The value types' methods are __thiscall.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct Loc { int x, y; };
struct Rect { int x1, y1, x2, y2; };

FILE* g_out = nullptr;

char* Obj(int index) { return *reinterpret_cast<char**>(0x54F848) + index * 0x78; }
bool Live(int i) {
  char* o = Obj(i);
  return *reinterpret_cast<int*>(o + 4) != -1 && !(*reinterpret_cast<unsigned*>(o + 0x44) & 0x20000);
}
const char* Code(char* o) {
  using Fn = char*(__thiscall*)(char*);
  char* type = reinterpret_cast<Fn>((*reinterpret_cast<void***>(o))[0])(o);
  using CodeFn = const char*(__thiscall*)(char*);
  return reinterpret_cast<CodeFn>((*reinterpret_cast<void***>(type))[1])(type);
}

// The tile word the game's own lookups read (GetTile 0x476D00): no check on y, x masked.
unsigned* Tile(int x, int y) {
  const int mask = *reinterpret_cast<int*>(0x54F804);
  const int lg = *reinterpret_cast<unsigned char*>(0x54F82C);
  const int mx = x & mask;
  return *reinterpret_cast<unsigned**>(0x54FC5C) + ((((mx >> 5) << lg) + y) << 5) + (mx & 31);
}

std::vector<bool> g_live;
void Snapshot() {
  g_live.assign(2048, false);
  for (int i = 1; i < 2048; ++i) g_live[i] = Live(i);
}
// The objects a call created: index, pixel position, heading, owner and type code.
void NewObjects() {
  fputs(",\"new\":[", g_out);
  bool first = true;
  for (int i = 1; i < 2048; ++i) {
    if (g_live[i] || !Live(i)) continue;
    char* o = Obj(i);
    fprintf(g_out, "%s[%d,%d,%d,%d,%d,\"%s\"]", first ? "" : ",", i, *reinterpret_cast<int*>(o + 0x14),
            *reinterpret_cast<int*>(o + 0x18), *reinterpret_cast<unsigned char*>(o + 0x1C),
            *reinterpret_cast<unsigned char*>(o + 0x1D) & 15, Code(o));
    first = false;
  }
  fputs("]", g_out);
  Snapshot();
}

void Begin(const char* name, std::initializer_list<int> args) {
  fprintf(g_out, "{\"p\":\"%s\",\"a\":[", name);
  bool first = true;
  for (int a : args) { fprintf(g_out, "%s%d", first ? "" : ",", a); first = false; }
  fputs("]", g_out);
}
void End() { fputs("}\n", g_out); }

using GetFn = int(__fastcall*)(int, int, int, int);
using SetFn = void(__fastcall*)(int, int, int, int);
GetFn const GetTile = reinterpret_cast<GetFn>(0x476D00);
GetFn const GetCellType = reinterpret_cast<GetFn>(0x476D40);
SetFn const SetTile = reinterpret_cast<SetFn>(0x476D80);
SetFn const SetCellType = reinterpret_cast<SetFn>(0x476E30);
SetFn const SetLavaPossible = reinterpret_cast<SetFn>(0x476F20);
using CreateUnitFn = int(__fastcall*)(int*, int, int, int, int, int, int);
CreateUnitFn const CreateUnit = reinterpret_cast<CreateUnitFn>(0x478780);
using CreateBeaconFn = int(__fastcall*)(int, int, int, int, int, int);
CreateBeaconFn const CreateBeacon = reinterpret_cast<CreateBeaconFn>(0x478940);
using CreateWreckFn = int(__fastcall*)(int, int, int, int);
CreateWreckFn const CreateWreck = reinterpret_cast<CreateWreckFn>(0x4789F0);
using PlaceMarkerFn = int(__fastcall*)(int*, int, int, int);
PlaceMarkerFn const PlaceMarker = reinterpret_cast<PlaceMarkerFn>(0x478BB0);
using CreateWallFn = int(__fastcall*)(int, int, int, int);
CreateWallFn const CreateWallOrTube = reinterpret_cast<CreateWallFn>(0x478AA0);
using Disaster3Fn = void(__fastcall*)(int, int, int);
Disaster3Fn const SetMeteor = reinterpret_cast<Disaster3Fn>(0x4783B0);
Disaster3Fn const SetEarthquake = reinterpret_cast<Disaster3Fn>(0x478320);

void(__thiscall* const Clip)(Loc*) = reinterpret_cast<void(__thiscall*)(Loc*)>(0x475960);
void(__thiscall* const Add)(Loc*, const Loc*) = reinterpret_cast<void(__thiscall*)(Loc*, const Loc*)>(0x475A30);
void(__thiscall* const Inflate)(Rect*, int, int) = reinterpret_cast<void(__thiscall*)(Rect*, int, int)>(0x475A60);
void(__thiscall* const Offset)(Rect*, int, int) = reinterpret_cast<void(__thiscall*)(Rect*, int, int)>(0x475BD0);
void(__thiscall* const ClipToMap)(Rect*) = reinterpret_cast<void(__thiscall*)(Rect*)>(0x475AF0);
int(__thiscall* const Width)(Rect*) = reinterpret_cast<int(__thiscall*)(Rect*)>(0x475AA0);
int(__thiscall* const Height)(Rect*) = reinterpret_cast<int(__thiscall*)(Rect*)>(0x475AE0);
int(__thiscall* const Check)(Rect*, Loc*) = reinterpret_cast<int(__thiscall*)(Rect*, Loc*)>(0x475D50);

void Probe() {
  // Value types: pure functions of their arguments and the map's size.
  const Loc clips[] = {{-5, -5}, {31, 0}, {32, 127}, {159, 128}, {160, 5}, {161, 5}, {255, 5}, {256, 5}, {1000, -1000}};
  for (Loc l : clips) {
    Loc c = l;
    Clip(&c);
    Begin("Clip", {l.x, l.y});
    fprintf(g_out, ",\"r\":[%d,%d]", c.x, c.y);
    End();
  }
  const int adds[][4] = {{40, 5, -50, 0}, {250, 5, 10, 3}, {100, 120, 0, 20}, {40, 5, 300, -10}};
  for (auto& a : adds) {
    Loc l = {a[0], a[1]};
    const Loc d = {a[2], a[3]};
    Add(&l, &d);
    Begin("Add", {a[0], a[1], a[2], a[3]});
    fprintf(g_out, ",\"r\":[%d,%d]", l.x, l.y);
    End();
  }
  const int rects[][4] = {{35, 5, 40, 10}, {40, 5, 40, 10}, {40, 5, 35, 10}, {35, 10, 40, 5}, {0, 0, 255, 127}, {-10, -10, 300, 200}};
  const Loc points[] = {{35, 7}, {38, 7}, {40, 7}, {41, 7}, {34, 7}, {100, 7}, {38, 4}, {38, 11}};
  for (auto& r : rects) {
    for (Loc p : points) {
      Rect rc = {r[0], r[1], r[2], r[3]};
      Loc q = p;
      Begin("Check", {r[0], r[1], r[2], r[3], p.x, p.y});
      fprintf(g_out, ",\"r\":%d", Check(&rc, &q));
      End();
    }
    Rect rc = {r[0], r[1], r[2], r[3]};
    Begin("Width", {r[0], r[1], r[2], r[3]});
    fprintf(g_out, ",\"r\":%d", Width(&rc));
    End();
    Begin("Height", {r[0], r[1], r[2], r[3]});
    fprintf(g_out, ",\"r\":%d", Height(&rc));
    End();
    for (int k = 0; k < 2; ++k) {
      Rect c = {r[0], r[1], r[2], r[3]};
      const int dx = k ? 50 : -50, dy = k ? 3 : -3;
      Inflate(&c, dx, dy);
      Begin("Inflate", {r[0], r[1], r[2], r[3], dx, dy});
      fprintf(g_out, ",\"r\":[%d,%d,%d,%d]", c.x1, c.y1, c.x2, c.y2);
      End();
      c = {r[0], r[1], r[2], r[3]};
      Offset(&c, dx, dy);
      Begin("Offset", {r[0], r[1], r[2], r[3], dx, dy});
      fprintf(g_out, ",\"r\":[%d,%d,%d,%d]", c.x1, c.y1, c.x2, c.y2);
      End();
    }
    Rect c = {r[0], r[1], r[2], r[3]};
    ClipToMap(&c);
    Begin("ClipToMap", {r[0], r[1], r[2], r[3]});
    fprintf(g_out, ",\"r\":[%d,%d,%d,%d]", c.x1, c.y1, c.x2, c.y2);
    End();
  }

  // Tile reads in the map, its padding and past its rows (inside the tile array: y -1 of a column >= 32 is the row
  // 127 of the column 32 to the left, y 128 of a column < 224 the row 0 of the column 32 to the right).
  const Loc reads[] = {{32, 0}, {31, 5}, {0, 5}, {159, 5}, {160, 5}, {200, 5}, {255, 5}, {256, 5}, {288, 5},
                       {-1, 5}, {80, -1}, {80, 128}, {80, 129}, {48, -1}, {100, 127}, {100, 0}};
  for (Loc l : reads) {
    Begin("GetTile", {l.x, l.y});
    fprintf(g_out, ",\"r\":%d", GetTile(0, 0, l.x, l.y));
    End();
    Begin("GetCellType", {l.x, l.y});
    fprintf(g_out, ",\"r\":%d", GetCellType(0, 0, l.x, l.y));
    End();
  }

  // Tile writes past the rows and in the padding: the words at the spill tiles, before and after.
  struct Write { const char* name; SetFn fn; int x, y, v, sx, sy; };
  const Write writes[] = {
      {"SetCellType", SetCellType, 70, 128, 6, 102, 0},   {"SetCellType", SetCellType, 70, -1, 6, 38, 127},
      {"SetCellType", SetCellType, 31, 10, 6, 31, 10},    {"SetCellType", SetCellType, 160, 10, 6, 160, 10},
      {"SetCellType", SetCellType, 290, 12, 6, 34, 12},   {"SetTile", SetTile, 72, 128, 20, 104, 0},
      {"SetTile", SetTile, 74, -1, 20, 42, 127},          {"SetTile", SetTile, 290, 14, 20, 34, 14},
      {"SetLavaPossible", SetLavaPossible, 76, 128, 1, 108, 0}, {"SetLavaPossible", SetLavaPossible, 76, -1, 1, 44, 127},
      {"SetLavaPossible", SetLavaPossible, 31, 20, 1, 31, 20},  {"SetLavaPossible", SetLavaPossible, 160, 20, 1, 160, 20},
      {"SetLavaPossible", SetLavaPossible, 159, 20, 1, 159, 20},
  };
  for (const Write& w : writes) {
    const unsigned before = *Tile(w.sx, w.sy);
    w.fn(w.v, 0, w.x, w.y);
    Begin(w.name, {w.x, w.y, w.v});
    fprintf(g_out, ",\"spill\":[%d,%d],\"before\":%u,\"after\":%u", w.sx, w.sy, before, *Tile(w.sx, w.sy));
    End();
  }
  {
    const unsigned before = *Tile(110, 0);
    CreateWallOrTube(78, 128, 0, 0x11);
    Begin("CreateWallOrTube", {78, 128, 0, 0x11});
    fprintf(g_out, ",\"spill\":[110,0],\"before\":%u,\"after\":%u", before, *Tile(110, 0));
    End();
  }

  // Objects created at and past the edges.
  Snapshot();
  const int units[][5] = {
      // type, x, y, owner, rotation
      {0x0B, -5, -5, 0, 0},    {0x0B, 31, 10, 0, 0},    {0x0B, 160, 12, 0, 0},  {0x0B, 300, 14, 0, 0},
      {0x0B, 1262, 16, 0, 0},  {0x0B, 60, 128, 0, 0},   {0x0B, 62, 140, 0, 0},  {0x0B, 64, -1, 0, 0},
      {0x0B, 80, 40, 0, -1},   {0x0B, 82, 40, 0, 8},    {0x0B, 84, 40, 0, 9},   {0x0B, 86, 40, 0, -9},
      {0x27, 31, 60, 0, 0}, {0x27, 170, 62, 0, 0}, {0x27, 90, 140, 0, 0},
  };
  for (auto& u : units) {
    int id = 0;
    const int r = CreateUnit(&id, u[0], u[1], u[2], u[3], 0, u[4]);
    Begin("CreateUnit", {u[0], u[1], u[2], u[3], 0, u[4]});
    fprintf(g_out, ",\"r\":%d,\"id\":%d", r, id);
    NewObjects();
    End();
  }
  const int beacons[][3] = {{31, 70}, {200, 72}, {100, 140}, {102, -3}};
  for (auto& b : beacons) {
    const int r = CreateBeacon(0x51, b[0], b[1], 0, 1, -1);
    Begin("CreateBeacon", {0x51, b[0], b[1], 0, 1, -1});
    fprintf(g_out, ",\"r\":%d", r);
    NewObjects();
    End();
  }
  const int wrecks[][2] = {{31, 80}, {300, 82}, {110, 140}, {112, -3}};
  for (auto& w : wrecks) {
    const int r = CreateWreck(w[0], w[1], 8801, 0);
    Begin("CreateWreck", {w[0], w[1], 8801, 0});
    fprintf(g_out, ",\"r\":%d", r);
    NewObjects();
    End();
  }
  const int markers[][2] = {{31, 90}, {300, 92}, {120, 140}, {122, -3}};
  for (auto& m : markers) {
    int id = 0;
    const int r = PlaceMarker(&id, m[0], m[1], 0);
    Begin("PlaceMarker", {m[0], m[1], 0});
    fprintf(g_out, ",\"r\":%d,\"id\":%d", r, id);
    NewObjects();
    End();
  }
  const int quakes[][2] = {{40, 140}, {42, -9}, {300, 50}, {-40, 52}};
  for (auto& q : quakes) {
    SetMeteor(q[0], q[1], 0);
    Begin("SetMeteor", {q[0], q[1], 0});
    NewObjects();
    End();
    SetEarthquake(q[0], q[1], 0);
    Begin("SetEarthquake", {q[0], q[1], 0});
    NewObjects();
    End();
  }
  fflush(g_out);
}

}  // namespace

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int tick) {
  if (tick != 2) return;
  char path[MAX_PATH];
  const char* out = getenv("OP2H_OUT");
  snprintf(path, sizeof(path), "%s\\edges.jsonl", out ? out : ".");
  g_out = fopen(path, "w");
  if (!g_out) return;
  Probe();
  fclose(g_out);
  g_out = nullptr;
}
