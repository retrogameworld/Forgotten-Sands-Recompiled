# Changelog

All releases of Forgotten Sands Recompiled (Windows PC). The newest version is at the top.

## 1.2 (2026-11-27)

- New: achievements. The game's 40 Xbox 360 achievements now unlock as you play. A notice with the achievement's icon,
  name and gamerscore appears at the top of the screen for a few seconds. Story achievements you passed before this
  version can unlock soon after you continue your save. Unlocks are kept in
  `achievements\unlocked.txt` in your save folder, and the full list (names, descriptions, icons) is in
  `achievements\list.json`. To reset them, set `reset_achievements = true` in `settings.toml` (clears them at the
  next start, then turns back to `false`) or delete `unlocked.txt`. Your game saves are separate and not touched.
- Removed: the "Uplay - Action completed" popup at the top right of the screen (left over from the game's Uplay
  rewards, which no longer exist).

## 1.1 (2026-11-27)

- Fixed: enemies did not attack and jars could not be broken in 1.0 (found through the Android version: the memory
  change in 1.0 placed the game's memory blocks directly one after another, which the game does not expect). The
  memory fix (no more running out of memory) stays.

## 1.0 (2026-11-27)

First numbered release. Releases before it were named by date (the last one: v2026.11.24). Changes since v2026.11.24:

- Fixed: walls, floors and stairs of some rooms were missing (the desert backdrop showed through), for example in the
  corridor after the first save and in the courtyard after the gate.
- Fixed: the game's memory filled up as you played on, which could make level parts go missing or crash the game after
  some minutes in places. It now uses about half as much of the console's memory.
- Fixed: a rare crash or freeze in the sound engine (sound contexts now come from their own pool, like on the console).
- Fixed: a flickering wall where the game reused a buffer for another kind of data.
- Fixed: a white stripe at the left edge after skipping the Ubisoft logo.
- If the graphics card or its driver lacks something the game needs, the error message now says what is missing.
