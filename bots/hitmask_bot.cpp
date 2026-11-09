// Hit mask probe ($ORACLE/input/make_hit.py, notes/findings-input.md "What a click hits"). At the
// scenario's "hitTicks" it asks the game, for every pixel around each unit of the "hitPlayers", which unit a click
// there would pick, with the object-list query the detail pane's click handler uses: MapObjectManager's point
// iterator (start 0x437940, next 0x437990, on the manager at 0x54F7F8).
//   - `sel`: the default-mode left click (0x41B00D): owner mask = the unit's player, every hit walked, the LAST one
//     wins (the list is sorted by pixel y, so the unit drawn on top).
//   - `tgt`: the command modes' target query (e.g. 0x4533D0): owner mask = all players, the FIRST hit wins.
// The masks are rows of characters, one per pixel: '.' no hit, else an index into `legend` (unit indices).
// Each unit's line also has its selection box (GetSelectionBoxPos/Size, vtable +0x34/+0x30), the animation the box
// comes from (buildings: +0x2C; vehicles: the chassis animation, +0x80) and, for buildings, the frame the pixel test
// uses (+0x98, 0x409310).
//
// Run: ./run.sh --scenario S.json --bot build/hitmask_bot.dll --trace-every 0 --out DIR
// Output: $OP2H_OUT\hitmask.jsonl. Coordinates are internal map pixels (file pixel + padding tiles * 32).
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Iter {
  char* cur;          // +0x00 the MapObject found
  int tileX, tileY;   // +0x04 the point's tile within the object's box
  int unused0c;
  unsigned owners;    // +0x10 bit per player
  unsigned flags;     // +0x14 MapObject flags that must overlap
  int rest[16];       // +0x18.. y range, x/y of the point
};

using StartFn = void(__thiscall*)(void*, int, int, Iter*);
using NextFn  = int(__thiscall*)(void*, Iter*);
void* const kManager = reinterpret_cast<void*>(0x54F7F8);

int Hit(int x, int y, unsigned owners, bool last) {
  Iter it = {};
  it.owners = owners;
  it.flags  = 0xFFFFFFFFu;
  reinterpret_cast<StartFn>(0x437940)(kManager, x, y, &it);
  int found = -1;
  while (reinterpret_cast<NextFn>(0x437990)(kManager, &it)) {
    found = *reinterpret_cast<int*>(it.cur + 0x10);
    if (!last) break;
  }
  return found;
}

char* Obj(int index) { return *reinterpret_cast<char**>(0x54F848) + index * 0x78; }

template <typename R, typename... A> R Virtual(char* obj, int offset, A... args) {
  using Fn = R(__thiscall*)(char*, A...);
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[offset / 4])(obj, args...);
}

FILE* g_out = nullptr;

// Settings: the scenario's "hitTicks" (list), "hitPlayers" (string of player digits, default "0") and "hitMargin"
// (pixels added round each unit's box, default 8); run.py copies the scenario to $OP2H_OUT\scenario.json.
struct Settings {
  std::vector<int> ticks = { 20 };
  std::string players = "0";
  int margin = 8;
};

Settings Load() {
  Settings st;
  char path[MAX_PATH];
  const char* out = getenv("OP2H_OUT");
  snprintf(path, sizeof(path), "%s\\scenario.json", out ? out : ".");
  FILE* f = fopen(path, "rb");
  if (!f) return st;
  std::string text;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
  fclose(f);
  auto after = [&](const char* key) -> const char* {
    const size_t k = text.find(key);
    if (k == std::string::npos) return nullptr;
    const size_t colon = text.find(':', k);
    return colon == std::string::npos ? nullptr : text.c_str() + colon + 1;
  };
  if (const char* p = after("\"hitTicks\"")) {
    st.ticks.clear();
    while (*p && *p != ']') {
      if (isdigit((unsigned char)*p)) { char* end;  st.ticks.push_back(int(strtol(p, &end, 10)));  p = end; } else ++p;
    }
  }
  if (const char* p = after("\"hitPlayers\"")) {
    const char* q = strchr(p, '"');
    const char* e = q ? strchr(q + 1, '"') : nullptr;
    if (e) st.players.assign(q + 1, e);
  }
  if (const char* p = after("\"hitMargin\"")) st.margin = atoi(p);
  return st;
}

void Mask(FILE* f, const char* name, int x0, int y0, int w, int h, unsigned owners, bool last, std::vector<int>* legend) {
  fprintf(f, ",\"%s\":[", name);
  for (int y = 0; y < h; ++y) {
    std::string row;
    for (int x = 0; x < w; ++x) {
      const int id = Hit(x0 + x, y0 + y, owners, last);
      if (id < 0) { row += '.';  continue; }
      size_t k = 0;
      while (k < legend->size() && (*legend)[k] != id) ++k;
      if (k == legend->size()) legend->push_back(id);
      row += char(k < 10 ? '0' + k : 'a' + k - 10);
    }
    fprintf(f, "%s\"%s\"", y ? "," : "", row.c_str());
  }
  fputs("]", f);
}

void Dump(int tick) {
  if (!g_out) {
    char path[MAX_PATH];
    const char* out = getenv("OP2H_OUT");
    snprintf(path, sizeof(path), "%s\\hitmask.jsonl", out ? out : ".");
    g_out = fopen(path, "w");
    if (!g_out) return;
  }
  static const Settings st = Load();
  const int margin = st.margin;
  for (int i = 0; i < 1024; ++i) {
    char* o = Obj(i);
    const unsigned fl = *reinterpret_cast<unsigned*>(o + 0x44);
    if (!(fl & 6) || *reinterpret_cast<int*>(o + 4) == -1 || (fl & 0x20000)) continue;
    const int owner = *reinterpret_cast<unsigned char*>(o + 0x1D) & 15;
    if (st.players.find(char('0' + owner)) == std::string::npos) continue;
    const int px = *reinterpret_cast<int*>(o + 0x14), py = *reinterpret_cast<int*>(o + 0x18);
    int bw = 0, bh = 0, bx = 0, by = 0;
    Virtual<int>(o, 0x30, &bw, &bh);
    Virtual<int>(o, 0x34, &bx, &by);
    if (bw <= 0 || bh <= 0 || bw > 512 || bh > 512) continue;
    const bool building = (fl & 4) != 0;
    // Buildings: GetAnimationIndex (+0x2C, 0x483560; it takes no argument, whatever TethysAPI says); vehicles:
    // GetChassisAnimationIndex (+0x80) for the 16-way direction, which their selection box comes from (0x44CDA0).
    const int dir16 = ((*reinterpret_cast<unsigned char*>(o + 0x1C) + 8) & 0xF0) >> 4;
    const int anim  = building ? Virtual<int>(o, 0x2C) : Virtual<int>(o, 0x80, dir16);
    const int frame = building ? Virtual<int>(o, 0x98) : *reinterpret_cast<short*>(o + 0x42);
    char* type = Virtual<char*>(o, 0x00);
    const char* code = Virtual<const char*>(type, 0x04);
    const int x0 = px - bx - margin, y0 = py - by - margin, w = bw + 2 * margin, h = bh + 2 * margin;
    fprintf(g_out, "{\"tick\":%d,\"id\":%d,\"owner\":%d,\"code\":\"%s\",\"px\":%d,\"py\":%d,\"dir\":%d,\"action\":%d,\"flags\":%u,"
            "\"anim\":%d,\"frame\":%d,\"animIndex\":%d,\"frameIndex\":%d,\"state\":%u,\"box\":[%d,%d,%d,%d],"
            "\"x0\":%d,\"y0\":%d,\"w\":%d,\"h\":%d",
            tick, i, owner, code, px, py, *reinterpret_cast<unsigned char*>(o + 0x1C), *reinterpret_cast<unsigned char*>(o + 0x22),
            fl, anim, frame, *reinterpret_cast<unsigned short*>(o + 0x40), *reinterpret_cast<short*>(o + 0x42),
            *reinterpret_cast<unsigned*>(o + 0x34), -bx, -by, bw, bh, x0, y0, w, h);
    std::vector<int> legend;
    Mask(g_out, "sel", x0, y0, w, h, 1u << owner, true, &legend);
    Mask(g_out, "tgt", x0, y0, w, h, 0xFFFFFFFFu, false, &legend);
    fputs(",\"legend\":[", g_out);
    for (size_t k = 0; k < legend.size(); ++k) fprintf(g_out, "%s%d", k ? "," : "", legend[k]);
    fputs("]}\n", g_out);
  }
  fflush(g_out);
}

}  // namespace

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int tick) {
  static const Settings st = Load();
  for (int t : st.ticks)
    if (t == tick) Dump(tick);
}
