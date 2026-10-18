// Cursor probe bot (notes/findings-polish.md "Command cursors"). Every tick it asks the detail pane's mouse handler
// (the global at 0x5471B0, vtable 0x4CFC60) which cursor it would show now, by calling its GetCursor (vtable +0x1C,
// 0x41B4E0) the way WM_SETCURSOR does, and maps the HCURSOR back to its index in the game's cursor table (0x56EA08,
// 35 handles loaded from the name/id table at 0x4E9878 by 0x488910). Drive the virtual mouse with the harness's input
// verbs (`mouse unit @3`, `key a`, ...): the handler reads the mouse map position the pane stored on WM_MOUSEMOVE
// (0x57B7B4/0x57B7B8) and the modifiers through GetAsyncKeyState, both of which the harness feeds.
//
// Run: harness/run.sh --scenario S.json --script X.txt --bot harness/build/cursor_bot.dll --out DIR
// Log: $OP2H_OUT\cursor.log, one line per tick on which anything logged changes (every tick with CURSOR_ALL=1):
//   tick cursor-index name state mode-vtable mouse-x mouse-y
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace {

FILE* g_log = nullptr;
void Log(const char* fmt, ...) {
  if (!g_log) {
    char path[MAX_PATH];
    const char* out = getenv("OP2H_OUT");
    snprintf(path, sizeof(path), "%s\\cursor.log", out ? out : ".");
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

// Index -> what the table at 0x4E9878 loads (system cursors, out2res ids, .ani files).
const char* const kNames[35] = {
  "IDC_WAIT", "IDC_CROSS", "IDC_ARROW", "destruct", "res204", "move", "no_move", "attack", "noattack", "bulldoze",
  "res204", "build", "nobuild", "dock", "nodock", "repair", "norepair", "reprog", "no_repro", "guard",
  "res204", "res176", "res204", "salvage", "res204", "waypoint", "res204", "scroll113", "scroll111", "scroll117",
  "scroll109", "scroll115", "scroll110", "scroll112", "scroll116",
};

int Index(HCURSOR c) {
  HCURSOR* table = reinterpret_cast<HCURSOR*>(0x56EA08);
  for (int i = 0; i < 35; ++i)
    if (table[i] == c) return i;
  return -1;
}

int g_last[5] = { -2, -2, -2, -2, -2 };

}  // namespace

extern "C" __declspec(dllexport) void __cdecl HarnessBotTick(int tick) {
  char* handler = reinterpret_cast<char*>(0x5471B0);
  using GetCursorFn = HCURSOR(__thiscall*)(void*);
  HCURSOR c = reinterpret_cast<GetCursorFn>(0x41B4E0)(handler);
  int idx = Index(c);
  int state = *reinterpret_cast<int*>(handler + 0x24);
  void* mode = *reinterpret_cast<void**>(handler + 0x2C);
  int vt = mode ? *reinterpret_cast<int*>(mode) : 0;
  int mx = *reinterpret_cast<int*>(0x57B7B4), my = *reinterpret_cast<int*>(0x57B7B8);
  int now[5] = { idx, state, vt, mx, my };
  bool same = true;
  for (int i = 0; i < 5; ++i) same = same && now[i] == g_last[i];
  static const bool all = getenv("CURSOR_ALL") != nullptr;
  if (same && !all) return;
  for (int i = 0; i < 5; ++i) g_last[i] = now[i];
  Log("%d %d %s %d 0x%06X %d %d", tick, idx, idx >= 0 ? kNames[idx] : "?", state, vt, mx, my);
}
