// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_gate.h"

namespace {

CRITICAL_SECTION g_io_lock;
volatile LONG g_initialized = 0;
volatile LONG g_ui_depth = 0;

}  // namespace

void StorageInitialize() {
  if (InterlockedCompareExchange(&g_initialized, 1, 0) == 0)
    InitializeCriticalSection(&g_io_lock);
}

bool StorageTryLock() {
  if (!InterlockedCompareExchange(&g_initialized, 0, 0)) return false;
  return TryEnterCriticalSection(&g_io_lock) != FALSE;
}

void StorageLock() {
  if (InterlockedCompareExchange(&g_initialized, 0, 0))
    EnterCriticalSection(&g_io_lock);
}

void StorageUnlock() {
  if (InterlockedCompareExchange(&g_initialized, 0, 0))
    LeaveCriticalSection(&g_io_lock);
}

void StorageUiEnter() { InterlockedIncrement(&g_ui_depth); }

void StorageUiLeave() {
  if (InterlockedCompareExchange(&g_ui_depth, 0, 0) > 0)
    InterlockedDecrement(&g_ui_depth);
}

bool StorageIsUiActive() {
  return InterlockedCompareExchange(&g_ui_depth, 0, 0) > 0;
}
