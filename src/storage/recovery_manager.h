#pragma once

#include "storage/disk_manager.h"
#include "wal/log_manager.h"

#include <functional>

namespace ocera {

// Replays the complete append-only WAL before any metadata or buffer-pool
// component is allowed to read database pages. The first implementation
// deliberately retains the full WAL and therefore does not need page LSNs
// persisted in the on-disk page format.
class RecoveryManager {
public:
    enum class Action { REDO, UNDO };
    using Observer = std::function<void(Action, const LogRecord&)>;

    // Optional observer supports deterministic recovery fault-injection tests.
    // It runs immediately after a page image has been written.
    RecoveryManager(DiskManager& disk_manager, LogManager& log_manager,
                    Observer observer = {});

    // Safe to run repeatedly: redo is idempotent, and undo is replayed at the
    // corresponding ABORT record or at end-of-log for loser transactions.
    void Recover();

private:
    DiskManager& disk_manager_;
    LogManager& log_manager_;
    Observer observer_;
};

}  // namespace ocera
