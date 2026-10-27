# About this fork

This is Shiklo's fork of [TraceJVM](https://github.com/tracecodeapp/tracejvm), used by the Java editor and tutorial
on code.shiklo.com. It stays under TraceJVM's licence (AGPL-3.0-only; third-party parts under their own licences, see
THIRD_PARTY_NOTICES.md).

- Every runtime served on code.shiklo.com is built by `.github/workflows/build-runtime.yml` and published as a
  release of this repository, tagged with the commit it was built from: that commit is its corresponding source.
- Changes made in this fork (fixes to the VM, its natives, the compiler build and the runtime images) are listed
  below with their date. Fixes that help everyone are offered back to TraceJVM.

## Changes

- 2026-10-28: added the runtime build workflow and this file. No engine change yet.
