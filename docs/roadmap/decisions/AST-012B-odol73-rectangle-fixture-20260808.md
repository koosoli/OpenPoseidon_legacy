# AST-012B — selected ODOL 73 fixture

- **Ticket:** AST-012B
- **Revision:** 73 (Arma 3)
- **Archive:** `a3-game:Addons/3den.pbo@e1ea4026a9d18d9b`
- **Entry:** `objects\\rectangle.p3d`
- **Extracted size:** 2,593 bytes
- **SHA-256:** `774a780db8209c556033f16d5b267522aa789776cc80e8d10b5c6b771adde54a`

This is the first fixture-led later-ODOL target. It is deliberately small and
real: the owner-provided Arma 3 archive contains the exact `ODOL` signature and
little-endian revision 73 at offsets 0 and 4. It is therefore suitable for
establishing the revision-73 ModelInfo and LOD-directory boundary before any
larger geometry or material model is attempted.

This selection does not claim that the file, or revision 73 generally, parses
yet. AST-012B must record the exact fields it implements, send output through
the existing common Model IR path, and keep every unparsed section explicit.
Revisions 48, 49, 50 and 52 remain negative-dispatch controls.
