// OP2 test harness: an op2ext ini module that, when OP2H_MISSION is set in the environment, skips the shell and
// starts that mission directly, optionally runs uncapped, writes JSONL telemetry and BMP screenshots every N ticks,
// replays a scripted command file, calls an optional bot DLL every tick, and exits after a tick limit.
// With OP2H_MISSION unset the module does nothing, so normal play is unaffected.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Tethys/API/API.h"
#include "Tethys/Game/TApp.h"
#include "Tethys/Game/GameImpl.h"
#include "Tethys/Game/Research.h"
#include "Tethys/Game/MapObjectType.h"
#include "Tethys/Game/MapImpl.h"
#include "Tethys/Game/MapObjectManager.h"
#include "Tethys/Game/PathFinder.h"
#include "Tethys/Game/Random.h"
#include "Tethys/UI/GameFrame.h"
#include "Tethys/Resource/CConfig.h"

using namespace Tethys;
using namespace TethysAPI;

namespace {

struct Config {
  std::string mission, out, script, bot;
  int ticks = 0, shotEvery = 0, telemEvery = 100, speed = 10, seed = 1, difficulty = 1, width = 1024, height = 768;
  int drawEvery = 0;  // Turbo only: render every N ticks (0 = only just before screenshots).
  int traceEvery = 0;  // trace.jsonl sample interval (0 = off).
  bool turbo = true;
} cfg;

std::string Env(const char* name, const char* def = "") {
  char buf[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string(def);
}
int EnvInt(const char* name, int def) { const auto s = Env(name);  return s.empty() ? def : atoi(s.c_str()); }

FILE* g_log   = nullptr;
FILE* g_telem = nullptr;

void Log(const char* fmt, ...) {
  if (!g_log) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(g_log, fmt, args);
  va_end(args);
  fputc('\n', g_log);
  fflush(g_log);
}

// ---------------------------------------------------------------------------------------------------------------------
// Code patching. Outpost2.exe always loads at 0x400000 (no relocations), so absolute addresses are stable.

bool WriteCode(uintptr_t addr, const void* data, size_t size) {
  DWORD old;
  if (!VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(reinterpret_cast<void*>(addr), data, size);
  VirtualProtect(reinterpret_cast<void*>(addr), size, old, &old);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
  return true;
}

// Redirects a rel32 CALL instruction to `target`; returns the previous callee (which may be another mod's hook).
void* RelinkCall(uintptr_t callAddr, void* target) {
  auto* p = reinterpret_cast<uint8_t*>(callAddr);
  if (p[0] != 0xE8) { Log("patch: no CALL at %08x", unsigned(callAddr));  return nullptr; }
  int32_t rel;
  memcpy(&rel, p + 1, 4);
  void* prev = reinterpret_cast<void*>(callAddr + 5 + rel);
  rel = int32_t(reinterpret_cast<uintptr_t>(target) - (callAddr + 5));
  uint8_t code[5] = { 0xE8 };
  memcpy(code + 1, &rel, 4);
  return WriteCode(callAddr, code, 5) ? prev : nullptr;
}

constexpr uintptr_t CallLoadDebugMap     = 0x486606;  // TApp::CreateFrames(): no-shell path calls GameImpl::LoadDebugMap()
constexpr uintptr_t CallProcessGameCycle = 0x49C4BD;  // GameFrame::OnIdle(): calls GameImpl::ProcessGameCycle()
constexpr uintptr_t OnIdleFrameLimiter   = 0x49C374;  // GameFrame::OnIdle(): `jae` taken when a tick is due
constexpr uintptr_t OnIdleDrawPanes      = 0x49C5FC;  // GameFrame::OnIdle(): detail/minimap/command pane OnIdle()
constexpr uint8_t   SkipDrawPanes[2]     = { 0xEB, 0x2B };  // jmp 0x49C629
uint8_t g_drawPanesCode[2];
bool    g_drawing = true;

// Rendering dominates the cost of a game cycle, so turbo runs skip it except right before a screenshot.
void SetDrawing(bool on) {
  if (on == g_drawing) return;
  WriteCode(OnIdleDrawPanes, on ? g_drawPanesCode : SkipDrawPanes, 2);
  g_drawing = on;
}

constexpr uintptr_t FnOP2MessageBox      = 0x41E0E0;  // int __fastcall(HWND, const char* msg, const char* title, uint flags)

void JumpTo(uintptr_t from, void* to) {
  uint8_t code[5] = { 0xE9 };
  const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(to) - (from + 5));
  memcpy(code + 1, &rel, 4);
  WriteCode(from, code, 5);
}

// The game's modal error/confirmation box would block an unattended run forever: log it and answer OK instead.
int __fastcall MessageBoxHook(HWND, const char* msg, const char* title, uint32_t) {
  Log("dialog: [%s] %s", title ? title : "", msg ? msg : "");
  return IDOK;
}

using GameCycleFn = void(__thiscall*)(GameImpl*);
GameCycleFn g_origGameCycle = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// Screenshots: copy the game window's client area into a 24-bit BMP.

void Screenshot(const char* path) {
  HWND hWnd = g_gameFrame.hWnd_;
  RECT rc;
  if (!hWnd || !GetClientRect(hWnd, &rc)) return;
  const int w = rc.right, h = rc.bottom;
  if (w <= 0 || h <= 0) return;

  HDC hdc = GetDC(hWnd), mem = CreateCompatibleDC(hdc);
  BITMAPINFO bi = {};
  bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, h, 1, 24, BI_RGB };
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
  GdiFlush();

  const DWORD stride = ((w * 3 + 3) & ~3), imageSize = stride * h;
  BITMAPFILEHEADER fh = { 0x4D42, DWORD(sizeof(fh) + sizeof(BITMAPINFOHEADER) + imageSize), 0, 0,
                          DWORD(sizeof(fh) + sizeof(BITMAPINFOHEADER)) };
  bi.bmiHeader.biSizeImage = imageSize;
  if (FILE* f = fopen(path, "wb")) {
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(bits, imageSize, 1, f);
    fclose(f);
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(hWnd, hdc);
}

// ---------------------------------------------------------------------------------------------------------------------
// Telemetry: one JSON object per line.

const char* CommandName(int c) {
  static const char* const names[] = {
    "Nop", "Doze", "Move", "Dock", "DockEG", "Stop", "Build", "BuildWall", "RemoveWall", "Produce", "TransferCargo",
    "LoadCargo", "UnloadCargo", "Recycle", "DumpCargo", "Scavenge", "SpecialWait", "Survey", "Idle", "Unidle",
    "SelfDestruct", "Scatter", "Research", "TrainScientists", "Transfer", "Launch", "FlyInSpace", "Repair", "RepairObj",
    "Reprogram", "Dismantle", "Salvage", "Create", "Develop", "UnDevelop", "LightToggle", "Attack", "Guard",
    "StandGround", "CargoRoute", "Patrol", "MapChange", "Poof", "GameOpt", "GodWeapon", "ChatText", "ChatSFX", "Death",
    "Chat", "Quit", "Ally", "GoAI", "MachineSettings", "InvalidCommand" };
  return (c >= 0 && c < int(std::size(names))) ? names[c] : "?";
}

const char* TypeName(MapID id) {
  if (id == MapID::None) return "None";
  auto* t = MapObjectType::GetInstance(int(id));
  return t ? t->GetCodeName() : "?";
}

void WriteUnit(FILE* f, bool& first, Unit u) {
  if (!u.IsLive()) return;
  const auto loc = u.GetLocation();
  fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"owner\":%d,\"x\":%d,\"y\":%d,\"hp\":%d,\"maxHp\":%d,\"cmd\":\"%s\"",
          first ? "" : ",", u.GetID(), TypeName(u.GetType()), u.GetOwner(), loc.x, loc.y,
          u.GetMaxHitpoints() - u.GetDamage(), u.GetMaxHitpoints(), CommandName(int(u.GetCommand())));
  if (const MapID w = u.GetWeapon(); u.IsVehicle() && w != MapID::None && int(w) > 0)
    fprintf(f, ",\"cargo\":\"%s\"", TypeName(w));
  fputc('}', f);
  first = false;
}

int g_lastTelemTick = -1;
bool g_inputUsed = false;  // The script has input verbs: telemetry adds the selection and the view.
void WriteInputState(FILE* f);

void WriteTelemetry(int tick) {
  FILE* f = g_telem;
  if (!f || tick == g_lastTelemTick) return;
  g_lastTelemTick = tick;
  auto& game = g_gameImpl;
  fprintf(f, "{\"tick\":%d,\"mark\":%d,\"end\":%d,\"players\":[", tick, tick / 100, int(game.gameTermReasons_));
  for (int i = 0; i < game.numPlayers_; ++i) {
    PlayerImpl* p = game.GetPlayer(i);
    const auto& m = p->moraleState_;
    fprintf(f, "%s{\"id\":%d,\"human\":%d,\"eden\":%d,\"food\":%d,\"commonOre\":%d,\"rareOre\":%d,\"workers\":%d,"
               "\"scientists\":%d,\"kids\":%d,\"morale\":%d,\"moraleLevel\":%d,\"powerGen\":%d,\"powerUsed\":%d,"
               "\"foodNet\":%d,\"buildings\":%d,\"idleBuildings\":%d,\"techs\":[",
            i ? "," : "", i, p->isHuman_ != 0, p->isEden_ != 0, p->foodStored_, p->commonOre_, p->rareOre_,
            p->numWorkers_, p->numScientists_, p->numKids_, m.morale, int(m.moraleLevel), p->amountPowerGenerated_,
            p->amountPowerConsumed_, p->netFoodProduction_, p->numBuildings_, p->numIdleBuildings_);
    bool first = true;
    for (int t = 0; t < g_research.numTechs_; ++t) {
      if (g_research.HasTech(i, t)) { fprintf(f, "%s%d", first ? "" : ",", g_research.ppTechInfos_[t]->techID);  first = false; }
    }
    fputs("]}", f);
  }
  fputs("],\"units\":[", f);
  bool first = true;
  for (int i = 0; i < game.numPlayers_; ++i) {
    for (Unit u : PlayerBuildingEnum(i)) WriteUnit(f, first, u);
    for (Unit u : PlayerVehicleEnum(i))  WriteUnit(f, first, u);
  }
  fputc(']', f);
  if (g_inputUsed) WriteInputState(f);
  fputs("}\n", f);
  fflush(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Map coordinates. Internal tile x = file tile x + padding (32 on non-wrapping maps, 0 on 512-wide world maps);
// internal y = file y. Internal pixel = internal tile * 32 + offset. See README "Coordinates".

int PadTiles() { return GameMap::GetPaddingWidth(); }

// ---------------------------------------------------------------------------------------------------------------------
// Trace: one JSONL line per sampled tick with sub-tile positions read straight from the MapObjects (ENGINE_PLAN trace
// format). Units spawned by the script are listed first, in spawn order, with their spawn index.

FILE* g_trace     = nullptr;
bool  g_traceRaw  = false;    // Also dump raw MapObject/PathContext bytes (for reverse engineering).
std::vector<int> g_spawned;   // Spawn index -> unit ID.
// Spawns that have died. The game recycles object indices, so a dead spawn's ID may later belong to something else.
std::vector<bool> g_spawnGone;
std::vector<MapID> g_spawnType;

const char* ActionName(int a) {
  static const char* const names[] = {
    "Done", "Move", "Build", "BuildMine", "Repair", "Develop", "ObjWait", "OperationalWait", "EMPWait", "Landing",
    "ObjFroze", "DelSelf", "DoResearch", "ObjDocking", "Scavenge", "WeaponMove", "WeatherMove", "EMPRecover",
    "WeaponAimCoarse", "WeaponAimFine", "SantaWalking", "Invalid" };
  return (a >= 0 && a < int(std::size(names))) ? names[a] : "?";
}

void Hex(FILE* f, const void* p, size_t n) {
  for (size_t i = 0; i < n; ++i) fprintf(f, "%02x", reinterpret_cast<const uint8_t*>(p)[i]);
}

void WriteTraceUnit(FILE* f, bool& first, Unit u, int spawnIndex) {
  if (!u.IsLive()) return;
  const MapObject* mo = u.GetMapObject();
  const int pad = PadTiles() * 32;
  const PathContext* pc = u.IsVehicle() ? mo->pPathContext_ : nullptr;
  if (pc && IsBadReadPtr(pc, sizeof(PathContext))) pc = nullptr;
  // Sub-pixel position: while a vehicle steps between tiles, PathContext::numPixelsMoved{X,Y} hold the 16-bit fraction
  // of a 16.16 fixed-point position, biased so 0x8000 means "exactly on pixelX_". They read 0 before the first step.
  // fixX/fixY are that position in exact 16.16 fixed point (file-relative), sx/sy the same as floats; px/py are the
  // game's own integer pixelX_/pixelY_ (the sub-pixel position rounded to nearest).
  int64_t fixX = int64_t(mo->pixelX_ - pad) << 16, fixY = int64_t(mo->pixelY_) << 16;
  if (pc && (pc->numPixelsMovedX | pc->numPixelsMovedY)) {
    fixX += int(pc->numPixelsMovedX) - 0x8000;
    fixY += int(pc->numPixelsMovedY) - 0x8000;
  }
  const double fx = fixX / 65536.0, fy = fixY / 65536.0;
  fprintf(f, "%s{\"id\":%d,", first ? "" : ",", u.GetID());
  if (spawnIndex >= 0) fprintf(f, "\"spawn\":%d,", spawnIndex);
  fprintf(f, "\"type\":\"%s\",\"owner\":%d,\"px\":%d,\"py\":%d,\"sx\":%.6f,\"sy\":%.6f,\"fix\":[%lld,%lld],\"dir\":%d,\"hp\":%d,\"cmd\":\"%s\",\"action\":\"%s\","
             "\"busy\":%d,\"ipx\":%d,\"ipy\":%d,\"frame\":%d,\"anim\":%d",
          TypeName(u.GetType()), u.GetOwner(), mo->pixelX_ - pad, mo->pixelY_, fx, fy, (long long)fixX, (long long)fixY, mo->rotation_,
          u.GetMaxHitpoints() - u.GetDamage(), CommandName(mo->command_), ActionName(int(mo->action_)), mo->isBusy_,
          mo->pixelX_, mo->pixelY_, mo->frameIndex_, mo->animationIndex_);
  if (u.IsVehicle()) {
    if (pc) {
      fprintf(f, ",\"dest\":[%d,%d],\"path\":{\"start\":[%d,%d],\"moved\":[%d,%d],\"toMove\":[%d,%d],\"rotSpeed\":%d,"
                 "\"wp\":%d,\"numWp\":%d,\"pt\":%d,\"numPts\":%d,\"steps\":\"",
              pc->dstTileX - PadTiles(), pc->dstTileY, pc->startPixelX - pad, pc->startPixelY, pc->numPixelsMovedX,
              pc->numPixelsMovedY, pc->numPixelsToMoveX, pc->numPixelsToMoveY, pc->rotationSpeed,
              pc->currentWaypointIndex, pc->numWaypoints, pc->currentPathfinderPoint, pc->numPathfinderPoints);
      // Remaining planned steps of the current path fragment, one UnitDirection digit (0 = E, clockwise) per tile.
      for (int i = pc->currentPathfinderPoint; i >= 0 && i < pc->numPathfinderPoints && i < 64; ++i)
        fputc('0' + (pc->direction[i] & 7), f);
      fputs("\"}", f);
      if (g_traceRaw) { fputs(",\"rawPath\":\"", f);  Hex(f, pc, 0xD4);  fputc('"', f); }
    }
  }
  // Turret state of armed units (tanks, guard posts). Offsets learned from Outpost2.exe: the turret rotation is the
  // byte at +0x2D (TankVehicle::Draw 0x484E5A passes it to SelectTurretGraphic; the aim steps at 0x43E6C5 slew it),
  // +0x2E is reloadTimer_, and +0x4C is the weapon target (low word = unit index when the high word is 0xFFFF,
  // high word 0x7FFF = none; else a ground tile), used by the fire handlers at 0x4A7650..0x4A7BC0.
  if (const int w = mo->weapon_; w >= int(MapID::AcidCloud) && w <= int(MapID::BFG) && (u.IsVehicle() || u.IsBuilding())) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(mo);
    const uint16_t tlo = *reinterpret_cast<const uint16_t*>(b + 0x4C), thi = *reinterpret_cast<const uint16_t*>(b + 0x4E);
    fprintf(f, ",\"weapon\":\"%s\",\"tur\":%d,\"reload\":%d,\"tgt\":", TypeName(MapID(w)), b[0x2D], mo->reloadTimer_);
    if (thi == 0x7FFF)      fputs("null", f);
    else if (thi == 0xFFFF) fprintf(f, "%d", int(tlo));
    else                    fprintf(f, "[%d,%d]", int(int16_t(tlo)) - PadTiles(), int(int16_t(thi)));
  }
  // Tiles that hold this unit's index (TileData::unitIndex) within 3 tiles of its pixel, file coordinates: a moving
  // vehicle's reserved tile, a building's footprint.
  fputs(",\"tiles\":[", f);
  bool firstTile = true;
  for (int dy = -3; dy <= 3; ++dy)
    for (int dx = -3; dx <= 3; ++dx) {
      int x = mo->pixelX_ / 32 + dx;
      if (PadTiles() == 0) x = (x + GameMap::GetWidth()) % GameMap::GetWidth();  // 512-wide maps wrap
      const Location at(x, mo->pixelY_ / 32 + dy);
      if (at.x < 0 || at.y < 0 || at.y >= GameMap::GetHeight() || GameMap::GetUnitOnTile(at).GetID() != u.GetID()) continue;
      fprintf(f, "%s[%d,%d]", firstTile ? "" : ",", at.x - PadTiles(), at.y);
      firstTile = false;
    }
  fputc(']', f);
  if (g_traceRaw) { fputs(",\"raw\":\"", f);  Hex(f, mo, 0x80);  fputc('"', f); }
  fputc('}', f);
  first = false;
}

// Live weapon-fire entities (lasers, projectiles: MapChildEntity, flag bit 8; bit 6 is not set on them) whose parent is a traced unit. Their action shows
// the aiming phases (WeaponAimCoarse tracks the target, WeaponAimFine holds the fire bearing, see 0x43E898).
void WriteTraceFx(FILE* f) {
  MapImpl* map = MapImpl::GetInstance();
  bool first = true;
  fputs(",\"fx\":[", f);
  for (size_t i = 1; i < map->MaxNumUnits(); ++i) {
    const MapObject* mo = MapObject::GetInstance(int(i));
    if (!mo || !mo->IsLive() || !(mo->flags_ & MoFlagEntChild)) continue;
    if (const int t = int(mo->GetTypeID()); t < int(MapID::AcidCloud) || t > int(MapID::BFG)) continue;
    const auto* ce = static_cast<const MapChildEntity*>(mo);
    int spawn = -1;
    for (size_t k = 0; k < g_spawned.size(); ++k) if (g_spawned[k] == ce->parentIndex_) spawn = int(k);
    if (spawn < 0) continue;
    fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"parent\":%d,\"action\":\"%s\",\"dir\":%d,\"px\":%d,\"py\":%d,\"timer\":%d}",
            first ? "" : ",", int(i), TypeName(mo->GetTypeID()), spawn, ActionName(int(mo->action_)), mo->rotation_,
            mo->pixelX_ - PadTiles() * 32, mo->pixelY_, mo->actionTimer_);
    first = false;
  }
  fputc(']', f);
  // Every other live object (explosions, death animations, other weapon fire, unspawned units) as
  // [index, type, action, timer, px, py]: they
  // hold object indices, which decide the heartbeat phase of objects created later (0x439A10).
  fputs(",\"ents\":[", f);
  first = true;
  for (size_t i = 1; i < map->MaxNumUnits(); ++i) {
    const MapObject* mo = MapObject::GetInstance(int(i));
    if (!mo || !mo->IsLive()) continue;
    if (std::find(g_spawned.begin(), g_spawned.end(), int(i)) != g_spawned.end()) continue;
    if (mo->flags_ & MoFlagEntChild) {
      const auto* ce = static_cast<const MapChildEntity*>(mo);
      if (const int t = int(mo->GetTypeID()); t >= int(MapID::AcidCloud) && t <= int(MapID::BFG))
        if (std::find(g_spawned.begin(), g_spawned.end(), ce->parentIndex_) != g_spawned.end()) continue;
    }
    fprintf(f, "%s[%d,\"%s\",\"%s\",%d,%d,%d]", first ? "" : ",", int(i), TypeName(mo->GetTypeID()),
            ActionName(int(mo->action_)), mo->actionTimer_, mo->pixelX_ - PadTiles() * 32, mo->pixelY_);
    first = false;
  }
  fputc(']', f);
}

int g_lastTraceTick = -1;

void WriteTrace(int tick) {
  FILE* f = g_trace;
  if (!f || tick == g_lastTraceTick) return;
  g_lastTraceTick = tick;
  fprintf(f, "{\"tick\":%d,\"units\":[", tick);
  bool first = true;
  for (size_t i = 0; i < g_spawned.size(); ++i) {
    const Unit u(g_spawned[i]);
    if (!g_spawnGone[i] && (!u.IsLive() || u.GetType() != g_spawnType[i])) g_spawnGone[i] = true;
    if (!g_spawnGone[i]) WriteTraceUnit(f, first, u, int(i));
  }
  if (g_spawned.empty()) {  // No scenario spawns: trace every vehicle.
    for (int p = 0; p < g_gameImpl.numPlayers_; ++p)
      for (Unit u : PlayerVehicleEnum(p)) WriteTraceUnit(f, first, u, -1);
  }
  fputc(']', f);
  WriteTraceFx(f);
  // Object index allocator (0x439A10): highest index handed out so far (0x54F838) and the ring of freed objects
  // (0x54F844, head 0x54F840, tail 0x54F83C) as [index, tick freed, reference count +0x32].
  {
    const int last = *reinterpret_cast<const int*>(0x54F838), head = *reinterpret_cast<const int*>(0x54F840),
              tail = *reinterpret_cast<const int*>(0x54F83C);
    const uint8_t* const* ring = *reinterpret_cast<const uint8_t* const* const*>(0x54F844);
    fprintf(f, ",\"alloc\":[%d,[", last);
    for (int k = head, n = 0; k != tail && n < 64; k = (k + 1) & 0x3FF, ++n) {
      const uint8_t* o = ring[k];
      fprintf(f, "%s[%d,%d,%d]", n ? "," : "", *reinterpret_cast<const int*>(o + 0x10), *reinterpret_cast<const int*>(o + 0x8), o[0x32]);
    }
    fputs("]]", f);
  }
  // Main gameplay RNG state (two 32-bit LFSRs at 0x56BE20, stepped by Random::Rand 0x46F000/0x46F060).
  const uint64 seed = g_gameRNG.GetSeed();
  fprintf(f, ",\"rng\":[%u,%u]}\n", unsigned(seed & 0xFFFFFFFF), unsigned(seed >> 32));
  fflush(f);
}

// Game constants the engine reimplements: map geometry, cell type speed table, and unit type stats, as loaded by the
// game (sheets + patches), written once at the first game cycle.
void WriteGameData(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  MapImpl* map = MapImpl::GetInstance();
  fprintf(f, "{\"map\":{\"width\":%d,\"height\":%d,\"paddedWidth\":%d,\"paddingTiles\":%d,\"tileXMask\":%u,"
             "\"pixelWidth\":%d,\"clipRect\":[%d,%d,%d,%d]},\n",
          GameMap::GetWidth(), GameMap::GetHeight(), map->tileWidth_, map->paddingOffsetTileX_, map->tileXMask_,
          map->pixelWidth_, map->clipRect_.x1, map->clipRect_.y1, map->clipRect_.x2, map->clipRect_.y2);
  fputs("\"trackTypes\":[\"Legged\",\"Wheeled\",\"Miner\",\"Tracked\",\"Hover\"],\n\"cellTypes\":[", f);
  for (int c = 0; c < int(CellType::Count); ++c) {
    const CellTypeInfo* ci = MapImpl::GetCellTypeInfo(CellType(c));
    fprintf(f, "%s\n {\"id\":%d,\"name\":\"%s\",\"trackSpeed\":[", c ? "," : "", c, ci && ci->pName ? ci->pName : "");
    for (int t = 0; ci && t < int(TrackType::Count); ++t) fprintf(f, "%s%d", t ? "," : "", ci->trackSpeed[t]);
    fprintf(f, "],\"blocksLineOfSight\":%d,\"blightSpeed\":%d}", ci ? ci->blocksLineOfSight : 0, ci ? ci->blightSpeed : 0);
  }
  fputs("],\n\"unitTypes\":[", f);
  bool first = true;
  for (size_t i = 1; i < MapObjectManager::NumMapObjTypes(); ++i) {
    MapObjectType* t = MapObjectType::GetInstance(int(i));
    if (!t) continue;
    const auto& s = t->playerStats_[0];
    fprintf(f, "%s\n {\"mapId\":%u,\"code\":\"%s\",\"name\":\"%s\",\"track\":%d,\"hp\":%d,\"armor\":%d,\"sight\":%d,"
               "\"moveSpeed\":%d,\"turnRate\":%d,\"reload\":%d,\"vehicleFlags\":%u",
            first ? "" : ",", unsigned(i), t->GetCodeName(), t->unitName_, int(t->trackType_), s.hp, int(s.armor),
            s.sightRange, s.vehicle.moveSpeed, s.vehicle.turnRate, s.vehicle.reloadTime, t->stats_.vehicle.flags);
    // Raw per-player stats words (PerPlayerUnitStats, 17 ints) so weapon fields can be read by offset: for weapons
    // +0x18 is the range in tiles, +0x24 the turret turn rate (aim step 0x43E76C), +0x30 the reload time (0x4A7DA7).
    fputs(",\"stats\":[", f);
    for (int k = 0; k < 17; ++k) fprintf(f, "%s%d", k ? "," : "", reinterpret_cast<const int*>(&s)[k]);
    fputc(']', f);
    // Buildings: footprint in tiles (GlobalUnitStats::building).
    if (i >= size_t(MapID::CommonOreMine) && i <= size_t(MapID::Tokamak))
      fprintf(f, ",\"size\":[%d,%d],\"dock\":[%d,%d],\"rubble\":[%d,%d],\"buildingFlags\":%u", t->stats_.building.width,
              t->stats_.building.height, t->stats_.building.edenDockLocation, t->stats_.building.plyDockLocation,
              t->stats_.building.commonRubble, t->stats_.building.rareRubble, t->stats_.building.flags);
    // Weapons: the shortest shot in pixels (type +0x24C, read by DoFire 0x4A3F8A) and the damage radius in pixels
    // (type +0x248, DoDamage 0x4A36C3); +0x250 is 1 for weapons that don't need line of sight (IsInRange 0x485297).
    if (i >= size_t(MapID::AcidCloud) && i <= size_t(MapID::BFG))
      fprintf(f, ",\"pixelsSkipped\":%d,\"radius\":%d,\"indirect\":%d",
              *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(t) + 0x24C),
              *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(t) + 0x248),
              *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(t) + 0x250));
    fputc('}', f);
    first = false;
  }
  // Cell type of every map tile in file coordinates (row-major), to check the coordinate conversion against the .map.
  fputs("],\n\"cells\":[", f);
  for (int y = 0; y < GameMap::GetHeight(); ++y)
    for (int x = 0; x < GameMap::GetWidth(); ++x)
      fprintf(f, "%s%d", (x | y) ? "," : "", int(GameMap::GetCellType(Location(x + PadTiles(), y))));
  fputs("]}\n", f);
  fclose(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scripted input. One command per line: `<tick> <verb> <args...>`; '#' starts a comment.
//   move <unit> <x> <y> | attack <unit> <target> | stop <unit> | selfdestruct <unit>
//   research <labId> <techId> <scientists> | shot | telemetry | trace | quit
//   spawn <TYPE> <owner> <x> <y> [weapon|-] [dir 0-255]   (TYPE/weapon are sheet code names, e.g. LIGHT_TANK LASER)
//   setcell <x1> <y1> <x2> <y2> <cellTypeId>                 (overwrite terrain cell types in a rect, inclusive)
//   coords file|internal                                     (how later x/y are read; default internal)
//   view <x> <y>                                             (centre player 0's view on a tile, for screenshots)
// A <unit> is an internal unit ID as telemetry reports it, or @N for the unit spawned by the Nth spawn command.

struct ScriptCmd { int tick;  std::string verb;  std::vector<std::string> args;  bool fileCoords; };
std::vector<ScriptCmd> g_script;
size_t g_scriptPos = 0;

bool IsInputVerb(const std::string& v);

void LoadScript(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) { Log("script: cannot open %s", path.c_str());  return; }
  char line[512];
  bool fileCoords = false;
  while (fgets(line, sizeof(line), f)) {
    if (char* hash = strchr(line, '#')) *hash = 0;
    std::vector<std::string> tok;
    for (char* t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) tok.emplace_back(t);
    if (tok.size() < 2) continue;
    if (tok[1] == "coords") { fileCoords = tok.size() > 2 && tok[2] == "file";  continue; }
    g_script.push_back({ atoi(tok[0].c_str()), tok[1], std::vector<std::string>(tok.begin() + 2, tok.end()), fileCoords });
  }
  fclose(f);
  std::stable_sort(g_script.begin(), g_script.end(), [](auto& l, auto& r) { return l.tick < r.tick; });
  for (const auto& c : g_script) g_inputUsed |= IsInputVerb(c.verb);
  Log("script: %u commands from %s", unsigned(g_script.size()), path.c_str());
}

void Quit(const char* why);
void TakeShot(int tick);

// ---------------------------------------------------------------------------------------------------------------------
// Virtual mouse and keyboard for the input verbs (see README "Mouse and keyboard input").
//
// Input is delivered in process, right after a game cycle, the same way TApp::PumpMessage (0x4867B0) would deliver it:
// mouse messages go straight to the pane window under the cursor (or the one holding capture), key and character
// messages go through GameFrame::TranslateAccelerators (0x49B3B0) and IDlgWnd::PretranslateModeless before being
// dispatched. Everything is synchronous, so an input at tick T is fully handled before cycle T+1 runs.
// The game also polls the mouse and keys: GetCursorPos (edge scrolling 0x407BBA, the cursor shape, mouse-over) and
// GetAsyncKeyState (Shift/Ctrl in MouseCommandFilter 0x41B50D/0x41BA60, arrow-key scrolling 0x407BCE). Their IAT slots
// are pointed at this virtual state, so nothing depends on the X server's pointer.

constexpr uintptr_t IatGetCursorPos      = 0x583898;
constexpr uintptr_t IatGetAsyncKeyState  = 0x58389C;
constexpr uintptr_t SelectedCount        = 0x547210;  // Selected units: count, then up to 32 unit indices (0x41ADF0).
constexpr uintptr_t SelectedUnits        = 0x547214;

struct VirtualInput {
  bool    active = false;  // Set once the cursor is parked at the first cycle; until then the real functions answer.
  POINT   pos    = {};     // Client coordinates of the game window (the same pixels as screenshots).
  uint8_t keys[256] = {};  // Held keys, by virtual-key code (VK_LBUTTON/VK_RBUTTON/VK_MBUTTON for mouse buttons).
} g_in;
int  g_lastInputTick = -1000;

using GetCursorPosFn     = BOOL(WINAPI*)(LPPOINT);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
GetCursorPosFn     g_realGetCursorPos     = nullptr;
GetAsyncKeyStateFn g_realGetAsyncKeyState = nullptr;

BOOL WINAPI VirtualGetCursorPos(LPPOINT p) {
  if (!g_in.active) return g_realGetCursorPos(p);
  if (!p) return FALSE;
  *p = g_in.pos;
  ClientToScreen(g_gameFrame.hWnd_, p);
  return TRUE;
}

SHORT WINAPI VirtualGetAsyncKeyState(int vk) {
  if (!g_in.active) return g_realGetAsyncKeyState(vk);
  return (vk >= 0 && vk < 256 && g_in.keys[vk]) ? SHORT(0x8000) : 0;
}

void* PatchIat(uintptr_t slot, void* fn) {
  void* prev = *reinterpret_cast<void**>(slot);
  WriteCode(slot, &fn, sizeof(fn));
  return prev;
}

// TPane::GetAbsolutePos (vtable slot 10): pane client point -> internal map pixel. Returns 0 outside the map.
int PaneToMap(TPane* pane, int x, int y, POINT* out) {
  using Fn = int(__thiscall*)(TPane*, int, int, POINT*);
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(pane))[10])(pane, x, y, out);
}

// The pane's top-left corner in game window client coordinates.
POINT PaneOrigin(const IWnd& pane) {
  POINT p = {0, 0};
  MapWindowPoints(pane.hWnd_, g_gameFrame.hWnd_, &p, 1);
  return p;
}

TPane* PaneAt(POINT client) {
  TPane* panes[] = { &g_gameFrame.detailPane_, &g_gameFrame.miniMapPane_, &g_gameFrame.commandPane_ };
  for (TPane* p : panes) {
    RECT rc;
    if (!p->hWnd_ || !GetClientRect(p->hWnd_, &rc)) continue;
    const POINT o = PaneOrigin(*p);
    if (client.x >= o.x && client.y >= o.y && client.x < o.x + rc.right && client.y < o.y + rc.bottom) return p;
  }
  return nullptr;
}

const char* PaneName(const IWnd* p) {
  return p == &g_gameFrame.detailPane_ ? "detail" : p == &g_gameFrame.miniMapPane_ ? "minimap" :
         p == &g_gameFrame.commandPane_ ? "command" : "frame";
}

// Internal map pixel -> game window client point through the detail pane's current view. The view origin comes from the
// game's own pane->map conversion, so zoom-free 1:1 pixels are the only assumption (checked in the log).
POINT MapToClient(int mapX, int mapY) {
  TPane* dp = &g_gameFrame.detailPane_;
  POINT o0 = {}, o1 = {};
  PaneToMap(dp, 0, 0, &o0);
  PaneToMap(dp, 64, 64, &o1);
  const int scale = std::max(1L, (o1.x - o0.x) / 64);  // 1 at normal zoom.
  int dx = mapX - o0.x;
  if (PadTiles() == 0) {  // World maps wrap: take the nearest copy.
    const int w = GameMap::GetWidth() * 32;
    dx = ((dx % w) + w + w / 2) % w - w / 2;
  }
  const POINT org = PaneOrigin(*dp);
  return { org.x + dx / scale, org.y + (mapY - o0.y) / scale };
}

// Finds the minimap pane point that shows internal tile `t`, searching the pane with MiniMap::TranslateCoordinates
// (0x48D930): the minimap's scale and centring depend on the map size and zoom.
bool MinimapToClient(Location t, POINT* out) {
  TPane* mp = &g_gameFrame.miniMapPane_;
  RECT rc;
  if (!mp->hWnd_ || !GetClientRect(mp->hWnd_, &rc)) return false;
  const int wantX = t.x * 32 + 16, wantY = t.y * 32 + 16;
  long best = LONG_MAX;
  for (int y = 0; y < rc.bottom; ++y)
    for (int x = 0; x < rc.right; ++x) {
      POINT m;
      if (!g_miniMap.TranslateCoordinates({ x, y }, &m)) continue;  // What the minimap pane's click handler calls.
      const long d = labs(m.x - wantX) + labs(m.y - wantY);
      if (d < best) { best = d;  *out = { x, y }; }
    }
  if (best == LONG_MAX) return false;
  const POINT o = PaneOrigin(*mp);
  out->x += o.x;
  out->y += o.y;
  return true;
}

// UI controls of the minimap and command panes (TPane::pControls_): UIElement objects with pane-relative rectangles.
// Buttons (UIButton::GetHelpText at vtable slot 7, checked against the known implementations) have a tooltip text.
std::string ControlHelp(UIElement* e) {
  void** vtbl = *reinterpret_cast<void***>(e);
  const uintptr_t f = reinterpret_cast<uintptr_t>(vtbl[7]);
  if (f != 0x40B2B0 && f != 0x462940) return std::string();  // UIGraphicalButton, BayButton
  char buf[256] = {};
  using Fn = void(__thiscall*)(UIElement*, char*, int);
  reinterpret_cast<Fn>(vtbl[7])(e, buf, sizeof(buf) - 1);
  return buf;
}

struct UiControl { TPane* pane;  int index;  UIElement* e;  RECT rc;  std::string help; };

std::vector<UiControl> UiControls() {
  std::vector<UiControl> out;
  TPane* panes[] = { &g_gameFrame.miniMapPane_, &g_gameFrame.commandPane_ };
  for (TPane* p : panes) {
    const POINT o = PaneOrigin(*p);
    for (int i = 0; i < p->numControls_ && i < 20; ++i) {
      auto* e = static_cast<UIElement*>(p->pControls_[i]);
      if (!e || IsBadReadPtr(e, sizeof(UIElement))) continue;
      RECT rc = e->position_;
      OffsetRect(&rc, o.x, o.y);
      out.push_back({ p, i, e, rc, ControlHelp(e) });
    }
  }
  return out;
}

void LogUiControls() {
  for (const auto& c : UiControls())
    Log("ui: %s[%d] (%ld,%ld)-(%ld,%ld) flags %x \"%s\"", PaneName(c.pane), c.index, c.rc.left, c.rc.top, c.rc.right,
        c.rc.bottom, unsigned(c.e->flags_), c.help.c_str());
}

// `name`: "tabN" (command pane report tab N, 0 = top), "minimapN" (minimap button N), or a case-insensitive substring of
// a control's tooltip with '_' for spaces (e.g. "Mission_Objectives").
bool UiToClient(const std::string& name, POINT* out) {
  auto centre = [&](const IWnd& pane, const RECT& r) {
    const POINT o = PaneOrigin(pane);
    *out = { o.x + (r.left + r.right) / 2, o.y + (r.top + r.bottom) / 2 };
    return true;
  };
  if (name.size() == 4 && _strnicmp(name.c_str(), "tab", 3) == 0 && name[3] >= '0' && name[3] <= '5')
    return centre(g_gameFrame.commandPane_, g_gameFrame.commandPane_.reportButtons_[name[3] - '0'].position_);
  if (name.size() == 8 && _strnicmp(name.c_str(), "minimap", 7) == 0 && name[7] >= '0' && name[7] <= '3')
    return centre(g_gameFrame.miniMapPane_, g_gameFrame.miniMapPane_.buttons_[name[7] - '0'].position_);
  std::string want = name;
  for (char& ch : want) ch = ch == '_' ? ' ' : char(tolower(ch));
  for (const auto& c : UiControls()) {
    std::string h = c.help;
    for (char& ch : h) ch = char(tolower(ch));
    if (!want.empty() && h.find(want) != std::string::npos) {
      *out = { (c.rc.left + c.rc.right) / 2, (c.rc.top + c.rc.bottom) / 2 };
      return true;
    }
  }
  return false;
}

// Telemetry extension for input runs: the selected unit IDs and the detail pane's top-left corner in file pixels.
void WriteInputState(FILE* f) {
  const int n = std::min(*reinterpret_cast<const int*>(SelectedCount), 32);
  fputs(",\"selected\":[", f);
  for (int k = 0; k < n; ++k) fprintf(f, "%s%d", k ? "," : "", reinterpret_cast<const int*>(SelectedUnits)[k]);
  POINT o = {};
  PaneToMap(&g_gameFrame.detailPane_, 0, 0, &o);
  fprintf(f, "],\"view\":[%ld,%ld]", o.x - PadTiles() * 32, o.y);
}

WPARAM MouseKeyFlags() {
  return (g_in.keys[VK_LBUTTON] ? MK_LBUTTON : 0) | (g_in.keys[VK_RBUTTON] ? MK_RBUTTON : 0) |
         (g_in.keys[VK_MBUTTON] ? MK_MBUTTON : 0) | (g_in.keys[VK_SHIFT] ? MK_SHIFT : 0) |
         (g_in.keys[VK_CONTROL] ? MK_CONTROL : 0);
}

// Mirrors the held keys into the thread's keyboard state, which TranslateAccelerator reads through GetKeyState.
void SyncKeyboardState() {
  BYTE ks[256] = {};
  for (int vk = 0; vk < 256; ++vk) if (g_in.keys[vk]) ks[vk] = 0x80;
  ks[VK_LSHIFT] = ks[VK_SHIFT];  ks[VK_LCONTROL] = ks[VK_CONTROL];  ks[VK_LMENU] = ks[VK_MENU];
  SetKeyboardState(ks);
}

// Sends a mouse message at client point `at`, to the pane holding capture or else the pane under the point.
void SendMouse(UINT msg, POINT at) {
  g_in.pos = at;
  HWND target = GetCapture();
  if (!target || (target != g_gameFrame.detailPane_.hWnd_ && target != g_gameFrame.miniMapPane_.hWnd_ &&
                  target != g_gameFrame.commandPane_.hWnd_)) {
    TPane* p = PaneAt(at);
    target = p ? p->hWnd_ : g_gameFrame.hWnd_;
  }
  POINT local = at;
  MapWindowPoints(g_gameFrame.hWnd_, target, &local, 1);
  SendMessageA(target, msg, MouseKeyFlags(), MAKELPARAM(int16_t(local.x), int16_t(local.y)));
}

// One message through the same steps as TApp::PumpMessage. Returns true when an accelerator took it.
bool PumpOne(UINT msg, WPARAM w, LPARAM l) {
  MSG m = {};
  m.hwnd    = g_gameFrame.hWnd_;  // The frame has the keyboard focus in play.
  m.message = msg;
  m.wParam  = w;
  m.lParam  = l;
  m.time    = DWORD(g_gameImpl.tick_);
  m.pt      = g_in.pos;
  ClientToScreen(g_gameFrame.hWnd_, &m.pt);
  if (g_gameFrame.TranslateAccelerators(&m)) return true;
  if (IDlgWnd::PretranslateModeless(&m)) return false;
  DispatchMessageA(&m);
  return false;
}

struct KeySpec { int vk = 0;  bool shift = false, ctrl = false, alt = false; };

// "a", "7", "enter", "f5", "ctrl+1", "alt+c", "shift+tab", "vk:0x41" ...
bool ParseKey(std::string s, KeySpec* k) {
  for (;;) {
    const size_t plus = s.find('+', 1);
    if (plus == std::string::npos) break;
    const std::string mod = s.substr(0, plus);
    if      (_stricmp(mod.c_str(), "shift") == 0) k->shift = true;
    else if (_stricmp(mod.c_str(), "ctrl") == 0)  k->ctrl = true;
    else if (_stricmp(mod.c_str(), "alt") == 0)   k->alt = true;
    else return false;
    s = s.substr(plus + 1);
  }
  static const std::pair<const char*, int> names[] = {
    {"enter", VK_RETURN}, {"return", VK_RETURN}, {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE}, {"space", VK_SPACE},
    {"tab", VK_TAB}, {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"insert", VK_INSERT}, {"home", VK_HOME},
    {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"left", VK_LEFT}, {"right", VK_RIGHT},
    {"up", VK_UP}, {"down", VK_DOWN}, {"pause", VK_PAUSE}, {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL},
    {"alt", VK_MENU}, {"numplus", VK_ADD}, {"numminus", VK_SUBTRACT}, {"plus", VK_OEM_PLUS}, {"minus", VK_OEM_MINUS} };
  for (const auto& [n, vk] : names) if (_stricmp(s.c_str(), n) == 0) { k->vk = vk;  return true; }
  if ((s[0] == 'f' || s[0] == 'F') && s.size() > 1 && isdigit(uint8_t(s[1]))) {
    const int n = atoi(s.c_str() + 1);
    if (n >= 1 && n <= 12) { k->vk = VK_F1 + n - 1;  return true; }
  }
  if (_strnicmp(s.c_str(), "vk:", 3) == 0) { k->vk = int(strtol(s.c_str() + 3, nullptr, 0)) & 0xFF;  return true; }
  if (s.size() == 1) {
    const SHORT v = VkKeyScanA(s[0]);
    if (v == -1) return false;
    k->vk = v & 0xFF;
    k->shift |= (v & 0x100) != 0;
    return true;
  }
  return false;
}

void KeyEvent(int vk, bool down) {
  const bool sys = g_in.keys[VK_MENU] || vk == VK_MENU || vk == VK_F10;
  g_in.keys[vk] = down;
  if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
    g_in.keys[vk == VK_SHIFT ? VK_LSHIFT : vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU] = down;
  }
  SyncKeyboardState();
  const UINT scan = MapVirtualKeyA(vk, 0);
  const LPARAM l = 1 | (LPARAM(scan) << 16) | (sys && g_in.keys[VK_MENU] ? (1 << 29) : 0) |
                   (down ? 0 : (LPARAM(3) << 30));
  const UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
  if (PumpOne(msg, vk, l) || !down) return;
  // What TranslateMessage would post: the character, pumped the same way (some accelerators are characters: + - = _).
  BYTE ks[256];
  GetKeyboardState(ks);
  WORD ch = 0;
  if (ToAscii(vk, scan, ks, &ch, 0) == 1) PumpOne(sys ? WM_SYSCHAR : WM_CHAR, ch & 0xFF, l);
}

void PressKey(const KeySpec& k) {
  const int mods[3][2] = { {VK_CONTROL, k.ctrl}, {VK_MENU, k.alt}, {VK_SHIFT, k.shift} };
  bool pressed[3] = {};
  for (int i = 0; i < 3; ++i) if (mods[i][1] && !g_in.keys[mods[i][0]]) { KeyEvent(mods[i][0], true);  pressed[i] = true; }
  KeyEvent(k.vk, true);
  KeyEvent(k.vk, false);
  for (int i = 2; i >= 0; --i) if (pressed[i]) KeyEvent(mods[i][0], false);
}

// All of these go through the unit command packet path the game uses for local input (single-player only; a
// multiplayer-safe version would have to queue packets through the network layer instead).
// Unit::DoMove() goes through MapObject::CmdMove() (the ScGroup/AI path), which crashed the renderer when used on a
// human player's unit, so build the same Move packet that clicking produces.
void IssueMove(Unit u, Location where) {
  if (!u.IsLive()) return;
  CommandPacket packet = { CommandType::Move, sizeof(MoveCommand) };
  packet.data.move.numUnits     = 1;
  packet.data.move.unitID[0]    = uint16(u.GetID());
  packet.data.move.numWaypoints = 1;
  packet.data.move.waypoint[0]  = where.AsWaypoint();
  g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
}

MapID TypeByName(const std::string& name) {
  if (name.empty() || name == "-") return MapID::None;
  for (size_t i = 1; i < MapObjectManager::NumMapObjTypes(); ++i)
    if (auto* t = MapObjectType::GetInstance(int(i)); t && _stricmp(t->GetCodeName(), name.c_str()) == 0) return MapID(i);
  return MapID::None;
}

Unit ScriptUnit(const std::string& s) {
  if (!s.empty() && s[0] == '@') { const size_t n = atoi(s.c_str() + 1);  return Unit(n < g_spawned.size() ? g_spawned[n] : 0); }
  return Unit(atoi(s.c_str()));
}

bool IsInputVerb(const std::string& v) {
  static const char* const verbs[] = { "mouse", "click", "rclick", "mclick", "dblclick", "down", "up", "drag", "key",
                                       "keydown", "keyup", "type", "select", "uilist" };
  for (const char* n : verbs) if (v == n) return true;
  return false;
}

std::vector<int> g_pendingShots;      // Ticks of screenshots requested with `+shot`.
std::vector<ScriptCmd> g_deferred;    // Button releases scheduled by `hold N`.

// Input verbs. A <pos> is `<x> <y>` (game window client pixels, as in screenshots), `tile <x> <y>` (that tile's centre in
// the detail pane), `unit <id>` (the unit's centre), `mini <x> <y>` (the minimap pixel showing that tile) or
// `ui <name>` (a control's centre, see UiToClient). Mouse verbs take optional `left|right|middle` and `shift`/`ctrl`/`alt`
// held while the button goes down; `hold N` on click/rclick/mclick/drag releases the button N ticks later instead of in
// the same tick (the minimap only pans while the button is held during a drawn frame). Any verb takes `+shot`
// (screenshot one tick later, showing the frame drawn after the input).
void RunInputCmd(const ScriptCmd& c, int tick) {
  g_lastInputTick = tick;
  std::vector<std::string> a;
  bool shot = false;
  for (const auto& s : c.args) { if (s == "+shot") shot = true;  else a.push_back(s); }
  if (shot) g_pendingShots.push_back(tick + 1);
  size_t i = 0;
  auto num = [&](size_t k) { return k < a.size() ? atoi(a[k].c_str()) : 0; };
  auto pos = [&](POINT* p) -> bool {
    if (i >= a.size()) return false;
    const std::string kind = a[i];
    if (kind == "tile" || kind == "mini") {
      const Location t(num(i + 1) + (c.fileCoords ? PadTiles() : 0), num(i + 2));
      i += 3;
      if (kind == "mini") return MinimapToClient(t, p);
      *p = MapToClient(t.x * 32 + 16, t.y * 32 + 16);
      return true;
    }
    if (kind == "unit") {
      const Unit u = ScriptUnit(i + 1 < a.size() ? a[i + 1] : "");
      i += 2;
      if (!u.IsLive()) return false;
      *p = MapToClient(u.GetMapObject()->pixelX_, u.GetMapObject()->pixelY_);
      return true;
    }
    if (kind == "ui") { i += 2;  return UiToClient(i - 1 < a.size() ? a[i - 1] : "", p); }
    if (!isdigit(uint8_t(kind[0])) && kind[0] != '-') return false;
    *p = { num(i), num(i + 1) };
    i += 2;
    return true;
  };
  // Trailing words: button and modifiers.
  int button = 0;  // 0 left, 1 right, 2 middle
  bool mod[3] = {};  // shift, ctrl, alt
  int hold = 0;
  auto words = [&] {
    for (size_t k = i; k < a.size(); ++k) {
      if      (a[k] == "hold" && k + 1 < a.size()) hold = std::max(0, atoi(a[++k].c_str()));
      else if (a[k] == "left")   button = 0;
      else if (a[k] == "right")  button = 1;
      else if (a[k] == "middle") button = 2;
      else if (a[k] == "shift")  mod[0] = true;
      else if (a[k] == "ctrl")   mod[1] = true;
      else if (a[k] == "alt")    mod[2] = true;
    }
  };
  const int modVk[3] = { VK_SHIFT, VK_CONTROL, VK_MENU };
  bool pressedMod[3] = {};
  auto holdMods = [&] {
    for (int k = 0; k < 3; ++k) if (mod[k] && !g_in.keys[modVk[k]]) { KeyEvent(modVk[k], true);  pressedMod[k] = true; }
  };
  auto releaseMods = [&] { for (int k = 2; k >= 0; --k) if (pressedMod[k]) KeyEvent(modVk[k], false); };
  static const UINT downMsg[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
  static const UINT upMsg[3]   = { WM_LBUTTONUP,   WM_RBUTTONUP,   WM_MBUTTONUP   };
  static const int  btnVk[3]   = { VK_LBUTTON,     VK_RBUTTON,     VK_MBUTTON     };
  auto press   = [&](POINT p) { g_in.keys[btnVk[button]] = 1;  SendMouse(downMsg[button], p); };
  auto release = [&](POINT p) {
    if (hold > 0) {
      static const char* const names[3] = { "left", "right", "middle" };
      g_deferred.push_back({ tick + hold, "up", { std::to_string(p.x), std::to_string(p.y), names[button] }, false });
      return;
    }
    g_in.keys[btnVk[button]] = 0;
    SendMouse(upMsg[button], p);
  };
  auto logAt = [&](POINT p) {
    TPane* pane = PaneAt(p);
    POINT m = {}, local = p;
    if (pane) MapWindowPoints(g_gameFrame.hWnd_, pane->hWnd_, &local, 1);
    const bool onMap = pane == &g_gameFrame.detailPane_ && PaneToMap(pane, local.x, local.y, &m);
    Log("input: tick %d %s at (%ld,%ld): %s pane (%ld,%ld)%s", tick, c.verb.c_str(), p.x, p.y, PaneName(pane), local.x,
        local.y, onMap ? (" map tile (" + std::to_string(m.x / 32 - PadTiles()) + "," + std::to_string(m.y / 32) +
                          ") file").c_str() : "");
  };

  POINT p = g_in.pos, q;
  const std::string& v = c.verb;
  if (v == "mouse" || v == "click" || v == "rclick" || v == "mclick" || v == "dblclick" || v == "down" || v == "up") {
    if (!pos(&p)) { Log("input: tick %d %s: bad position", tick, v.c_str());  return; }
    words();
    if (v == "rclick") button = 1;
    if (v == "mclick") button = 2;
    if (v == "dblclick" || v == "down" || v == "up" || v == "mouse") hold = 0;
    logAt(p);
    holdMods();
    SendMouse(WM_MOUSEMOVE, p);
    if (v == "down" || v == "click" || v == "rclick" || v == "mclick" || v == "dblclick") press(p);
    if (v == "up"   || v == "click" || v == "rclick" || v == "mclick" || v == "dblclick") release(p);
    // No CS_DBLCLKS on the game's window classes (IWnd::RegisterClassA 0x4314D0 uses style 0x20), so Windows never sends
    // WM_LBUTTONDBLCLK to them: a double click arrives as a second down/up pair.
    if (v == "dblclick") { press(p);  release(p); }
    releaseMods();
  } else if (v == "drag") {
    if (!pos(&p) || !pos(&q)) { Log("input: tick %d drag: bad position", tick);  return; }
    words();
    logAt(p);
    logAt(q);
    holdMods();
    SendMouse(WM_MOUSEMOVE, p);
    press(p);
    for (int s = 1; s <= 4; ++s) SendMouse(WM_MOUSEMOVE, { p.x + (q.x - p.x) * s / 4, p.y + (q.y - p.y) * s / 4 });
    release(q);
    releaseMods();
  } else if (v == "select") {
    // A left click on the unit, centring the view on it first if it is off screen.
    const Unit u = ScriptUnit(a.empty() ? "" : a[0]);
    if (!u.IsLive()) { Log("input: tick %d select: no unit %s", tick, a.empty() ? "" : a[0].c_str());  return; }
    p = MapToClient(u.GetMapObject()->pixelX_, u.GetMapObject()->pixelY_);
    if (PaneAt(p) != &g_gameFrame.detailPane_) {
      Player[0].CenterViewOn(u.GetLocation());
      p = MapToClient(u.GetMapObject()->pixelX_, u.GetMapObject()->pixelY_);
    }
    logAt(p);
    SendMouse(WM_MOUSEMOVE, p);
    press(p);
    release(p);
  } else if (v == "key" || v == "keydown" || v == "keyup") {
    for (const auto& s : a) {
      KeySpec k;
      if (!ParseKey(s, &k)) { Log("input: tick %d %s: unknown key %s", tick, v.c_str(), s.c_str());  continue; }
      Log("input: tick %d %s %s (vk 0x%02x%s%s%s)", tick, v.c_str(), s.c_str(), k.vk, k.shift ? " shift" : "",
          k.ctrl ? " ctrl" : "", k.alt ? " alt" : "");
      if (v == "key") PressKey(k);
      else KeyEvent(k.vk, v == "keydown");
    }
  } else if (v == "type") {
    std::string text;
    for (const auto& s : a) text += (text.empty() ? "" : " ") + s;
    Log("input: tick %d type \"%s\"", tick, text.c_str());
    for (char ch : text) {
      KeySpec k;
      if (ParseKey(std::string(1, ch), &k)) PressKey(k);
    }
  } else if (v == "uilist") {
    LogUiControls();
  }
}

void RunScriptCmd(const ScriptCmd& c, int tick) {
  auto arg = [&](size_t i) -> std::string { return i < c.args.size() ? c.args[i] : std::string(); };
  auto num = [&](size_t i) { return atoi(arg(i).c_str()); };
  auto unit = [&](size_t i) {
    const std::string s = arg(i);
    if (!s.empty() && s[0] == '@') { const size_t n = atoi(s.c_str() + 1);  return Unit(n < g_spawned.size() ? g_spawned[n] : 0); }
    return Unit(atoi(s.c_str()));
  };
  auto loc = [&](size_t i) { return Location(num(i) + (c.fileCoords ? PadTiles() : 0), num(i + 1)); };
  std::string all;
  for (auto& a : c.args) all += " " + a;
  Log("script: tick %d: %s%s", tick, c.verb.c_str(), all.c_str());

  if      (c.verb == "move")         IssueMove(unit(0), loc(1));
  else if (c.verb == "attack")       unit(0).DoAttack(unit(1));
  else if (c.verb == "stop")         unit(0).DoStop();
  else if (c.verb == "selfdestruct") unit(0).DoSelfDestruct();
  else if (c.verb == "research")     unit(0).DoResearch(num(1), num(2));
  else if (c.verb == "shot")         TakeShot(tick);
  else if (c.verb == "telemetry")    WriteTelemetry(tick);
  else if (c.verb == "trace")        WriteTrace(tick);
  else if (c.verb == "quit")         Quit("script quit");
  else if (c.verb == "spawn") {
    const MapID type = TypeByName(arg(0)), weapon = TypeByName(arg(4));
    const int dir = c.args.size() > 5 ? (num(5) & 0xFF) : 0;
    Unit u;
    if (type != MapID::None) u = Game::CreateUnit(type, loc(2), num(1), weapon, UnitDirection(dir / 32));
    if (u.IsLive() && dir % 32) u.GetMapObject()->rotation_ = uint8(dir);
    Log("spawn %u: %s -> unit %d", unsigned(g_spawned.size()), arg(0).c_str(), u.GetID());
    g_spawned.push_back(u.GetID());
    g_spawnGone.push_back(!u.IsLive());
    g_spawnType.push_back(type);
  }
  else if (c.verb == "view") {
    Player[0].CenterViewOn(loc(0));
    // The detail pane applies the new view when it is next drawn; in input runs draw now, so later inputs see it.
    if (g_inputUsed) g_lastInputTick = tick;
  }
  else if (IsInputVerb(c.verb))      RunInputCmd(c, tick);
  else if (c.verb == "setcell") {
    const int pad = c.fileCoords ? PadTiles() : 0;
    for (int y = num(1); y <= num(3); ++y)
      for (int x = num(0); x <= num(2); ++x) GameMap::SetCellType(Location(x + pad, y), CellType(num(4)));
  }
  else Log("script: unknown verb %s", c.verb.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------
// Optional bot DLL: exports `void __cdecl HarnessBotTick(int tick)`, called after every game cycle.

using BotTickFn = void(__cdecl*)(int);
BotTickFn g_botTick = nullptr;

// ---------------------------------------------------------------------------------------------------------------------

GameStartInfo g_gsi;
constexpr int DrawLead = 4;  // Ticks rendered ahead of a capture (and at startup) so all panes are current.
int  g_lastTick = -1, g_nextShot = DrawLead + 1, g_nextTelem = 0, g_nextTrace = 0;
bool g_started = false;
int64_t g_startTime = 0;
int64_t Now()       { LARGE_INTEGER t;  QueryPerformanceCounter(&t);    return t.QuadPart; }
int64_t Frequency() { LARGE_INTEGER f;  QueryPerformanceFrequency(&f);  return f.QuadPart; }

void TakeShot(int tick) {
  char path[MAX_PATH * 2];
  snprintf(path, sizeof(path), "%s\\shot_%07d.bmp", cfg.out.c_str(), tick);
  Screenshot(path);
}

void Quit(const char* why) {
  const double secs = (Now() - g_startTime) * 1.0 / Frequency();
  Log("quit at tick %d: %s (%.2fs in game, %.0f ticks/s)", g_gameImpl.tick_, why, secs, g_gameImpl.tick_ / (secs > 0 ? secs : 1));
  WriteTelemetry(g_gameImpl.tick_);
  WriteTrace(g_gameImpl.tick_);
  if (cfg.shotEvery > 0) TakeShot(g_gameImpl.tick_);
  if (g_telem) fclose(g_telem);
  if (g_trace) fclose(g_trace);
  if (g_log) fclose(g_log);
  // Skip normal teardown: it would flush window/ini state and can block on modal dialogs.
  TerminateProcess(GetCurrentProcess(), 0);
}

void ParkCursor() {
  HWND hWnd = g_gameFrame.hWnd_;
  if (!hWnd) return;
  RECT win, client;
  GetWindowRect(hWnd, &win);
  GetClientRect(hWnd, &client);
  Log("window %ldx%ld at %ld,%ld, client %ldx%ld", win.right - win.left, win.bottom - win.top, win.left, win.top,
      client.right, client.bottom);
  // The game polls the cursor through the virtual GetCursorPos, which starts over the empty part of the side pane:
  // outside the window or near an edge it would scroll the view (the game edge-scrolls on any cursor beyond the detail
  // pane), over the map it would show tooltips. The real X pointer goes to the screen's corner, outside the window, so
  // Wine sends the game no mouse messages of its own (over a pane they would show button tooltips, for example).
  g_in.pos = { client.right - 90, client.bottom / 2 };
  g_in.active = true;
  SetCursorPos(GetSystemMetrics(SM_CXSCREEN) - 1, GetSystemMetrics(SM_CYSCREEN) - 1);
  // Without a window manager the window is sometimes not yet active here (an X focus race). An inactive game ignores
  // view changes until it is activated, so make it the active, focused window now.
  const bool wasActive = g_tApp.AppActive() && GetFocus() == hWnd;
  if (!wasActive) {
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    SetFocus(hWnd);
  }
  if (g_inputUsed) {
    Log("input: frame %p %s, now active %d paused %d focus %p, class style %lx", (void*)hWnd,
        wasActive ? "was active" : "activated", g_tApp.AppActive(), g_tApp.AppPaused(), (void*)GetFocus(),
        GetClassLongA(g_gameFrame.detailPane_.hWnd_, GCL_STYLE));
    for (const IWnd* p : { (const IWnd*)&g_gameFrame.detailPane_, (const IWnd*)&g_gameFrame.miniMapPane_,
                           (const IWnd*)&g_gameFrame.commandPane_ }) {
      RECT rc;
      GetClientRect(p->hWnd_, &rc);
      const POINT o = PaneOrigin(*p);
      Log("input: %s pane %p at (%ld,%ld) size %ldx%ld", PaneName(p), (void*)p->hWnd_, o.x, o.y, rc.right, rc.bottom);
    }
  }
}

// Whether to render after this cycle: frames are drawn for a few ticks ahead of any capture so the panes are current.
bool WantDraw(int tick) {
  constexpr int Lead = DrawLead;
  if (tick <= Lead) return true;  // The panes must render at least once before drawing can be skipped safely.
  if (cfg.drawEvery > 0 && tick % cfg.drawEvery >= cfg.drawEvery - Lead) return true;
  if (cfg.shotEvery > 0 && (g_nextShot - tick <= Lead || (cfg.ticks > 0 && cfg.ticks - tick <= Lead))) return true;
  for (size_t i = g_scriptPos; i < g_script.size() && g_script[i].tick - tick <= Lead; ++i)
    if (g_script[i].verb == "shot" || IsInputVerb(g_script[i].verb) || (g_inputUsed && g_script[i].verb == "view"))
      return true;
  // Keep the panes current around inputs: UI views and controls are laid out as they are drawn.
  if (tick - g_lastInputTick <= Lead) return true;
  for (int t : g_pendingShots) if (t - tick <= Lead) return true;
  for (const auto& d : g_deferred) if (d.tick - tick <= Lead) return true;
  return false;
}

void __fastcall GameCycleHook(GameImpl* self, void*) {
  g_origGameCycle(self);
  const int tick = self->tick_;
  if (tick == g_lastTick) return;
  if (g_lastTick < 0) {
    ParkCursor();
    g_startTime = Now();
    Log("first game cycle: tick %d, map %dx%d, padding %d tiles", tick, GameMap::GetWidth(), GameMap::GetHeight(),
        PadTiles());
    WriteGameData((cfg.out + "\\gamedata.json").c_str());
  }
  g_lastTick = tick;
  // Parallel runs share one wineserver, so they take the foreground from each other, and an inactive game ignores keys
  // and edge/arrow scrolling (TApp::OnActivateApp 0x4862F0). Keep it active: this runs before the frame is drawn.
  if (g_inputUsed && !g_tApp.AppActive()) g_tApp.OnActivateApp(1);

  for (size_t k = 0; k < g_deferred.size();) {
    if (g_deferred[k].tick <= tick) { const ScriptCmd d = g_deferred[k];  g_deferred.erase(g_deferred.begin() + k);  RunInputCmd(d, tick); }
    else ++k;
  }
  for (size_t k = 0; k < g_pendingShots.size();) {
    if (g_pendingShots[k] <= tick) { TakeShot(tick);  g_pendingShots.erase(g_pendingShots.begin() + k); }
    else ++k;
  }
  while (g_scriptPos < g_script.size() && g_script[g_scriptPos].tick <= tick) RunScriptCmd(g_script[g_scriptPos++], tick);
  if (cfg.turbo) SetDrawing(WantDraw(tick));
  if (g_botTick) g_botTick(tick);

  // Sample at tick 1 (telemetry) / the first fully drawn frame (screenshot), then at multiples of the interval.
  if (cfg.telemEvery > 0 && tick >= g_nextTelem) { WriteTelemetry(tick);  g_nextTelem = (tick / cfg.telemEvery + 1) * cfg.telemEvery; }
  if (cfg.traceEvery > 0 && tick >= g_nextTrace) { WriteTrace(tick);      g_nextTrace = (tick / cfg.traceEvery + 1) * cfg.traceEvery; }
  if (cfg.shotEvery  > 0 && tick >= g_nextShot)  { TakeShot(tick);        g_nextShot  = (tick / cfg.shotEvery  + 1) * cfg.shotEvery;  }
  if (self->gameTermReasons_ != GameTermReasons::Running) Quit("mission ended");
  if (cfg.ticks > 0 && tick >= cfg.ticks) Quit("tick limit");
}

// Replaces GameImpl::LoadDebugMap() on the no-shell startup path with starting the requested mission.
ibool __fastcall StartMissionHook(GameImpl* self, void*) {
  g_gsi = {};
  g_gsi.startupFlags.disastersEnabled = 1;
  g_gsi.startupFlags.dayNightEnabled  = 1;
  g_gsi.startupFlags.maxPlayers       = 1;
  g_gsi.startupFlags.missionType      = uint32(cfg.mission[0] == 't' || cfg.mission[0] == 'T' ? -3 : -1) & 0x1F;
  g_gsi.gameFlags.numPlayers          = 1;
  strncpy(g_gsi.scriptName, cfg.mission.c_str(), sizeof(g_gsi.scriptName) - 1);
  for (int i = 0; i < 6; ++i) {
    g_gsi.playerFlags[i].resources = cfg.difficulty;
    strncpy(g_gsi.playerName[i].str, "Harness", sizeof(g_gsi.playerName[i].str) - 1);
  }
  g_gsi.gameSpeed        = cfg.speed * 4;
  g_gsi.randomNumberSeed = cfg.seed;

  const ibool ok = self->PrepareGame(&g_gsi);
  Log("PrepareGame(%s) = %d, players %d", cfg.mission.c_str(), ok, self->numPlayers_);
  if (!ok) { Log("mission failed to start");  if (g_log) fclose(g_log);  TerminateProcess(GetCurrentProcess(), 2); }
  g_started = true;
  return ok;
}

} // namespace

extern "C" __declspec(dllexport) void InitMod(const char* /*iniSectionName*/) {
  cfg.mission = Env("OP2H_MISSION");
  if (cfg.mission.empty()) return;  // Not a harness run: stay completely inert.

  cfg.out        = Env("OP2H_OUT", ".");
  cfg.script     = Env("OP2H_SCRIPT");
  cfg.bot        = Env("OP2H_BOT");
  cfg.ticks      = EnvInt("OP2H_TICKS", 0);
  cfg.shotEvery  = EnvInt("OP2H_SHOT_EVERY", 0);
  cfg.telemEvery = EnvInt("OP2H_TELEM_EVERY", 100);
  cfg.speed      = EnvInt("OP2H_SPEED", 10);
  cfg.seed       = EnvInt("OP2H_SEED", 1);
  cfg.difficulty = EnvInt("OP2H_DIFFICULTY", 1);
  cfg.turbo      = EnvInt("OP2H_TURBO", 1) != 0;
  cfg.drawEvery  = EnvInt("OP2H_DRAW_EVERY", 0);
  cfg.traceEvery = EnvInt("OP2H_TRACE_EVERY", 0);
  g_traceRaw     = EnvInt("OP2H_TRACE_RAW", 0) != 0;
  cfg.width      = EnvInt("OP2H_WIDTH", 1024);
  cfg.height     = EnvInt("OP2H_HEIGHT", 768);

  g_log   = fopen((cfg.out + "\\harness.log").c_str(), "w");
  g_telem = cfg.telemEvery > 0 ? fopen((cfg.out + "\\telemetry.jsonl").c_str(), "w") : nullptr;
  g_trace = cfg.traceEvery > 0 ? fopen((cfg.out + "\\trace.jsonl").c_str(), "w") : nullptr;
  Log("harness: mission=%s ticks=%d shot=%d telem=%d speed=%d turbo=%d seed=%d", cfg.mission.c_str(), cfg.ticks,
      cfg.shotEvery, cfg.telemEvery, cfg.speed, cfg.turbo, cfg.seed);

  // The game writes window/desktop sizes (and OPU its defaults) back to outpost2.ini while running. Point it at a
  // private copy so harness runs never touch the user's settings, and silence audio in that copy.
  const std::string ini = cfg.out + "\\outpost2.ini";
  if (CopyFileA(g_configFile.iniPath_, ini.c_str(), FALSE) && ini.size() < sizeof(g_configFile.iniPath_)) {
    strcpy(g_configFile.iniPath_, ini.c_str());
    for (const char* key : { "Music", "Sound" }) WritePrivateProfileStringA("Game", key, "0", ini.c_str());
    // Window geometry comes from here at creation; resizing later would shift the mission's initial view.
    // The game's frame adds 24x34 px around the client area.
    const std::pair<const char*, int> window[] = {
      { "MAXIMIZED", 0 }, { "H_POS", 0 }, { "V_POS", 0 }, { "H_SIZE", cfg.width + 48 }, { "V_SIZE", cfg.height + 36 } };
    for (const auto& [key, value] : window)
      WritePrivateProfileStringA("GAME_WINDOW", key, std::to_string(value).c_str(), ini.c_str());
    // The saved geometry is only honoured if the saved desktop size matches the current one.
    WritePrivateProfileStringA("DESKTOP", "H_SIZE", std::to_string(GetSystemMetrics(SM_CXSCREEN)).c_str(), ini.c_str());
    WritePrivateProfileStringA("DESKTOP", "V_SIZE", std::to_string(GetSystemMetrics(SM_CYSCREEN)).c_str(), ini.c_str());
    Log("config: using %s", ini.c_str());
  } else {
    Log("config: could not redirect %s; the game may write window settings to it", g_configFile.iniPath_);
  }

  // InitMod runs from op2ext's TApp::Init() hook, after TApp::ParseCommandLine(): force the /noshell startup path.
  g_tApp.bShowShell_ = 0;

  RelinkCall(CallLoadDebugMap, reinterpret_cast<void*>(&StartMissionHook));
  g_origGameCycle = reinterpret_cast<GameCycleFn>(RelinkCall(CallProcessGameCycle, reinterpret_cast<void*>(&GameCycleHook)));
  if (cfg.turbo) { const uint8_t jmp = 0xEB;  WriteCode(OnIdleFrameLimiter, &jmp, 1); }
  memcpy(g_drawPanesCode, reinterpret_cast<void*>(OnIdleDrawPanes), 2);
  JumpTo(FnOP2MessageBox, reinterpret_cast<void*>(&MessageBoxHook));
  g_realGetCursorPos     = reinterpret_cast<GetCursorPosFn>(PatchIat(IatGetCursorPos, reinterpret_cast<void*>(&VirtualGetCursorPos)));
  g_realGetAsyncKeyState =
    reinterpret_cast<GetAsyncKeyStateFn>(PatchIat(IatGetAsyncKeyState, reinterpret_cast<void*>(&VirtualGetAsyncKeyState)));

  if (!cfg.script.empty()) LoadScript(cfg.script);
  if (!cfg.bot.empty()) {
    if (HMODULE h = LoadLibraryA(cfg.bot.c_str())) g_botTick = reinterpret_cast<BotTickFn>(GetProcAddress(h, "HarnessBotTick"));
    Log("bot: %s -> %s", cfg.bot.c_str(), g_botTick ? "ok" : "HarnessBotTick not found");
  }
}

// op2ext calls this when OP2Shell.dll gets loaded. In a harness run that only happens if the game fell back to the
// shell (e.g. the mission ended through the normal end-of-game path), so treat it as the end of the run.
extern "C" __declspec(dllexport) void RunMod() {
  if (!cfg.mission.empty() && g_started) Quit("returned to shell");
}

extern "C" __declspec(dllexport) void DestroyMod() {}
