# Controls

The original game is played with the PC keyboard. The port translates the Vita's buttons into the keyboard events the game reads. The tables below were written from the port's input code (`source/win32/dinput.cpp`) and the original game's key handling.

## 1. Default controls (v1.1)

| Vita | Keyboard key sent | What it does in the game |
|---|---|---|
| D-pad ← → | Left / Right arrow | Menu navigation; in a race: **steering** |
| D-pad ↑ ↓ | Up / Down arrow | Menu navigation; **no action in a race** (see below) |
| Left stick | Arrow keys | Same as the D-pad. A direction counts as pressed once the stick is about 47% of the way out (60 of 128 raw counts) and is released when it falls back below about 28% (36 counts); diagonals are two directions. The vertical directions are inactive in a race |
| ✕ Cross | Enter in menus; **the key assigned to ACC. in a race** | Confirm / accept. In a **Yes/No prompt** (for example the quit-race question) it sends the "Yes" key of the language selected in the game (Y, J, S or O). In a race: **accelerate** |
| □ Square | `.` in menus; **Down arrow in a race** | In a race: **brake / reverse** (the key of the *BRAKE* action in the default configuration) |
| ○ Circle | Esc | Back / cancel in menus; in a race it opens the pause / quit prompt |
| Start | Esc | Same as ○ |
| △ Triangle | `/` | Shift up in a race (the original manual-gearbox key). In a **Yes/No prompt** it sends **N** ("No") |
| L | `'` | Rear view |
| R | Right Shift | Turbo / boost |
| Select (tap) | Tab | Change view after a race (the original Tab key). The key is sent when you **release** Select, as a pulse of 150 ms, and not at all if □ was pressed while Select was held |
| Select held + □ | `` ` `` | Toggle the race results display |

Notes

- A "race" here means the car is under your control: the race is running and not paused, no results phase or question is on screen (the port reads the game's own race state words). Everywhere else the buttons behave as in the menus.
- In **menus**, confirm is ✕ and back is ○ (or Start). △ does not go back in menus; its "No" function applies to the game's Yes/No questions.
- The Yes/No prompts only change the mapping while the question is actually on screen **in a race**, so ✕ keeps being "confirm" everywhere else. The key is chosen when the button is pressed and the same key is released with the button.
- ✕ sends **the key that the game's configuration assigns to *ACC.***. With the default configuration that is the Up arrow; a save in which you had assigned Enter to *ACC.* (for example with ✕ in v1.0) keeps working because ✕ sends Enter in that case. □ sends the Down arrow, which is *BRAKE* in the default configuration.
- **Why D-pad ↑ ↓ and stick ↑ ↓ do nothing in a race:** they used to send the same arrow keys as accelerate and brake and interfered with ✕ and □. They are reserved so that accelerating and braking are done with ✕ and □ (or the buttons you assign). They still navigate the menus.
- Touch screen, rear touch pad, gyroscope and the right stick are not used.

## 2. Configurable controls (v1.1)

The game's own configuration screen can assign any Vita button to the race actions. Open **Options → Player 1 → Keyboard config**:

1. Select an action and confirm with ✕. The game waits for a key.
2. **Release every button** (the port ignores buttons that are still held when the screen starts waiting, so the ✕ that opened it is never assigned) and press the button you want. The screen shows its name.
3. The game stores the assignment immediately. Repeat for other actions; leave the screen as usual.

The eight actions of Player 1 (labels of the game, default keys): **TURN LEFT** (←), **TURN RIGHT** (→), **ACC.** (↑), **BRAKE** (↓), **GEAR UP** (`/`), **GEAR DOWN** (`.`), **BOOST** (Right Shift), **REAR VIEW** (`'`). The game has no horn and no separate camera-change action (*REAR VIEW* is the only view key), and pause is not configurable (○ / Start). The same eight actions exist for Player 2 (*Options → Player 2*); they only matter for split screen, which is disabled in this port.

Names shown by the screen: `PAD X`, `PAD O`, `PAD SQ`, `PAD TRI`, `PAD L`, `PAD R`, `START`, `SELECT`, `PAD UP`, `PAD DN`, `PAD LT`, `PAD RT`.

Rules and behaviour

- **Duplicates:** the game itself refuses a button that is already assigned to another action (of either player); the screen keeps waiting and the next button you press is evaluated. The port does not change this check.
- **Pause guard:** ○ and Start cannot both be assigned, because one of them must remain the pause button; the second is ignored.
- **No cancel:** the original screen cannot be left without choosing a button. To keep an action unchanged press the button it already has.
- **Persistence:** assignments live in the 16 bytes of the game's key table, which are part of `ign_3dfx.btz` (saved with the rest of the preferences). A save made by v1.0 or by earlier builds loads without changes.
- **In a race**, a button assigned to a race action sends that action's key **instead of** its default key (for example, with *ACC.* on R, R accelerates and ✕ does nothing). A button that is not assigned keeps its default behaviour. Assignments made on Player 2's page do not change races.
- **D-pad ↑ ↓ and stick ↑ ↓ stay inactive in races** whatever you assign (the buttons themselves, D-pad ↑ ↓, can be assigned as `PAD UP` / `PAD DN` and then act as the action you gave them).
- **Steering:** the game only reads the keys that are configured. If you assign *TURN LEFT / TURN RIGHT* to Vita buttons, the D-pad ← → and the stick (which send the arrow keys) no longer steer. To steer with the D-pad assign `PAD LT` and `PAD RT`. The left stick itself cannot be assigned.
- **Restoring the defaults:** assign the actions again (for *ACC.* use `PAD X`, for *BRAKE* `PAD SQ`, for steering `PAD LT` / `PAD RT`). The original keyboard keys (arrows, `.`, `/` ...) cannot be selected from the Vita. The only complete reset is deleting `ux0:data/ignition/ign_3dfx.btz`, which also erases your progress and options: **back it up first**.
- Menus are never affected: in menus the buttons always send the keys of the table above, so the configuration screen can always be reached and every action can be reassigned.

## 3. Technical table: Vita button → virtual key

While the configuration screen waits for a key, each Vita button is delivered to the game as a **virtual DirectInput key code** that no real key uses, one code per button, so that every button can be told apart and none of them is a navigation or reserved key. The game's own capture routine stores that code in the key table, and in a race the port sends the same code when you press the button.

| Virtual DIK | Vita button | `SCE_CTRL` bit | Name shown by the game |
|---|---|---|---|
| `0xA0` | ✕ Cross | `0x4000` | `PAD X` |
| `0xA1` | ○ Circle | `0x2000` | `PAD O` |
| `0xA2` | □ Square | `0x8000` | `PAD SQ` |
| `0xA3` | △ Triangle | `0x1000` | `PAD TRI` |
| `0xA4` | L | `0x0100` | `PAD L` |
| `0xA5` | R | `0x0200` | `PAD R` |
| `0xA6` | Start | `0x0008` | `START` |
| `0xA7` | Select | `0x0001` | `SELECT` |
| `0xA8` | D-pad ↑ | `0x0010` | `PAD UP` |
| `0xA9` | D-pad ↓ | `0x0040` | `PAD DN` |
| `0xAA` | D-pad ← | `0x0080` | `PAD LT` |
| `0xAB` | D-pad → | `0x0020` | `PAD RT` |

The table is the array `kG167Btns` in `source/win32/dinput.cpp` (the only place that lists the codes). The codes `0xA0`–`0xAB` are unused by the game and by the port's own key set, have no command in the menu key table, and are accepted by the game's allowed-key table; each has its own name slot, rewritten in place at start-up. Mechanism (capture, delivery, duplicates, persistence): [TECHNICAL_NOTES.md](TECHNICAL_NOTES.md#12-configurable-controls-and-virtual-keys).

## Changing the player name

The player name has **three characters** and is entered with the official Vita keyboard:

1. Open the player options menu (for player 1, or player 2) and select the first item, the **name**.
2. Press ✕. The Vita's system keyboard opens with the current name; its title is "Player name" and it accepts up to three characters.
3. Type the name and confirm with the keyboard's own confirm button. The menu shows the new name immediately.
4. To keep the old name, cancel the keyboard: nothing changes.

What is accepted: letters (they are converted to capitals, as in the PC game), digits, the space and a few symbols the game's own keyboard also accepted (`- = { } ; ' \ , . / * +`). Anything else (accented letters, emoji, other symbols) is dropped. A name with no valid character is ignored and the old name is kept.

The name is saved by the game (like the PC version; leaving with the *Quit* option is the safest way), so it is still there the next time.

While the keyboard is open the game ignores the Vita's buttons; they are given back to the game once you have released them after the keyboard closes.
