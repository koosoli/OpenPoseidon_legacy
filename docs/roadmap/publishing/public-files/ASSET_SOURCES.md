# Test Fixture Asset Policy

Fixtures under this directory must be authored for this repository or generated
from trivial synthetic inputs. Do not copy assets from game packages, demo
packages, proprietary tools, or third-party mods unless their license is
explicitly GPLv3-compatible and documented with the fixture.

Binary fixtures may be one-off generated and committed directly when the test
needs an exact binary format. Prefer neutral fixture names that describe the
test role, such as `complex_vehicle`, `proxy_structure`, `synthetic_dxt5`, or
`test_world`.

Do not add fixture names, comments, or file contents that preserve original game
asset identities. Tests should assert parser behavior and structural properties,
not the specific counts, names, missions, or media from original content.

This source release omits prebuilt binary fixtures. Textual fixtures and source
generators remain. Asset-dependent tests require separately authored local
fixtures and are not certified by the release build check. These fixtures are
not runtime game assets.
