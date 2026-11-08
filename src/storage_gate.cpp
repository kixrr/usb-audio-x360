// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_gate.h"

namespace {

CRITICAL_SECTION g_io_lock;
volatile LONG g_initialized = 0;
volatile LONG g_guide_depth = 0;
volatile LONG g_panel_depth = 0;

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

void StorageGuideEnter() { InterlockedIncrement(&g_guide_depth); }

void StorageGuideLeave() {
  if (InterlockedCompareExchange(&g_guide_depth, 0, 0) > 0)
    InterlockedDecrement(&g_guide_depth);
}

bool StorageIsGuideOpen() {
  return InterlockedCompareExchange(&g_guide_depth, 0, 0) > 0;
}

void StoragePanelEnter() { InterlockedIncrement(&g_panel_depth); }

void StoragePanelLeave() {
  if (InterlockedCompareExchange(&g_panel_depth, 0, 0) > 0)
    InterlockedDecrement(&g_panel_depth);
}

bool StorageIsPanelOpen() {
  return InterlockedCompareExchange(&g_panel_depth, 0, 0) > 0;
}

bool StorageIsUiActive() { return StorageIsGuideOpen() || StorageIsPanelOpen(); }
