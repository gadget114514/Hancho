#pragma once

#include "process_manager.h"
#include "ipc_server.h"
#include "taskqueue.h"

namespace pc {

int RunGuiDashboard(ProcessManager& pm, IpcServer& ipc, TaskQueueManager& tqm);

} // namespace pc
