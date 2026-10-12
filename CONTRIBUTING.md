# Contributing

Thank you for your interest in Ignition Vita.

## Ground rules

- **No game data.** Never add, attach or link the original game's executable, data files, music, videos, models, textures or anything extracted from a commercial installation. Pull requests or issues that include them will be rejected.
- Keep personal paths, tokens and credentials out of commits.
- This is an unofficial, non-profit fan project. Be kind to other contributors and users.

## Reporting bugs

Use the **Bug report** issue template and read `docs/TROUBLESHOOTING.md` first. Please attach `boot.txt`, `ignitionvita.log` and `ignition_critical.log` (from `ux0:data/ignition/logs/`) and tell us your console model, firmware, plugins, clock frequencies, game mode / track / car and the steps to reproduce. Issues without enough information may need clarifications.

## Building and testing

See `docs/BUILDING.md`. A change that touches the rendering, input, audio or menu code should be checked on a real console: the emulated environment of a PC cannot show most of the problems of this port. Include the relevant lines of the log in your pull request.

## Code notes

- `source/` is the port; `external/WinVita/` is a vendored third-party engine (Franck Roustan, MIT). Changes that belong to the engine should preferably be proposed upstream (<https://github.com/Franckrst/WinVita>); say so in your pull request if you modify the vendored copy.
- Many comments in the sources explain *why* a decision was taken and cite the evidence (disassembly addresses, real-console logs). Please keep that habit: state what you verified and how.
- Do not add per-frame logging; the logger has an allowlist and caps by design.
- New third-party code needs its license in `THIRD_PARTY_NOTICES.md` and `licenses/`.

## Licensing of contributions

By submitting a contribution you agree that it is released under the license of the part of the repository it modifies: MIT for the original code (`LICENSE`), LGPL-2.1-only for `source/win32/glide2x_ign.cpp` and `.h`, and the license of the respective project for anything under `external/` and `third_party/` (see `THIRD_PARTY_NOTICES.md`). Do not copy code from projects whose license is incompatible, and say where any borrowed code comes from.