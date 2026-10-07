# Physical inventory source receipt — 2026-10-07

Source: author-supplied **CWR-Physical Inventory**, received locally by the project owner.
Only the eight inventory/model/mass files below are adapted; the older engine snapshot,
binaries and proprietary game data are not imported. Original LICENSE SHA-256:
`ad3e819baa7a89b32204478da77adc5bb5569a4a4f846716295c92c29794d0ce`, byte-identical
to this engine licence (GPL-3.0-or-later with the same Section 7 terms).

| Original relative file | Original SHA-256 |
| --- | --- |
| `engine/Poseidon/UI/Inventory/DisplayInventory.cpp` | `ceadb88056e0114e7e214d9202da1bf447ef4f9698352926e64b26ac19a6b46a` |
| `engine/Poseidon/UI/Inventory/DisplayInventory.hpp` | `d126d75f745d30cbecb251b634769bd1765224ed8a4d9db5e26fcbe313583eb4` |
| `engine/Poseidon/UI/Inventory/InventoryModel.cpp` | `442add9642dbe96f17694f98f6c2c529ad473c857c0820e67b735b54978b97f7` |
| `engine/Poseidon/UI/Inventory/InventoryModel.hpp` | `b1c2bada72a376b5733ad59d4e0c6de5ce4d79f0360c625993c39783be669ac0` |
| `engine/Poseidon/UI/Inventory/ItemFootprint.cpp` | `9027994628f7db0b419432c0bbbdc1c1f44254ebc8fad00bf7d8f670248e2723` |
| `engine/Poseidon/UI/Inventory/ItemFootprint.hpp` | `1e82569c47953546879f56e3d572efdf60ea8d7ca4fd3490c6001dd1090f7651` |
| `engine/Poseidon/World/Entities/Weapons/ItemMass.cpp` | `9b7e8955fd481d1535b0887e5cb79de2395bf9c9fbdccb8e68d245f8288af77f` |
| `engine/Poseidon/World/Entities/Weapons/ItemMass.hpp` | `6e24cba7eaa5341fc3363998447a71823aaceb94b985264526d32cf1466d297b` |

Adaptations: current input/config migration and SP-only integration; built-in UI resource;
stock-picture/label fallback without an icon pack; user-directory settings; transfer
ownership, capacity and success-before-removal checks. Donor recoil, medical/fuel and
altered-ammunition extensions are excluded. Optional `invicons` artwork is not supplied.
