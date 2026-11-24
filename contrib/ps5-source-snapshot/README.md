# PS5 runtime source snapshot

This is a source-only development snapshot of the PS5 menu, input, rendering, texture-piece and timing integration. `aica.patch` applies to `port/m1/aica.c` at Voldorium commit b889faa5da2cbf6a390e7b37faf9db0c5fb828c3. The other files are captured from the same combined runtime development work. They are not wired into this public tree as a complete PS5 build: referenced integration headers, shaders, graphics dependencies and the player-generated game module must be supplied through a separately reviewed integration. No game code, disc data, images or compiled binaries are included. Preserve the project and third-party terms in the root README. In particular, the root project describes its own code as personal, non-commercial use with a legally owned copy; game-derived translated modules must not be distributed.

This snapshot was compiled in a private combined-link probe; this export has not been independently built or tested on a PS5. It does not establish gameplay, audio, texture, or performance behavior.

On the 1.2.1 integration branch, `aica.patch` remains a historical b889faa patch; do not apply it directly to the new upstream source. A separate three-way merge with the actual r11 AICA source compiled locally, but is not yet integrated into a complete reproducible PS5 build.

The PS5 disc fallback checks `/app0/Soulcalibur (USA).gdi`, `/data/PPSA99841/Soulcalibur (USA).gdi`, and `/mnt/usb0` through `/mnt/usb7/homebrew/PPSA99841/Soulcalibur (USA).gdi`, in that order. An existing readable configured path takes precedence. GDI track files belong beside the descriptor; saves are not migrated. `test-disc-paths.c` exercises fallback paths and errors with `-I port/m1`. These paths still require console verification.
