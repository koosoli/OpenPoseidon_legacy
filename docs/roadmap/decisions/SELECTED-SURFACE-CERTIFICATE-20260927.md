# Optional offline selected-surface certificate

The original-MLOD Tools producer can now emit a separate conservative object-space surface-distance descriptor under `--surface-certificate`. The option is OFF by default. Actual OFF/ON output comparison preserves the selected disk package and manifest byte for byte. No renderer, installed-game consumer, projection policy or automatic LOD decision uses this descriptor.

## Mathematical scope and refusal contract

For every actual source triangle, the construction chooses one actual referenced vertex on the opposing triangle union. The maximum of the three corner distances to that same anchor bounds every point on the source triangle by convexity of the Euclidean norm. Applying this to every triangle in both directions gives a conservative symmetric Hausdorff upper bound between the two complete selected packed triangle unions. Unreferenced vertices cannot serve as anchors. It can be very loose, including for identical surfaces; it is not a closest-surface computation or proof of fidelity to the original unsimplified source.

The actual packed streams, external original and derived identities, pinned versions, thresholds and selected IDs are validated before construction. Trusted expected identities remain caller authority. Coordinates must be finite normal-or-zero floats within ±10,000; subnormal inputs and fast-math compilation are refused. Individually materialized IEEE binary64 operations and outward `nextafter` endpoints enclose the bounded computations. Genuine nonzero differences, squares and cross products remain normal binary64 in this float-input domain. Interval-uncertain or degenerate triangles refuse. Square-root candidates are accepted only after a downward-rounded square dominates the upward squared sum, with bounded repair attempts. This is not a promise for arbitrary floating-point environments outside those contracts.

One shared cap permits at most two million actual distance visits, including target scans and three corner checks. Results publish transactionally. No hard allocation/RSS/time bound is claimed. The certificate excludes pixels, screen-space error, UVs, normals, topology, material shading, all rendering passes, runtime source freshness and retail asset compatibility.

## Publication binding

Only a successful optional `BuildOwned` creates the private fixed-size publication seal over length-prefixed exact encoded bytes and manifest. Before creating the private output directory, optional publication checks that seal and requires canonical encoding of the current selected package to equal the encoded bytes. The descriptor includes actual `selected.gcd` length and SHA. Mutation of encoded bytes, manifest or selected threshold/key cannot silently bind a certificate to different output. This is integrity of this producer transaction, not authentication against malicious code holding arbitrary external authority.

Classic locale, `max_digits10` and exact double bit strings preserve numeric serialization. Descriptor length is bounded to 8,192 bytes. The optional third file is written before stdout; failures roll back only owned explicit files and the empty private directory. Existing output paths are refused. OFF avoids the new certificate/seal hashing work and emits the original two artifacts; the small offline Product proof fields are not zero footprint.

## Completed evidence

Source `5957e289120cb97fe65f1712d577b070fdaa4a9d` is committed and safety-verified by root. The actual Tools build carried dirty `09adb3e0`; its compiled source matches the committed source. The first helper build failed on ambiguous test names `Vertex`/`Vector3P`; the preserved failure is followed by the repaired all-five build. The helper checks passed 19,736 assertions in nine cases, and final optional CLI checks passed 19,916 assertions in twelve cases, without skips. The CLI build initially showed 21 scheduled steps, then reconfiguration changed the displayed schedule to eleven; this is not a fixed 21-step final count.

Actual fresh OFF/ON CLI executions exited zero. Original input is 24,136 bytes, SHA `c7c5694c700286300af559d98212017dbbc028206a5ca6c9f9af1f6905bc77f0`. Both selected packages are 70,112 bytes, SHA `b16234e1327a6acbd00b42999bf5b1d64b0aa0d5cb5b6d3efc44dd2beaaa082c`; both manifests are 1,581 bytes, SHA `d0d36ce17faf733c6e739bc1123d54ecd276b8bffb4a3eb00376a68bf7be1eb6`. OFF has no certificate. Selected cuts contain coarse64/fine512 triangles, coarse ID11/fine IDs0–5, threshold bits1035049833/0.

The descriptor records fine-to-coarse upper1.1853434305796802 (`3ff2f72aac4de409`), coarse-to-fine and symmetric upper4.0000000000000027 (`4010000000000003`), and41,600 visits under2,000,000. The large bound is intentionally conservative and is not a useful pixel-error policy by itself. Root ran the independent Python verifier successfully: full raw/file/key/layout/threshold/cut/hash/double-bit binding plus240 bounded barycentric spot samples per direction. The 8,400/82,320 sample visits are auxiliary checks, not the continuous theorem; the continuous claim follows the reviewed interval/convex construction over all triangles. Actual invalid-tail CLI exited2 with no stdout or output directory.

Installed game remains `f9e87d41`; no new game deployment or smoke is needed for this unused offline-only path. Existing render/selection gates, original source fidelity, all-pass/pixel/projection acceptance, and whole-plan performance remain OPEN. `defaultReady=false` and `performanceAccepted=false`. The later warm-material preflight candidate is not part of this checkpoint.

## Artifact manifest

The manifest includes exact committed-source snapshots and actual completed artifacts. No growing log or unexecuted candidate is recorded as passing evidence.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| build/build-selected-surface-certificate-20260927.log | 54738 | `54366E21EA5EF92D16F136672EE85602F88A2009BEA73D089A30316CCD9F60A7` |
| build/build-selected-surface-certificate-repaired-20260927.log | 19149 | `3DE1B7839F05424AC6143BD94CBD4CB492A0DCFC3B3EC8CB847A723437226912` |
| build/build-selected-surface-cli-20260927.log | 17664 | `483E09138C456003379C5D2D6370182D6B7ECD1B0382449B31FDA57470E89747` |
| build/test-selected-surface-certificate-20260927.log | 154 | `E41AAA2222AF3CB9EAC51C8922E68B400CF8337BAF66E60801A2CADC552D1090` |
| build/test-selected-surface-cli-20260927.log | 182 | `C645B8EE80F5DDBED7B71D6631EE5C8C8B5A7D763AE22D5151B6F4EE7E24C55B` |
| build/surface-certificate-input-20260927.json | 970 | `F146C0A56171157F28B75DC8897A2F1EDCE2AA2D7EF1F43218E4E74E1871AB82` |
| build/surface-certificate-original-20260927/input.p3d | 24136 | `C7C5694C700286300AF559D98212017DBBC028206A5CA6C9F9AF1F6905BC77F0` |
| build/surface-certificate-original-20260927/invalid-tail.p3d | 24137 | `74E0C6BB97CB329E15EF6150C1CB402C5DE78E7E815109A2EC8671AD040B3C71` |
| build/surface-certificate-cli-proof-20260927.json | 882 | `94C18161A587C2A0DC48B801B04479C6FEEB218431B8EAB99772E9BFCC6D2216` |
| build/verify-surface-certificate-cli.py | 10193 | `B86042E73594E42F67A8AE074E8A8CC1BF43BE5C948CEE2D3723FC10CAF1A314` |
| build/surface-certificate-cli-invalid-20260927.stdout.json | 0 | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| build/surface-certificate-cli-invalid-20260927.stderr.log | 76 | `C1A2C8F7ACE347D7DC5A217288836C41B301E37A02260C7A285E17370C7BDA66` |
| build/surface-certificate-cli-off-20260927/selected.gcd | 70112 | `B16234E1327A6ACBD00B42999BF5B1D64B0AA0D5CB5B6D3EFC44DD2BEAAA082C` |
| build/surface-certificate-cli-off-20260927/manifest.json | 1581 | `D0D36CE17FAF733C6E739BC1123D54ECD276B8BFFB4A3EB00376A68BF7BE1EB6` |
| build/surface-certificate-cli-off-20260927.stdout.json | 1584 | `9949583DD0EDF6A6D5248A58A1F42F856070A5669CF97C2A07E78CDA4C5F79CE` |
| build/surface-certificate-cli-off-20260927.stderr.log | 0 | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| build/surface-certificate-cli-on-20260927/selected.gcd | 70112 | `B16234E1327A6ACBD00B42999BF5B1D64B0AA0D5CB5B6D3EFC44DD2BEAAA082C` |
| build/surface-certificate-cli-on-20260927/manifest.json | 1581 | `D0D36CE17FAF733C6E739BC1123D54ECD276B8BFFB4A3EB00376A68BF7BE1EB6` |
| build/surface-certificate-cli-on-20260927.stdout.json | 1584 | `9949583DD0EDF6A6D5248A58A1F42F856070A5669CF97C2A07E78CDA4C5F79CE` |
| build/surface-certificate-cli-on-20260927.stderr.log | 0 | `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855` |
| build/surface-certificate-cli-on-20260927/surface-certificate.json | 1628 | `BB9131FC618ADFE2E4A76879CB7EA08BE6CB8D6C60A8D8E6C87CFB76F3099D3F` |
| build/surface-certificate-source-5957e289/apps/tools/Tools/commands/SelectedCutSurfaceCertificate.hpp | 10032 | `FA9045FAAB77555ED069817099023EFF519DACC09435308099D40089AD54BEE2` |
| build/surface-certificate-source-5957e289/apps/tools/Tools/commands/ControlledMlodPageProducer.hpp | 19348 | `32A6E4C0FCE65867B874246E6FEF478777459D2CEE9C701DE101E8C88CA3E4F7` |
| build/surface-certificate-source-5957e289/apps/tools/Tools/commands/GeometryPageCommand.cpp | 8272 | `62EA86161BF57510FAC6893001EC6FD8C7049444DD9B3F3813A855BB513447A8` |
| build/surface-certificate-source-5957e289/tests/unit/engine/Poseidon/Tools/test_selected_cut_surface_certificate.cpp | 21396 | `2211EF907E9653DBBFD3B765B67D395B4EA59B41B04AD296D880876E70F1CD5F` |
