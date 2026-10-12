# From Open Malprave's THIRD_PARTY_NOTICES.md

The rows of the dependency table that came with the audio backend:

| Library | Licence | Copyright | Source |
|---|---|---|---|
| SDL3 | Zlib | © 1997–2025 Sam Lantinga | <https://libsdl.org> |
| Steam Audio *(4.8.1)* | Apache-2.0 (see [§2.5](#25-steam-audio-and-its-bundled-data)) | © 2017–2024 Valve Corporation | <https://github.com/ValveSoftware/steam-audio> |
| flatbuffers | Apache-2.0 | © Google Inc. | <https://github.com/google/flatbuffers> |
| PFFFT | BSD-style (FFTPACK) | © 2013 Julien Pommier et al. | <https://github.com/marton78/pffft> |
| libmysofa | BSD-3-Clause | © 2016–2017 Symonics GmbH, Christian Hoene | <https://github.com/hoene/libmysofa> |
| dr_libs (dr_wav, dr_flac, dr_mp3) | Unlicense **OR** MIT-0 | David Reid | <https://github.com/mackron/dr_libs> |
Applies to **CLI11, Opus, libogg, libvorbis, zstd, libmysofa**, each with its own copyright holder as
- **flatbuffers**, **Google Spherical Harmonics library** — Apache-2.0 ([§2.8](#28-apache-license-20)).
- **libmysofa** — BSD-3-Clause ([§2.2](#22-bsd-3-clause-license)), © 2016–2017 Symonics GmbH, Christian Hoene.
- **PFFFT** — FFTPACK-derived BSD-style license, © 2013 Julien Pommier, © 2019 Hayati Ayguen,
## 2.7 The Unlicense (stb, dr_libs — public-domain option)
**stb** is offered under MIT (see [§2.1](#21-mit-license)) **or** the public-domain Unlicense, and **dr_libs** under MIT-0 **or** the Unlicense,
Applies to Steam Audio, flatbuffers and the Google Spherical Harmonics library (see [§2.5](#25-steam-audio-and-its-bundled-data)).

## 2.5 Steam Audio and its bundled data

Steam Audio (© 2017–2024 Valve Corporation) is licensed under the **Apache License
2.0** ([§2.8](#28-apache-license-20)). It is linked statically; its own `LICENSE.md` and
`THIRDPARTY.md` ship next to the game binaries as `SteamAudio.LICENSE.txt`. In this
build only the following third-party parts of Steam Audio are used (IPP, FFTS, Embree,
Radeon Rays and TrueAudio Next are disabled):

- **flatbuffers**, **Google Spherical Harmonics library** — Apache-2.0 ([§2.8](#28-apache-license-20)).
- **libmysofa** — BSD-3-Clause ([§2.2](#22-bsd-3-clause-license)), © 2016–2017 Symonics GmbH, Christian Hoene.
- **PFFFT** — FFTPACK-derived BSD-style license, © 2013 Julien Pommier, © 2019 Hayati Ayguen,
  © 2020 Dario Mambro, © 2004 the University Corporation for Atmospheric Research (UCAR).
- **CIPIC HRTF Database** — the head-related transfer functions used for headphone
  (binaural) output. Required notice:

```
Copyright (c) 2001 The Regents of the University of California. All Rights Reserved

This work was supported by the National Science Foundation under Grants
IRI-9619339 and ITR-0086075.
```

  CIPIC — Center for Image Processing and Integrated Computing, University of
  California, Davis.

