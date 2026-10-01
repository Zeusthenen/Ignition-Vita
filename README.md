# Ignition-Vita

Unofficial PlayStation Vita port of **Ignition (1997)**.

> [!WARNING]
> **Work in Progress**
>
> This project is under active development and is currently not ready for normal gameplay.

---

## About

**Ignition-Vita** is an experimental project aiming to bring the original PC version of **Ignition (1997)** to the PlayStation Vita.

The project is focused on running the original game logic while replacing or adapting the PC-specific systems required to make the game work correctly on PlayStation Vita hardware.

The current development path is focused primarily on the **3dfx / Glide version of the game**, with the goal of translating the original rendering behaviour to a Vita-compatible graphics backend.

---

## Current Status

The project is currently in an early experimental stage.

### Working / partially working

- Application launches on real PlayStation Vita hardware.
- Original game executable behaviour is being analysed and adapted.
- Initial game rendering has been reached.
- In-game graphics can already appear on the Vita.
- Game background / scene movement has been observed on real hardware.
- The current build can reach game rendering without immediately crashing.
- Development is now focused on reproducing the original 3dfx / Glide rendering correctly.

### Not working correctly yet

- Rendering is currently incomplete / corrupted.
- Textures, geometry, colours and/or screen output may be incorrect.
- Performance has not yet been optimised.
- Audio is not considered finished.
- Controls are not final.
- Menus and gameplay are not guaranteed to work correctly.
- Multiplayer is not implemented yet.
- Stability is still under investigation.

The first major milestone is:

> **Get the complete game rendering correctly and looking as close as possible to the original PC version.**

Performance optimisation will come after rendering correctness.

---

## Main Goals

The long-term objectives of the project are:

- Run Ignition on a real PlayStation Vita.
- Preserve the original game behaviour as closely as possible.
- Correctly reproduce the original 3dfx / Glide renderer.
- Use Vita-compatible graphics APIs such as **vitaGL** where appropriate.
- Reach stable and playable performance.
- Target approximately **30 FPS** where possible.
- Preserve original music, sound effects, videos and game assets.
- Implement proper Vita controls.
- Investigate multiplayer support after the single-player game is stable.
- Support up to the original multiplayer capabilities if technically possible.

---

## Development Strategy

The project is currently based around analysing and adapting the original PC version rather than recreating Ignition from scratch.

The main areas currently being investigated include:

- Original executable behaviour
- 3dfx / Glide rendering calls
- Graphics state translation
- Texture management
- Framebuffer handling
- Resolution conversion
- Input
- Audio
- File system access
- Timing
- Memory management
- Vita-specific optimisation

The target Vita resolution is:

`960x544`

---

## Project Phases

### Phase 1 — Rendering

Current priority.

Goal:

**Make the entire game render correctly on PlayStation Vita.**

This includes:

- Geometry
- Textures
- Colours
- Transparency
- Depth
- Lighting
- HUD
- Menus
- Video output
- Resolution / viewport handling

Performance is secondary during this phase.

---

### Phase 2 — Gameplay

Once rendering is correct:

- Input
- Race logic validation
- Menus
- Game progression
- Audio
- Save data
- Timing
- Stability

---

### Phase 3 — Optimisation

After the game is functionally correct:

- GPU optimisation
- CPU optimisation
- Memory usage
- Texture optimisation
- Loading times
- Frame pacing

Target:

**Stable and playable 30 FPS whenever possible.**

---

### Phase 4 — Multiplayer

Multiplayer will only be investigated after the main game is stable.

Possible targets include:

- 2-player Vita testing
- Local network multiplayer
- Investigation of the original game's multiplayer implementation
- Higher player counts if technically practical

---

## Installation

Installation instructions are intentionally incomplete while the project remains under heavy development.

Future builds will require:

1. A modified PlayStation Vita capable of running homebrew.
2. The `Ignition-Vita` VPK.
3. Files from a legally obtained copy of **Ignition (1997)**.

Exact file paths and required game data will be documented when the project reaches a usable state.

---

## Original Game Files

**No copyrighted game data is included in this repository.**

This repository must not contain:

- Original Ignition executables
- Game data
- Music
- Videos
- Textures
- CD images
- ISOs
- Commercial assets

Users will be required to provide files from their own legally obtained copy of the original game.

---

## Development Hardware

Primary testing is performed on:

- PlayStation Vita 1000
- Real Vita hardware
- Native Vita resolution: `960x544`

Testing on real hardware is an important part of the development process.

---

## Credits

### Project Direction & Real Hardware Testing

**Zeus**

Project supervision, testing, decision making and validation on real PlayStation Vita hardware.

### Architecture, Research & Technical Direction

**ChatGPT**

Technical analysis, architecture planning, debugging strategy, research coordination and development guidance.

### Main AI Development Assistance

**Claude Code**

Implementation, code analysis, debugging and iterative development assistance.

### Additional Development & Code Review

**OpenAI Codex**

Additional implementation assistance, independent code review, debugging and experimentation.

---

### Development workflow

Architecture, research and technical direction assisted by **ChatGPT**.

Implementation and iterative development assisted by **Claude Code**.

Additional development, experimentation and code review assisted by **OpenAI Codex**.

All project decisions, hardware testing and final validation are performed under the supervision of **Zeus** using real PlayStation Vita hardware.

> No AI was harmed during the making of this port.
>
> Claude may disagree.

---

## Inspiration / References

The project studies techniques used by other homebrew ports and compatibility projects in order to better understand how legacy PC software can be adapted to PlayStation Vita hardware.

Projects used as technical references may be listed here as development progresses.

---

## Disclaimer

This is an unofficial fan project.

This project is not affiliated with or endorsed by the original developers, publishers, Sony Interactive Entertainment or any rights holders associated with Ignition.

All trademarks and copyrighted material belong to their respective owners.

The project does not distribute copyrighted game files.

---

## License

The license for the original code created specifically for this project will be decided before the repository becomes public.

Third-party code retains its original licenses.

---

## Development Status

🚧 **EARLY WORK IN PROGRESS**

Do not expect normal gameplay yet.

Current objective:

### Get Ignition rendering correctly on PlayStation Vita.

---

### Ignition-Vita

**Because apparently running a 1997 PC racing game on a PlayStation Vita sounded like a reasonable idea.**
