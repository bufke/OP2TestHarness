// Cheat code bot (notes/findings-cheats.md). Run with the scenario mission:
//   CHEAT_MODE=chat harness/run.sh --scenario harness/test/cheats.json --bot harness/build/cheat_bot.dll ...
//   CHEAT_MODE=labs harness/run.sh --scenario harness/test/cheats.json --bot harness/build/cheat_bot.dll ...
//
// chat: types chat lines the way the in-game chat box does (0x41197B: the local player's next CommandPacket, type
//       ctChat 0x30, playerID, chatDstMask_, text). Player::ProcessCommandPacket's ctChat case (0x40FD7C) passes the text
//       to the DSEG checker 0x58642A, which hashes it and runs one of the four handlers BlightManager::Init (0x49E250)
//       installed. The bot logs player 0's ore, population and the weapon stats the handlers touch around every line.
//       CHEAT_LINES="tick:text|tick:text|..." overrides the default list (negative controls first).
// labs: the Advanced Lab destructor (0x431FD0) counts lab deaths per owner; the 6th within 50 ticks of the first spawns
//       a 1-HP Scorpion with Thor's Hammer. The bot builds 7 labs for player 0 and kills 6 of them (DoDeath) within a few
//       ticks, then logs the Scorpion. CHEAT_LABS_BIKE=1 types the bike cheat first (it sets 0x4E0BDC, which makes the
//       Scorpion undamageable). CHEAT_LABS_SPREAD=N kills one lab every N ticks instead (N=12: the 6th dies 60 ticks
//       after the first, a negative control). CHEAT_LABS_KILLS=N kills only N labs (default 6).
//
// Log: $OP2H_OUT\cheats.log.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Tethys/API/API.h"

using namespace Tethys;
using namespace TethysAPI;

namespace {

FILE* g_log = nullptr;
void Log(const char* fmt, ...) {
  if (!g_log) {
    char path[MAX_PATH];
    const char* out = getenv("OP2H_OUT");
    snprintf(path, sizeof(path), "%s\\cheats.log", out ? out : ".");
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

int EnvInt(const char* name, int def) { const char* v = getenv(name); return v ? atoi(v) : def; }

PlayerImpl& P(int n) { return GameImpl::GetInstance()->GetPlayerArray()[n]; }

// Weapon type tables the handlers walk: bike = 16 entries from 0x4E1434, bite = the last 14 (from 0x4E143C).
MapObjectType** WeaponTable() { return reinterpret_cast<MapObjectType**>(0x4E1434); }
constexpr int kWeapons = 16;

uint32_t HandlerSlot(int i) { return reinterpret_cast<uint32_t*>(0x5863C8)[i]; }
int StatsFlag() { return *reinterpret_cast<int*>(0x4E0BDC); }

void LogPlayer(const char* tag) {
  const PlayerImpl& p = P(0);
  Log("%s: common %d/%d rare %d/%d workers %d scientists %d kids %d cheatedTick %d flag4E0BDC %d", tag, p.commonOre_,
      p.maxCommonOre_, p.rareOre_, p.maxRareOre_, p.numWorkers_, p.numScientists_, p.numKids_,
      GameImpl::GetInstance()->tickOfLastSetGameOpt_, StatsFlag());
}

void LogWeapons(const char* tag) {
  std::string s;
  for (int i = 0; i < kWeapons; ++i) {
    MapObjectType* t = WeaponTable()[i];
    const auto& st = t->playerStats_[0].weapon;
    char b[96];
    snprintf(b, sizeof(b), " %s(0x%02X) rof %d pen %d con %d;", t->GetCodeName(), int(t->type_), st.reloadTime,
             st.penetrationDamage, st.concussionDamage);
    s += b;
  }
  Log("%s weapons p0:%s", tag, s.c_str());
}

void SendChat(const char* text) {
  GameImpl* g = GameImpl::GetInstance();
  const int me = g->localPlayer_;
  CommandPacket* pk = P(me).GetNextCommandPacket();
  auto* raw = reinterpret_cast<uint8_t*>(pk);
  *reinterpret_cast<uint32_t*>(raw) = 0x30;  // ctChat
  raw[0xE] = uint8_t(me);
  raw[0xF] = uint8_t(*reinterpret_cast<uint32_t*>(&g->chatDstMask_));
  const size_t n = strnlen(text, 63);
  memcpy(raw + 0x10, text, n);
  raw[0x10 + n] = 0;
  *reinterpret_cast<uint16_t*>(raw + 4) = uint16_t(n + 3);
}

struct Line { int tick; std::string text; };
std::vector<Line> g_lines;

const char* kDefaultLines =
    "60:Dan's bike could backfire more often.|"   // wrong: trailing period
    "70:Dan's dog could bite harder.|"            // wrong: period instead of '!'
    "80:dan's car could hold more people.|"       // wrong: lower-case d
    "90:Dan's dog could make more ore|"           // wrong: one character short (length matters)
    "200:Dan's dog could make more ore.|"
    "300:Dan's car could hold more people.|"
    "400:Dan's bike could backfire more often|"
    "500:Dan's dog could bite harder!|"
    "610:Dan's dog could make more orXY|"         // same hash: the dword holding the terminator is not hashed
    "700:Dan's car could hold more people?";

void ParseLines() {
  const char* src = getenv("CHEAT_LINES");
  std::string all = src ? src : kDefaultLines;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t end = all.find('|', pos);
    if (end == std::string::npos) end = all.size();
    std::string item = all.substr(pos, end - pos);
    size_t colon = item.find(':');
    if (colon != std::string::npos) g_lines.push_back({atoi(item.c_str()), item.substr(colon + 1)});
    pos = end + 1;
  }
}

Location Centre() { return Location(GameMap::GetWidth() / 2 + GameMap::GetPaddingWidth(), GameMap::GetHeight() / 2); }

void ChatTick(int tick) {
  if (tick == 1) {
    ParseLines();
    const Location c = Centre();
    Player[0].SetWorkers(40);
    Player[0].SetScientists(20);
    Player[0].SetKids(20);
    Player[0].SetFoodStored(5000);
    if (EnvInt("CHEAT_NO_BUILD", 0)) return;
    Game::CreateUnit(MapID::CommandCenter, c, 0);
    Game::CreateUnit(MapID::CommonOreSmelter, Location(c.x + 6, c.y), 0);
    Game::CreateUnit(MapID::CommonStorage, Location(c.x - 5, c.y), 0);
    Game::CreateUnit(MapID::RareStorage, Location(c.x, c.y + 5), 0);
    Game::CreateUnit(MapID::Residence, Location(c.x - 5, c.y + 5), 0);
    Game::CreateUnit(MapID::Agridome, Location(c.x + 6, c.y + 5), 0);
    Game::CreateUnit(MapID::Tokamak, Location(c.x + 12, c.y + 5), 0);
    Player[0].CenterViewOn(c);
    Log("handler slots: %08x %08x %08x %08x", HandlerSlot(0), HandlerSlot(1), HandlerSlot(2), HandlerSlot(3));
    LogPlayer("tick 1");
    LogWeapons("tick 1");
  }
  if (tick == 150) {  // spend the dog's ore so the second dog line has something to refill
    P(0).commonOre_ = 0;
    P(0).rareOre_ = 0;
    Log("tick 150: bot zeroed player 0's ore");
  }
  if (tick == 600) {
    P(0).commonOre_ = 0;
    P(0).rareOre_ = 0;
    Log("tick 600: bot zeroed player 0's ore");
  }
  for (const Line& l : g_lines) {
    if (tick == l.tick) {
      LogPlayer(("before \"" + l.text + "\"").c_str());
      LogWeapons("before");
      SendChat(l.text.c_str());
      Log("tick %d: sent chat \"%s\"", tick, l.text.c_str());
    }
    if (tick == l.tick + 12) {
      LogPlayer(("after  \"" + l.text + "\"").c_str());
      LogWeapons("after");
    }
  }
}

std::vector<Unit> g_labs;
int g_firstKill = -1;
int g_seenScorpion = 0;

void LabsTick(int tick) {
  const Location c = Centre();
  if (tick == 1) {
    for (int i = 0; i < 7; ++i) g_labs.push_back(Game::CreateUnit(MapID::AdvancedLab, Location(c.x - 15 + 5 * i, c.y), 0));
    Game::CreateUnit(MapID::Lynx, Location(c.x, c.y + 8), 1, MapID::Laser);  // a Plymouth tank to shoot at it
    Player[0].CenterViewOn(c);
    Log("labs: %d labs built; flag4E0BDC %d", int(g_labs.size()), StatsFlag());
  }
  if (tick == 20 && EnvInt("CHEAT_LABS_BIKE", 0)) {
    SendChat("Dan's bike could backfire more often");
    Log("tick 20: sent the bike cheat");
  }
  const int spread = EnvInt("CHEAT_LABS_SPREAD", 2);
  const int start = 60;
  if (tick >= start && (tick - start) % spread == 0) {
    const int k = (tick - start) / spread;
    if (k < EnvInt("CHEAT_LABS_KILLS", 6)) {
      MapObject* o = g_labs[k].GetMapObject();
      Log("tick %d: killing lab %d (unit %d) flag4E0BDC %d", tick, k, g_labs[k].GetID(), StatsFlag());
      o->DoDeath(1);
      if (g_firstKill < 0) g_firstKill = tick;
    }
  }
  int n = 0;
  for (int i = 1; i <= g_mapImpl.lastUsedUnitIndex_; ++i) {
    MapObject* o = MapObject::GetInstance(i);
    if (!o || !o->IsLive() || o->GetTypeID() != MapID::Scorpion || o->ownerNum_ != 0) continue;
    ++n;
    if (n > g_seenScorpion || tick % 50 == 0) {
      const Unit u(i);
      Log("tick %d: player-0 Scorpion unit %d at tile %d,%d weapon 0x%02X damage %d maxHp %d flags %08x (CanBeDamaged %d)",
          tick, i, u.GetLocation().x, u.GetLocation().y, int(o->weapon_), int(o->damage_), u.GetMaxHitpoints(),
          o->flags_, (o->flags_ & 0x400) != 0);
      if (n > g_seenScorpion) Player[0].CenterViewOn(u.GetLocation());
    }
  }
  g_seenScorpion = n;
  if (tick == 400) Log("tick 400: %d player-0 Scorpion(s) alive", n);
}

}  // namespace

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int tick) {
  const char* m = getenv("CHEAT_MODE");
  if (m && !strcmp(m, "labs")) LabsTick(tick); else ChatTick(tick);
}
