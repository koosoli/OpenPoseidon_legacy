param([Parameter(Mandatory)][Diagnostics.Process]$Process,
      [Parameter(Mandatory)][string]$ExpectedExe,
      [Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Windows minidump only' }
if ($Process.HasExited) { throw 'The owned process already exited' }
$expected = [IO.Path]::GetFullPath($ExpectedExe)
if (![string]::Equals($Process.Path, $expected, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Owned-process executable mismatch; refusing dump'
}
if (!('PoseidonOwnedMinidump' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class PoseidonOwnedMinidump {
    [DllImport("dbghelp.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MiniDumpWriteDump(IntPtr process, uint processId,
        SafeFileHandle file, uint type, IntPtr exception, IntPtr streams, IntPtr callback);
}
'@
}
# Thread contexts/stacks and module metadata, NOT the full game heap.
# CreateNew preserves earlier evidence instead of overwriting an existing file.
$file = [IO.File]::Open([IO.Path]::GetFullPath($Path), [IO.FileMode]::CreateNew,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    if (![PoseidonOwnedMinidump]::MiniDumpWriteDump($Process.Handle, [uint32]$Process.Id,
            $file.SafeFileHandle, 0, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw "MiniDumpWriteDump failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
}
finally { $file.Dispose() }
Write-Output "Owned-process minidump saved: $Path"
