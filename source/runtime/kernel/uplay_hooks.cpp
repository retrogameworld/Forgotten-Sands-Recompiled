#include <stdafx.h>
#include <os/logger.h>

// Step 38f (the tester: "a uplay window popped up at the top right saying action complete? should be removed"): the game's
// own Uplay layer shows "Uplay - Action completed" (UI message "UplayActionCompletedMessage") when a Uplay action is
// reached. sub_8277F888 only builds that message (localized text, icon, parameters) and hands it to the UI object for
// display; it changes no game state. It is skipped now, so no popup appears; everything else of the game is unchanged.
// FS_UPLAY_POPUP=1 shows it again.
PPC_FUNC_IMPL(__imp__sub_8277F888);
PPC_FUNC(sub_8277F888)
{
    static const bool s_show = getenv("FS_UPLAY_POPUP") != nullptr;
    if (s_show)
    {
        __imp__sub_8277F888(ctx, base);
        return;
    }
    static std::atomic<uint32_t> s_count{0};
    if (s_count++ < 5)
        LOGN("[uplay] \"Action completed\" popup skipped");
}
