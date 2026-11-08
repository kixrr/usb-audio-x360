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

// Two different lifetimes drive the save gate:
//  - the Guide volume panel, which is rebuilt with GuideMain.xur on every
//    Guide open, brackets the Guide itself;
//  - the settings panels (settings and properties pages), which are open only
//    while the user is editing.
// The settings worker refuses to write while a settings panel is alive, and
// waits a few seconds after the last change of either lifetime.
void StorageGuideEnter();
void StorageGuideLeave();
bool StorageIsGuideOpen();
void StoragePanelEnter();
void StoragePanelLeave();
bool StorageIsPanelOpen();
// True while either lifetime is alive; used to keep the diagnostics log handle
// open across a Guide session.
bool StorageIsUiActive();
