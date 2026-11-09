#pragma once

namespace IWXMVM::T5::Diagnostics
{
    // Harness-only checks for two failures (hangs in the script memory tree, crashes in
    // CG_DoControllers while a demo loads), logged to diag.txt in the folder: blocks of the script memory tree freed
    // twice, with the stacks of both frees, the free lists of the script variables broken, and what the game was doing
    // when it crashed at a known address.
    void Install(const std::filesystem::path& directory);
    // takes out the handler of crashes, before the harness unloads with the mod (not the watchpoints of
    // IWXMVM_DIAG_WATCH)
    void Uninstall();

    // checks the script memory tree and the variables' free lists once a frame, on the main thread
    void OnMainThreadFrame();

    // the server commands the playback ran and cleared as the demo was read, ahead of the game (see
    // T5Interface::ExecuteNewServerCommands), logged by the next frame
    void OnEarlyServerCommands(int from, int to);

    // which threads changed the script strings, variables and memory, by instance (with IWXMVM_DIAG_THREADS)
    std::string DescribeScriptThreads();
    // notes the render thread for that, from its frames
    void OnRenderThreadFrame();
}  // namespace IWXMVM::T5::Diagnostics
