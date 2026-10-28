# Sakura Wars overlay assets

Place the supplied portrait at `gpu/assets/sakura_wars_overlay.png` and these
short 48 kHz PCM WAV cues in `gpu/assets/sounds/`:

- `open.wav`: opening the settings menu and confirming a selection
- `close.wav`: closing the settings menu or cancelling
- `scroll.wav`: menu navigation, including controller and held-key repeats

`scripts/ps4rt_launch.py` detects these assets at startup. If artwork is absent,
the menu remains usable with its pink/plum theme. If sound files are missing,
SDL3 player generates synthetic fallback cues. Sound playback
can be disabled with `PS4RT_MENU_SOUNDS=0` or adjusted with
`PS4RT_MENU_SOUND_VOLUME=0..100`. Override asset paths using
`PS4RT_OVERLAY_BACKGROUND` and `PS4RT_MENU_SOUND_DIR`.

**Redistribution:** These particular assets originate from the commercial game
or its promotional imagery. The cleanup repository is private; do not copy
these assets into a public release without permission or a redistribution
rights review. The source code may be distributed separately.

No setting, save directory, controller binding or game state is modified by
this asset setup.
