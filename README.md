# SMALL PARTS

After open hack night, Moheeb and Luis stay late at HeatSync Labs to test a project: a laser cutter someone reflashed with firmware from a forum, which now has a setting called SCALE. Luis reads the label. Moheeb presses the button. Both of them end up nine centimetres tall, and five hundred freshly printed plastic army men in the print farm sit up and look around.

The way back is the main breaker panel in the machine shop, at the far end of the lab. A hackerspace at toy scale is an armoury: a rubber band repeater laser-cut from scrap plywood, a hot glue gun, a coilgun off a capacitor bank, a soldering iron, LiPo packs that puff and then pop, and a tiny drone's motors strapped on as jetpacks. Six levels across the real lab, from the laser corner to the machine shop, two bosses (the Sergeant and the General), and one friend at your shoulder who shoots back and goes down instead of dying. `docs/STORY.md` is the story bible every line follows.

A third-person (or first-person) shooter for the HeatSync Labs game station (wasmcart, Batocera) and the browser. The scene renders at 480x270 with a dither and 15 bit colour, like a console of 1997; the HUD is drawn crisp at 1280x720 on top.

## Play

Download `small-parts.html` from the latest release (or build it, below) and open it. It is one file with the game inside: double-click it, no server needed. Click the game to aim with the mouse. Radio calls and dialogue are spoken aloud.

Pick who you are on the title screen (Moheeb or Luis); the other one is your buddy, or a second player's. Hold jump in the air for the jetpack, and watch the battery icon: it recharges on the ground. Find the weapons along the route, scope the coilgun with the aim button and shoot the weak points on the bosses. Follow the orange arrow for the objective; distances are in centimetres.

| | Pad | Keyboard | Mouse |
|---|---|---|---|
| Move | left stick | WASD | |
| Aim / camera | right stick | arrows | move |
| Jump, jetpack (hold in the air) | A | Space | |
| Fire | RT | J, F or Left Ctrl | left button |
| Aim (coilgun: scope) | LT | K | right button |
| Zoom while scoped | d-pad up / down, LB / RB | R / V | wheel |
| Dodge (air dash in the air) | B | Shift | |
| Soldering iron, interact (pull the plug on a printer) | X | E | |
| Throw a LiPo | Y | Q or G | |
| Weapon | LB / RB, d-pad left / right | 1 to 4, Tab | wheel |
| First / third person | SELECT, R3 | C | |
| Pause | START | Enter, Esc or P | |

The controls are drawn on the level card at the start of each level and in the pause menu, with the pad and keyboard layouts side by side; level 1 also walks you through move, look, jump, jet, fire and aim as you play. When you stand near something to use, a big prompt with the X or E button appears under the crosshair.

Menus: stick, d-pad or arrows; A or Enter picks, B or Esc backs out. Hold START or Enter to skip a cutscene.

Two players: plug in a second pad and press START (or A) on it, during play or on the title, and player 2 takes over the buddy; the screen splits, player 1 on the left with the keyboard and the first pad, player 2 on the right with the same pad layout. The title's PLAYERS row picks 1 or 2 for the next level. A player who goes down gets up again at the other's side after six seconds; both down at once is back to the checkpoint. Player 2 leaves with LEAVE CO-OP in the pause menu (or by unplugging), and the buddy takes back over.

On the station, copy `build/small-parts.wasc` to `/userdata/roms/wasmcart/` (back up `gamelist.xml` first if you add an entry).

## Build

```sh
./build.sh                 # build/small-parts.wasc and web/small-parts.html
OUT=build-x ./build.sh     # a private build tree (no web page)
REGEN_STATES=1 ./build.sh  # after adding states to build/missing.txt
```

Needs emsdk and Node 22. Clone with `--recursive` (or the first build fetches them): three.c and wasmcart are git submodules, and `patches/three.c.patch` is applied to three.c on the first build.

`tools/shot.mjs` runs the cart headless with scripted input and writes screenshots. If the cart logs a line like `[cart] standard+vcol+fog+flat+ds+l1001`, that shader state is missing from the program table: add it to a file and run `tools/regen.sh <file>`. `EXTRA_CFLAGS=-DDS_TRACE` logs the player and draw counts once a second.

`docs/DESIGN.md` has the units, the controls, the weapons, the army, the levels and which source file owns what. `tools/prep-lab.mjs` builds the lab from the HSL-108 model.

## Credits

- Moheeb and Luis: the character models from COFFEE & KILL.
- Army men: Quaternius's CC0 soldier, in moulded plastic.
- The lab: the HSL-108 model of HeatSync Labs, 108 West Main, Mesa, with its GPL-3.0 printer parts removed.
- Fonts: Archivo, IBM Plex Mono and JetBrains Mono, under the SIL Open Font License (`assets/fonts/OFL.txt`).
- Voices: Moheeb and Luis speak in their own voices, cloned from short recordings of each of them with Kyutai Pocket TTS (weights under CC BY 4.0, kyutai/pocket-tts) and pregenerated by `tools/voices.mjs` into `assets/voice/voices.bin`; the Sergeant and the General use Pocket TTS's built-in voices. The recordings themselves are not in this repository. The army and the alerts are CMU Flite (BSD-style licence, `third_party/flite/COPYING`) compiled into the cart.
- canvas_ity by Andrew Kensler (ISC licence), for the HUD.
- Code, music and sound: original, written for this game, under the MIT licence (`LICENSE`).
