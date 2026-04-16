#pragma once

// Manages the UCrSaveGameUtils::WriteUserFile detour and async save thread.
namespace SaveHook
{
    // Install the hook.  Returns false if the pattern could not be found or
    // the hook could not be installed.
    bool Initialize();

    // Remove the hook and wait for any in-flight saves to finish.
    void Shutdown();
}
