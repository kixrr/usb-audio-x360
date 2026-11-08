// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include "audio.h"
#include "diagnostics.h"
#include "storage_gate.h"
#include <stdio.h>
#include <string.h>

namespace {
using namespace audio_settings;
static CRITICAL_SECTION g_lock;
static Table g_table;
static volatile LONG g_ready = 0, g_active = -1, g_generation = 0;
// A device can be claimed before the storage worker finishes its first read.
// Remember it instead of dropping it, and select it once the table is loaded.
static volatile LONG g_pending_key = 0, g_pending_device = 0;
static volatile LONG g_fallback = kDefault;
static char g_path[MAX_PATH], g_backup[MAX_PATH];
// Large buffers belong to this worker, not its stack or the audio thread.
static Table g_snapshot;
static char g_bytes[32769];
// Size of the last successful write. Rewriting the same size needs no
// SetEndOfFile, which itself can take seconds on this console.
static size_t g_last_size = (size_t)-1;
// The INI handle stays open for the whole boot. CreateFile was the single
// slowest step of a save on hardware (3.5-20 s for a 286-byte file); with the
// handle already open a save is one write plus a truncate.
static HANDLE g_ini = INVALID_HANDLE_VALUE;

static void CloseIni() {
  if (g_ini != INVALID_HANDLE_VALUE) {
    CloseHandle(g_ini);
    g_ini = INVALID_HANDLE_VALUE;
  }
}

static volatile LONG* Levels(LONG index) {
  return index >= 0 && index < (LONG)kCapacity
      ? (volatile LONG*)&g_table.entries[index].levels : &g_fallback;
}
// 1=valid, 0=missing, -1=unreadable/invalid. Never overwrite an unread file.
static int Read(const char* path, Table* table) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
  DWORD high = 0, size = GetFileSize(f, &high), got = 0;
  bool ok = !high && size && size <= 32768 && ReadFile(f, g_bytes, size, &got, 0) && got == size;
  CloseHandle(f);
  return ok && Parse(g_bytes, size, table) ? 1 : -1;
}
static bool Load(bool* writable) {
  *writable = false;
  int primary = Read(g_path, &g_snapshot);
  if (primary == 1) { *writable = true; return true; }
  int backup = Read(g_backup, &g_snapshot);
  if (backup == 1) {
    // Use the valid backup and allow the primary to be replaced. With in-place
    // writes an interrupted save can leave an unreadable primary; refusing to
    // overwrite it would silently disable persistence for the whole session.
    // The data written back is the valid backup plus live changes.
    *writable = true;
    return true;
  }
  if (primary == 0 && backup == 0) {
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    *writable = true;
    return true;
  }
  return false;
}
static bool MergeLoaded() {
  EnterCriticalSection(&g_lock);
  unsigned missing = 0;
  for (unsigned i = 0; i < g_snapshot.count; ++i) {
    unsigned j = 0;
    while (j < g_table.count && g_table.entries[j].key != g_snapshot.entries[i].key) ++j;
    if (j == g_table.count) ++missing;
  }
  if (g_table.count + missing > kCapacity) { LeaveCriticalSection(&g_lock); return false; }
  for (unsigned i = 0; i < g_snapshot.count; ++i) {
    unsigned j = 0;
    while (j < g_table.count && g_table.entries[j].key != g_snapshot.entries[i].key) ++j;
    // Never change a live record on delayed storage recovery. It may already
    // contain user adjustments; its slot must remain stable for atomic setters.
    if (j == g_table.count) g_table.entries[g_table.count++] = g_snapshot.entries[i];
  }
  LeaveCriticalSection(&g_lock);
  return true;
}
static bool OpenIni() {
  if (g_ini != INVALID_HANDLE_VALUE) return true;
  DWORD started = GetTickCount();
  g_ini = CreateFileA(g_path, GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  DWORD open_ms = GetTickCount() - started;
  if (g_ini == INVALID_HANDLE_VALUE) {
    DiagnosticsStorage("save-open", open_ms, GetLastError());
    return false;
  }
  DiagnosticsStorage("save-open", open_ms, 1);
  return true;
}

static bool Save() {
  size_t size = 0;
  DWORD format_started = GetTickCount();
  if (!Format(g_snapshot, g_bytes, sizeof(g_bytes), &size)) return false;
  DiagnosticsStorage("save-format", GetTickCount() - format_started, (DWORD)size);
  if (!OpenIni()) return false;
  DWORD started = GetTickCount();
  SetFilePointer(g_ini, 0, 0, FILE_BEGIN);
  DWORD written = 0;
  bool ok = WriteFile(g_ini, g_bytes, (DWORD)size, &written, 0) != FALSE &&
            written == size;
  DWORD write_ms = GetTickCount() - started;
  DWORD truncate_ms = 0;
  if (ok && size != g_last_size) {
    started = GetTickCount();
    ok = SetEndOfFile(g_ini) != FALSE;
    truncate_ms = GetTickCount() - started;
  }
  // A failed write leaves the handle in an unknown state; reopen next time.
  if (!ok) CloseIni();
  else g_last_size = size;
  DiagnosticsStorage("save-io", write_ms, truncate_ms);
  return ok;
}
static void UpdateName() {
  AudioDeviceInfo info;
  if (!AudioGetDeviceInfo(&info) || !info.connected || !info.product_name[0]) return;
  char encoded[256] = {}, name[128] = {};
  if (!WideCharToMultiByte(CP_UTF8, 0, info.product_name, -1, encoded, sizeof(encoded), 0, 0)) return;
  size_t length = strlen(encoded);
  if (length >= sizeof(name)) {
    length = sizeof(name)-1;
    while (length && ((unsigned char)encoded[length] & 0xc0) == 0x80) --length;
  }
  memcpy(name, encoded, length);
  for (size_t c = 0; c < length; ++c)
    if ((unsigned char)name[c] < 32 || name[c] == 127) name[c] = ' ';
  unsigned key = ((unsigned)info.vendor_id << 16) | info.product_id;
  EnterCriticalSection(&g_lock);
  for (unsigned i = 0; i < g_table.count; ++i) {
    Entry& e = g_table.entries[i];
    if (e.key == key && strcmp(e.name, name)) {
      strcpy(e.name, name);
      InterlockedIncrement(&g_generation);
      break;
    }
  }
  LeaveCriticalSection(&g_lock);
}
}

VOID SettingsInitialize() {
  InitializeCriticalSection(&g_lock);
  // Reuse the resolved module-directory alias, never a title-relative path.
  if (DiagnosticsSettingsPath(g_path, sizeof(g_path))) {
    sprintf(g_backup, "%s.bak", g_path);
  }
}
LONG SettingsGet(audio_settings::Field field) {
  LONG index = InterlockedCompareExchange(&g_active, 0, 0);
  return audio_settings::Get(InterlockedCompareExchange(Levels(index), 0, 0), field);
}
BOOL SettingsSet(audio_settings::Field field, LONG value) {
  LONG index = InterlockedCompareExchange(&g_active, 0, 0);
  volatile LONG* target = Levels(index);
  LONG old = InterlockedCompareExchange(target, 0, 0);
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    LONG next = audio_settings::Set(old, field, value < 0 ? 0 : (unsigned)value);
    if (next == old) return FALSE;
    LONG actual = InterlockedCompareExchange(target, next, old);
    if (actual == old) {
      if (index >= 0) InterlockedIncrement(&g_generation);
      return TRUE;
    }
    old = actual;
  }
  return FALSE; // Never wait indefinitely in the IR callback.
}
// Caller holds g_lock. Returns the entry index for key, creating it if needed.
static LONG SelectKeyLocked(unsigned key) {
  unsigned i = 0;
  while (i < g_table.count && g_table.entries[i].key != key) ++i;
  if (i == g_table.count && i < kCapacity) {
    Entry& e = g_table.entries[i];
    e.key = key; e.levels = kDefault; e.name[0] = 0;
    ++g_table.count;
    InterlockedIncrement(&g_generation);
  }
  return i < kCapacity ? (LONG)i : -1;
}
VOID SettingsSelectDevice(WORD vendor, WORD product) {
  // Only the audio worker selects devices, before opening their endpoints.
  // Disk I/O never holds this lock; setters/getters never acquire it.
  unsigned key = ((unsigned)vendor << 16) | product;
  if (!InterlockedCompareExchange(&g_ready, 0, 0)) {
    // Storage is optional: do not wait on a blocked initial read. Keep the
    // key so the first device of the boot still gets persisted.
    InterlockedExchange(&g_pending_key, (LONG)key);
    InterlockedExchange(&g_pending_device, 1);
    if (!InterlockedCompareExchange(&g_ready, 0, 0)) {
      InterlockedExchange(&g_active, -1);
      return;
    }
  }
  EnterCriticalSection(&g_lock);
  InterlockedExchange(&g_pending_device, 0);
  InterlockedExchange(&g_active, SelectKeyLocked(key));
  LeaveCriticalSection(&g_lock);
}
DWORD WINAPI SettingsWorker(void*) {
  bool writable = false;
  if (g_path[0]) {
    StorageLock();
    bool loaded = Load(&writable);
    // Open the write handle at boot, while no Guide is on screen, so the
    // first in-Guide save does not pay for CreateFile (seconds on hardware).
    // Only when the file is writable: the recovery path below still needs to
    // read it without a write handle in the way.
    if (loaded && writable) OpenIni();
    StorageUnlock();
    if (loaded) g_table = g_snapshot;
  }
  InterlockedExchange(&g_ready, 1);
  if (InterlockedCompareExchange(&g_pending_device, 0, 0) == 1) {
    unsigned key = (unsigned)InterlockedCompareExchange(&g_pending_key, 0, 0);
    EnterCriticalSection(&g_lock);
    InterlockedExchange(&g_active, SelectKeyLocked(key));
    InterlockedExchange(&g_pending_device, 0);
    LeaveCriticalSection(&g_lock);
    DiagnosticsStorage("settings-pending", key, 1);
  }
  LONG saved = 0, observed = 0;
  DWORD changed = GetTickCount(), attempted = changed - 10000u;
  DWORD retry = 10000;
  DWORD last_load = changed;
  // Write as soon as the settings page is gone: the page transition has
  // finished after half a second and the user is normally still in the Guide.
  // GuideMain.xur creation is the one thing a write must never overlap, so
  // opening the Guide restarts this short timer. `changed` still enforces a
  // two-second debounce so rapid toggles collapse into one write.
  DWORD ui_quiet = GetTickCount();
  LONG guide_seen = StorageIsGuideOpen() ? 1 : 0;
  DWORD panel_since = 0;
  bool defer_logged = false;
  for (;;) {
    Sleep(500);
    if (!writable && g_path[0] && GetTickCount() - last_load >= 30000u) {
      bool can_write = false;
      StorageLock();
      bool loaded = Load(&can_write);
      if (loaded && can_write && MergeLoaded()) {
        writable = true;
        OpenIni();
      }
      StorageUnlock();
      last_load = GetTickCount();
    }
    UpdateName();
    LONG generation = InterlockedCompareExchange(&g_generation, 0, 0);
    DWORD now = GetTickCount();
    if (generation != observed) { observed = generation; changed = now; }
    if (!writable || generation == saved) { defer_logged = false; continue; }
    const LONG guide_now = StorageIsGuideOpen() ? 1 : 0;
    if (guide_now != guide_seen) {
      guide_seen = guide_now;
      // Only a Guide *open* postpones the write: GuideMain.xur creation is the
      // one scene load a write must not overlap. Guide close does not restart
      // the timer, so leaving the Guide quickly still saves promptly.
      if (guide_now) ui_quiet = now;
    }
    const bool panel = StorageIsPanelOpen();
    if (panel) {
      if (!panel_since) panel_since = now;
      ui_quiet = now;
    } else {
      panel_since = 0;
    }
    // A settings panel should never live this long; if one leaks, do not let
    // persistence stop forever.
    const bool panel_stuck = panel && panel_since && now - panel_since >= 600000u;
    if (!panel_stuck && now - ui_quiet < 500u) {
      if (!defer_logged && SaveDue(now, changed, attempted, retry)) {
        DiagnosticsStorage("settings-defer", (DWORD)generation, (DWORD)saved);
        defer_logged = true;
      }
      continue;
    }
    if (panel_stuck) DiagnosticsStorage("settings-forced", panel_since, now);
    if (!SaveDue(now, changed, attempted, retry)) continue;
    defer_logged = false;
    EnterCriticalSection(&g_lock);
    LONG snapshot_generation = InterlockedCompareExchange(&g_generation, 0, 0);
    g_snapshot.count = g_table.count;
    for (unsigned i = 0; i < g_table.count; ++i) {
      g_snapshot.entries[i].key = g_table.entries[i].key;
      strcpy(g_snapshot.entries[i].name, g_table.entries[i].name);
      g_snapshot.entries[i].levels = InterlockedCompareExchange(Levels(i), 0, 0);
    }
    LeaveCriticalSection(&g_lock);
    DWORD save_started = GetTickCount();
    StorageLock();
    DWORD lock_ms = GetTickCount() - save_started;
    bool ok = Save();
    StorageUnlock();
    DWORD elapsed = GetTickCount() - save_started;
    DiagnosticsStorage("settings-lock", lock_ms, elapsed);
    DiagnosticsStorage("settings-save", elapsed, ok ? 1 : 0);
    attempted = GetTickCount();
    // Space repeated writes by at least as long as the last one took, with a
    // floor and a ceiling. A slow write must not turn into a fixed minute of
    // no persistence for the next change.
    DWORD backoff = elapsed < 5000u ? 5000u : elapsed;
    if (backoff > 60000u) backoff = 60000u;
    retry = ok ? backoff : 30000;
    if (ok) saved = snapshot_generation;
  }
}
