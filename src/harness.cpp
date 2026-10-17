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
  fputs("]}\n", f);
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
  Log("script: %u commands from %s", unsigned(g_script.size()), path.c_str());
}

void Quit(const char* why);
void TakeShot(int tick);

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
  else if (c.verb == "view")         Player[0].CenterViewOn(loc(0));
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
  // Park the cursor over the empty part of the side pane: outside the window or near an edge it would scroll the
  // view (the game edge-scrolls on any cursor beyond the detail pane), over the map it would show tooltips.
  POINT park = { client.right - 90, client.bottom / 2 };
  ClientToScreen(hWnd, &park);
  SetCursorPos(park.x, park.y);
}

// Whether to render after this cycle: frames are drawn for a few ticks ahead of any capture so the panes are current.
bool WantDraw(int tick) {
  constexpr int Lead = DrawLead;
  if (tick <= Lead) return true;  // The panes must render at least once before drawing can be skipped safely.
  if (cfg.drawEvery > 0 && tick % cfg.drawEvery >= cfg.drawEvery - Lead) return true;
  if (cfg.shotEvery > 0 && (g_nextShot - tick <= Lead || (cfg.ticks > 0 && cfg.ticks - tick <= Lead))) return true;
  for (size_t i = g_scriptPos; i < g_script.size() && g_script[i].tick - tick <= Lead; ++i)
    if (g_script[i].verb == "shot") return true;
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
