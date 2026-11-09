// Easter egg bot (notes/findings-easter-eggs.md). Run with the scenario mission:
//   EGG_MODE=dog   ./run.sh --scenario test/egg_dandog.json --bot build/easter_bot.dll ...
//   EGG_MODE=santa ./run.sh --scenario test/egg_santa.json  --bot build/easter_bot.dll ...
//
// dog:   Dan's Dog. Meteor::DoEvent (0x44A320) spawns an AmbientAnimation playing anim 0x87B (prt 2070) at the impact
//        tile when tick % 32768 == 0, the tile's pre-scorch index is in terrain type 2 and no unit is on it
//        (0x44A9D2..0x44AA71). DoEvent runs only on the meteor's heartbeat ((index ^ tick) & 7 == 0), so the meteor's
//        index must be a multiple of 8. The bot finds a qualifying tile, measures one meteor's flight time, then drops
//        a second, index-aligned meteor so that it lands at tick 32768. Everything after the drop is the game's own code.
// santa: nothing in Outpost2.exe ever starts the SantaWalking action (20, handler 0x43E866), so this mode does what the
//        missing code would have: a marker (Game::CreateMarker, an AmbientAnimation) gets anim 0x876 (prt 2065), action
//        20, a speed at +0x60 and an end pixel x at +0x64. The game's handler then walks it west and deletes it.
//
// Log: $OP2H_OUT\easter.log.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Tethys/API/API.h"

using namespace Tethys;
using namespace TethysAPI;

namespace {

FILE* g_log = nullptr;
void Log(const char* fmt, ...) {
  if (!g_log) {
    char path[MAX_PATH];
    const char* out = getenv("OP2H_OUT");
    snprintf(path, sizeof(path), "%s\\easter.log", out ? out : ".");
    g_log = fopen(path, "w");
    if (!g_log) return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

int PadTiles() { return GameMap::GetPaddingWidth(); }

int EnvInt(const char* name, int def) { const char* v = getenv(name); return v ? atoi(v) : def; }

uint8_t* Raw(Unit u) { return reinterpret_cast<uint8_t*>(u.GetMapObject()); }

// TerrainManager +0x0C = number of terrain types, +0x10 = array of TerrainType* (TethysAPI declares only one pointer).
// The dog code reads +0x18, i.e. terrain type 2 (0x44A9EB).
const TerrainType* TerrainTypeAt(int i) {
  auto* tm = reinterpret_cast<uint8_t*>(g_mapImpl.pTerrainManager_);
  if (i >= *reinterpret_cast<int*>(tm + 0xC)) return nullptr;
  return reinterpret_cast<TerrainType**>(tm + 0x10)[i];
}

bool DogTile(Location at) {
  const TerrainType* tt = TerrainTypeAt(2);
  const int tile = GameMap::GetTile(at);
  return tt && tile >= tt->firstTile && tile <= tt->lastTile &&
         g_mapImpl.pTerrainManager_->HasScorchMarkTransformation(tile) && !GameMap::GetUnitOnTile(at).IsValid();
}

// Nearest qualifying tile to `from`, scanning outwards; `avoid` keeps calibration and real targets apart.
Location FindDogTile(Location from, Location avoid, int minDist) {
  for (int r = 0; r < 60; ++r)
    for (int dy = -r; dy <= r; ++dy)
      for (int dx = -r; dx <= r; ++dx) {
        if (abs(dx) != r && abs(dy) != r) continue;
        const Location at(from.x + dx, from.y + dy);
        if (abs(at.x - avoid.x) + abs(at.y - avoid.y) < minDist) continue;
        if (DogTile(at)) return at;
      }
  return Location(-1, -1);
}

// Creates a meteor whose object index is a multiple of 8. Fresh indices are handed out in order (freed ones are only
// reused after 128 ticks), so markers parked in a corner fill indices until the next one is aligned.
Unit AlignedMeteor(Location at, Location park) {
  for (int i = 0; i < 16; ++i) {
    Unit probe = Game::CreateMarker(park, MarkerType::Circle);
    if ((probe.GetID() & 7) == 7) break;
  }
  Unit m = Game::CreateMeteor(at, MeteorSize::Small, true);
  Log("meteor id %d (mod 8 = %d) target %d,%d", m.GetID(), m.GetID() & 7, at.x, at.y);
  return m;
}

// Every live object playing ambient animation `anim` (AmbientAnimation +0x54: delay << 24 | anim, set at 0x405110).
int FindAnim(int anim, int* firstId) {
  int n = 0;
  for (int i = 1; i <= g_mapImpl.lastUsedUnitIndex_; ++i) {
    MapObject* o = MapObject::GetInstance(i);
    if (!o || !o->IsLive() || o->GetTypeID() != MapID::AmbientAnimation) continue;
    if ((*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(o) + 0x54) & 0xFFFFFF) == uint32_t(anim)) {
      if (n++ == 0 && firstId) *firstId = i;
    }
  }
  return n;
}

enum Mode { Dog, Santa };
Mode g_mode = Dog;
int g_target = 32768;
Location g_calTile(-1, -1), g_dogTile(-1, -1), g_park;
int g_calTick = -1, g_calTileIdx = 0, g_delay = -1, g_dropTick = -1, g_dropTileIdx = 0, g_dogSeen = 0;
Unit g_santa;

void DogTick(int tick) {
  if (tick == 2) {
    const Location centre(GameMap::GetWidth() / 2 + PadTiles(), GameMap::GetHeight() / 2);
    g_park = Location(PadTiles() + 2, 2);
    const TerrainType* tt = TerrainTypeAt(2);
    Log("terrain type 2: tiles %d..%d", tt ? tt->firstTile : -1, tt ? tt->lastTile : -1);
    g_dogTile = FindDogTile(centre, Location(-1000, -1000), 0);
    g_calTile = FindDogTile(Location(g_dogTile.x, g_dogTile.y + 12), g_dogTile, 10);
    Log("dog tile %d,%d (tile %d), calibration tile %d,%d", g_dogTile.x, g_dogTile.y, GameMap::GetTile(g_dogTile),
        g_calTile.x, g_calTile.y);
    Player[0].CenterViewOn(g_dogTile);
  }
  // Calibration drop at a tick that is a multiple of 8 (the aligned meteor's heartbeat phase).
  if (tick == 200 && g_calTile.x >= 0) {
    g_calTileIdx = GameMap::GetTile(g_calTile);
    Unit m = AlignedMeteor(g_calTile, g_park);
    if (m.GetID() & 7) { Log("calibration meteor not aligned; giving up"); return; }
    g_calTick = tick;
  }
  if (g_calTick >= 0 && g_delay < 0 && GameMap::GetTile(g_calTile) != g_calTileIdx) {
    g_delay = tick - g_calTick;
    Log("calibration impact seen at bot tick %d (delay %d, tick mod 8 = %d)", tick, g_delay, tick & 7);
    // Bot tick b sees the cycle processed with tick_ = b - offset; the impact runs at a heartbeat (tick_ % 8 == 0).
    const int offset = tick & 7;
    g_dropTick = g_target + offset - g_delay;
    Log("will drop at bot tick %d for an impact at tick_ %d", g_dropTick, g_target);
  }
  if (tick == g_dropTick) {
    g_dropTileIdx = GameMap::GetTile(g_dogTile);
    Unit m = AlignedMeteor(g_dogTile, g_park);
    if (m.GetID() & 7) Log("real meteor not aligned (id %d); the dog will not appear", m.GetID());
    Player[0].CenterViewOn(g_dogTile);
  }
  if (g_dropTick > 0 && tick > g_dropTick && g_dropTileIdx && GameMap::GetTile(g_dogTile) != g_dropTileIdx) {
    Log("impact seen at bot tick %d: tile %d -> %d", tick, g_dropTileIdx, GameMap::GetTile(g_dogTile));
    g_dropTileIdx = 0;
  }
  {  // Watched on every tick, so the calibration meteor (landing off the 32768 boundary) is the negative control.
    int id = 0;
    const int n = FindAnim(0x87B, &id);
    if (n != g_dogSeen) {
      Log("bot tick %d: %d object(s) playing anim 0x87B (Dan's Dog)%s", tick, n, n ? "" : " - gone");
      if (n) {
        MapObject* o = MapObject::GetInstance(id);
        Log("  dog object %d at pixel %d,%d, actionTimer %d", id, o->pixelX_, o->pixelY_, o->actionTimer_);
      }
      g_dogSeen = n;
    }
  }
}

void SantaTick(int tick) {
  const int start = EnvInt("EGG_SANTA_TICK", 20);
  if (tick == start) {
    const Location centre(GameMap::GetWidth() / 2 + PadTiles(), GameMap::GetHeight() / 2);
    const Location at(centre.x + 5, centre.y);
    Player[0].CenterViewOn(centre);
    g_santa = Game::CreateMarker(at, MarkerType::Circle);
    MapObject* o = g_santa.GetMapObject();
    // Looping walk cycle (delay 2 ticks/frame), drawn in the normal layer, no self-deletion.
    o->SetAnimation(0x876, 2, 0, 0, 1);
    o->flags_ &= ~0x4000u;           // +0x60 is a draw parameter when this flag is set (0x4051DD)
    uint8_t* raw = Raw(g_santa);
    *reinterpret_cast<int*>(raw + 0x60) = EnvInt("EGG_SANTA_SPEED", 1);              // pixels per tick, westwards
    *reinterpret_cast<int*>(raw + 0x64) = o->pixelX_ - EnvInt("EGG_SANTA_RANGE", 320);  // delete when pixelX_ < this
    // EGG_SANTA_ACTION=0: leave the action alone and walk him from the bot instead, because the game's SantaWalking
    // handler returns "delete me" to the object loop (0x43AD6E) on every tick, so he vanishes after one step.
    if (EnvInt("EGG_SANTA_ACTION", 1)) o->action_ = ActionType::SantaWalking;
    Log("santa: object %d at pixel %d,%d, end x %d", g_santa.GetID(), o->pixelX_, o->pixelY_,
        *reinterpret_cast<int*>(raw + 0x64));
  }
  if (tick > start && g_santa.IsValid()) {
    MapObject* o = g_santa.GetMapObject();
    if (!EnvInt("EGG_SANTA_ACTION", 1) && o->IsLive()) {
      uint8_t* raw = Raw(g_santa);
      o->pixelX_ -= *reinterpret_cast<int*>(raw + 0x60);
      if (o->pixelX_ < *reinterpret_cast<int*>(raw + 0x64) && o->command_ != 0x2F) o->DoDeath(1);  // once
    }
    if (tick % 40 == 0 || !o->IsLive() || tick < start + 4)
      Log("bot tick %d: santa live %d pixelX %d action %d cmd %d busy %d flags %08x next %p timer %d", tick,
          o->IsLive(), o->pixelX_, int(o->action_), o->command_, o->isBusy_, o->flags_, (void*)o->pNext_,
          o->actionTimer_);
    if (!o->IsLive()) g_santa = Unit();
  }
}

}  // namespace

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int tick) {
  if (tick == 1) {
    const char* m = getenv("EGG_MODE");
    g_mode = (m && !strcmp(m, "santa")) ? Santa : Dog;
    g_target = EnvInt("EGG_TICK", 32768);
    Log("easter bot: mode %s", g_mode == Santa ? "santa" : "dog");
  }
  if (g_mode == Dog) DogTick(tick); else SantaTick(tick);
}
