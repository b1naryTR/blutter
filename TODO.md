# TODO

Working list for the `gplayscan` fork. Tick items in the commit that finishes them.

## Dart 3.13 support (branch `dart-3.13`)

- [x] Load single-snapshot apps (`_kDartSnapshotData`/`_kDartSnapshotText`, no VM snapshot)
- [x] Object store stubs merged into `VM_STUB_CODE_LIST` (served from `dart::Roots`)
- [x] Variable-length `Closure` (context slot depends on type-argument elements)
- [x] Null result/parameter types in kept AOT signatures map to `dynamic`
- [x] Validated: MediaMarkt TR (Dart 3.13.7) 0 analysis errors; Dart 3.9.2 output identical to `c5e5749` (948/948 files, ASLR addresses masked)
- [ ] Rebuild the `gplayscan-blutter` image with `BLUTTER_COMMIT` set to this branch (Dockerfile lives in the gplayscan repo)
- [ ] Merge `dart-3.13` into `gplayscan` once the image is validated
- [ ] Generic closures: the ArgumentsDescriptor type-args check that comes before the delayed type arguments test (`ldur w?, [x4, #0xf]; cbnz ...`) is not absorbed into `SetupParameters`, so the delayed-TA prologue shows as raw `LoadField`. This also happens on Dart 3.9. On 3.13 the compiler emits it before every generic closure (24 of 10,540 closures in MediaMarkt TR). Output stays correct.
- [ ] Untested with the port: Dart 3.12.x, Shorebird engines on 3.13, x64 targets
