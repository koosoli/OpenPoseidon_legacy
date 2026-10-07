# Included assets and test data

The source release contains engine source and project runtime assets. Retail
game archives, models, animations, sounds, maps and external SDK binaries are
not bundled. A compatible, separately obtained game-data installation is needed.

| Included group | Purpose and provenance |
| --- | --- |
| `assets/inventory/` | 17 original equipment illustrations and converted runtime PAAs. Written-description AI generation, original PNGs, prompts and hashes are recorded in `content/inventory-icons/`. Supplied under the project licence. |
| `assets/ui/`, `resources/icon-*.ico` | Project logo, its runtime conversion and application icons. The release uses project artwork, replacing earlier application icons. |
| `resources/sky/` | Converted Yale Bright Star Catalogue and NASA SVS Deep Star Maps data, with sources and credits in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md#sky-data-shipped-in-resourcessky). Conversion tools are in `tools/sky/`. |
| `screenshots/` | Ten screenshots created and supplied by the project owner, plus the project logo. They document engine features; they do not grant redistribution rights to the depicted games' assets or trademarks. |
| `content/` and `dev-missions/` | Project configuration, scripts, material parameters and demonstration mission definitions. These reference separately installed game data rather than redistribute it. |
| `tests/content/sinkhole/` | Original cave/basement geometry and procedural textures, with generators and documented authorship in its README. These are project-authored demonstration models, not retail models. |
| `tests/fixtures/` | Textual parser/regression inputs and synthetic generator source. Prebuilt binary fixtures are omitted pending documented provenance. |

Prebuilt binary test fixtures are not needed to build or play the engine. Some
asset-dependent tests need separately authored local fixtures; this release does
not claim that those tests pass without their inputs. The omitted files include
PBO/P3D/PAA/PAC/RTM/WRP data, audio, images, fonts, binary config and savegame data.
The standalone sound-tool check also needs a locally authored WAV fixture.
Existing source generators can produce some synthetic inputs, but are not a
complete replacement pack. No automatic download of unreviewed assets occurs.

Generated procedural grass, water detail and default material textures are
created by engine/build code; they do not require copied retail texture packs.
Third-party source notices remain in [CREDITS.md](CREDITS.md),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and component licence files.
