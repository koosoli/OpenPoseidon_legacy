# Dynamic-row source witness for cached-image diagnosis (2026-09-28)

The selected-model revision used by private COUNT facts deliberately ignores
foreign dynamic instances. A soldier or vehicle with another model can move
through a cached shadow image while the selected model's COUNT inputs stay
identical. The retained-static epoch also does not advance for dynamic-tail
changes. Therefore neither witness can establish cached-image currency.

An optional, boxed `DynamicImageMutationWitness` now gives the dynamic instance
source its own nonwrapping serial. Only after an explicit diagnostic arm does
`set_dynamic` compare the complete ordered bytes of its previous and incoming
`InstanceGpu` rows. Identical sets leave the serial unchanged; any row change,
addition, removal or reorder advances it. A changed foreign row is deliberately
included even if the target-model revision is unchanged. Overflow permanently
returns Unknown instead of aliasing an old stamp. The ordinary, unarmed path
does no extra row scan, hash, allocation, GPU work or readback.

`Unchanged` establishes only that this CPU dynamic row source did not change
between stamps. It is **not** a cached-image publication certificate: retained
static geometry, mesh/section/material or texture updates, animation poses,
skin bake, other casters and GPU completion remain separate concerns. This
witness is not wired to the cached COUNT getter or image-current classifier.
Existing global epoch, publication, source, COUNT and Fine gates are unchanged;
GeometryPage stays off and physical reclamation remains forbidden.

Two focused device-free Rust tests passed. They cover unchanged rows, a
foreign-model transform, reorder, removal, unchanged target revision/global
epoch, and fail-closed serial overflow. Integrated desktop build, installed-game smoke and matched
performance verification are delegated to the integrating owner.
