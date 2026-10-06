// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keeps a copy of the emulated SD card, NAND and save states on the USB drive,
// so reinstalling the app (which wipes its LocalState) does not lose progress.
#pragma once
#include "onyx/settings.h"

namespace onyx::app {
// "<usb>/ONYX3DS/SaveBackup", or empty when no USB folder is configured.
std::string SaveBackupDir(const FolderConfig& folders);
// Mirror saves to the backup folder (no-op when Saves already lives on USB).
void BackupSaves(const FolderConfig& folders);
// If the app has no saves of its own but a backup exists, copy it back.
void RestoreSavesIfFresh(const FolderConfig& folders);
} // namespace onyx::app
