#pragma once

// Manages the UCrSaveGameUtils::WriteUserFile detour and async save thread.
struct IPluginSelf;
struct IPluginHookScanner;

namespace SaveHook
{
    // Resolve every AOB the save path needs.  Callable only from the plugin's
    // OnPluginLoadHooks export -- the loader refuses scans made anywhere else.
    // All four are required: without them the plugin cannot write a save at all,
    // so a miss refuses the plugin instead of leaving it silently broken.
    void ResolvePatterns(IPluginSelf* self, IPluginHookScanner* scanner);

    // Install the hook.  Returns false if a pattern did not resolve or the hook
    // could not be installed.
    bool Initialize();

    // Remove the hook and wait for any in-flight saves to finish.
    void Shutdown();
}
