# Island Editor

A standalone editor for the islands (`.wrp`) of Cold War Assault / Operation
Flashpoint and Open Malprave. It does not need the game running, and it works on
an unmodified Cold War Assault folder as well as on Open Malprave's.

Built as `PoseidonIslandEditor.exe`; copied to the game folder as
`Island Editor.exe`.

## What it reads and writes

- **Opens** 4WVR and 3WVR islands, and the binarized OPRW v2 / v3 the game ships.
- **Saves 4WVR**, always. The games load 4WVR directly (it is what WrpTool
  saves). The file being replaced is kept beside it as `<name>.wrp.bak`.
- Sizes: square, a power of two from 32 to 1024 cells of 50 m (1.6 km to 51.2 km).
  256 cells (12.8 km) is the size of the game's own islands.
- Elevations are stored in steps of 4.5 cm, from -1474 m to +1474 m. Sea level is 0.
- At most 512 ground textures.
- Binarized islands can give several objects the same id, which 4WVR does not
  allow. Those objects get new ids when the island is opened - the same ids the
  game gives them when it loads the binarized file, so missions that name objects
  by id still find them.

Ground textures and model names are read straight from the game's archives
(`DTA`, `AddOns`, `Res`): nothing has to be unpacked. Set the folder under
**Info > Choose game folder**; an island opened from a game's `Worlds` folder
sets it by itself.

## Arma and DayZ worlds (Open Poseidon)

Besides the OFP formats, this build opens the later binarized worlds, read with the engine's own reader
(`Asset/Formats/World/Oprw25.hpp`):

| World | OPRW | Tried on |
| --- | --- | --- |
| Arma (Armed Assault) | 18, 20 | Sahrani Lite |
| Arma 2, Operation Arrowhead | 24 | Utes, Chernarus (1,055,157 objects), Takistan |
| Arma 3 | 25 | read by the same code; no Arma 3 world was at hand to try |
| DayZ | 29 | ChernarusPlus (2,971,218 objects) |

Take the `.wrp` out of its `.pbo` first (`PoseidonTools pbo extract`). Opening one is a conversion, because these
worlds are not built as an OFP island is:

* **Land.** The elevations come from the world's fine terrain grid. The editor holds at most 1024 points a side,
  so a 2048 grid is taken at every second point and the cell grows to match (Chernarus: 7.5 m becomes 15 m).
  Elevations beyond the 4WVR format's 1474 m are cut, and the status line says how many.
* **Ground.** These worlds have no ground texture per cell (their ground is satellite and mask layers), so the
  island starts untextured: use Paint or Auto paint with an OFP texture set.
* **Objects** come as they stand, with their own model paths. With the game folder set to that game's folder
  (`--game "...\Arma 2"`), their real models are drawn.
* **Saving** writes 4WVR under a name of its own, `<world>_4wvr.wrp`, never over the world it was read from.
  4WVR has no cell size, so an island that is not 50 m a cell keeps it in `<file>.cell` beside it, which the
  editor reads back.
* **Not yet playable at its own scale.** The game loads every OFP-format island at 50 m a cell
  (`SetLandGrid(50)` in the engine's `LandSave.cpp`) and has no setting for another size. A converted island
  whose cells are 10 or 15 m is right in the editor and wrong in the game until the engine reads that size.
* **Any number of objects.** The object list takes its memory from the system, not from the engine's allocator
  (which gives out at most 256 MiB in one piece), so a world of three million objects opens like any other.

## Working with it

| | |
| --- | --- |
| Right drag | look around |
| Middle drag | slide over the land |
| Wheel | closer / further (towards the cursor) |
| W A S D, Q E | fly; Shift = fast |
| Alt + wheel, `[` `]` | brush size |
| 1 .. 9, 0 | the first nine tools; 0 = Objects |
| G | cell grid |
| T | look straight down |
| H | take the height under the cursor (Set height tool) |
| Ctrl+Z / Ctrl+Y | undo / redo (200 steps) |
| Ctrl+S, Ctrl+Shift+S | save, save as |

The window: the tools at the left, grouped as Land, Ground, Build and Select,
with the settings of the tool in hand under them; the island's textures, objects,
map and facts at the right; a strip over the view for what is shown (textures /
heights / slopes, grid, sea, objects, contours, top view); the place under the
cursor in the bar at the bottom. The **View** menu has the same and the finer
settings (contour spacing, where the light comes from, how far objects are shown);
**Help > Keys and mouse** lists the keys. Hold the mouse over a tool for what it does.

Tools: **Raise**, **Lower** (Shift swaps them, Ctrl smooths), **Smooth**,
**Flatten** (to the height where the stroke began), **Set height**, **Noise**,
**Erode**, **Ramp** (press at the start, release at the end), **Paint** (ground
textures; Alt+click picks one up), **Auto paint** (textures by rule under the
brush, see below), **Road**, **Forest**, **Bridge**, **Region** (see Builders),
**Objects** (click to select, drag to move, R / Shift+R to turn, Del to remove;
pick a model in the Objects tab to place more).

Objects keep their height above the ground: shape the land and what stands on it
follows. Bridges are the exception: they keep their height above the sea.

**Edit > Whole island** smooths, scales or offsets every elevation;
**Edit > Generate land** makes hills and a coast from a seed.

### Land from a height map

**File > New > A height map** makes a new island from a picture or grid of
elevations; **File > Import height map** puts one over the island that is open
(undoable). Read: PNG (8 or 16 bit; colour pictures by brightness), PGM, RAW /
R16 (square, 16 bit), ESRI ASCII grids (`.asc`, already in metres), BMP, TGA, JPG.
The picture is stretched over the whole island, north at the top; you say what
height black and white stand for. 8-bit pictures have only 256 heights, so they
are smoothed a little by default. **File > Export height map** writes a 16-bit PGM.

### Auto texture

**Edit > Auto texture** textures the land by rule: beach and sea bed, shore,
grass (with patches of a second grass), hills, rock on steep ground, peaks, and
forest floor under forest blocks. Between the kinds of ground it uses the texture
set's own transition textures, and where two kinds have no transition between
them it steps through the kinds that do, so borders come out as on the game's
own islands. Borders wander with noise rather than following contour lines.

Which texture plays which part is worked out for you: from how the island itself
uses its textures (at what heights, on what slopes, under forests); for a new
island, from the game's own island that uses the same set; failing both, from the
colours. Every choice can be changed. Apply it to the whole island, or use
**Auto paint** to brush it on where you want it.

A texture set is a folder named the game's way: two letters for a plain texture
(`pi.paa`), eight for a transition - four pairs for the cell's south-west,
south-east, north-west and north-east corners (`tntvtntn.paa`).

### Reskins

**Edit > Reskin** turns the island into another climate: Snow, Desert, Dry
season, Autumn, Lush, Mud, Scorched, Volcanic, Mars, Moon. Every ground texture
the island uses is recoloured (transitions stay right, since each is recoloured
like the plain textures it joins; roads and concrete keep more of themselves) and
written as a new texture into an addon of its own. Trees and bushes can be
thinned out in the same step. The window shows the result on the island before
anything is written.

What is made, in the folder you choose (Documents\Island Editor by default):

- `<addon>\` with the textures, the island as `<addon>.wrp` and a `config.cpp`
  that makes it an island of its own in the game, inheriting the settings of the
  world you name (Eden = Everon, Abel = Malden, Cain = Kolgujev, Noe = Nogova);
- `<addon>.pbo`, the same packed. Put it into the game's `AddOns` folder (there
  is a tick box for that); missions for it are named `name.<addon>`.

After more editing, **File > Save and pack the addon again** refreshes the `.pbo`.
The original island and the game's files are never changed.

### Builders

They place the game's own models the way the game's own islands do, so they need
the game folder. Each is one undo step.

**Road.** Pick a kind of road (asphalt, the wide road, the dirt track ... every
family of road models found in the game folder), then click along the way it
should go; the road is shown as you go. Enter or a double click lays it,
Backspace takes the last point back, Esc drops it. Straight pieces and curves are
chosen to follow your line, each joined to the last by the joining points the
models carry, so the pieces meet exactly as on the stock islands. Curves are
only as tight as the models (25 m radius at best): at a sharp corner the road
swings wide. Start near the open end of a road that is there and the new one
carries on from it, in line. End pieces close both ends if the kind has one.
Roads lie on the ground as it is: shape the land first.

**Forest.** Drag to plant forest by the cell; Shift clears it. A forest is what
the game makes of it: two half blocks to every 50 m cell, and a triangular corner
block in each open cell with forest on two neighbouring sides, turned to face
them - the edges are put right as you paint. Nothing is planted in the sea. With
"forest floor" ticked the ground under and around the stroke is textured again by
the auto-texture rules. Taking the forests of Everon, Malden and Kolgujev out and
planting the same cells again gives about 99 % of the blocks back exactly where they were.

**Bridge.** Click one bank, then the other. Sections are put in a row between
them with the roadway level, by default at the height of the first bank. Bridges
keep that height whatever is done to the land under them (any object can be made
to: "Keeps its height" in the Objects tab).

**Region.** Drag over the cells to mark a rectangle. *Export* writes its
elevations, ground textures and objects as a small island file (4WVR, so it can
be opened and edited like any island; the editor notes in it which part is the
region and where it came from). *Import* puts a region - or a whole other island
- down at the marked spot or where it came from; elevations, textures and objects
can each be left out, and what stands there can be kept or removed. Textures the
island does not have yet are added to its table. Also: remove every object in the
region.

### Objects in the view

Near the camera the objects are drawn as they are in the game: their own models
with their textures, read from the game folder - houses, trees, fences, forest
blocks, bridges, and roads lying on the ground the way the game lays them (cut
along the cells so they follow every fold of it). Forest blocks and other models
that follow the slope do so here too. Further off they are drawn as markers
coloured by kind, road pieces and bridge sections as the outline of their
roadway, forest blocks as the cell they fill.

**Models** in the strip over the view turns the models off (markers everywhere).
The View menu sets how far they are drawn (900 m to begin with) and how far in
full detail (260 m; beyond that the models' own coarsest level is used). Models
appear a moment after the camera arrives somewhere new; shaping the land or
moving, adding and removing objects updates them as you go. With Objects, a click
picks the model under the cursor, and the one selected is shown in its box.

Without a game folder, or for a model the game folder does not have, there are
only the markers. Textures are read as PAA / PAC, and as DDS for the addons that
bring theirs so; one that cannot be found is drawn grey. Building and reading are
given a few milliseconds of each frame, so arriving somewhere new does not stall
the view: the models fill in over the next moments.

Dragging an object keeps the point it was taken hold of under the cursor.

Views: textures, heights (colours by elevation), slopes (red = steep), with
optional cell grid and contour lines. Missing textures are drawn red with a cross.

The Textures tab can be narrowed by name, or to the plain textures only (without
the transitions between them).

The interface follows the screen's scale (100 %, 150 % ...); `--ui-scale 1.25`
sets another.

## Command line

```
"Island Editor.exe" [island.wrp] [--game <folder>]
    --convert out.wrp            save the island as 4WVR and exit (no window)
    --heightmap map.png          start from a height map instead of an island
        [--size N] [--low m] [--high m] [--smooth passes]
    --auto-texture [--set eden]  texture by rule
    --reskin snow --addon name --out folder [--install]
    --road x z x z ...           lay a road [--road-kind sil] [--no-caps]
    --forest x z radius          plant forest [--forest-kind jehl] [--clear-forest]
    --bridge x z x z [deck]      span a bridge, deck at this height
    --region x0 z0 x1 z1 --export-region part.wrp
    --import-region part.wrp [--at x z]
    --replant                    test: take the forests out, plant them again, compare
    --check-models [--shot prefix]  test: the editor driven by a script of mouse and key
                                 events, its window hidden - click, drag, turn, remove,
                                 undo, a stroke of Raise, placing - and what the models did
    --model-formats              how many of the game's models are binarized, how many not
    --list-models part           models by name: size, level of detail, textures (and
                                 whether each texture is found)
    --probe-roads                test: how well the island's road pieces meet
    --selftest island.wrp        load, save, reload, try every tool and undo; report
    --shot picture.png           write a picture of the window (which is not shown), then exit
        [--cam x y z yaw pitch] [--tool n] [--tab n] [--show new|auto|reskin|keys ...]
        [--models-to metres] [--select n]
    --ui-scale 1.5               size of the interface
```

## Not there yet

- Models are drawn plainly: no shadows, no see-through glass (it is cut out or
  solid), and parts that the game hides or moves (doors, damage) are all shown
  as modelled.
- Roads do not make crossings (the `kr_` models) and do not level the ground
  under them; a bridge is not joined to the road at its ends for you.
- Reskins recolour the ground only: the models (trees, houses) keep their looks.
- It does not write the binarized OPRW format. The game does not need it.
