#pragma once

// Save progress stages shared between hook and widget.
enum class SaveStage : int
{
    Idle        = 0,   // Nothing in progress
    Queued      = 1,   // Data copied, thread about to start
    Compressing = 2,   // Background thread running zlib
    Writing     = 3,   // Background thread writing to disk
    Done        = 4,   // Write succeeded
    Failed      = 5,   // Write failed
};
