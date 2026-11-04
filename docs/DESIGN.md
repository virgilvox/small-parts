# SMALL PARTS design

A third-person (or first-person, SELECT / C swaps) shooter for the HeatSync Labs game station (wasmcart, Batocera) and the browser. Moheeb and Luis, shrunk to nine centimetres, fight five hundred plastic army men across the real HeatSync Labs, built from the HSL-108 model. PS1 look: the scene renders at 480x270 and is drawn up with nearest sampling, an ordered dither and 15 bit colour (`present_retro`); flat shaded, fog, no shadows. The HUD is drawn at 1280x720 over it.

`docs/STORY.md` is the story bible. Every line of dialogue follows it.

## Scale and units

- 1 m of the lab is 20 game units (`LAB_SCALE`). A hero is 1.8 units (9 cm), an army man about 1.5.
- The lab: x 0 (the street, front windows) to 808 (the back: kitchen, metal fab bay), z 0 (west wall: printers, sewing, member storage, lasers) to 188 (east wall: coworking, electronics, woodshop, machine shop), floor y 0, ceiling about 60. Tables are 15 units high, the laser lid 19.4, shelves up to 40.
- Gravity 24 units/s². Run 7 units/s. A table is a jetpack hop; the top of a shelf is a full battery.
- `src/gen/lab_areas.h` (made by `tools/prep-lab.mjs`) has the model's 29 area boxes (`LAB_AREAS`, `AREA_*`) and landmark boxes (`LM_*`): `LM_LASER_LID`, `LM_SCRAP_BIN`, `LM_SCRAP_SHELF`, `LM_MEMBER_BOX` (58 banker boxes), `LM_SEWING_TABLE`, `LM_THREAD_RACK`, `LM_LEGO_TOWER`, `LM_CRAFT_TABLE`, `LM_PRINTER_RACK`, `LM_PRINTER` (the four print farm printers, boxed), `LM_COWORK_TABLE`, `LM_SOFA_SEAT`, `LM_COFFEE_TABLE`, `LM_ESD_MAT` (electronics benches), `LM_SOLDER_STATION`, `LM_MCU_CABINET`, `LM_WOOD_BENCH`, `LM_BACK_BENCH`, `LM_LATHE_BED`, `LM_PANEL` (the breaker panel, on the east wall of the machine shop, y 29 to 44), `LM_COUNTER_DRONE`. Use these for every placement; never hand-type a coordinate that a landmark gives.
- `world_floor_y`, `world_raycast`, `world_visible`, `world_move`, `world_push_sphere` collide against every triangle of the lab.

## Controls

| | Pad | Keyboard | Mouse (web page) |
|---|---|---|---|
| Move | left stick | WASD | |
| Aim / camera | right stick | arrows | move (pointer lock) |
| Jump, jetpack (hold in the air) | A | Space | |
| Fire | RT | J, F, Left Ctrl | left button |
| Aim (coilgun: scope) | LT | K | right button |
| Zoom while scoped | d-pad up / down, LB / RB | R / V | wheel |
| Dodge (air dash in the air) | B | Shift | |
| Soldering iron | X | E | |
| Throw a LiPo | Y | Q, G | |
| Weapon | LB / RB, d-pad left / right | 1 to 4, Tab | wheel |
| First / third person | SELECT, R3 | C | |
| Pause | START | Enter, Esc, P | |

Menus: stick, d-pad or arrows; A or Enter picks, B or Esc backs out; no menu wraps.

Two players (drop-in co-op, `input.c`): player 1 is the keyboard and the first pad; player 2 is the second pad there (the first that is not player 1's), with the same pad layout. With one player any pad drives player 1, as before. START or A on a second pad joins: in play player 2 takes the other hero from the buddy where it stands, and on the title it sets PLAYERS to 2 (the title's SETTINGS panel has a PLAYERS 1 / 2 row; levels then start with two). While a second pad is there and nobody has joined, the HUD shows P2 PRESS START TO JOIN. Either pad pauses and works the menus. Player 2 leaves from the pause menu (LEAVE CO-OP, only with two players), or by unplugging; the buddy takes the hero back. A pad counts as there when the host flags it connected, or, for a host that flags only port 0, once it has pressed a button.

## The player

- 100 HP. The jetpack battery: about three seconds of thrust from full, recharges on the ground after a short delay. Thrust climbs at 16 units/s² net; air control is better while jetting.
- The dodge is 0.28 s of i-frames; in the air it is an air dash that costs battery.
- The buddy (whichever hero the player did not pick) follows at the player's left shoulder, jets after them, shoots the nearest army man in sight (7 damage, rubber bands), and goes down for six seconds instead of dying.
- Two players: player 2 drives the other hero by exactly the player's rules (movement, the jetpack, the dodge, the aim layer, their own camera with first person on SELECT, the coilgun's scope and zoom, every weapon, LiPos, the iron and interact, the army's damage). Each player has their own kit (weapon in hand, weapons found, ammo, LiPos, the trigger and the swing; `weapon_set_player`); weapons found and pickups go to both, a fresh battery to whoever walked through it. A player who runs out of HP is DOWN for six seconds on the floor, then gets up at the other's side with 60 HP; both down at once is the death screen and back to the checkpoint. Pickups, checkpoints, story triggers, objectives (plugs, props, crates, the charger hold) and each level's end go off for either player (`mission_set_second`). The game state keeps one `player_t` per hero (`PL[2]` in game.c): position, velocity, facing, the jetpack, the timers, HP, and the camera's yaw, pitch, fov, shake, view, scope and zoom.
- Split screen: with two players the scene draws twice into the 480x270 target, player 1 in the left half and player 2 in the right, each 240x270 through its own camera (viewport and scissor, so the second render clears only its own half; `render_scene_split`), then the one PS1 pass. Particles are turned to each camera before its half (`fx_view`) and aged once after (`fx_age`). Each half has its own crosshair, health, battery, weapon and ammo, hit marker, hurt flash, interact prompt, scope and DOWN card; the objective, banners, the boss bar, the radio and the level card stay shared and centred.

## Weapons (weapon.c)

Everything is something you could make in a hackerspace, at toy scale.

| Weapon | Fire | Damage | Ammo | Notes |
|---|---|---|---|---|
| RUBBER BAND REPEATER | semi auto, 5 a second, a fast projectile with a little drop | 14 | endless | the start weapon, laser-cut plywood |
| HOT GLUE GUN | a lobbed glob, 3 a second | 10 on hit, then 6 a second for 2 s | glue sticks, 40 | slows to 40 percent; a puddle where it lands slows anything walking through for 6 s |
| COILGUN | hold to charge (0.8 s), release | 40 to 120 by charge, pierces up to 3 | charge cells, 10 | LT is the scope (3 zoom steps); double on weak points |
| SOLDERING IRON | X / E, a swing | 55, melts | endless | melee, 2 units of reach, a 90 degree arc |
| LiPo pack | Y / Q, thrown | 60 in a 4 unit blast | 3 | puffs for 1.2 s, then pops |

Pickups (mission.c places them, game.c and weapon.c apply them): electrical tape (35 HP to both heroes), a fresh battery (a full jetpack for whoever takes it), glue sticks (+15), charge cells (+4), a LiPo (+1); with two players the ammo and LiPos go to both kits.

## The army (enemy.c)

Quaternius's CC0 soldier (`assets/models/soldier.glb`, one mesh, 3k triangles, clips Idle, Walk, Shoot_OneHanded, RecieveHit, Defeat, Victory, Punch, StandUp, PickUp, SitDown, Walk_Carry) in moulded plastic: green `C_ARMY`, tan `C_TAN`, a little gloss. A procedural rifle in its hands. Army men are 1.5 units tall.

| Type | HP | Behaviour | Attack |
|---|---|---|---|
| RIFLE (green) | 30 | squads of 3 to 5 march in step on a patrol, spot within 40 units with sight, take cover behind nearby furniture, advance | a pellet every 1.2 s, 6 damage, visible projectile |
| GRENADIER (green) | 30 | keeps 15 to 25 units away | lobs a tiny grenade every 3 s, 15 in a 3 unit blast, a fuse spark warns |
| GUNNER (tan) | 70 | dug in on high ground (a shelf, a stack of member boxes, behind a thread spool), turns to track | bursts of 6 pellets at 3 damage, then a 2 s reload |
| PARA (green) | 30 | drifts down from height under a plastic parachute (0.5 units/s down), then fights as a rifleman | as RIFLE |
| VETERAN (tan) | 60 | flanks, faster, dodges sideways when aimed at | a pellet every 0.9 s, 8 damage |
| THE SERGEANT | 900 | level 5's mid boss: tan, twice the size, a megaphone; calls in squads and paratroopers, charges | a three-pellet spread, a megaphone shout that knocks back |
| THE GENERAL | 2400 | the final boss: the 12-inch display figure (the soldier at 8x, olive drab, a pull-string box on the back). Three phases | stomps (shockwave), a sweeping pellet stream, calls squads; weak points: the pull-string box, the battery hatch, both knee joints |

Down, an army man plays Defeat and lies still for a few seconds, then is removed with a small puff of plastic dust; the soldering iron leaves him melted (squashed scale, a darker tint). No gore. Keep at most 40 alive at once (the mission streams the rest in), and the whole army's draw calls under 60.

## Levels (mission.c)

One continuous lab; each level is a route through real areas, with a start, checkpoints, objectives and an exit trigger. Areas by name are in `LAB_AREAS`.

1. **LASER CORNER** (areas: laser corner). Start on the laser lid (`LM_LASER_LID`). Tutorial prompts: move, jump, jetpack, fire. The rubber band repeater is found at the scrap bin (`LM_SCRAP_BIN`). Two small squads. Exit: the aisle toward member storage (x below 470, z 80 to 110).
2. **MEMBER STORAGE** (member storage and jewelry, sewing, sewing divider). Gunners on the banker boxes, a rifle squad in the aisle, paratroopers off the shelves. The hot glue gun is in a member's bin (one `LM_MEMBER_BOX` top). Exit: past the sewing table toward the art and LEGO wall.
3. **PRINT FARM** (3D printing, art LEGO and soda wall, craft tables, welcome). Objective: PULL THE PLUGS. Each of the four `LM_PRINTER`s has a coloured light and a plug of the same colour in a power strip on the floor at the rack's foot (`LM_PRINTER_RACK[0]`); within 3 units of a plug (or 3.5 of a printer's front, on its shelf) the HUD prompt reads PULL THE PLUG and X / E (`mission_interact`) yanks it: the plug arcs out, the screen goes dark, PRINT CANCELLED. Shooting still works (150 damage each, `mission_hit`; a LiPo beside one cancels it). An arrow bobs over each remaining plug. Veterans on the LEGO towers. Exit: out to the front windows (x below about 122, z 34 to 92) once all four are done.
4. **WINDOW DISPLAYS** (window display: gallery bay, robot counter, hacker bay). Objective: FIND THE SPARE PROPS, four drone props: on a gallery tier (`LM_GALLERY_TIER`), by the LED cube (`LM_LED_CUBE`), on the hacker bay deck behind the duck battalion (`LM_HACKER_DECK`, `LM_DUCK_RISER`), by the counter drone (`LM_COUNTER_DRONE`). Exit: past the robot counter (`LM_ROBOT_COUNTER`) back toward coworking.
5. **COWORKING** (coworking, lounge, AV station, music station). Under the tables, across the chairs; the army holds the sofa (`LM_SOFA_SEAT`) with gunners. The coilgun is on the electronics side at the end (`LM_ESD_MAT`). Exit: 2.5 s after the coilgun is taken.
6. **WOODSHOP** (electronics, soldering station, microcontroller alcove, electronics and woodshop divide, woodshop). The soldering iron at the soldering station (`LM_SOLDER_STATION`). THE SERGEANT on the eight foot bench (`LM_WOOD_BENCH`): entering the woodshop starts his reveal (`boss_arena`). Exit, once he is down: past the woodshop's back wall into the back hall (the machine shop door is blocked).
7. **KITCHEN AND DARKROOM** (the back hall, supply closet, biolab, `LM_BASEMENT`, kitchen and darkroom). Three steps: DOWN THE HATCH FOR THE PACK (the charger's battery pack lies on the basement dirt; the hatch is in the supply closet floor, through the door propped open; the way in is the gap the shoved plate leaves at the near end of `LM_HATCH`), CLIMB OUT THE HATCH (up the steel shelving dumped under the plate, shelf by shelf, then a jump and the jetpack through the gap), PLUG IN THE CHARGER on the kitchen table (`LM_KITCHEN_TABLE`, X / E) and HOLD THE KITCHEN: the charge runs only while the player is within 18 units of the table, 60 s in all, with a wave at each third. Exit: full charge.
8. **METAL FAB BAY** (metal fab bay). Objective: WRECK THE SUPPLY DUMP, five crate stacks around the weld table (`LM_WELD_TABLE`) and the anvil (`LM_ANVIL`), 90 damage each or a LiPo beside one (`mission_hit`, `mission_raycast`). Exit: the connecting door west into the machine shop.
9. **MACHINE SHOP** (machine shop), entered from the metal fab bay. THE GENERAL on the clear floor in front of the lathe (`LM_LATHE_BED`); his reveal once the player is 22 units in. Once he is down, the objective is the breaker panel (`LM_PANEL`, jetpack up to it): `done` when the player reaches it.

Each level has 6 to 10 walkie beats (STORY.md), 2 to 4 checkpoints, pickups along the route and 20 to 45 army men over its length; squads stream in by zone or by the objective's count, at most 40 alive. Weapons found: the repeater (level 1), the glue gun (2), the coilgun (5), the soldering iron (6); `mission_weapons_at_start` gives each level the ones found before it.

## Code layout and ownership

| File | Owns |
|---|---|
| `src/main.c`, `src/present.c` | wasmcart glue, the 480x270 target (two halves for split screen) and the PS1 pass |
| `src/game.c`, `game.h` | the state machine, the players (`PL[2]`), the buddy, the cameras, scope, joining and leaving, the glue between modules |
| `src/input.c`, `input.h` | each player's intent from the pads and the keys, the second pad's join |
| `src/world.c`, `world.h` | the lab mesh and its collision |
| `src/hero.c`, `hero.h` | Moheeb and Luis: models, clips (anim.c), the aim layer, weapon props, the jetpack |
| `src/weapon.c`, `weapon.h` | each player's kit, shots, glue puddles, LiPos |
| `src/enemy.c`, `enemy.h` | the army and both bosses |
| `src/mission.c`, `mission.h` | the six levels: starts, squads, pickups, objectives, walkie beats, exits |
| `src/cutscene.c`, `cutscene.h` | the cutscenes and their staging |
| `src/hud*.c`, `hud.h` | the HUD and every screen |
| `src/audio.c`, `audio.h` | the synth: effects and music moods |
| `src/voice.c`, `src/tts.c` | speech: the recorded bank (`assets/voice/voices.bin` from `tools/voices.mjs` and `tools/voices_kyutai.py`: Kyutai Pocket TTS cloning Moheeb and Luis from recordings of their own voices, the bosses on its built-in voices) and CMU Flite in the cart for the army and alerts; coloured per speaker |
| `src/fx.c`, `fx.h` | particles, tracers, puffs, explosions |
| `web/index.src.html` | the one-file web player |
| `tools/prep-lab.mjs` | the lab from the HSL-108 model (drops its GPL printer parts) |

## Working in this tree

- Build: `./build.sh` (or `OUT=build-x ./build.sh` for your own tree, so parallel builds do not collide). The web player is only made for `OUT=build`.
- Run headless: `node tools/shot.mjs <cart> <out prefix> <frames> [script.json]` (keys and pads in the script; see the top of the file). Look at the PNGs.
- A line like `[cart] standard+vcol+fog+flat+ds+l1001` is a shader state the table lacks; collect them in a file and run `tools/regen.sh <file> [your OUT dir]` (it takes a lock and rebuilds).
- `EXTRA_CFLAGS=-DDS_TRACE` logs the player and draw counts once a second (and player 2 with two players). Its test keys: 8 heals, 7, 6 and 5 move player 1 to the basement hatch; with two players 9 knocks player 2 down, 0 player 1, and 4 puts player 2 on the objective arrow.
- Two players in the rig: script entries with `"pad": 1` drive the second pad (START joins); `"connected": false` unplugs a pad over a range. The rig passes all four pads connected unless told otherwise, so one-player runs show the join hint; `STATION=1` flags only pad 0, as the station's core may.
- Style: C11, the comment density and naming of the surrounding code; no emdashes, no double dashes, no emoji in code, comments or strings; no real brand names in the game.
