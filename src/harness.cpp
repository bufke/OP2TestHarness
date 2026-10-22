// OP2 test harness: an op2ext ini module that, when OP2H_MISSION is set in the environment, skips the shell and
// starts that mission directly, optionally runs uncapped, writes JSONL telemetry and BMP screenshots every N ticks,
// replays a scripted command file, calls an optional bot DLL every tick, and exits after a tick limit.
// With OP2H_SHELL=1 it lets the game boot normally instead and only logs what the shell and game do (shell.py).
// With neither set the module does nothing, so normal play is unaffected.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>
#include <mmsystem.h>
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
  std::string campaign;  // OP2H_CAMPAIGN: campaign carry-over file (see StartMissionHook).
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

bool g_eco = false;  // OP2H_ECO: economy telemetry (player colony state, building/vehicle internals). See ECONOMY.md.
const char* ActionName(int a);
void Hex(FILE* f, const void* p, size_t n);

void WriteUnit(FILE* f, bool& first, Unit u) {
  if (!u.IsLive()) return;
  const auto loc = u.GetLocation();
  fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"owner\":%d,\"x\":%d,\"y\":%d,\"hp\":%d,\"maxHp\":%d,\"cmd\":\"%s\"",
          first ? "" : ",", u.GetID(), TypeName(u.GetType()), u.GetOwner(), loc.x, loc.y,
          u.GetMaxHitpoints() - u.GetDamage(), u.GetMaxHitpoints(), CommandName(int(u.GetCommand())));
  if (const MapID w = u.GetWeapon(); u.IsVehicle() && w != MapID::None && int(w) > 0)
    fprintf(f, ",\"cargo\":\"%s\"", TypeName(w));
  if (g_eco) {
    // flags_ (MoFlagBld*: 0x1 active, 0x2000 power, 0x4000 workers, 0x8000 scientists, 0x40000 infected, 0x80000
    // EMPed), action and its timer, the 16-bit cargo/researchTimer word (+0x24), and the subclass bytes 0x48..0x7F.
    const MapObject* mo = u.GetMapObject();
    const uint8_t* b = reinterpret_cast<const uint8_t*>(mo);
    fprintf(f, ",\"flags\":%u,\"act\":\"%s\",\"timer\":%d,\"busy\":%d,\"w24\":%u,\"raw\":\"", unsigned(mo->flags_),
            ActionName(int(mo->action_)), mo->actionTimer_, mo->isBusy_, unsigned(*reinterpret_cast<const uint16_t*>(b + 0x24)));
    Hex(f, b + 0x48, 0x38);
    fputc('"', f);
    if (u.IsVehicle() && u.GetType() == MapID::CargoTruck)
      fprintf(f, ",\"truck\":[%d,%d]", int(mo->truckCargoType_), int(mo->truckCargoAmount_));
    if (u.IsBuilding())
      if (const Location d = u.GetDockLocation(); d) fprintf(f, ",\"dock\":[%d,%d]", d.x, d.y);
  }
  fputc('}', f);
  first = false;
}

// Economy telemetry per player: named PlayerImpl fields (offsets in harness/oracle/ECONOMY.md) plus every dword of
// PlayerImpl 0x00..0x1EF as `pi`, so fields can be read by offset.
void WritePlayerEco(FILE* f, const PlayerImpl* p) {
  const auto& m = p->moraleState_;
  fprintf(f, ",\"eco\":{\"maxCommon\":%d,\"maxRare\":%d,\"maxFood\":%d,\"availWorkers\":%d,\"availScientists\":%d,"
             "\"workersReq\":%d,\"scientistsReq\":%d,\"sciAsWorkers\":%d,\"sciResearching\":%d,\"powerAvail\":%d,"
             "\"powerInactive\":%d,\"foodProd\":%d,\"foodCons\":%d,\"foodLacking\":%d,\"active\":%d,\"unpowered\":%d,"
             "\"recalc\":%d,\"starveCase\":%d,\"growth\":[%d,%d,%d],\"death\":[%d,%d,%d],\"caps\":[%d,%d,%d,%d],"
             "\"activeCC\":%d,\"nurseries\":%d,\"universities\":%d,\"moraleEvent\":%d,\"morale08\":%d,\"morale10\":%d,"
             "\"resDemand\":%d,\"foodSupply\":%d,\"disabledRatio\":%d,\"recForumDemand\":%d,\"morale24\":%d,"
             "\"medDemand\":%d,\"unoccupied\":%d,\"sciAsWorkersPct\":%d,\"rcc\":%d},\"pi\":[",
          p->maxCommonOre_, p->maxRareOre_, p->maxFood_, p->numAvailableWorkers_, p->numAvailableScientists_,
          p->numWorkersRequired_, p->numScientistsRequired_, p->numScientistsAsWorkers_, p->numScientistsResearching_,
          p->amountPowerAvailable_, p->inactivePowerCapacity_, p->totalFoodProduction_, p->totalFoodConsumption_,
          p->foodLacking_, p->numActiveBuildings_, p->numUnpoweredStructures_, p->recalculateValues_, p->starvationCase_,
          p->kidGrowthRemainder_, p->workerGrowthRemainder_, p->scientistGrowthRemainder_, p->kidDeathRemainder_,
          p->workerDeathRemainder_, p->scientistDeathRemainder_, p->totalResidenceCapacity_, p->totalMedCenterCapacity_,
          p->totalRecFacilityCapacity_, p->totalForumCapacity_, p->numActiveCommandCenters_, p->numActiveNurseries_,
          p->numActiveUniversities_, m.eventMoraleModifier, m.field_08, m.field_10, m.residenceDemandPercent,
          int(m.foodSupply), m.disabledBuildingRatio, m.recForumDemandPercent, m.field_24, m.medCenterDemandPercent,
          m.unoccupiedColonistsPercent, m.scientistsAsWorkersPercent, p->rccOperational_);
  const int* d = reinterpret_cast<const int*>(p);
  for (int k = 0; k < 0x1F0 / 4; ++k) fprintf(f, "%s%d", k ? "," : "", d[k]);
  fputc(']', f);
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
    fputc(']', f);
    if (g_eco) WritePlayerEco(f, p);
    fputc('}', f);
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
bool  g_traceMap  = false;    // Also list the map tiles that changed since the previous trace line (`--trace-map`).
std::vector<uint32_t> g_mapPrev;  // The map's cell, graphic and wall bits at the previous trace line.

// `"map":[[x, y, cell, graphic, wall], ...]`: the tiles (file coordinates) whose cell type, tile graphic or
// wall-or-building bit (TileData) changed since the previous trace line; the first line lists none.
void WriteTraceMap(FILE* f) {
  const int w = GameMap::GetWidth(), h = GameMap::GetHeight(), pad = PadTiles();
  const bool fresh = g_mapPrev.size() != size_t(w) * h;
  if (fresh) g_mapPrev.assign(size_t(w) * h, 0);
  fputs(",\"map\":[", f);
  bool first = true;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint32_t t = GameMap::GetImpl()->Tile(x + pad, y).u32All & 0x8000FFFFu;
      uint32_t& prev = g_mapPrev[size_t(y) * w + x];
      if (!fresh && t != prev)
        fprintf(f, "%s[%d,%d,%u,%u,%u]", first ? "" : ",", x, y, t & 0x1F, (t >> 5) & 0x7FF, t >> 31), first = false;
      prev = t;
    }
  fputc(']', f);
}

std::vector<int> g_spawned;   // Spawn index -> unit ID.
// Spawns that have died. The game recycles object indices, so a dead spawn's ID may later belong to something else.
// A spawn that failed (no free object index) is gone from the start.
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
    // Headlights (MoFlagVecHeadlights, flags bit 0).
    fprintf(f, ",\"lights\":%d", int(mo->flags_ & 1));
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
  // Turret state of armed units (vehicles, guard posts; other structures use weapon_ for other things, e.g. a lab's
  // research countdown, chaos_same_topic). Offsets learned from Outpost2.exe: the turret rotation is the
  // byte at +0x2D (TankVehicle::Draw 0x484E5A passes it to SelectTurretGraphic; the aim steps at 0x43E6C5 slew it),
  // +0x2E is reloadTimer_, and +0x4C is the weapon target (low word = unit index when the high word is 0xFFFF,
  // high word 0x7FFF = none; else a ground tile), used by the fire handlers at 0x4A7650..0x4A7BC0.
  if (const int w = mo->weapon_; w >= int(MapID::AcidCloud) && w <= int(MapID::BFG) && (u.IsVehicle() || u.GetType() == MapID::GuardPost)) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(mo);
    const uint16_t tlo = *reinterpret_cast<const uint16_t*>(b + 0x4C), thi = *reinterpret_cast<const uint16_t*>(b + 0x4E);
    fprintf(f, ",\"weapon\":\"%s\",\"tur\":%d,\"reload\":%d,\"tgt\":", TypeName(MapID(w)), b[0x2D], mo->reloadTimer_);
    if (thi == 0x7FFF)      fputs("null", f);
    else if (thi == 0xFFFF) fprintf(f, "%d", int(tlo));
    else                    fprintf(f, "[%d,%d]", int(int16_t(tlo)) - PadTiles(), int(int16_t(thi)));
  }
  // Weapon status effects, only while one is on: [EMP, stickyfoam, ESG] timers (+0x56, +0x54, +0x58; the game counts
  // them down once per 8-tick phase, 0x43DF7A), each 0 unless its flag (MoFlagEMPed, MoFlagVecStickyfoamed, MoFlagESGed)
  // is set. Buildings keep only the EMP timer (bit 18 is MoFlagBldInfected on them).
  {
    const uint32_t fl = mo->flags_;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(mo);
    const bool veh = u.IsVehicle();
    const bool emp = fl & MoFlagEMPed, foam = veh && (fl & MoFlagVecStickyfoamed), esg = veh && (fl & MoFlagESGed);
    if (emp || foam || esg)
      fprintf(f, ",\"status\":[%d,%d,%d]", emp ? *reinterpret_cast<const uint16_t*>(b + 0x56) : 0,
              foam ? *reinterpret_cast<const uint16_t*>(b + 0x54) : 0, esg ? *reinterpret_cast<const uint16_t*>(b + 0x58) : 0);
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
    fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"parent\":%d,\"action\":\"%s\",\"dir\":%d,\"px\":%d,\"py\":%d,\"timer\":%d",
            first ? "" : ",", int(i), TypeName(mo->GetTypeID()), spawn, ActionName(int(mo->action_)), mo->rotation_,
            mo->pixelX_ - PadTiles() * 32, mo->pixelY_, mo->actionTimer_);
    // An armed ESG field (flag 0x8000, set by its first DoEvent after landing, 0x481120): the mines left, a bit each
    // (+0x6E, 13 bits).
    if (mo->GetTypeID() == MapID::ESG && (mo->flags_ & 0x8000))
      fprintf(f, ",\"mines\":%d", int(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(mo) + 0x6E)));
    fputc('}', f);
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
    fprintf(f, "%s[%d,\"%s\",\"%s\",%d,%d,%d", first ? "" : ",", int(i), TypeName(mo->GetTypeID()),
            ActionName(int(mo->action_)), mo->actionTimer_, mo->pixelX_ - PadTiles() * 32, mo->pixelY_);
    // --trace-raw: also the animation and frame drawn (set when the object is drawn: use --draw-every 1) and the raw
    // MapObject bytes, e.g. of the structure a ConVec builds as it develops.
    if (g_traceRaw) {
      fprintf(f, ",%d,%d,\"", mo->animationIndex_, mo->frameIndex_);
      Hex(f, mo, 0x80);
      fputc('"', f);
    }
    fputc(']', f);
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
    // Gone for good once dead (the flag stays set after the object is freed, until its index is reused) or replaced. A
    // vehicle in a Garage bay is marked dead too, with pNext_ -2 (0x44D000): it isn't listed while it's in there, and
    // is again once it comes out (chaos_garage_full).
    if (g_spawnGone[i]) continue;  // (a spawn that failed has no object at all)
    const MapObject* mo = u.GetMapObject();
    const bool inBay = reinterpret_cast<uintptr_t>(mo->pNext_) == 0xFFFFFFFEu;
    if (((mo->flags_ & MoFlagDead) && !inBay) || u.GetType() != g_spawnType[i]) g_spawnGone[i] = true;
    else if (u.IsLive()) WriteTraceUnit(f, first, u, int(i));
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
  if (g_traceMap) WriteTraceMap(f);
  // Main gameplay RNG state (two 32-bit LFSRs at 0x56BE20, stepped by Random::Rand 0x46F000/0x46F060).
  const uint64 seed = g_gameRNG.GetSeed();
  fprintf(f, ",\"rng\":[%u,%u]}\n", unsigned(seed & 0xFFFFFFFF), unsigned(seed >> 32));
  fflush(f);
}

// Economy tables as loaded (eco runs only): the morale levels (MoraleManager 0x565280: name, M_PTS_TOT, then the
// power/research/factory/food bonuses in 1/256 and the defect/fertility/death rates), the derived maxima at +0xF0..
// +0x104, every morale modifier (0x4E1F10: name, value, percent flag), the mine yield table (MineManager 0x5651A0),
// and per unit type the resource priority (+0x2A0, the building list order) and research topic (+0x1E4).
void WriteEcoData(FILE* f) {
  const uint8_t* mm = reinterpret_cast<const uint8_t*>(0x565280);
  fputs(",\n\"morale\":{\"levels\":[", f);
  for (int k = 0; k < 5; ++k) {
    const uint8_t* row = mm + k * 0x30;
    char name[16] = {};
    memcpy(name, row, 15);
    fprintf(f, "%s{\"name\":\"%s\",\"v\":[", k ? "," : "", name);
    for (int j = 0; j < 8; ++j) fprintf(f, "%s%d", j ? "," : "", reinterpret_cast<const int*>(row + 0x10)[j]);
    fputs("]}", f);
  }
  fputs("],\"maxima\":[", f);
  for (int j = 0; j < 6; ++j) fprintf(f, "%s%d", j ? "," : "", reinterpret_cast<const int*>(mm + 0xF0)[j]);
  fputs("],\"modifiers\":[", f);
  struct Mod { const char* name;  int value;  int pct; };
  const Mod* mods = reinterpret_cast<const Mod*>(0x4E1F10);
  for (int k = 0; k < 0x43; ++k)
    fprintf(f, "%s[\"%s\",%d,%d]", k ? "," : "", mods[k].name ? mods[k].name : "", mods[k].value, mods[k].pct);
  fputs("]},\n\"mines\":[", f);
  const int* mine = reinterpret_cast<const int*>(0x5651A0);
  for (int k = 0; k < 9; ++k)
    fprintf(f, "%s{\"variant\":%d,\"bar\":%d,\"trucks\":[%d,%d,%d],\"yield\":[%d,%d,%d]}", k ? "," : "", k / 3, k % 3,
            mine[k * 3], mine[k * 3 + 1], mine[k * 3 + 2], mine[27 + k * 3], mine[27 + k * 3 + 1], mine[27 + k * 3 + 2]);
  // Tech tree as loaded, in techNum order: [techID, plymouthCost, edenCost, maxScientists, lab, category].
  fputs("],\n\"techTree\":[", f);
  for (int k = 0; k < g_research.numTechs_; ++k) {
    const TechInfo* ti = g_research.ppTechInfos_[k];
    fprintf(f, "%s[%d,%d,%d,%d,%d,%d]", k ? "," : "", ti->techID, ti->plymouthCost, ti->edenCost, ti->maxScientists,
            int(ti->lab), int(ti->category));
  }
  fputs("],\n\"typeExtra\":[", f);
  bool first = true;
  for (size_t i = 1; i < MapObjectManager::NumMapObjTypes(); ++i) {
    const uint8_t* t = reinterpret_cast<const uint8_t*>(MapObjectType::GetInstance(int(i)));
    if (!t) continue;
    fprintf(f, "%s[%u,%d,%d]", first ? "" : ",", unsigned(i), t[0x2A0], *reinterpret_cast<const int*>(t + 0x1E4));
    first = false;
  }
  fputc(']', f);
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
  fputc(']', f);
  if (g_eco) WriteEcoData(f);
  // Cell type of every map tile in file coordinates (row-major), to check the coordinate conversion against the .map.
  fputs(",\n\"cells\":[", f);
  for (int y = 0; y < GameMap::GetHeight(); ++y)
    for (int x = 0; x < GameMap::GetWidth(); ++x)
      fprintf(f, "%s%d", (x | y) ? "," : "", int(GameMap::GetCellType(Location(x + PadTiles(), y))));
  fputs("]}\n", f);
  fclose(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scripted input. One command per line: `<tick> <verb> <args...>`; '#' starts a comment.
//   move <unit> <x> <y> | dock <unit> <x> <y> | attack <unit> <target> | stop <unit> | selfdestruct <unit>
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

// The `hotkeys` verb: one line of hotkeys.jsonl with the buttons' hotkey table, the filter object at 0x546BB0 (vtable
// 0x4CF890, its OnUIEvent 0x40A710) that sends a WM_KEYDOWN or WM_SYSKEYDOWN to the button registered for that virtual
// key while its +0x404 counter is 0 (Pause and the chat line raise it, 0x40A6E0). A button takes its key from the
// letter after its tooltip's '&' (UIGraphicalButton::SetDisplayInfo 0x40AD70, UIButton::SetHotKey 0x40A9F0, upper
// case) and registers it when added to a pane, unless another button already holds that key (UIButton::OnAdd
// 0x40A920). Each entry: [vk, flags (UIButton +4: 2 disabled), tooltip, vtable].
void WriteHotkeys(int tick) {
  static FILE* f = fopen((cfg.out + "\\hotkeys.jsonl").c_str(), "w");
  if (!f) return;
  auto* filter = reinterpret_cast<const uint8_t*>(0x546BB0);
  auto* table = reinterpret_cast<UIElement* const*>(filter + 4);
  fprintf(f, "{\"tick\":%d,\"off\":%d,\"keys\":[", tick, *reinterpret_cast<const int*>(filter + 0x404));
  bool first = true;
  for (int vk = 0; vk < 256; ++vk) {
    UIElement* e = table[vk];
    if (!e || IsBadReadPtr(e, 0x20)) continue;
    std::string help;
    for (char ch : ControlHelp(e)) {
      if (ch == '"' || ch == '\\') help += '\\';
      if (uint8_t(ch) >= 0x20) help += ch;
    }
    fprintf(f, "%s[%d,%u,\"%s\",%u]", first ? "" : ",", vk, unsigned(e->flags_), help.c_str(),
            unsigned(*reinterpret_cast<uintptr_t*>(e)));
    first = false;
  }
  fputs("]}\n", f);
  fflush(f);
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
    std::string h;
    for (char ch : c.help) if (ch != '&') h += char(tolower(ch));  // '&' marks the hotkey letter.
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
void IssueMove(Unit u, Location where, CommandType type = CommandType::Move) {
  if (!u.IsLive()) return;
  CommandPacket packet = { type, sizeof(MoveCommand) };
  packet.data.move.numUnits     = 1;
  packet.data.move.unitID[0]    = uint16(u.GetID());
  packet.data.move.numWaypoints = 1;
  packet.data.move.waypoint[0]  = where.AsWaypoint();
  g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
}

// Unit orders with more than a Move's arguments, built the way PlayerImpl::ProcessCommandPacket (0x40E300) parses
// them: Move/Dock/StandGround/Patrol at 0x40E3B0, Attack/Guard at 0x40FAB7. `waypoints` are tiles, each with a range
// in the waypoint's top 3 bits (how near counts as reached, 0x44BFD0; the UI uses 2 for Ctrl+click waypoints and 3 for
// patrol points); Patrol appends the waypoint index the patrol loops back to (stored at MapObject +0x5E).
void IssueWaypoints(Unit u, CommandType type, const std::vector<Location>& waypoints, const std::vector<int>& ranges,
                    int patrolLoop = -1) {
  if (!u.IsLive() || waypoints.empty() || waypoints.size() > 8) return;
  CommandPacket packet = { type, 0 };
  auto& m = packet.data.move;
  m.numUnits     = 1;
  m.unitID[0]    = uint16(u.GetID());
  m.numWaypoints = uint16(waypoints.size());
  Waypoint* wp = m.waypoint;
  for (size_t i = 0; i < waypoints.size(); ++i) {
    wp[i] = waypoints[i].AsWaypoint();
    wp[i].u32All = (wp[i].u32All & 0x1FFFFFFF) | (uint32(i < ranges.size() ? ranges[i] & 7 : 0) << 29);
  }
  size_t size = sizeof(MoveCommand) + (waypoints.size() - 1) * sizeof(Waypoint);
  if (patrolLoop >= 0) {
    const uint16 loop = uint16(patrolLoop);
    memcpy(reinterpret_cast<uint8*>(&m) + size, &loop, 2);
    size += 2;
  }
  packet.dataLength = uint16(size);
  g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
}

// Attack or Guard on a unit (`tile` false: x = unit id) or on a tile.
void IssueTargeted(Unit u, CommandType type, int x, int y, bool tile) {
  if (!u.IsLive()) return;
  CommandPacket packet = { type, sizeof(AttackCommand) };
  packet.data.attack.numUnits  = 1;
  packet.data.attack.unitID[0] = uint16(u.GetID());
  packet.data.attack.unknown   = 0;
  packet.data.attack.target    = { uint16(x), uint16(tile ? y : 0xFFFF) };
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
                                       "keydown", "keyup", "type", "select", "uilist", "hotkeys" };
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
  } else if (v == "hotkeys") {
    WriteHotkeys(tick);
  }
}

// Economy verbs (README "Economy verbs"):
//   player <p> <field> <value>        set a PlayerImpl field: workers scientists kids food common rare morale
//                                     moraleLevel solarsat, or a byte offset (0x94); marks colony values for recalculation
//   poke <unit> <offset> <1|2|4> <value>   write a MapObject field (e.g. damage at 0x1E)
//   beacon <x> <y> <ore 0|1> <bar 0-2> <variant 0-2> | beacon <x> <y> vent|fumarole
//   route <truck> <mine> <smelter>    cargo route (ctMoCargoRoute), as the UI builds it
//   produce <factory> <TYPE> [weapon] | idle <bldg> | unidle <bldg> | train <university> <n>
//   kit <convec> <TYPE> [weapon]      put a structure kit in a ConVec
//   build <convec> <x> <y> [TYPE]     ctMoBuild to the bottom-right tile of footprint+margin (TYPE: Robo-Miner, GeoCon)
//   bay <factory> <bay> <TYPE> [weapon] | transfercargo <factory> <bay>
//   cargo <truck> <type 0-10> <amount> | load <truck> | unload <truck>
bool RunEcoCmd(const ScriptCmd& c, int tick) {
  auto arg = [&](size_t i) -> std::string { return i < c.args.size() ? c.args[i] : std::string(); };
  auto num = [&](size_t i) { return int(strtol(arg(i).c_str(), nullptr, 0)); };
  auto loc = [&](size_t i) { return Location(num(i) + (c.fileCoords ? PadTiles() : 0), num(i + 1)); };
  const std::string& v = c.verb;
  if (v == "tube" || v == "wall") {  // tube|wall x1 y1 x2 y2 [TYPE]: Game::CreateWall over the inclusive rectangle
    const Location a = loc(0), b = loc(2);
    const MapID type = v == "tube" ? MapID::Tube : (c.args.size() > 4 ? TypeByName(arg(4)) : MapID::Wall);
    for (int y = a.y; y <= b.y; ++y)
      for (int x = a.x; x <= b.x; ++x) Game::CreateWall(type, Location(x, y));
    return true;
  }
  if (v == "mem") {  // mem <address> <1|2|4> <value>: write game memory, e.g. the forced-morale masks at 0x56EAEC
    uint8_t* b = reinterpret_cast<uint8_t*>(uintptr_t(strtoul(arg(0).c_str(), nullptr, 0)));
    const int size = num(1), value = num(2);
    if (IsBadWritePtr(b, size)) { Log("script: mem: bad address %s", arg(0).c_str());  return true; }
    if (size == 1) *b = uint8_t(value); else if (size == 2) *reinterpret_cast<uint16_t*>(b) = uint16_t(value);
    else *reinterpret_cast<int*>(b) = value;
    return true;
  }
  if (v == "tech") {  // tech <player> <techID>: _Player::MarkResearchComplete (0x477510), as a mission DLL grants one
    // A topic the loaded tech tree doesn't have would end the mission, so it's logged with the valid ids instead.
    bool known = false;
    for (int t = 0; t < g_research.numTechs_ && !known; ++t) known = g_research.ppTechInfos_[t]->techID == num(1);
    if (!known || num(0) < 0 || num(0) >= int(MaxPlayers)) {
      std::string ids;
      for (int t = 0; t < g_research.numTechs_; ++t) ids += (ids.empty() ? "" : " ") + std::to_string(g_research.ppTechInfos_[t]->techID);
      Log("script: tech %s %s: no such player or topic in this tech tree (topics: %s)", arg(0).c_str(), arg(1).c_str(), ids.c_str());
      return true;
    }
    Player[num(0)].MarkResearchComplete(num(1));
    return true;
  }
  if (v == "player") {
    PlayerImpl* p = g_gameImpl.GetPlayer(num(0));
    static const std::pair<const char*, int> fields[] = {
      {"food", 0x10}, {"common", 0x20}, {"rare", 0x24}, {"moraleLevel", 0x34}, {"morale", 0x40}, {"workers", 0x94},
      {"scientists", 0x98}, {"kids", 0x9C}, {"solarsat", 0xEC} };
    int off = -1;
    for (const auto& [n, o] : fields) if (arg(1) == n) off = o;
    if (off < 0 && isdigit(uint8_t(arg(1)[0]))) off = num(1);
    if (!p || off < 0 || off >= int(sizeof(PlayerImpl))) { Log("script: player: bad field %s", arg(1).c_str());  return true; }
    if (arg(1) == "solarsat") p->satellites_.numSolarSatellites = num(2);
    else *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(p) + off) = num(2);
    p->recalculateValues_ = 1;
    return true;
  }
  // Disasters, as a mission DLL schedules them (the TethysGame::Set* exports 0x4782E0..0x4783E0, which create the
  // disaster object at the tile's centre); a trailing `now` skips the two warning waits (Disaster::StartDisaster).
  //   meteor x y size | quake x y magnitude | eruption x y speed | lightning x1 y1 duration x2 y2
  //   tornado x1 y1 duration x2 y2 | blight x y [speed] | lavaspeed n | lavapossible x1 y1 x2 y2 0|1
  if (v == "meteor" || v == "quake" || v == "eruption" || v == "lightning" || v == "tornado") {
    const bool now = arg(c.args.size() - 1) == "now";
    Unit d;
    if (v == "meteor")    d = Game::CreateMeteor(loc(0), MeteorSize(num(2)), now);
    if (v == "quake")     d = Game::CreateEarthquake(loc(0), num(2), now);
    if (v == "eruption")  d = Game::CreateEruption(loc(0), num(2), now);
    // An end x or y of -1 is left for the game to pick when the disaster strikes.
    const Location end(num(3) == -1 ? -1 : loc(3).x, num(4));
    if (v == "lightning") d = Game::CreateLightning(loc(0), end, num(2), now);
    if (v == "tornado")   d = Game::CreateTornado(loc(0), end, num(2), now);
    Log("%s at (%s,%s): unit %d", v.c_str(), arg(0).c_str(), arg(1).c_str(), d.GetID());
    return true;
  }
  if (v == "blight") {
    // Game::CreateBlight: the speed (BlightManager::SetSpreadSpeed), then GameMap::SetVirusUL (0x476EA0), which takes
    // its flag in ecx and the Location on the stack (MinGW's fastcall passes TethysAPI's (Location, ibool) otherwise).
    if (c.args.size() > 2 && num(2) >= 0) Game::SetBlightSpeed(num(2));
    using SetVirusFn = void(__fastcall*)(int flag, int unused, int x, int y);
    reinterpret_cast<SetVirusFn>(0x476EA0)(1, 0, loc(0).x, loc(0).y);
    return true;
  }
  if (v == "lavaspeed") { Game::SetLavaSpeed(num(0));  return true; }
  if (v == "lavapossible") {
    const Location a = loc(0), b = loc(2);
    // GameMap::SetLavaPossible (0x476F20) takes the flag in ecx and the Location on the stack; MinGW's fastcall
    // passes TethysAPI's (Location, ibool) differently, so call it with that layout spelled out.
    using SetLavaPossibleFn = void(__fastcall*)(int flag, int unused, int x, int y);
    for (int y = a.y; y <= b.y; ++y)
      for (int x = a.x; x <= b.x; ++x) reinterpret_cast<SetLavaPossibleFn>(0x476F20)(num(4) != 0, 0, x, y);
    return true;
  }
  Unit u = ScriptUnit(arg(0));
  if (v == "poke") {
    if (!u.IsLive()) return true;
    uint8_t* b = reinterpret_cast<uint8_t*>(u.GetMapObject()) + num(1);
    const int size = num(2), value = num(3);
    if (size == 1) *b = uint8_t(value); else if (size == 2) *reinterpret_cast<uint16_t*>(b) = uint16_t(value);
    else *reinterpret_cast<int*>(b) = value;
    return true;
  }
  if (v == "beacon") {
    // Game::CreateMine's internal call (0x478940), with the internal variant number, so no random draw is made.
    const Location at = loc(0);
    const std::string kind = arg(2);
    const MapID type = kind == "vent" ? MapID::MagmaVent : kind == "fumarole" ? MapID::Fumarole : MapID::MiningBeacon;
    const int mineType = type == MapID::MiningBeacon ? num(2) : -1;  // as CreateMine: max(type, RandomOre)
    const ibool ok = OP2Thunk<0x478940, ibool FASTCALL(MapID, int, int, int, int, int)>(
      type, at.x, at.y, mineType, type == MapID::MiningBeacon ? num(3) : -1, type == MapID::MiningBeacon ? num(4) : -1);
    Log("beacon %s at (%d,%d): %d", kind.c_str(), at.x, at.y, ok);
    return true;
  }
  if (v == "route") {
    const Unit mine = ScriptUnit(arg(1)), smelter = ScriptUnit(arg(2));
    if (!u.IsLive() || !mine.IsLive() || !smelter.IsLive()) return true;
    CommandPacket packet = { CommandType::CargoRoute, sizeof(CargoRouteCommand) };
    auto& r = packet.data.cargoRoute;
    r.numUnits = 1;
    r.unitID[0] = uint16(u.GetID());
    r.numWaypoints = 3;
    r.waypoint[0] = mine.GetDockLocation().AsWaypoint();
    r.waypoint[1] = smelter.GetDockLocation().AsWaypoint();
    r.waypoint[2] = mine.GetDockLocation().AsWaypoint();
    r.mineWaypointIndex = 0;
    r.smelterWaypointIndex = 1;
    r.mineUnitId = uint16(mine.GetID());
    r.smelterUnitId = uint16(smelter.GetID());
    g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    return true;
  }
  if (v == "produce")  { if (u.IsLive()) u.DoProduce(TypeByName(arg(1)), TypeByName(arg(2)), 0xFFFF, false);  return true; }
  if (v == "idle")     { u.DoIdle();  return true; }
  if (v == "unidle")   { u.DoUnidle();  return true; }
  if (v == "train")    { u.DoTrainScientists(num(1));  return true; }
  if (v == "load")     { u.DoLoadCargo();  return true; }
  if (v == "unload")   { u.DoUnloadCargo();  return true; }
  if (v == "kit")      { if (u.IsLive()) u.SetCargo(TypeByName(arg(1)), TypeByName(arg(2)));  return true; }
  if (v == "cargo")    { if (u.IsLive()) u.SetCargo(CargoType(num(1)), num(2));  return true; }
  if (v == "bay")      { if (u.IsLive()) u.SetFactoryCargo(num(1), TypeByName(arg(2)), TypeByName(arg(3)));  return true; }
  if (v == "transfercargo") {  // a ctMoTransferCargo packet (TethysAPI's DoTransferCargo is the AI path; it crashed)
    if (!u.IsLive()) return true;
    CommandPacket packet = { CommandType::TransferCargo, sizeof(TransferCargoCommand) };
    packet.data.transferCargo.unitID  = uint16(u.GetID());
    packet.data.transferCargo.bay     = uint16(num(1));
    // The third argument is the packet's second word: a Spaceport's "load bay into the rocket" sends 1
    // (Spaceport::ActivateDock 0x47F420 swaps the bay with the rocket's cargo); 0 is a transfer to the dock.
    packet.data.transferCargo.unknown = uint16(c.args.size() > 2 ? num(2) : 0);
    g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    return true;
  }
  if (v == "launch") {  // a ctMoLaunch packet: launch the rocket on a Spaceport's pad (optional target pixel x y)
    if (!u.IsLive()) return true;
    CommandPacket packet = { CommandType::Launch, sizeof(LaunchCommand) };
    packet.data.launch.unitID       = uint16(u.GetID());
    packet.data.launch.targetPixelX = uint16(c.args.size() > 2 ? num(1) : 0);
    packet.data.launch.targetPixelY = uint16(c.args.size() > 2 ? num(2) : 0);
    g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    return true;
  }
  if (v == "build") {
    // The ctMoBuild packet of the legacy Unit::DoBuild export (0x476350): waypoint = the centre of the bottom-right tile,
    // rect = the footprint plus its 1-tile margin.
    // A Robo-Miner or GeoCon has no kit: name the structure (MINE, GEOTHERMAL) as the 4th argument for its size.
    const MapID kit = c.args.size() > 3 ? TypeByName(arg(3)) : MapID(u.IsLive() ? u.GetMapObject()->cargo_ : 0);
    if (!u.IsLive() || kit == MapID::None) { Log("script: build: no ConVec kit");  return true; }
    const auto& s = MapObjectType::GetInstance(kit)->stats_.building;
    const Location br = loc(1);
    CommandPacket packet = { CommandType::Build, sizeof(BuildCommand) };
    packet.data.build.numUnits     = 1;
    packet.data.build.unitID[0]    = uint16(u.GetID());
    packet.data.build.numWaypoints = 1;
    packet.data.build.waypoint[0]  = br.AsWaypoint(true, true);  // as the legacy export 0x476350 (TethysAPI uses y = tile top)
    packet.data.build.rect         = MapRect(br.x - s.width - 1, br.y - s.height - 1, br.x, br.y).AsPacked();
    packet.data.build.unknown      = -1;
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("build: %s at (%d,%d) -> %d", TypeName(kit), br.x, br.y, ok);
    return true;
  }
  // The Robo-Dozer's and the Earthworker's orders, as their commands' clicks build the packets (0x4538A0, 0x452780,
  // 0x452E30). Rectangles are inclusive here; ctMoBuildWall's bottom-right is exclusive in the packet (its handler
  // 0x4235E0 takes one off), and ctMoRemoveWall carries one waypoint (the tile's centre here) besides its tile.
  if (v == "doze" || v == "buildwall") {  // doze <dozer> x1 y1 x2 y2 | buildwall <earthworker> x1 y1 x2 y2 [TYPE]
    if (!u.IsLive()) return true;
    const Location a = loc(1), b = loc(3);
    CommandPacket packet = {};
    if (v == "doze") {
      packet = { CommandType::Doze, sizeof(DozeCommand) };
      packet.data.doze.numUnits  = 1;
      packet.data.doze.unitID[0] = uint16(u.GetID());
      packet.data.doze.rect      = MapRect(a.x, a.y, b.x, b.y).AsPacked();
    } else {
      packet = { CommandType::BuildWall, sizeof(BuildWallCommand) };
      auto& w = packet.data.buildWall;
      w.numUnits     = 1;
      w.unitID[0]    = uint16(u.GetID());
      w.rect.x1      = uint16(a.x);
      w.rect.y1      = uint16(a.y);
      w.rect.x2      = uint16(b.x + 1);
      w.rect.y2      = uint16(b.y + 1);
      w.tubeWallType = uint16(c.args.size() > 5 ? TypeByName(arg(5)) : MapID::Tube);
      w.unknown      = 0;
    }
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("%s: (%d,%d)-(%d,%d) -> %d", v.c_str(), a.x, a.y, b.x, b.y, ok);
    return true;
  }
  if (v == "removewall") {  // removewall <earthworker> x y
    if (!u.IsLive()) return true;
    const Location at = loc(1);
    CommandPacket packet = { CommandType::RemoveWall, sizeof(RemoveWallCommand) };
    auto& r = packet.data.removeWall;
    r.numUnits     = 1;
    r.unitID[0]    = uint16(u.GetID());
    r.numWaypoints = 1;
    r.waypoint[0]  = at.AsWaypoint();
    r.rect         = MapRect(at.x, at.y, at.x, at.y).AsPacked();
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("removewall: (%d,%d) -> %d", at.x, at.y, ok);
    return true;
  }
  // The targeted orders of the Repair, Reprogram and Dismantle commands, as CommandRepair::OnClick (0x4549F0) builds
  // its packet: the unit, no waypoints and the target's index (the click sends 0 when no valid target is under it;
  // `none` sends that). One packet per verb; more units after the target go into the same packet.
  if (v == "repairobj" || v == "reprogram" || v == "dismantle") {  // repairobj <unit> <target|none> [unit...]
    if (!u.IsLive()) return true;
    const Unit target = arg(1) == "none" ? Unit() : ScriptUnit(arg(1));
    CommandPacket packet = { v == "repairobj" ? CommandType::RepairObj : v == "reprogram" ? CommandType::Reprogram
                                                                       : CommandType::Dismantle, 0 };
    auto& r = packet.data.repair;
    r.numUnits  = 1;
    r.unitID[0] = uint16(u.GetID());
    for (size_t i = 2; i < c.args.size() && r.numUnits < 32; ++i) {
      const Unit more = ScriptUnit(arg(i));
      if (more.IsLive()) r.unitID[r.numUnits++] = uint16(more.GetID());
    }
    // SimpleCommand's unit list is followed by the waypoint count (0) and the target.
    uint8_t* after = reinterpret_cast<uint8_t*>(&r.unitID[r.numUnits]);
    *reinterpret_cast<uint16_t*>(after) = 0;
    *reinterpret_cast<uint32_t*>(after + 2) = uint32_t(arg(1) == "none" ? 0 : target.GetID()) | 0xFFFF0000u;
    packet.dataLength = uint16(after + 6 - reinterpret_cast<uint8_t*>(&packet.data));
    const ibool ok = g_gameImpl.GetPlayer(u.GetOwner())->ProcessCommandPacket(packet);
    Log("%s: %d -> %s (%d units) -> %d", v.c_str(), u.GetID(), arg(1).c_str(), int(r.numUnits), ok);
    return true;
  }
  (void)tick;
  return false;
}

// Mission AI groups (README "Group verbs"): `group <name> <op> [args]`, the mission API's ScGroup calls on a named group.
//   new fight|mining|building <player> | take <unit>... | remove <unit> | lights 0|1 | enable | disable | destroy
//   rect x1 y1 x2 y2 (FightGroup or BuildingGroup SetRect) | guardedrect x1 y1 x2 y2 | clearguarded
//   guardrect | patrolonly | attackenemy | guardgroup | guardunit | attackunit | exitmap      (the Do* objectives)
//   patrol <loop> x,y ... | clearpatrol | combine 0|1 | follow <mode> | targetunit <unit> | targetgroup <name>
//   attacktype <TYPE> | mine <mine unit> <smelter unit> x1 y1 x2 y2 | targcount <TYPE> <WEAPON|-> <n> | cleartargcount
//   reinforce <group> <priority> | unreinforce <group> | building x y <TYPE> <WEAPON|-> [group] | tube x y | wall x y [TYPE]
//   dump  (log the group's members in list order, and a fight group's state)
std::vector<std::pair<std::string, int>> g_groups;

int GroupId(const std::string& name) {
  for (const auto& [n, id] : g_groups) if (n == name) return id;
  return -1;
}

void RunGroupCmd(const ScriptCmd& c) {
  auto arg = [&](size_t i) -> std::string { return i < c.args.size() ? c.args[i] : std::string(); };
  auto num = [&](size_t i) { return int(strtol(arg(i).c_str(), nullptr, 0)); };
  auto loc = [&](size_t i) { return Location(num(i) + (c.fileCoords ? PadTiles() : 0), num(i + 1)); };
  auto rect = [&](size_t i) { const Location a = loc(i), b = loc(i + 2);  return MapRect(a.x, a.y, b.x, b.y); };
  const std::string name = arg(0), op = arg(1);
  if (op == "new") {
    const std::string kind = arg(2);
    const _Player p = Player[num(3)];
    const int id = kind == "mining" ? CreateMiningGroup(p).GetID() : kind == "building" ? CreateBuildingGroup(p).GetID()
                 : CreateFightGroup(p).GetID();
    g_groups.emplace_back(name, id);
    Log("group %s: %s group of player %d -> stub %d", name.c_str(), kind.c_str(), num(3), id);
    return;
  }
  const int id = GroupId(name);
  if (id < 0) { Log("script: group %s: unknown group", name.c_str());  return; }
  ScGroup g(id);
  FightGroup f(id);
  BuildingGroup b(id);
  MiningGroup m(id);
  auto* impl = g.GetImpl();
  const bool fight = impl && *reinterpret_cast<uintptr_t*>(impl) == 0x4CFF00;  // FightGroupImpl's vtable
  if      (op == "take")       for (size_t i = 2; i < c.args.size(); ++i) g.TakeUnit(ScriptUnit(arg(i)));
  else if (op == "remove")     g.RemoveUnit(ScriptUnit(arg(2)));
  else if (op == "lights")     g.SetLights(num(2) != 0);
  else if (op == "enable")     g.Enable();
  else if (op == "disable")    g.Disable();
  else if (op == "destroy")    g.Destroy();
  else if (op == "rect")       { if (fight) f.SetRect(rect(2));  else b.SetRect(rect(2)); }
  else if (op == "guardedrect") f.AddGuardedRect(rect(2));
  else if (op == "clearguarded") f.ClearGuardedRects();
  else if (op == "guardrect")  f.DoGuardRect();
  else if (op == "patrolonly") f.DoPatrolOnly();
  else if (op == "attackenemy") f.DoAttackEnemy();
  else if (op == "guardgroup") f.DoGuardGroup();
  else if (op == "guardunit")  f.DoGuardUnit();
  else if (op == "attackunit") f.DoAttackUnit();
  else if (op == "exitmap")    f.DoExitMap();
  else if (op == "clearpatrol") f.ClearPatrolMode();
  else if (op == "combine")    { if (num(2)) f.SetCombineFire();  else f.ClearCombineFire(); }
  else if (op == "follow")     f.SetFollowMode(num(2));
  else if (op == "targetunit") f.SetTargetUnit(ScriptUnit(arg(2)));
  else if (op == "targetgroup") {
    // SetTargetGroup takes ScGroup by value: MSVC copies its one int onto the stack, but MinGW passes a class with
    // a destructor by reference, so call the export with the stub index itself.
    using Fn = void(__thiscall*)(FightGroup*, int);
    reinterpret_cast<Fn>(0x479F90)(&f, GroupId(arg(2)));
  }
  else if (op == "attacktype") f.SetAttackType(TypeByName(arg(2)));
  else if (op == "patrol") {
    static Location points[9];
    int n = 0;
    for (size_t i = 3; i < c.args.size() && n < 8; ++i) {
      int x = 0, y = 0;
      if (sscanf(c.args[i].c_str(), "%d,%d", &x, &y) == 2) points[n++] = Location(x + (c.fileCoords ? PadTiles() : 0), y);
    }
    points[n] = Location(-1, -1);
    PatrolRoute route = { num(2), points };
    f.SetPatrolMode(route);
  }
  else if (op == "mine")       m.Setup(ScriptUnit(arg(2)), ScriptUnit(arg(3)), rect(4));
  else if (op == "targcount")  g.SetTargCount(TypeByName(arg(2)), TypeByName(arg(3)), num(4));
  else if (op == "cleartargcount") g.ClearTargCount();
  else if (op == "reinforce")  b.RecordVehReinforceGroup(ScGroup(GroupId(arg(2))), num(3));
  else if (op == "unreinforce") b.UnRecordVehGroup(ScGroup(GroupId(arg(2))));
  else if (op == "building") {
    if (c.args.size() > 6) b.RecordBuilding(loc(2), TypeByName(arg(4)), TypeByName(arg(5)), ScGroup(GroupId(arg(6))));
    else b.RecordBuilding(loc(2), TypeByName(arg(4)), TypeByName(arg(5)));
  }
  else if (op == "tube")       b.RecordTube(loc(2));
  else if (op == "wall")       b.RecordWall(loc(2), c.args.size() > 4 ? TypeByName(arg(4)) : MapID::Wall);
  else if (op == "dump") {
    std::string members;
    if (impl) for (auto* n = impl->pUnitListHead_; n; n = n->pNext)
      members += " " + std::to_string(n->pUnit->index_) + ":" + std::to_string(int(n->classification)) + "@" +
                 std::to_string(n->issueCommandTick);
    Log("group %s (stub %d): %d units:%s", name.c_str(), id, impl ? impl->numUnits_ : -1, members.c_str());
    if (fight) {
      const auto* fi = f.GetImpl();
      Log("group %s: objective %d combine %d/%d@%d patrol %d follow %d rect (%ld,%ld,%ld,%ld) guarded %d", name.c_str(),
          fi->field_3E4, fi->combineFire_, fi->combineFireUnitIndex_, fi->field_310, fi->patrolMode_, fi->followMode_,
          fi->pixelRect_.left, fi->pixelRect_.top, fi->pixelRect_.right, fi->pixelRect_.bottom, fi->numGuardedRects_);
    }
  }
  else Log("script: group: unknown op %s", op.c_str());
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
  // A spawn that has died (or failed) gets no more orders: its object index may since belong to a shot or an
  // explosion, and an order to that crashes the game (Vehicle::SetDestinationWaypoints 0x42324F; fuzz seed 5, a Move to
  // a Scout 180 ticks after it died). The UI can't give such an order.
  if (const std::string a0 = arg(0); !a0.empty() && a0[0] == '@') {
    const size_t n = atoi(a0.c_str() + 1);
    if (n < g_spawnGone.size() && g_spawnGone[n]) { Log("script: spawn %u is gone: skipped", unsigned(n));  return; }
  }

  if      (c.verb == "move")         IssueMove(unit(0), loc(1));
  else if (c.verb == "dock")         IssueMove(unit(0), loc(1), CommandType::Dock);  // ctMoDock, to a dock tile
  else if (c.verb == "attack")       unit(0).DoAttack(unit(1));
  else if (c.verb == "guard")        IssueTargeted(unit(0), CommandType::Guard, unit(1).GetID(), 0, false);
  else if (c.verb == "attackground") IssueTargeted(unit(0), CommandType::Attack, loc(1).x, loc(1).y, true);
  else if (c.verb == "lights")       unit(0).DoSetLights(num(1) != 0);
  else if (c.verb == "movewp" || c.verb == "standground" || c.verb == "patrol") {
    // movewp/standground <unit> x,y[,range] ...; patrol <unit> <loop index> x,y[,range] ...
    const bool patrol = c.verb == "patrol";
    std::vector<Location> wps;
    std::vector<int> ranges;
    for (size_t i = patrol ? 2 : 1; i < c.args.size(); ++i) {
      int x = 0, y = 0, r = 0;
      if (sscanf(c.args[i].c_str(), "%d,%d,%d", &x, &y, &r) < 2) continue;
      wps.push_back(Location(x + (c.fileCoords ? PadTiles() : 0), y));
      ranges.push_back(r);
    }
    const CommandType type = patrol ? CommandType::Patrol
                           : c.verb == "standground" ? CommandType::StandGround : CommandType::Move;
    IssueWaypoints(unit(0), type, wps, ranges, patrol ? num(1) : -1);
  }
  else if (c.verb == "scatter") {
    // scatter <unit> <unit> ...: one ctMoScatter packet for the units, as the pane's SCATTER sends it (0x4242F0 moves
    // each unit away from their centre). Dead spawns are left out.
    CommandPacket packet = { CommandType::Scatter, 0 };
    auto& s = packet.data.move;
    uint8 n = 0;
    int owner = -1;
    for (size_t i = 0; i < c.args.size() && n < 32; ++i) {
      const std::string a = arg(i);
      if (!a.empty() && a[0] == '@' && size_t(atoi(a.c_str() + 1)) < g_spawnGone.size() && g_spawnGone[atoi(a.c_str() + 1)])
        continue;
      Unit u = unit(i);
      if (!u.IsLive()) continue;
      s.unitID[n++] = uint16(u.GetID());
      owner = u.GetOwner();
    }
    s.numUnits = n;
    packet.dataLength = uint16(1 + 2 * n);
    if (n) g_gameImpl.GetPlayer(owner)->ProcessCommandPacket(packet);
  }
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
  else if (c.verb == "group")        RunGroupCmd(c);
  else if (c.verb == "setcell") {
    const int pad = c.fileCoords ? PadTiles() : 0;
    for (int y = num(1); y <= num(3); ++y)
      for (int x = num(0); x <= num(2); ++x) GameMap::SetCellType(Location(x + pad, y), CellType(num(4)));
  }
  else if (!RunEcoCmd(c, tick)) Log("script: unknown verb %s", c.verb.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------
// Audio log (OP2H_AUDIO, `run.sh --audio`): every sound the game asks for, as `audio.jsonl` (README "Audio log").
// The game's own sound calls are hooked at their entry, so requests are logged whether or not sound is on:
//   AddMapSound (SoundManager 0x47EDB0, thiscall x, y, soundID; x = -1 for a sound without a position): every effect;
//   AddGameSound (0x47EFD0, thiscall soundID, player flag): a sound without a position, through AddMapSound;
//   AddSavantSound (0x47F010, thiscall soundID, x, y, player flag): a Savant line for the queue (0x47F050, priority
//   byte 0x56E7D0[id - 0x5E]);
//   AddMessage (MessageLog 0x439070, thiscall x, y, text, soundID): a status message, with its throttle verdict from
//   0x439590 (Savant ids 0x60..0xB1 are dropped, message and all, while a recent one of their group stands).
// The Savant queue is drained by SoundManager's per-frame update (0x47E9F0 -> 0x47F200: next line when the clock passes
// the last line's length + 750 ms), which only runs with sound on; the harness calls 0x47F200 itself on the same
// ticks (tick & 3 == 2) and logs what it starts as `play`. The throttle and the queue use timeGetTime: their call sites
// read a virtual clock instead, tick * 1000 / 36 ms (real time at speed 10), so the run is deterministic.
// The music playlist (MusicManager 0x565390: +0xC4 count, +0xC8 repeat index, +0xCC song list, +0xD0 position) is
// logged when it changes.

FILE* g_audio = nullptr;
bool  g_inPump = false;
int64_t Now();
int64_t Frequency();
constexpr uint32_t VirtualClockBase = 100000000;  // A machine up for a day: no throttle slot looks recent at start.
DWORD WINAPI VirtualTimeGetTime() { return VirtualClockBase + DWORD(int64_t(g_gameImpl.tick_) * 1000 / 36); }
void* g_virtualClock = reinterpret_cast<void*>(&VirtualTimeGetTime);  // The `call [ptr]` sites point here.

const char* SoundName(int id) {
  const auto* names = reinterpret_cast<const char* const*>(0x4E0170);  // SoundManager's file names, by SoundID.
  if (id == 0) return "";  // A message without a sound (AddMessage plays beep8 for it).
  return (id > 0 && id < 0xFA && names[id]) ? names[id] : "?";
}

// Internal pixel -> file pixel (x); -1 stays -1 (no position).
int FileX(int x) { return x == -1 ? -1 : x - PadTiles() * 32; }

// `at` is the return address in Outpost2.exe of the call that asked for the sound, or "opu" for code outside it (OPU
// replaces some of the game's message code; its DLL's addresses aren't stable between runs).
void AudioEvent(const char* ev, int id, int x, int y, void* caller, const char* extra = "") {
  if (!g_audio) return;
  const uintptr_t a = reinterpret_cast<uintptr_t>(caller);
  char at[16];
  if (a >= 0x401000 && a < 0x4CF000) snprintf(at, sizeof(at), "%06x", unsigned(a));
  else strcpy(at, "opu");
  fprintf(g_audio, "{\"tick\":%d,\"ev\":\"%s\",\"id\":%d,\"name\":\"%s\",\"x\":%d,\"y\":%d,\"at\":\"%s\"%s}\n",
          g_gameImpl.tick_, ev, id, SoundName(id), FileX(x), y, at, extra);
}

// An inline hook: the function's first `len` bytes (whole, position-independent instructions, checked against
// `expect`) move to a trampoline that jumps back, and the function starts with a jump to `hook`.
// A function another mod already starts with a jump (OPU hooks some) is chained: the hook calls that jump's target.
void* HookEntry(uintptr_t fn, const uint8_t* expect, size_t len, void* hook) {
  const auto* code = reinterpret_cast<const uint8_t*>(fn);
  if (code[0] == 0xE9) {
    int32_t rel;
    memcpy(&rel, code + 1, 4);
    void* prev = reinterpret_cast<void*>(fn + 5 + rel);
    Log("audio: %08x already jumps to %p; chaining", unsigned(fn), prev);
    JumpTo(fn, hook);
    return prev;
  }
  if (memcmp(code, expect, len) != 0) {
    Log("audio: unexpected code at %08x: %02x %02x %02x %02x %02x %02x", unsigned(fn), code[0], code[1], code[2], code[3],
        code[4], code[5]);
    return nullptr;
  }
  auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  memcpy(t, reinterpret_cast<void*>(fn), len);
  t[len] = 0xE9;
  const int32_t rel = int32_t(fn + len - (reinterpret_cast<uintptr_t>(t) + len + 5));
  memcpy(t + len + 1, &rel, 4);
  JumpTo(fn, hook);
  return t;
}

using AddMapSoundFn = int(__thiscall*)(void*, int, int, int);
using AddGameSoundFn = int(__thiscall*)(void*, int, int);
using SavantFn      = int(__thiscall*)(void*, int, int, int, int);
using AddMessageFn  = int(__thiscall*)(void*, int, int, const char*, int);
using ThrottleFn    = int(__thiscall*)(void*, int, int, int);
AddMapSoundFn g_addMapSound;
AddGameSoundFn g_addGameSound;
SavantFn      g_savant;
void*         g_outerCaller = nullptr;  // AddGameSound's caller while it calls AddMapSound.
AddMessageFn  g_addMessage;
ThrottleFn    g_throttle;
int g_throttled = -1;  // The verdict of the throttle call made by the AddMessage in progress.

int __fastcall AddMapSoundHook(void* self, void*, int x, int y, int id) {
  AudioEvent(g_inPump ? "play" : "sfx", id, x, y, g_outerCaller ? g_outerCaller : __builtin_return_address(0));
  return g_addMapSound(self, x, y, id);
}

int __fastcall AddGameSoundHook(void* self, void*, int id, int flag) {
  g_outerCaller = __builtin_return_address(0);
  const int r = g_addGameSound(self, id, flag);
  g_outerCaller = nullptr;
  return r;
}

int __fastcall SavantHook(void* self, void*, int id, int x, int y, int flag) {
  AudioEvent("savant", id, x, y, __builtin_return_address(0));
  return g_savant(self, id, x, y, flag);
}

int __fastcall ThrottleHook(void* self, void*, int id, int x, int y) {
  return g_throttled = g_throttle(self, id, x, y);
}

void MessageEvent(int x, int y, const char* text);
int __fastcall AddMessageHook(void* self, void*, int x, int y, const char* text, int sound) {
  g_throttled = -1;
  void* caller = __builtin_return_address(0);
  const int r = g_addMessage(self, x, y, text, sound);
  // pass: 1 shown (and voiced), 0 dropped by the throttle, -1 not a throttled id.
  char extra[32];
  snprintf(extra, sizeof(extra), ",\"pass\":%d", g_throttled);
  AudioEvent("msg", sound, x, y, caller, extra);
  MessageEvent(x, y, text);
  return r;
}

void WriteAudioTables() {
  if (!g_audio) return;
  // Savant priorities as SavantQueue reads them (0x47F180), the table SoundManager's constructor fills (0x47DCE0) and
  // each sound's length in ms (0x56DC00, 12 bytes per id, from the WAV header at load, 0x47F300).
  const auto* b = reinterpret_cast<const uint8_t*>(0x56E7D0);
  fprintf(g_audio, "{\"tick\":%d,\"ev\":\"tables\",\"prio\":[", g_gameImpl.tick_);
  for (int id = 0x5E; id <= 0xE3; ++id) fprintf(g_audio, "%s%d", id > 0x5E ? "," : "", b[id - 0x5E]);
  fprintf(g_audio, "],\"prio_ctor\":[");
  for (int id = 0x5E; id <= 0xE3; ++id) fprintf(g_audio, "%s%d", id > 0x5E ? "," : "", b[id]);
  fprintf(g_audio, "],\"ms\":[");
  for (int id = 0; id < 0xFA; ++id) fprintf(g_audio, "%s%d", id ? "," : "", *reinterpret_cast<const int*>(0x56DC00 + 12 * id));
  fprintf(g_audio, "]}\n");
}

struct MusicState {
  std::vector<int> songs;
  int repeat = -1, pos = -1;
  bool operator==(const MusicState& o) const { return songs == o.songs && repeat == o.repeat && pos == o.pos; }
};
MusicState g_music;

void LogMusic(const char* when) {
  if (!g_audio) return;
  MusicState m;
  const int n = *reinterpret_cast<const int*>(0x565454);
  const int* list = *reinterpret_cast<int* const*>(0x56545C);
  if (list) m.songs.assign(list, list + std::clamp(n, 0, 64));
  m.repeat = *reinterpret_cast<const int*>(0x565458);
  m.pos    = *reinterpret_cast<const int*>(0x565460);
  if (m == g_music) return;
  g_music = m;
  fprintf(g_audio, "{\"tick\":%d,\"ev\":\"music\",\"when\":\"%s\",\"songs\":[", g_gameImpl.tick_, when);
  for (size_t i = 0; i < m.songs.size(); ++i) fprintf(g_audio, "%s%d", i ? "," : "", m.songs[i]);
  fprintf(g_audio, "],\"repeat\":%d,\"pos\":%d}\n", m.repeat, m.pos);
}

// After each game cycle: the Savant queue's turn (as SoundManager's per-frame update would take it), then the music.
void AudioTick(int tick) {
  if (!g_audio) return;
  if ((tick & 3) == 2) {
    g_inPump = true;
    reinterpret_cast<void(__thiscall*)(void*)>(0x47F200)(reinterpret_cast<void*>(0x56D250));
    g_inPump = false;
  }
  LogMusic("tick");
  fflush(g_audio);
}

// PrepareGame starts the music (MusicManager::Init 0x4508D0, called at 0x489530: the default playlist and a 92 ms
// streaming timer) before the mission's InitProc runs (0x48987A), so a mission's SetMusicPlayList replaces a list that
// is already running. Whether the default's first track got to start depends on the time in between: logged here.
using ThisFn = int(__thiscall*)(void*);
ThisFn  g_musicInit, g_initProc;
int64_t g_musicInitAt = 0;
int __fastcall MusicInitHook(void* self, void*) {
  g_musicInitAt = Now();
  return g_musicInit(self);
}
// The message log's six throttle stamps (MessageLog 0x54FCF0 +0x1310..+0x132C) are set by its static initializer
// (0x438FE0) from the real timeGetTime, before the harness hooks the clock; once the machine has been up past the virtual
// clock's base (~27.7 h) they all look recent and no throttled message passes all run. They are rebased here to what
// the message log's reset (0x439510) would write at time 0, so a run's verdicts don't depend on the machine's uptime.
void RebaseMessageThrottle() {
  static const struct { uint32_t addr, offset; } stamps[] = {
    { 0x55101C, 5000 }, { 0x551010, 15000 }, { 0x551000, 25000 }, { 0x551008, 35000 }, { 0x551014, 45000 }, { 0x551018, 55000 } };
  for (const auto& s : stamps) *reinterpret_cast<uint32_t*>(s.addr) = s.offset;
}
int __fastcall InitProcHook(void* self, void*) {
  Log("audio: InitProc starts %.1f ms after MusicManager::Init", (Now() - g_musicInitAt) * 1000.0 / Frequency());
  RebaseMessageThrottle();
  return g_initProc(self);
}

// Session state without the audio log: only the status message hook, for `msg` events (nothing else changes).
void InstallMessageHook() {
  static const uint8_t message[] = { 0x53, 0x56, 0x57, 0x8B, 0xD9 };  // push ebx; push esi; push edi; mov ebx,ecx
  g_addMessage = reinterpret_cast<AddMessageFn>(HookEntry(0x439070, message, 5, reinterpret_cast<void*>(&AddMessageHook)));
  Log("state: message hook %s", g_addMessage ? "installed" : "FAILED");
}

void InstallAudioHooks() {
  g_musicInit = reinterpret_cast<ThisFn>(RelinkCall(0x489530, reinterpret_cast<void*>(&MusicInitHook)));
  g_initProc  = reinterpret_cast<ThisFn>(RelinkCall(0x48987A, reinterpret_cast<void*>(&InitProcHook)));
  static const uint8_t mapSound[] = { 0x83, 0xEC, 0x10, 0x53, 0x56 };  // sub esp,10h; push ebx; push esi
  static const uint8_t gameSound[] = { 0x8B, 0x44, 0x24, 0x08, 0x56 };  // mov eax,[esp+8]; push esi
  static const uint8_t savant[]   = { 0x53, 0x8B, 0xD1, 0x8B, 0x44, 0x24, 0x14 };  // push ebx; mov edx,ecx; mov eax,[esp+14h]
  static const uint8_t message[]  = { 0x53, 0x56, 0x57, 0x8B, 0xD9 };  // push ebx; push esi; push edi; mov ebx,ecx
  static const uint8_t throttle[] = { 0x83, 0xEC, 0x04, 0x53, 0x56 };  // sub esp,4; push ebx; push esi
  g_addMapSound = reinterpret_cast<AddMapSoundFn>(HookEntry(0x47EDB0, mapSound, 5, reinterpret_cast<void*>(&AddMapSoundHook)));
  g_addGameSound = reinterpret_cast<AddGameSoundFn>(HookEntry(0x47EFD0, gameSound, 5, reinterpret_cast<void*>(&AddGameSoundHook)));
  g_savant      = reinterpret_cast<SavantFn>(HookEntry(0x47F010, savant, 7, reinterpret_cast<void*>(&SavantHook)));
  g_addMessage  = reinterpret_cast<AddMessageFn>(HookEntry(0x439070, message, 5, reinterpret_cast<void*>(&AddMessageHook)));
  g_throttle    = reinterpret_cast<ThrottleFn>(HookEntry(0x439590, throttle, 5, reinterpret_cast<void*>(&ThrottleHook)));
  // `call [timeGetTime]` (FF 15 E0 38 58 00) in AddMessage, the throttle, the message log reset and the Savant queue.
  for (uintptr_t site : { 0x4390D1u, 0x439514u, 0x439599u, 0x47F108u, 0x47F206u, 0x47F2DBu }) {
    static const uint8_t call[] = { 0xFF, 0x15, 0xE0, 0x38, 0x58, 0x00 };
    if (memcmp(reinterpret_cast<void*>(site), call, 6) != 0) { Log("audio: no timeGetTime call at %08x", unsigned(site));  continue; }
    void** slot = &g_virtualClock;
    WriteCode(site + 2, &slot, 4);
  }
  Log("audio: hooks %s", g_addMapSound && g_addGameSound && g_savant && g_addMessage && g_throttle ? "installed" : "INCOMPLETE");
}

// ---------------------------------------------------------------------------------------------------------------------
// Optional bot DLL: exports `void __cdecl HarnessBotTick(int tick)`, called after every game cycle.

using BotTickFn = void(__cdecl*)(int);
BotTickFn g_botTick = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// Session state (OP2H_STATE, `run.sh --state`; README "Session state"): for tools that play a mission by replaying a
// growing script (op2-inspect's game_* tools). Throughout the run `events.jsonl` gets what a player would notice:
//   new / gone   a unit (structure or vehicle of any player) appeared / disappeared (object index, type, owner, tile)
//   hit          a unit lost hit points (the amount and what is left)
//   idle         a unit of player 0 finished its command (`was`: the command it had, e.g. Develop for a new structure)
//   tech         player 0 got a topic; objective: a victory condition's "met" changed; msg: a status message;
//   disaster     a disaster object appeared (scheduled: it warns, then strikes); end: the mission's end reason
// and at the end `state.json` holds the whole state (colony, labs, every unit and Gaia object, the tech tree, the
// objectives, the cell grid). Coordinates are file tiles. Reading state changes nothing in the game: a run with
// --state plays exactly like one without. OP2H_STOP (`--stop`) ends the run at the first event of the listed kinds
// after tick OP2H_STOP_FROM (`kind` or `kind:<unit id>`, comma separated; `any` = every kind but msg and hit).

FILE* g_events = nullptr;
bool  g_stateOn = false;
std::string g_stopSpec;
int   g_stopFrom = 0;
std::string g_stopReason;  // Set when an event matched OP2H_STOP: the cycle hook quits after this tick.

void JsonStr(FILE* f, const char* s) {
  fputc('"', f);
  for (; s && *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c == '"' || c == '\\') { fputc('\\', f);  fputc(c, f); }
    else if (c < 0x20 || c >= 0x80) fprintf(f, "\\u%04x", c);  // Control characters; Latin-1 as its code point.
    else fputc(c, f);
  }
  fputc('"', f);
}

// Whether an event of `kind` (about unit `id`, -1 for none) is one OP2H_STOP asks to stop at.
bool StopMatches(const char* kind, int id) {
  if (g_stopSpec.empty() || g_gameImpl.tick_ <= g_stopFrom) return false;
  const std::string k = kind;
  size_t pos = 0;
  while (pos <= g_stopSpec.size()) {
    const size_t end = std::min(g_stopSpec.find(',', pos), g_stopSpec.size());
    const std::string tok = g_stopSpec.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = tok.find(':');
    const std::string tk = tok.substr(0, colon);
    if (tk == "any" && k != "msg" && k != "hit") return true;
    if (tk != k) continue;
    if (colon == std::string::npos || atoi(tok.c_str() + colon + 1) == id) return true;
  }
  return false;
}

// Starts an event line; the caller adds fields (",\"x\":1") and calls EventEnd.
bool EventBegin(const char* kind) {
  if (!g_events) return false;
  fprintf(g_events, "{\"tick\":%d,\"ev\":\"%s\"", g_gameImpl.tick_, kind);
  return true;
}
void EventEnd(const char* kind, int id) {
  fputs("}\n", g_events);
  if (g_stopReason.empty() && StopMatches(kind, id)) g_stopReason = std::string(kind) + (id >= 0 ? " " + std::to_string(id) : "");
}

void EventUnit(const char* kind, int id, int type, int owner, int x, int y, const char* extra = "") {
  if (!EventBegin(kind)) return;
  fprintf(g_events, ",\"id\":%d,\"type\":\"%s\",\"owner\":%d,\"x\":%d,\"y\":%d%s", id, TypeName(MapID(type)), owner, x, y, extra);
  EventEnd(owner == 0 || std::string(kind) != "new" ? kind : "enemy", id);
}

void MessageEvent(int x, int y, const char* text) {
  if (!EventBegin("msg")) return;
  fputs(",\"text\":", g_events);
  JsonStr(g_events, text);
  // x/y are internal pixels, -1 for a message without a place.
  fprintf(g_events, ",\"x\":%d,\"y\":%d", x < 0 ? -1 : x / 32 - PadTiles(), y < 0 ? -1 : y / 32);
  EventEnd("msg", -1);
}

// What the last session tick saw of each object index.
struct SeenUnit { int type = -1, owner = -1, cmd = 0, damage = 0, x = 0, y = 0;  bool live = false;  int destX = -1, destY = -1; };
std::vector<SeenUnit> g_seen;
std::vector<uint8_t> g_seenTech;      // Per tech number: player 0 has it.
std::vector<int>     g_seenObjective;  // Per ScStub index: -1 not a victory condition, else met (0/1).
std::vector<int>     g_seenDisaster;   // Per object index: the disaster type seen there (-1 none).
bool g_sessionStarted = false;

constexpr uintptr_t VictoryConditionVtbl = 0x4D68C0;  // VictoryConditionImpl (TethysAPI ScBase.h).

// The victory conditions' trigger has fired for player 0: the objective is met now.
int ObjectiveMet(const VictoryConditionImpl* v) {
  const TriggerImpl* t = TriggerImpl::GetInstance(v->victoryTriggerIndex_);
  if (!t) return 0;
  uint32_t mask;
  memcpy(&mask, &t->hasFired_, sizeof(mask));
  return int(mask & 1);
}

template <typename F> void ForEachObjective(F f) {
  ScStubList* list = ScStubList::GetInstance();
  for (size_t i = 0; i < ScStubList::MaxNumScStubs; ++i) {
    ScBase* s = list->At(i);
    if (!s || IsBadReadPtr(s, sizeof(VictoryConditionImpl)) || *reinterpret_cast<uintptr_t*>(s) != VictoryConditionVtbl) continue;
    f(int(i), static_cast<const VictoryConditionImpl*>(static_cast<TriggerImpl*>(s)));
  }
}

int HighestObjectIndex() { return std::min(*reinterpret_cast<const int*>(0x54F838), int(MapImpl::GetInstance()->MaxNumUnits()) - 1); }

bool IsDisaster(MapID t) { return t >= MapID::Lightning && t <= MapID::Meteor; }

// After each cycle (and its script commands): compare with what the previous tick saw and log the differences.
void SessionTick(int tick) {
  if (!g_events) return;
  const bool baseline = !g_sessionStarted;
  g_sessionStarted = true;
  const int pad = PadTiles();
  // Units of every player.
  if (g_seen.size() < MapImpl::GetInstance()->MaxNumUnits()) g_seen.resize(MapImpl::GetInstance()->MaxNumUnits());
  std::vector<uint8_t> now(g_seen.size(), 0);
  for (int p = 0; p < g_gameImpl.numPlayers_; ++p) {
    auto visit = [&](Unit u) {
      if (!u.IsLive()) return;
      const MapObject* mo = u.GetMapObject();
      const int id = u.GetID();
      if (id <= 0 || size_t(id) >= g_seen.size()) return;
      now[id] = 1;
      SeenUnit& s = g_seen[id];
      const Location at = u.GetLocation();
      const int type = int(u.GetType()), owner = u.GetOwner(), x = at.x - pad, y = at.y;
      if (!s.live || s.type != type || s.owner != owner) {
        if (s.live && !baseline) EventUnit("gone", id, s.type, s.owner, s.x, s.y);
        if (!baseline) EventUnit("new", id, type, owner, x, y);
        s = SeenUnit{ type, owner, mo->command_, mo->damage_, x, y, true };
        return;
      }
      if (mo->damage_ > s.damage && !baseline) {
        char extra[64];
        snprintf(extra, sizeof(extra), ",\"dmg\":%d,\"hp\":%d", mo->damage_ - s.damage, u.GetMaxHitpoints() - mo->damage_);
        EventUnit("hit", id, type, owner, x, y, extra);
      }
      const int cmd = mo->command_;
      if (owner == 0 && cmd == int(CommandType::Nop) && s.cmd != cmd && s.cmd != int(CommandType::Idle) && !baseline) {
        // Where it was going (a vehicle stopped short, e.g. by an EMP, isn't "arrived"), and whether it is EMPed.
        char extra[96];
        snprintf(extra, sizeof(extra), ",\"was\":\"%s\",\"dest\":[%d,%d],\"emp\":%d", CommandName(s.cmd), s.destX, s.destY,
                 (mo->flags_ & MoFlagEMPed) ? 1 : 0);
        EventUnit("idle", id, type, owner, x, y, extra);
      }
      if (u.IsVehicle() && cmd != int(CommandType::Nop)) {
        const PathContext* pc = mo->pPathContext_;
        if (pc && !IsBadReadPtr(pc, sizeof(PathContext))) s.destX = pc->dstTileX - pad, s.destY = pc->dstTileY;
      } else if (cmd == int(CommandType::Nop)) {
        s.destX = s.destY = -1;
      }
      s.cmd = cmd, s.damage = mo->damage_, s.x = x, s.y = y;
    };
    for (Unit u : PlayerBuildingEnum(p)) visit(u);
    for (Unit u : PlayerVehicleEnum(p))  visit(u);
  }
  for (size_t id = 1; id < g_seen.size(); ++id)
    if (g_seen[id].live && !now[id]) {
      if (!baseline) EventUnit("gone", int(id), g_seen[id].type, g_seen[id].owner, g_seen[id].x, g_seen[id].y);
      g_seen[id].live = false;
    }
  // Player 0's research.
  if (g_seenTech.size() != size_t(g_research.numTechs_)) g_seenTech.assign(g_research.numTechs_, 0);
  for (int t = 0; t < g_research.numTechs_; ++t) {
    const uint8_t has = g_research.HasTech(0, t) ? 1 : 0;
    if (has && !g_seenTech[t] && !baseline && EventBegin("tech")) {
      fprintf(g_events, ",\"id\":%d,\"name\":", g_research.ppTechInfos_[t]->techID);
      JsonStr(g_events, g_research.ppTechInfos_[t]->pTechName);
      EventEnd("tech", g_research.ppTechInfos_[t]->techID);
    }
    g_seenTech[t] = has;
  }
  // Objectives.
  if (g_seenObjective.empty()) g_seenObjective.assign(ScStubList::MaxNumScStubs, -1);
  ForEachObjective([&](int i, const VictoryConditionImpl* v) {
    const int met = ObjectiveMet(v);
    if (g_seenObjective[i] != met && (g_seenObjective[i] >= 0 || met) && EventBegin("objective")) {
      fprintf(g_events, ",\"met\":%d,\"text\":", met);
      JsonStr(g_events, v->pObjectiveText_);
      EventEnd("objective", -1);
    }
    g_seenObjective[i] = met;
  });
  // Disasters (scheduled ones exist from their first warning on).
  if (g_seenDisaster.size() != g_seen.size()) g_seenDisaster.assign(g_seen.size(), -1);
  const int last = HighestObjectIndex();
  for (int i = 1; i <= last && size_t(i) < g_seenDisaster.size(); ++i) {
    const MapObject* mo = MapObject::GetInstance(i);
    const int t = (mo && mo->IsLive() && IsDisaster(mo->GetTypeID())) ? int(mo->GetTypeID()) : -1;
    if (t >= 0 && g_seenDisaster[i] != t && EventBegin("disaster")) {
      fprintf(g_events, ",\"type\":\"%s\",\"x\":%d,\"y\":%d,\"timer\":%d", TypeName(MapID(t)), mo->pixelX_ / 32 - pad,
              mo->pixelY_ / 32, mo->actionTimer_);
      EventEnd("disaster", -1);
    }
    g_seenDisaster[i] = t;
  }
  if (g_gameImpl.gameTermReasons_ != GameTermReasons::Running && EventBegin("end")) {
    fprintf(g_events, ",\"reason\":%d", int(g_gameImpl.gameTermReasons_));
    EventEnd("end", -1);
  }
  (void)tick;
  fflush(g_events);
}

// One unit of state.json: the telemetry fields plus what a player reads off its panel.
void WriteStateUnit(FILE* f, bool& first, Unit u) {
  if (!u.IsLive()) return;
  const MapObject* mo = u.GetMapObject();
  const auto* b = reinterpret_cast<const uint8_t*>(mo);
  const Location at = u.GetLocation();
  fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"owner\":%d,\"x\":%d,\"y\":%d,\"hp\":%d,\"maxHp\":%d,\"cmd\":\"%s\",\"act\":\"%s\"",
          first ? "" : ",\n", u.GetID(), TypeName(u.GetType()), u.GetOwner(), at.x - PadTiles(), at.y,
          u.GetMaxHitpoints() - u.GetDamage(), u.GetMaxHitpoints(), CommandName(mo->command_), ActionName(int(mo->action_)));
  first = false;
  if (u.IsVehicle()) {
    const auto* v = static_cast<const Vehicle*>(mo);
    if (u.GetType() == MapID::CargoTruck)
      fprintf(f, ",\"truck\":[%d,%d]", int(mo->truckCargoType_), int(mo->truckCargoAmount_));
    else if (u.GetType() == MapID::ConVec) {
      if (mo->cargo_ > 0 && mo->cargo_ < 0x80) fprintf(f, ",\"kit\":\"%s\"", TypeName(MapID(mo->cargo_)));
      if (v->weaponOfCargo_ > 0) fprintf(f, ",\"kitWeapon\":\"%s\"", TypeName(MapID(v->weaponOfCargo_)));
    } else if (const int w = mo->weapon_; w >= int(MapID::AcidCloud) && w <= int(MapID::BFG))
      fprintf(f, ",\"weapon\":\"%s\"", TypeName(MapID(w)));
    const PathContext* pc = mo->pPathContext_;
    if (pc && !IsBadReadPtr(pc, sizeof(PathContext)) && mo->command_ != int(CommandType::Nop))
      fprintf(f, ",\"dest\":[%d,%d]", pc->dstTileX - PadTiles(), pc->dstTileY);
    if (mo->flags_ & MoFlagEMPed) fputs(",\"emp\":1", f);
    fputc('}', f);
    return;
  }
  // Structures: status flags, the dock, labs' research, factories' bays and production.
  const uint32_t fl = mo->flags_;
  fprintf(f, ",\"flags\":%u", unsigned(fl));
  if (const Location d = u.GetDockLocation(); d) fprintf(f, ",\"dock\":[%d,%d]", d.x - PadTiles(), d.y);
  if (const int w = mo->weapon_; u.GetType() == MapID::GuardPost && w >= int(MapID::AcidCloud) && w <= int(MapID::BFG))
    fprintf(f, ",\"weapon\":\"%s\"", TypeName(MapID(w)));
  if (mo->command_ == int(CommandType::Research)) {
    // ECONOMY.md section 7: tech number +0x60, team +0x64, work left in actionTimer (cost << 8 at the start).
    const int num = *reinterpret_cast<const int*>(b + 0x60), team = *reinterpret_cast<const int*>(b + 0x64);
    if (num >= 0 && num < g_research.numTechs_) {
      const TechInfo* t = g_research.ppTechInfos_[num];
      const int cost = (fl & 0x1000) ? t->edenCost : t->plymouthCost;
      fprintf(f, ",\"research\":{\"tech\":%d,\"team\":%d,\"left\":%d,\"cost\":%d}", t->techID, team, mo->actionTimer_, cost << 8);
    }
  }
  if (fl & MoFlagBldFactory) {
    const auto* fb = static_cast<const FactoryBuilding*>(mo);
    fputs(",\"bays\":[", f);
    for (int i = 0; i < 6; ++i) {
      const int c = fb->cargoBayContents_[i], w = static_cast<const Building*>(mo)->cargoBayCargoOrWeapon_[i];
      if (i) fputc(',', f);
      if (c > 0) {
        fprintf(f, "\"%s", TypeName(MapID(c)));
        if (w > 0) fprintf(f, " %s", TypeName(MapID(w)));
        fputc('"', f);
      } else fputs("null", f);
    }
    fputc(']', f);
    if (mo->command_ == int(CommandType::Produce) && fb->itemToProduce_ > 0)
      fprintf(f, ",\"producing\":\"%s\",\"timer\":%d", TypeName(MapID(fb->itemToProduce_)), mo->actionTimer_);
  }
  fputc('}', f);
}

void WriteSessionState(int tick) {
  if (!g_stateOn) return;
  FILE* f = fopen((cfg.out + "\\state.json").c_str(), "w");
  if (!f) return;
  auto& game = g_gameImpl;
  const int w = GameMap::GetWidth(), h = GameMap::GetHeight(), pad = PadTiles();
  fprintf(f, "{\"tick\":%d,\"end\":%d,\"stop\":", tick, int(game.gameTermReasons_));
  JsonStr(f, g_stopReason.c_str());
  fprintf(f, ",\"map\":{\"width\":%d,\"height\":%d,\"pad\":%d},\n\"players\":[", w, h, pad);
  for (int i = 0; i < game.numPlayers_; ++i) {
    PlayerImpl* p = game.GetPlayer(i);
    const auto& m = p->moraleState_;
    fprintf(f, "%s{\"id\":%d,\"human\":%d,\"eden\":%d,\"food\":%d,\"commonOre\":%d,\"rareOre\":%d,\"workers\":%d,"
               "\"scientists\":%d,\"kids\":%d,\"morale\":%d,\"moraleLevel\":%d,\"powerGen\":%d,\"powerUsed\":%d,"
               "\"foodNet\":%d,\"foodProd\":%d,\"foodCons\":%d,\"maxCommon\":%d,\"maxRare\":%d,\"maxFood\":%d,"
               "\"workersReq\":%d,\"scientistsReq\":%d,\"availWorkers\":%d,\"availScientists\":%d,\"sciResearching\":%d,"
               "\"sciAsWorkers\":%d,\"buildings\":%d,\"idleBuildings\":%d,\"unpowered\":%d}",
            i ? ",\n" : "", i, p->isHuman_ != 0, p->isEden_ != 0, p->foodStored_, p->commonOre_, p->rareOre_,
            p->numWorkers_, p->numScientists_, p->numKids_, m.morale, int(m.moraleLevel), p->amountPowerGenerated_,
            p->amountPowerConsumed_, p->netFoodProduction_, p->totalFoodProduction_, p->totalFoodConsumption_,
            p->maxCommonOre_, p->maxRareOre_, p->maxFood_, p->numWorkersRequired_, p->numScientistsRequired_,
            p->numAvailableWorkers_, p->numAvailableScientists_, p->numScientistsResearching_, p->numScientistsAsWorkers_,
            p->numBuildings_, p->numIdleBuildings_, p->numUnpoweredStructures_);
  }
  // The tech tree: id, name, lab (1 basic, 2 standard, 3 advanced), Eden and Plymouth cost, max scientists, the
  // topics it requires (ids) and which players have it (bitmask).
  fputs("],\n\"techs\":[", f);
  for (int t = 0; t < g_research.numTechs_; ++t) {
    const TechInfo* ti = g_research.ppTechInfos_[t];
    fprintf(f, "%s{\"id\":%d,\"name\":", t ? ",\n" : "", ti->techID);
    JsonStr(f, ti->pTechName);
    uint32_t has;
    memcpy(&has, &ti->playerHasTechMask, sizeof(has));
    fprintf(f, ",\"lab\":%d,\"eden\":%d,\"ply\":%d,\"max\":%d,\"has\":%u,\"req\":[", int(ti->lab), ti->edenCost,
            ti->plymouthCost, ti->maxScientists, has & 0x3F);
    for (int r = 0; r < ti->numRequiredTechs && ti->pRequiredTechNum; ++r) {
      const int n = ti->pRequiredTechNum[r];
      fprintf(f, "%s%d", r ? "," : "", n >= 0 && n < g_research.numTechs_ ? g_research.ppTechInfos_[n]->techID : -1);
    }
    fputs("]}", f);
  }
  // Per vehicle, structure and weapon type, the id of the research topic that makes it available (MapObjectType +0x1E4, as
  // the eco run's typeExtra; 0 for none): what the factory and build panes offer.
  fputs("],\n\"typeTopics\":{", f);
  for (int i = 1; i <= int(MapID::BFG); ++i) {
    const uint8_t* t = reinterpret_cast<const uint8_t*>(MapObjectType::GetInstance(i));
    if (!t) continue;
    const int num = *reinterpret_cast<const int*>(t + 0x1E4);  // A tech number: the index in the loaded tree.
    fprintf(f, "%s\"%s\":%d", i > 1 ? "," : "", TypeName(MapID(i)),
            num > 0 && num < g_research.numTechs_ ? g_research.ppTechInfos_[num]->techID : 0);
  }
  fputs("},\n\"objectives\":[", f);
  bool first = true;
  ForEachObjective([&](int, const VictoryConditionImpl* v) {
    fprintf(f, "%s{\"met\":%d,\"text\":", first ? "" : ",", ObjectiveMet(v));
    JsonStr(f, v->pObjectiveText_);
    fputc('}', f);
    first = false;
  });
  fputs("],\n\"units\":[\n", f);
  first = true;
  for (int i = 0; i < game.numPlayers_; ++i) {
    for (Unit u : PlayerBuildingEnum(i)) WriteStateUnit(f, first, u);
    for (Unit u : PlayerVehicleEnum(i))  WriteStateUnit(f, first, u);
  }
  // Gaia: beacons (ore 0 common / 1 rare; bar 0 = 3-bar, 1 = 2-bar, 2 = 1-bar; surveyed by player 0), vents,
  // fumaroles, wreckage (its tech) and disasters.
  fputs("],\n\"gaia\":[", f);
  first = true;
  const int last = HighestObjectIndex();
  for (int i = 1; i <= last; ++i) {
    const MapObject* mo = MapObject::GetInstance(i);
    if (!mo || !mo->IsLive()) continue;
    const MapID t = mo->GetTypeID();
    if (!(t == MapID::MiningBeacon || t == MapID::MagmaVent || t == MapID::Fumarole || t == MapID::Wreckage || IsDisaster(t))) continue;
    fprintf(f, "%s{\"id\":%d,\"type\":\"%s\",\"x\":%d,\"y\":%d", first ? "" : ",\n", i, TypeName(t), mo->pixelX_ / 32 - pad,
            mo->pixelY_ / 32);
    first = false;
    if (t == MapID::MiningBeacon) {
      const auto* mb = static_cast<const MapObj::MiningBeacon*>(mo);
      fprintf(f, ",\"ore\":%d,\"bar\":%d,\"variant\":%d,\"surveyed\":%d", int(mb->mineType_), int(mb->mineYield_),
              mb->mineVariant_, int(mb->playerSurveyedMask_.Get(0)));
    } else if (t == MapID::Wreckage) {
      const auto* wr = static_cast<const MapObj::Wreckage*>(mo);
      uint32_t found;
      memcpy(&found, &wr->playerDiscoveredMask_, sizeof(found));
      fprintf(f, ",\"tech\":%d,\"discovered\":%d", wr->techID_, int(found & 1));
    } else if (IsDisaster(t)) {
      fprintf(f, ",\"timer\":%d,\"act\":\"%s\"", mo->actionTimer_, ActionName(int(mo->action_)));
    }
    fputc('}', f);
  }
  // Cells, file coordinates row by row: cell type | lava << 5 | microbe << 6 | wall or building << 7 | lava possible << 8.
  fputs("],\n\"cells\":[", f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const TileData t = GameMap::GetImpl()->Tile(x + pad, y);
      fprintf(f, "%s%u", (x | y) ? "," : "", t.cellType | (t.lava << 5) | (t.microbe << 6) | (t.wallOrBuilding << 7) | (t.lavaPossible << 8));
    }
  fputs("]}\n", f);
  fclose(f);
}

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
  WriteSessionState(g_gameImpl.tick_);
  if (g_events) fclose(g_events);
  if (g_telem) fclose(g_telem);
  if (g_trace) fclose(g_trace);
  if (g_audio) fclose(g_audio);
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
    WriteAudioTables();
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
  AudioTick(tick);

  // Sample at tick 1 (telemetry) / the first fully drawn frame (screenshot), then at multiples of the interval.
  if (cfg.telemEvery > 0 && tick >= g_nextTelem) { WriteTelemetry(tick);  g_nextTelem = (tick / cfg.telemEvery + 1) * cfg.telemEvery; }
  if (cfg.traceEvery > 0 && tick >= g_nextTrace) { WriteTrace(tick);      g_nextTrace = (tick / cfg.traceEvery + 1) * cfg.traceEvery; }
  if (cfg.shotEvery  > 0 && tick >= g_nextShot)  { TakeShot(tick);        g_nextShot  = (tick / cfg.shotEvery  + 1) * cfg.shotEvery;  }
  SessionTick(tick);
  if (!g_stopReason.empty()) Quit(("stop: " + g_stopReason).c_str());
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

  // Campaign start: PrepareGame takes a MissionResults (GameStartInfo plus the carry-over) when isCampaign is set
  // and applies it with LoadCampaignState (0x4899C0) before the mission runs. OP2H_CAMPAIGN names a text file of
  // `key value` lines: mission (the finished campaign mission's number; default the DLL's minus one), food, common, rare, workers, scientists, kids, satellites (raw
  // SatelliteCounts word), moraleLevel, morale, event, techLevel. The research list stays empty (numTechs 0).
  static MissionResults mr;
  GameStartInfo* start = &g_gsi;
  if (!cfg.campaign.empty()) {
    mr = {};
    memcpy(static_cast<GameStartInfo*>(&mr), &g_gsi, sizeof(GameStartInfo));
    // MissionResults.missionType is the campaign mission just finished: the game starts the next one, picking the
    // colony's DLL by the local player's isEden flag (measured: number 2 with isEden 0 started p03).
    int number = atoi(cfg.mission.c_str() + 1) - 1;
    for (auto& f : mr.playerFlags) f.isEden = (cfg.mission[0] == 'e' || cfg.mission[0] == 'E');
    if (FILE* f = fopen(cfg.campaign.c_str(), "r")) {
      char key[64];
      int v;
      while (fscanf(f, "%63s %d", key, &v) == 2) {
        const std::string k = key;
        auto& ps = mr.playerStartInfo;
        auto& ms = mr.moraleState;
        if      (k == "mission")     number = v;
        else if (k == "food")        ps.food = v;
        else if (k == "common")      ps.commonOre = v;
        else if (k == "rare")        ps.rareOre = v;
        else if (k == "workers")     ps.workers = v;
        else if (k == "scientists")  ps.scientists = v;
        else if (k == "kids")        ps.kids = v;
        else if (k == "satellites")  memcpy(&ps.satelliteCounts, &v, sizeof(v));
        else if (k == "moraleLevel") ms.moraleLevel = MoraleLevel(v);
        else if (k == "morale")      ms.morale = v;
        else if (k == "event")       ms.eventMoraleModifier = v;
        else if (k == "techLevel")   mr.researchState.techLevel = uint16(v);
        else Log("campaign: unknown key %s", key);
      }
      fclose(f);
    } else {
      Log("campaign: cannot open %s", cfg.campaign.c_str());
    }
    mr.startupFlags.isCampaign  = 1;
    mr.startupFlags.missionType = uint32(number) & 0x1F;
    mr.missionType              = TethysAPI::MissionType(number);
    mr.gameTermReason           = GameTermReasons::MissionAccomplished;
    Log("campaign start: mission %d, food %d common %d rare %d workers %d scientists %d kids %d, morale %d level %d",
        number, mr.playerStartInfo.food, mr.playerStartInfo.commonOre, mr.playerStartInfo.rareOre,
        mr.playerStartInfo.workers, mr.playerStartInfo.scientists, mr.playerStartInfo.kids, mr.moraleState.morale,
        int(mr.moraleState.moraleLevel));
    start = &mr;
  }
  const ibool ok = self->PrepareGame(start);
  Log("PrepareGame(%s) = %d, players %d", cfg.mission.c_str(), ok, self->numPlayers_);
  LogMusic("start");
  if (!ok) { Log("mission failed to start");  if (g_log) fclose(g_log);  TerminateProcess(GetCurrentProcess(), 2); }
  g_started = true;
  return ok;
}


// ---------------------------------------------------------------------------------------------------------------------
// Shell mode (OP2H_SHELL=1, `harness/shell.py`): the game boots normally (intro video, title, menus, briefings) to
// measure the original's startup flow. The harness only watches and logs; shell.py drives the game with real X input
// and takes X screenshots of the whole (virtual) desktop. Events go to shell.jsonl, one JSON object per line, with
// `ms` since InitMod:
//   mci     every mciSendCommandA of OP2Shell.dll (movies: device type, file, window, rectangles, notifications)
//   notify  MM_MCINOTIFY as the shell receives it (1 = played to the end, 2 superseded, 4 aborted, 8 failed)
//   post    OP2Shell's PostMessage/SendMessage calls with message >= WM_USER: the shell's state machine (0x72B = play
//           the movie in +0x53C, then post +0x540; see notes/findings-shell.md)
//   modal / modal_end, modeless   IDlgWnd::DoModal/DoModeless (0x431A80/0x431BF0): dialog resource id and DLL, caller
//   activate_shell   TApp::ActivateShell (0x486DB0): back to the shell from a game, with the end reason
//   dialog  a dialog after WM_INITDIALOG, with every control (id, class, text, screen rectangle)
//   show / hide / destroy   top-level windows and dialogs
//   file    ResManager::OpenStream / GetFilePath asked by the shell (briefing RTFs, story text, music)
//   lib     LoadLibraryA from OP2Shell
//   input   key and mouse button messages as the game receives them (to confirm the driver's input landed)
//   start   TApp::StartSingleGame: the GameStartInfo the shell built
//   game    in-game milestones: the first cycle, every 500 ticks, the end reason changing
//   dump    windows and controls, on request (shell.py `dump`)
// shell.py's control file (ctl.txt in the output directory) is polled by a thread: `dump <tag>` logs every window of
// the game, `quit` ends the process.

bool    g_shellMode = false;
FILE*   g_shell = nullptr;
int64_t g_shellT0 = 0;
CRITICAL_SECTION g_shellLock;
DWORD   g_mainThread = 0;

std::string JsonStr(const char* s, size_t max = 400) {
  std::string out = "\"";
  for (size_t i = 0; s && s[i] && i < max; ++i) {
    const unsigned char c = s[i];
    if (c == '"' || c == '\\') { out += '\\';  out += char(c); }
    else if (c < 0x20) { char b[8];  snprintf(b, sizeof(b), "\\u%04x", c);  out += b; }
    else if (c >= 0x80) { char b[8];  snprintf(b, sizeof(b), "\\u%04x", c);  out += b; }  // Windows-1252, as code points.
    else out += char(c);
  }
  return out + "\"";
}

void ShellEvent(const char* fmt, ...) {
  if (!g_shell) return;
  va_list args, copy;
  va_start(args, fmt);
  va_copy(copy, args);
  std::string body(size_t(std::max(0, vsnprintf(nullptr, 0, fmt, copy))) + 1, '\0');
  va_end(copy);
  vsnprintf(body.data(), body.size(), fmt, args);
  va_end(args);
  body.pop_back();
  EnterCriticalSection(&g_shellLock);
  fprintf(g_shell, "{\"ms\":%.1f,%s}\n", (Now() - g_shellT0) * 1000.0 / Frequency(), body.c_str());
  fflush(g_shell);
  LeaveCriticalSection(&g_shellLock);
}

std::string ResName(const char* res) {
  if (IS_INTRESOURCE(res)) return std::to_string(unsigned(reinterpret_cast<uintptr_t>(res)));
  return JsonStr(res);
}

std::string WindowText(HWND h) {
  char buf[256] = {};
  DWORD_PTR n = 0;
  // Through a timeout: the dump thread asks for windows owned by the busy main thread.
  if (!SendMessageTimeoutA(h, WM_GETTEXT, sizeof(buf), reinterpret_cast<LPARAM>(buf), SMTO_ABORTIFHUNG, 500, &n)) buf[0] = 0;
  return buf;
}

std::string WindowJson(HWND h) {
  char cls[64] = {};
  GetClassNameA(h, cls, sizeof(cls));
  RECT r = {};
  GetWindowRect(h, &r);
  const LONG style = GetWindowLongA(h, GWL_STYLE);
  char buf[1024];
  snprintf(buf, sizeof(buf), "\"hwnd\":\"%p\",\"cls\":%s,\"text\":%s,\"id\":%d,\"rect\":[%ld,%ld,%ld,%ld],\"style\":\"%08lx\","
           "\"visible\":%d,\"enabled\":%d,\"parent\":\"%p\"",
           (void*)h, JsonStr(cls).c_str(), JsonStr(WindowText(h).c_str()).c_str(), GetDlgCtrlID(h), r.left, r.top,
           r.right - r.left, r.bottom - r.top, (unsigned long)style, IsWindowVisible(h) ? 1 : 0, IsWindowEnabled(h) ? 1 : 0,
           (void*)GetParent(h));
  return buf;
}

BOOL CALLBACK AddChildJson(HWND c, LPARAM p) {
  auto& s = *reinterpret_cast<std::string*>(p);
  s += (s.empty() ? "{" : ",{") + WindowJson(c) + "}";
  return TRUE;
}

std::string ChildrenJson(HWND h) {
  std::string out;
  EnumChildWindows(h, &AddChildJson, reinterpret_cast<LPARAM>(&out));
  return "[" + out + "]";
}

bool IsDialog(HWND h) { char cls[16] = {};  GetClassNameA(h, cls, sizeof(cls));  return strcmp(cls, "#32770") == 0; }
bool IsTopOrDialog(HWND h) { return !(GetWindowLongA(h, GWL_STYLE) & WS_CHILD) || IsDialog(h); }

LRESULT CALLBACK ShellCallWndRet(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION) {
    const auto* m = reinterpret_cast<const CWPRETSTRUCT*>(l);
    switch (m->message) {
    case WM_INITDIALOG:
      ShellEvent("\"ev\":\"dialog\",%s,\"controls\":%s", WindowJson(m->hwnd).c_str(), ChildrenJson(m->hwnd).c_str());
      break;
    case WM_SHOWWINDOW:
      if (IsTopOrDialog(m->hwnd)) ShellEvent("\"ev\":\"%s\",%s", m->wParam ? "show" : "hide", WindowJson(m->hwnd).c_str());
      break;
    case WM_DESTROY:
      if (IsTopOrDialog(m->hwnd)) ShellEvent("\"ev\":\"destroy\",%s", WindowJson(m->hwnd).c_str());
      break;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

LRESULT CALLBACK ShellGetMsg(int code, WPARAM w, LPARAM l) {
  if (code == HC_ACTION && w == PM_REMOVE) {
    const auto* m = reinterpret_cast<const MSG*>(l);
    char cls[64] = {};
    switch (m->message) {
    case MM_MCINOTIFY:
      ShellEvent("\"ev\":\"notify\",\"flags\":%u,\"dev\":%u,\"hwnd\":\"%p\"", unsigned(m->wParam), unsigned(m->lParam), (void*)m->hwnd);
      break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONUP:
      GetClassNameA(m->hwnd, cls, sizeof(cls));
      ShellEvent("\"ev\":\"input\",\"msg\":\"%s\",\"w\":%u,\"x\":%d,\"y\":%d,\"hwnd\":\"%p\",\"cls\":%s,\"id\":%d",
                 m->message == WM_KEYDOWN ? "keydown" : m->message == WM_SYSKEYDOWN ? "syskeydown" :
                 m->message == WM_LBUTTONDOWN ? "ldown" : m->message == WM_LBUTTONUP ? "lup" : "rdown",
                 unsigned(m->wParam), short(LOWORD(m->lParam)), short(HIWORD(m->lParam)), (void*)m->hwnd,
                 JsonStr(cls).c_str(), GetDlgCtrlID(m->hwnd));
      break;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

// ---- Hooks on OP2Shell.dll's imports (patched whenever op2ext reports the shell loaded, see RunMod).

// Patches `fn` imported from `dll` in `mod`'s import table; returns the previous target, or nullptr if the import isn't
// there or already points at `hook` (the shell can be loaded again into the same place).
void* PatchImport(HMODULE mod, const char* dll, const char* fn, void* hook) {
  auto* base = reinterpret_cast<uint8_t*>(mod);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
      const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(ibn->Name), fn) != 0) continue;
      if (reinterpret_cast<void*>(slots->u1.Function) == hook) return nullptr;
      return PatchIat(reinterpret_cast<uintptr_t>(&slots->u1.Function), hook);
    }
  }
  Log("shell: import %s!%s not found", dll, fn);
  return nullptr;
}

// ---- Mission DLLs' own modal dialogs. Some missions (Pursued, Rising From the Ashes) open a briefing box from InitProc
// with user32's DialogBoxParamA, which would block an unattended run forever (they ignore its result). OPU loads mission
// DLLs itself (its hook at 0x402B44), so instead of their imports the harness watches window creation on the game's
// thread: a dialog whose instance is a mission DLL (it exports InitProc) isn't created, and DialogBoxParamA returns -1
// at once, as if the player had closed it.

LRESULT CALLBACK MissionDialogCbt(int code, WPARAM w, LPARAM l) {
  if (code == HCBT_CREATEWND) {
    const auto* cs = reinterpret_cast<const CBT_CREATEWNDA*>(l)->lpcs;
    const bool dialog = cs && (reinterpret_cast<uintptr_t>(cs->lpszClass) == 0x8002 ||
                               (HIWORD(reinterpret_cast<uintptr_t>(cs->lpszClass)) && strcmp(cs->lpszClass, "#32770") == 0));
    HMODULE mod = cs ? reinterpret_cast<HMODULE>(cs->hInstance) : nullptr;
    if (dialog && mod && mod != GetModuleHandleA(nullptr) && GetProcAddress(mod, "InitProc")) {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(mod, path, sizeof(path) - 1);
      Log("dialog: a mission dialog box (%s, \"%s\") not opened", path, cs->lpszName ? cs->lpszName : "");
      return 1;
    }
  }
  return CallNextHookEx(nullptr, code, w, l);
}

const char* MciName(UINT m) {
  switch (m) {
  case MCI_OPEN: return "open";      case MCI_CLOSE: return "close";    case MCI_PLAY: return "play";
  case MCI_STOP: return "stop";      case MCI_PAUSE: return "pause";    case MCI_SEEK: return "seek";
  case MCI_SET: return "set";        case MCI_STATUS: return "status";  case MCI_WINDOW: return "window";
  case MCI_PUT: return "put";        case MCI_WHERE: return "where";    case MCI_REALIZE: return "realize";
  case MCI_UPDATE: return "update";  case MCI_GETDEVCAPS: return "getdevcaps";  case MCI_INFO: return "info";
  case MCI_CUE: return "cue";        case MCI_RESUME: return "resume";
  default: return "?";
  }
}

using MciSendCommandFn = MCIERROR(WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
MciSendCommandFn g_mciSendCommand = nullptr;
MCIERROR WINAPI MciHook(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR parms) {
  const MCIERROR r = g_mciSendCommand(id, msg, flags, parms);
  char extra[600] = "";
  if (msg == MCI_OPEN && parms) {
    const auto* p = reinterpret_cast<const MCI_OPEN_PARMSA*>(parms);
    const std::string type = !(flags & MCI_OPEN_TYPE) ? "null"
                           : (flags & MCI_OPEN_TYPE_ID) ? std::to_string(uintptr_t(p->lpstrDeviceType)) : JsonStr(p->lpstrDeviceType);
    snprintf(extra, sizeof(extra), ",\"type\":%s,\"file\":%s,\"newdev\":%u", type.c_str(),
             (flags & MCI_OPEN_ELEMENT) ? JsonStr(p->lpstrElementName).c_str() : "null", unsigned(p->wDeviceID));
  } else if (msg == MCI_PLAY && parms) {
    const auto* p = reinterpret_cast<const MCI_PLAY_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"from\":%ld,\"to\":%ld,\"callback\":\"%p\"", (flags & MCI_FROM) ? long(p->dwFrom) : -1L,
             (flags & MCI_TO) ? long(p->dwTo) : -1L, (flags & MCI_NOTIFY) ? (void*)p->dwCallback : nullptr);
  } else if ((msg == MCI_PUT || msg == MCI_WHERE) && parms) {
    const auto* p = reinterpret_cast<const MCI_ANIM_RECT_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"rect\":[%ld,%ld,%ld,%ld]", p->rc.left, p->rc.top, p->rc.right, p->rc.bottom);
  } else if (msg == MCI_WINDOW && parms) {
    const auto* p = reinterpret_cast<const MCI_ANIM_WINDOW_PARMSA*>(parms);
    snprintf(extra, sizeof(extra), ",\"window\":\"%p\",\"show\":%u", (flags & MCI_ANIM_WINDOW_HWND) ? (void*)p->hWnd : nullptr,
             (flags & MCI_ANIM_WINDOW_STATE) ? unsigned(p->nCmdShow) : 999u);
  } else if (msg == MCI_STATUS && parms) {
    const auto* p = reinterpret_cast<const MCI_STATUS_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"item\":%lu,\"value\":%lu", (unsigned long)p->dwItem, (unsigned long)p->dwReturn);
  } else if (msg == MCI_SET && parms) {
    const auto* p = reinterpret_cast<const MCI_SET_PARMS*>(parms);
    snprintf(extra, sizeof(extra), ",\"timeFormat\":%lu", (flags & MCI_SET_TIME_FORMAT) ? (unsigned long)p->dwTimeFormat : 0ul);
  }
  ShellEvent("\"ev\":\"mci\",\"cmd\":\"%s\",\"msg\":\"%#x\",\"dev\":%u,\"flags\":\"%#lx\",\"ret\":%lu%s", MciName(msg), msg,
             unsigned(id), (unsigned long)flags, (unsigned long)r, extra);
  return r;
}

using PostMessageFn = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using SendMessageFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
PostMessageFn g_postMessage = nullptr;
SendMessageFn g_sendMessage = nullptr;
BOOL WINAPI PostMessageHook(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m >= WM_USER) ShellEvent("\"ev\":\"post\",\"msg\":\"%#x\",\"w\":%u,\"l\":%ld,\"hwnd\":\"%p\"", m, unsigned(w), long(l), (void*)h);
  return g_postMessage(h, m, w, l);
}
LRESULT WINAPI SendMessageHook(HWND h, UINT m, WPARAM w, LPARAM l) {
  // Below 0x7FF: the shell's own messages; dialog/control messages (BM_*, LB_*...) start at WM_USER too, so skip the
  // common control ranges.
  if (m >= 0x700 && m < 0x800)
    ShellEvent("\"ev\":\"send\",\"msg\":\"%#x\",\"w\":%u,\"l\":%ld,\"hwnd\":\"%p\"", m, unsigned(w), long(l), (void*)h);
  return g_sendMessage(h, m, w, l);
}

using DoModalFn    = int(__thiscall*)(void*, const char*, HINSTANCE);
using DoModelessFn = int(__thiscall*)(void*, const char*, HINSTANCE, HWND);
DoModalFn    g_doModal = nullptr;
DoModelessFn g_doModeless = nullptr;
// `module`: the resource DLL (op2shres.dll for the shell's dialogs, out2res.dll for the game's); `at`: the caller.
std::string ModuleName(HINSTANCE h) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(h, path, sizeof(path));
  const char* slash = strrchr(path, '\\');
  return slash ? slash + 1 : path;
}
int __fastcall DoModalHook(void* self, void*, const char* res, HINSTANCE inst) {
  void* at = __builtin_return_address(0);
  ShellEvent("\"ev\":\"modal\",\"res\":%s,\"module\":%s,\"at\":\"%p\",\"vtbl\":\"%p\"", ResName(res).c_str(),
             JsonStr(ModuleName(inst).c_str()).c_str(), at, *reinterpret_cast<void**>(self));
  const int r = g_doModal(self, res, inst);
  ShellEvent("\"ev\":\"modal_end\",\"res\":%s,\"ret\":%d", ResName(res).c_str(), r);
  return r;
}
int __fastcall DoModelessHook(void* self, void*, const char* res, HINSTANCE inst, HWND parent) {
  void* at = __builtin_return_address(0);
  ShellEvent("\"ev\":\"modeless\",\"res\":%s,\"module\":%s,\"at\":\"%p\",\"vtbl\":\"%p\"", ResName(res).c_str(),
             JsonStr(ModuleName(inst).c_str()).c_str(), at, *reinterpret_cast<void**>(self));
  return g_doModeless(self, res, inst, parent);
}

// TApp::ActivateShell (0x486DB0): the game hands control back to the shell with the end reason and the results.
using ActivateShellFn = int(__thiscall*)(void*, int, MissionResults*);
ActivateShellFn g_activateShell = nullptr;
int __fastcall ActivateShellHook(void* self, void*, int reason, MissionResults* r) {
  ShellEvent("\"ev\":\"activate_shell\",\"reason\":%d,\"at\":\"%p\",\"results\":%d,\"mission\":%d,\"resultReason\":%d", reason,
             __builtin_return_address(0), r ? 1 : 0, r ? int(r->missionType) : 0, r ? int(r->gameTermReason) : 0);
  return g_activateShell(self, reason, r);
}

using OpenStreamFn  = void*(__thiscall*)(void*, const char*);
using GetFilePathFn = int(__thiscall*)(void*, const char*, char*);
OpenStreamFn  g_openStream = nullptr;
GetFilePathFn g_getFilePath = nullptr;
void* __fastcall OpenStreamHook(void* self, void*, const char* name) {
  void* s = g_openStream(self, name);
  ShellEvent("\"ev\":\"file\",\"fn\":\"OpenStream\",\"name\":%s,\"found\":%d", JsonStr(name).c_str(), s ? 1 : 0);
  return s;
}
int __fastcall GetFilePathHook(void* self, void*, const char* name, char* path) {
  const int r = g_getFilePath(self, name, path);
  ShellEvent("\"ev\":\"file\",\"fn\":\"GetFilePath\",\"name\":%s,\"found\":%d,\"path\":%s", JsonStr(name).c_str(), r,
             JsonStr(r ? path : "").c_str());
  return r;
}

using LoadLibraryFn = HMODULE(WINAPI*)(const char*);
LoadLibraryFn g_loadLibrary = nullptr;
HMODULE WINAPI LoadLibraryHook(const char* name) {
  HMODULE h = g_loadLibrary(name);
  ShellEvent("\"ev\":\"lib\",\"name\":%s,\"ok\":%d", JsonStr(name).c_str(), h ? 1 : 0);
  return h;
}

using StartSingleGameFn = int(__thiscall*)(void*, GameStartInfo*);
StartSingleGameFn g_startSingleGame = nullptr;
int __fastcall StartSingleGameHook(void* self, void*, GameStartInfo* gsi) {
  const auto& f = gsi->startupFlags;
  const bool campaign = f.isCampaign;
  ShellEvent("\"ev\":\"start\",\"script\":%s,\"missionType\":%d,\"isCampaign\":%d,\"speed\":%d,\"seed\":%d,"
             "\"difficulty\":%d,\"eden\":%d,\"cheats\":%d,\"daynight\":%d,\"disasters\":%d,\"mission\":%d",
             JsonStr(gsi->scriptName).c_str(), int(f.missionType), campaign ? 1 : 0, gsi->gameSpeed, int(gsi->randomNumberSeed),
             int(gsi->playerFlags[0].resources), int(gsi->playerFlags[0].isEden), int(f.cheatsEnabled), int(f.dayNightEnabled),
             int(f.disastersEnabled), campaign ? int(static_cast<MissionResults*>(gsi)->missionType) : -1);
  return g_startSingleGame(self, gsi);
}

void PatchShellImports() {
  HMODULE shell = GetModuleHandleA("OP2Shell.dll");
  if (!shell) { Log("shell: OP2Shell.dll not loaded");  return; }
  ShellEvent("\"ev\":\"shell_loaded\",\"base\":\"%p\"", (void*)shell);
  auto patch = [&](auto& orig, const char* dll, const char* fn, void* hook) {
    if (void* prev = PatchImport(shell, dll, fn, hook)) orig = reinterpret_cast<std::remove_reference_t<decltype(orig)>>(prev);
  };
  patch(g_mciSendCommand, "WINMM.dll", "mciSendCommandA", reinterpret_cast<void*>(&MciHook));
  patch(g_postMessage, "USER32.dll", "PostMessageA", reinterpret_cast<void*>(&PostMessageHook));
  patch(g_sendMessage, "USER32.dll", "SendMessageA", reinterpret_cast<void*>(&SendMessageHook));
  patch(g_loadLibrary, "KERNEL32.dll", "LoadLibraryA", reinterpret_cast<void*>(&LoadLibraryHook));
  patch(g_openStream, "Outpost2.exe", "?OpenStream@ResManager@@QAEPAVStreamIO@@PBD@Z", reinterpret_cast<void*>(&OpenStreamHook));
  patch(g_getFilePath, "Outpost2.exe", "?GetFilePath@ResManager@@QAEHPBDPAD@Z", reinterpret_cast<void*>(&GetFilePathHook));
  patch(g_startSingleGame, "Outpost2.exe", "?StartSingleGame@TApp@@QAEHPAUGameStartInfo@@@Z",
        reinterpret_cast<void*>(&StartSingleGameHook));
}

// ---- In game: the cycle hook only logs (and runs a tick script, e.g. a mission's winning orders); no turbo, no quit.

int g_shellGameTick = -1, g_shellEnd = 0;
void __fastcall ShellCycleHook(GameImpl* self, void*) {
  g_origGameCycle(self);
  const int tick = self->tick_;
  if (tick == g_shellGameTick) return;
  if (tick < g_shellGameTick || g_shellGameTick < 0) {
    // A new game: reset the per-game state.
    g_scriptPos = 0;
    g_shellEnd = 0;
    g_nextTelem = 0;
    RECT r = {};
    GetWindowRect(g_gameFrame.hWnd_, &r);
    ShellEvent("\"ev\":\"game\",\"what\":\"first_cycle\",\"tick\":%d,\"map\":[%d,%d],\"frame\":[%ld,%ld,%ld,%ld]", tick,
               GameMap::GetWidth(), GameMap::GetHeight(), r.left, r.top, r.right - r.left, r.bottom - r.top);
    LogMusic("start");
  }
  g_shellGameTick = tick;
  while (g_scriptPos < g_script.size() && g_script[g_scriptPos].tick <= tick) RunScriptCmd(g_script[g_scriptPos++], tick);
  AudioTick(tick);
  if (cfg.telemEvery > 0 && tick >= g_nextTelem) { WriteTelemetry(tick);  g_nextTelem = (tick / cfg.telemEvery + 1) * cfg.telemEvery; }
  if (tick % 500 == 0) ShellEvent("\"ev\":\"game\",\"what\":\"tick\",\"tick\":%d", tick);
  const int end = int(self->gameTermReasons_);
  if (end != g_shellEnd) {
    ShellEvent("\"ev\":\"game\",\"what\":\"end\",\"tick\":%d,\"reason\":%d", tick, end);
    g_shellEnd = end;
  }
}

// ---- Control file thread.

std::string g_shellCtl;
BOOL CALLBACK AddWindowJson(HWND h, LPARAM p) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != GetCurrentProcessId()) return TRUE;
  auto& s = *reinterpret_cast<std::string*>(p);
  s += (s.empty() ? "{" : ",{") + WindowJson(h) + ",\"children\":" + ChildrenJson(h) + "}";
  return TRUE;
}

void DumpWindows(const std::string& tag) {
  std::string out;
  EnumWindows(&AddWindowJson, reinterpret_cast<LPARAM>(&out));
  ShellEvent("\"ev\":\"dump\",\"tag\":%s,\"windows\":[%s]", JsonStr(tag.c_str()).c_str(), out.c_str());
}

DWORD WINAPI ShellControlThread(void*) {
  for (;;) {
    Sleep(25);
    FILE* f = fopen(g_shellCtl.c_str(), "r");
    if (!f) continue;
    char line[256];
    std::vector<std::string> cmds;
    while (fgets(line, sizeof(line), f)) cmds.emplace_back(line);
    fclose(f);
    DeleteFileA(g_shellCtl.c_str());
    for (auto c : cmds) {
      while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) c.pop_back();
      if (c.rfind("dump", 0) == 0) DumpWindows(c.size() > 5 ? c.substr(5) : "");
      else if (c == "quit") {
        ShellEvent("\"ev\":\"quit\"");
        if (g_telem) fclose(g_telem);
        if (g_audio) fclose(g_audio);
        if (g_log) fclose(g_log);
        TerminateProcess(GetCurrentProcess(), 0);
      }
    }
  }
}

void ShellInit() {
  InitializeCriticalSection(&g_shellLock);
  g_shellT0 = Now();
  g_mainThread = GetCurrentThreadId();
  cfg.out        = Env("OP2H_OUT", ".");
  cfg.script     = Env("OP2H_SCRIPT");
  cfg.telemEvery = EnvInt("OP2H_TELEM_EVERY", 0);
  g_log   = fopen((cfg.out + "\\harness.log").c_str(), "w");
  g_shell = fopen((cfg.out + "\\shell.jsonl").c_str(), "w");
  g_telem = cfg.telemEvery > 0 ? fopen((cfg.out + "\\telemetry.jsonl").c_str(), "w") : nullptr;
  g_audio = EnvInt("OP2H_AUDIO", 0) ? fopen((cfg.out + "\\audio.jsonl").c_str(), "w") : nullptr;
  g_shellCtl = cfg.out + "\\ctl.txt";
  Log("harness: shell mode");

  // A private copy of the settings, as in mission runs; here sound and the movies stay as configured (OP2H_INI_<key>
  // overrides a [Game] key, e.g. OP2H_INI_IntroAVI=1).
  const std::string ini = cfg.out + "\\outpost2.ini";
  if (CopyFileA(g_configFile.iniPath_, ini.c_str(), FALSE) && ini.size() < sizeof(g_configFile.iniPath_)) {
    strcpy(g_configFile.iniPath_, ini.c_str());
    for (const char* key : { "IntroAVI", "ShowMovies", "Music", "Sound", "GameSpeed", "Demo", "MusicVol", "SoundVol" }) {
      const std::string v = Env((std::string("OP2H_INI_") + key).c_str());
      if (!v.empty()) WritePrivateProfileStringA("Game", key, v.c_str(), ini.c_str());
    }
    Log("config: using %s", ini.c_str());
  }
  JumpTo(FnOP2MessageBox, reinterpret_cast<void*>(&MessageBoxHook));
  if (g_audio) InstallAudioHooks();
  g_origGameCycle = reinterpret_cast<GameCycleFn>(RelinkCall(CallProcessGameCycle, reinterpret_cast<void*>(&ShellCycleHook)));
  // Every dialog of the shell and the game goes through IDlgWnd::DoModal/DoModeless in Outpost2.exe.
  static const uint8_t doModal[]    = { 0x83, 0xEC, 0x04, 0xA1, 0xAC, 0xE8, 0x56, 0x00 };  // sub esp,4; mov eax,[56E8ACh]
  static const uint8_t doModeless[] = { 0x83, 0xEC, 0x10, 0xA1, 0xAC, 0xE8, 0x56, 0x00 };  // sub esp,10h; mov eax,[56E8ACh]
  static const uint8_t activate[]   = { 0x83, 0xEC, 0x2C, 0x53, 0x56 };  // sub esp,2Ch; push ebx; push esi
  g_doModal       = reinterpret_cast<DoModalFn>(HookEntry(0x431A80, doModal, 8, reinterpret_cast<void*>(&DoModalHook)));
  g_doModeless    = reinterpret_cast<DoModelessFn>(HookEntry(0x431BF0, doModeless, 8, reinterpret_cast<void*>(&DoModelessHook)));
  g_activateShell = reinterpret_cast<ActivateShellFn>(HookEntry(0x486DB0, activate, 5, reinterpret_cast<void*>(&ActivateShellHook)));
  SetWindowsHookExA(WH_CALLWNDPROCRET, &ShellCallWndRet, nullptr, g_mainThread);
  SetWindowsHookExA(WH_GETMESSAGE, &ShellGetMsg, nullptr, g_mainThread);
  if (!cfg.script.empty()) LoadScript(cfg.script);
  CreateThread(nullptr, 0, &ShellControlThread, nullptr, 0, nullptr);
  ShellEvent("\"ev\":\"init\",\"screen\":[%d,%d]", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

} // namespace

extern "C" __declspec(dllexport) void InitMod(const char* /*iniSectionName*/) {
  if (EnvInt("OP2H_SHELL", 0)) { g_shellMode = true;  ShellInit();  return; }  // Normal boot, watched (shell.py).
  cfg.mission = Env("OP2H_MISSION");
  if (cfg.mission.empty()) return;  // Not a harness run: stay completely inert.

  cfg.out        = Env("OP2H_OUT", ".");
  cfg.script     = Env("OP2H_SCRIPT");
  cfg.bot        = Env("OP2H_BOT");
  cfg.campaign   = Env("OP2H_CAMPAIGN");
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
  g_traceMap     = EnvInt("OP2H_TRACE_MAP", 0) != 0;
  g_eco          = EnvInt("OP2H_ECO", 0) != 0;
  cfg.width      = EnvInt("OP2H_WIDTH", 1024);
  cfg.height     = EnvInt("OP2H_HEIGHT", 768);

  g_log   = fopen((cfg.out + "\\harness.log").c_str(), "w");
  g_telem = cfg.telemEvery > 0 ? fopen((cfg.out + "\\telemetry.jsonl").c_str(), "w") : nullptr;
  g_trace = cfg.traceEvery > 0 ? fopen((cfg.out + "\\trace.jsonl").c_str(), "w") : nullptr;
  g_audio = EnvInt("OP2H_AUDIO", 0) ? fopen((cfg.out + "\\audio.jsonl").c_str(), "w") : nullptr;
  g_stateOn = EnvInt("OP2H_STATE", 0) != 0;
  g_events  = g_stateOn ? fopen((cfg.out + "\\events.jsonl").c_str(), "w") : nullptr;
  g_stopSpec = Env("OP2H_STOP");
  g_stopFrom = EnvInt("OP2H_STOP_FROM", 0);
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
  if (g_audio) InstallAudioHooks();
  else if (g_events) InstallMessageHook();
  SetWindowsHookExA(WH_CBT, &MissionDialogCbt, nullptr, GetCurrentThreadId());
  g_realGetCursorPos     = reinterpret_cast<GetCursorPosFn>(PatchIat(IatGetCursorPos, reinterpret_cast<void*>(&VirtualGetCursorPos)));
  g_realGetAsyncKeyState =
    reinterpret_cast<GetAsyncKeyStateFn>(PatchIat(IatGetAsyncKeyState, reinterpret_cast<void*>(&VirtualGetAsyncKeyState)));

  if (!cfg.script.empty()) LoadScript(cfg.script);
  if (!cfg.bot.empty()) {
    if (HMODULE h = LoadLibraryA(cfg.bot.c_str())) g_botTick = reinterpret_cast<BotTickFn>(GetProcAddress(h, "HarnessBotTick"));
    Log("bot: %s -> %s", cfg.bot.c_str(), g_botTick ? "ok" : "HarnessBotTick not found");
  }
}

// op2ext calls this when OP2Shell.dll gets loaded. In a mission run that only happens if the game fell back to the
// shell (e.g. the mission ended through the normal end-of-game path), so treat it as the end of the run. In shell mode
// it is when the shell's imports get hooked.
extern "C" __declspec(dllexport) void RunMod() {
  if (g_shellMode) { PatchShellImports();  return; }
  if (!cfg.mission.empty() && g_started) Quit("returned to shell");
}

extern "C" __declspec(dllexport) void DestroyMod() {}
