# Changelog

All releases of Forgotten Sands Recompiled (Windows PC). The newest version is at the top.

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
