# Controls

The original game is played with the PC keyboard. The port translates the Vita's buttons into the keyboard events the game reads. The table below was written from the port's input code (`source/win32/dinput.cpp`) and the original game's key handling; it lists **every** assignment that exists.

| Vita | Keyboard key sent | What it does in the game |
|---|---|---|
| D-pad ↑ ↓ ← → | Arrow keys | Menu navigation; in a race: steering (← →) and accelerate / brake (↑ ↓) |
| Left stick | Arrow keys | Same as the D-pad. A direction counts as pressed once the stick is about 47% of the way out (60 of 128 raw counts) and is released when it falls back below about 28% (36 counts); diagonals are two directions |
| ✕ Cross | Enter | Confirm / accept. In a **Yes/No prompt** (for example the quit-race question) it sends the "Yes" key of the language selected in the game (Y, J, S or O) |
| ○ Circle | Esc | Back / cancel in menus; in a race it opens the pause / quit prompt |
| Start | Esc | Same as ○ |
| △ Triangle | `/` | Shift up in a race (the original manual-gearbox key). In a **Yes/No prompt** it sends **N** ("No") |
| □ Square | `.` | Shift down in a race |
| L | `'` | Rear view |
| R | Right Shift | Turbo / boost |
| Select (tap) | Tab | Change view after a race (the original Tab key). The key is sent when you **release** Select, as a pulse of 150 ms, and not at all if □ was pressed while Select was held |
| Select held + □ | `` ` `` | Toggle the race results display |

Notes

- In the **menus**, confirm is ✕ and back is ○ (or Start). The △ button does not go back in menus; its "No" function applies to the game's Yes/No questions.
- The Yes/No prompts only change the mapping while the question is actually on screen **in a race** (the port reads the game's own prompt flag, the menu state and the race state), so ✕ keeps being "confirm" everywhere else. The key is chosen when the button is pressed and the same key is released with the button.
- Touch screen, rear touch pad, gyroscope and the right stick are not used.
- There are no remappable controls in v1.0.

## Changing the player name

The player name has **three characters** and is entered with the official Vita keyboard:

1. Open the player options menu (for player 1, or player 2) and select the first item, the **name**.
2. Press ✕. The Vita's system keyboard opens with the current name; its title is "Player name" and it accepts up to three characters.
3. Type the name and confirm with the keyboard's own confirm button. The menu shows the new name immediately.
4. To keep the old name, cancel the keyboard: nothing changes.

What is accepted: letters (they are converted to capitals, as in the PC game), digits, the space and a few symbols the game's own keyboard also accepted (`- = { } ; ' \ , . / * +`). Anything else (accented letters, emoji, other symbols) is dropped. A name with no valid character is ignored and the old name is kept.

The name is saved by the game when you leave it with its *Quit* option (like the PC version), so it is still there the next time.

While the keyboard is open the game ignores the Vita's buttons; they are given back to the game once you have released them after the keyboard closes.
