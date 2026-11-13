#pragma once

#include "common.h"

namespace pc {

// Runs the standalone IPMsg compatible dialog (host list, messages, file
// transfer, log and settings) as an IPC client. It does not create the main
// dashboard window; the editor itself starts the headless core first.
// startHidden = true (`--tray`) starts minimized to the notification area.
// One dialog exists per data directory (named mutex); a second launch brings
// the existing window to the front instead of creating another one.
// Blocks until the dialog exits; returns the process exit code.
int RunIpmsgDialog(bool startHidden);

// Brings an already running Chat window for the current data directory to the
// front. Returns false when no window exists.
bool ActivateIpmsgDialog();

} // namespace pc
