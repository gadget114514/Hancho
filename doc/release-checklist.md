# Release checklist

Run this checklist for every release. It keeps the graduation guidance in `design-v3-en.md` section 5 current.

## Graduation and feature map

- [ ] Review the feature map in [`design-v3-en.md` section 5.3](design-v3-en.md): for each Hancho feature, re-check the harness-side candidate and whether the replacement conditions now hold.
- [ ] Reflect any change in [`critical-comparison.md`](critical-comparison.md).
- [ ] Mark features now covered by the harness as "compatibility" and stop new investment; remove them only after the replacement meets the conditions in section 5.3.
- [ ] Check that section 5.5 still matches how Hancho itself is operated.
- [ ] Verify the cleanup path still works: `hancho-cli.exe mcp uninstall` removes the MCP entry and `hancho-cli.exe agent uninstall-template` removes `hancho-runner.md`; the data directory is kept unless `--delete-data-dir` is given.
- [ ] Confirm the README still tells users how to leave and that data stays readable as JSON / JSONL.

## Build and release

- [ ] Update the version in `CMakeLists.txt`.
- [ ] Build `Hancho` and `hancho-cli` in Release.
- [ ] Confirm `hancho-cli.exe --version` matches.
- [ ] Package the zip and installer, publish the GitHub release, and install it.
