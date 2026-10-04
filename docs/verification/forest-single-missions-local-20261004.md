# Local Forest Single Missions installation

The owner's existing `Mods/Forest-OpenP` content is available on a normal installed-game launch. No engine change or asset upload was needed.

- Existing `Addons/forestisland.pbo` is hard-linked into the game's global `AddOns` directory, preserving the original and avoiding a second 1.8 GB copy.
- Existing beach and cave missions are copied into global `Missions/ForestFreeRoam.TheForest` and `Missions/ForestCaveRoam.TheForest`. Local Intel titles and overviews identify **Forest - Beach Free Roam** and **Forest - Cave Exploration**. Original mod missions remain intact.
- Both mission loads passed without `--mod`: source world `forestisland/theforest.wrp`, normal exit, no world-load, unhandled-exception or GPU validation failures. Beach/cave evidence: private `build/forest-single-menu/20261004-202958/receipt.json` and captures. These are runtime-load checks, not broad map visual acceptance.
- Actual SDL mouse navigation from the main menu confirms both entries in Single Missions / Einzeleinsatz: private `build/forest-single-menu/20261004-204121/menu.png`. Earlier `--start-display 2` and click fixtures did not establish menu navigation; their receipts are not accepted as menu passes.
- Press **L** for the cave flashlight.

Installed binary source remains `08ecd9a4`, stamped `2026-10-04_19:04`; executable SHA256 `C1BD7083DC7758C10D19A0A445013F88D6C32A691CE5EEC79C6EC229740C63D0`, renderer SHA256 `C4E0DA9BAFC18F8B736E4DC52793C8702F6CC68DEEC7D3DEA9B667BB7EFA7C00`. This local content installation changes neither binary nor stamp. Only this receipt and ledger metadata enter Git; no Forest archive, mission content or screenshots enter Git.
