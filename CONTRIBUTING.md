# Contributing to µFW

Contributions are welcome — issues, fixes, benchmarks, and docs alike.

## License of contributions (please read — it is the one non-negotiable)

µFW is dual-licensed (AGPL-3.0-only + commercial, see [COMMERCIAL.md](COMMERCIAL.md)).
To keep the dual offering possible, **all inbound contributions are accepted under
the Apache License 2.0**, regardless of the project's outbound license. This lets
your contribution ship in both the AGPL and the commercial editions. By submitting
a contribution you:

1. license it to the project under **Apache-2.0** (inbound), and
2. certify the [Developer Certificate of Origin](https://developercertificate.org)
   — sign your commits off (`git commit -s`).

If you cannot contribute under these terms, open an issue instead of a PR.

## Practicalities

- Build/test/lint instructions live in the README; `CLAUDE.md` documents the
  layout and custom targets (`unit-test`, `tidy`, `benchmark`).
- Benchmark-relevant changes should re-record numbers in-source via
  `tools/bench.py <domain>` from an optimized build, so the perf delta travels
  with the diff.
- The standalone clang-tidy harness (`--target tidy`) must come back clean.
