# Arma 1 / 2 / 3 compat -- coordination between the sessions

Three sessions work on playing original Arma missions straight from an installed game
folder (nothing copied into the CWA install). They share one compat layer
(`engine/Poseidon/Game/Compat/`, namespace `Poseidon::Arma1Compat`) and one game folder.

| Session | Branch | Started with | Doc |
|---|---|---|---|
| "ARMA 1 mission in Open Poseidon" | `MrDudeX-arma1` | `--arma1 <ArmA Gold>` | `docs/verification/arma1-direct-missions-20261011.md` |
| "Arma 2 Open Poseidon integration" | `MrDudeX-arma2` (merged into `MrDudeX-arma1` at `13f1773`) | `--arma2 <Arma 2>` | `docs/verification/arma2-direct-missions-20261012.md` |
| "Arma 3" (this one) | `MrDudeX-arma3`, branched from `MrDudeX-arma1` `178de3d` | `--arma3 <Arma 3>` | `docs/verification/arma3-direct-missions-20261014.md` |

## Rules for the shared layer

- `Arma1Compat::Generation()` is 1, 2 or 3. **Generation 3 is Arma 3.** A check that means
  "Arma 2 *and later*" (Dta layout, XML stringtables, BMTR animations, fibers, single-quoted
  strings, ...) is written `Generation() >= 2`; `== 2` is kept only where the behaviour is
  really Arma 2's alone. The Arma 3 branch converts the existing `== 2` checks accordingly;
  the conversion does not change anything for `--arma1` / `--arma2`.
- Arma 3 specifics live in new files (`Arma3Compat.cpp`, `Arma3Eden.cpp`, ...) so the
  three branches touch the shared files as little as possible.
- `MrDudeX-arma3` is rebased onto `MrDudeX-arma1` (and through it `MrDudeX-arma2`) when those
  move; it is never merged the other way without the owner saying so.
- One session owns the game folder at a time. Every deploy rewrites
  `Remastered\DEPLOYED-FROM.txt` (branch + commit); read it before measuring anything.
- Copy-protected or encrypted archives are never read: Arma 2's `hprotect` DLC payloads and
  Arma 3's Creator-DLC `.ebo` files (Global Mobilization, S.O.G., CSLA, Spearhead,
  Western Sahara, Expeditionary Forces, Reaction Forces) are skipped and logged.
- Clean room: only the documented data formats are read (PBO, raP, ODOL/OPRW, PAA, RTM,
  SQF/SQS text). No Bohemia executable is disassembled or decompiled, and no Bohemia data
  is committed.
