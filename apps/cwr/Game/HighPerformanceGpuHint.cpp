// On Windows laptops with switchable graphics the OS/driver may run the game on
// the integrated GPU, tanking performance. Exporting these two symbols from the
// executable tells NVIDIA Optimus and AMD switchable graphics to prefer the
// discrete high-performance GPU. The vendor contract requires the symbols in the
// EXE's own export table, so this translation unit is compiled directly into each
// visual app (PoseidonGame, PoseidonGameDemo, PoseidonTetris) — not into a shared
// library or an unreferenced static archive, where the export could be stripped.
//
// Verify after a Windows build with:
//   llvm-readobj --coff-exports build/<preset>/.../OpenPoseidon.exe
//   dumpbin /exports OpenPoseidon.exe
// Both names must appear; Tetris links with /OPT:REF /OPT:ICF, so check it too.
#ifdef _WIN32
#include <cstdint>

extern "C"
{
    // Low bit set = request the high-performance GPU. The driver reads these by
    // name at process start; the value is a 32-bit word (vendor docs call it
    // DWORD — uint32_t is the same 4 bytes).
    __declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 0x00000001;
}
#endif
