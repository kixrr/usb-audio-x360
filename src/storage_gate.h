// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <xtl.h>

// Serializes every filesystem operation the plugin performs. The diagnostics
// writer only ever *tries* to take the lock so the notification thread can
// never block behind a slow settings save. Settings I/O takes it for real.
void StorageInitialize();
bool StorageTryLock();
void StorageLock();
void StorageUnlock();

// The Guide owns the same storage device while it is navigating. Track how
// many plugin-created panels are alive so the settings worker can keep its
// writes out of that window (and out of the transition that follows it).
void StorageUiEnter();
void StorageUiLeave();
bool StorageIsUiActive();
