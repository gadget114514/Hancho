# ww-unity: ww with Unity support

`tools/ww-unity` is the `tools/ww` task-card runner plus the Unity additions used in the
Survivors project. Everything in `tools/ww/README-ww.md` still applies; this file lists only
what is different.

## Setup

- Task cards: `ww-data\tasks\<id>-*.md`, reports in `ww-data\reports`, as in `tools/ww`.
- Optional `ww-data\config.json`:

  ```json
  {
    "watch": ["Assets", "Tools"],
    "unity": { "editor": "C:\Program Files\Unity\Hub\Editor\<version>\Editor\Unity.exe", "startTimeoutSec": 300 }
  }
  ```

  - `watch`: folders whose git changes count as run progress.
  - `unity.editor`: if no Unity.exe is running before a main-checkout run, it is started with
    `-projectPath <root>` and the run waits for its window. Without it, a missing editor is only
    reported.

## What is added

- **`-QueueLocked`** (`ww-run`): for hancho-cli task-queue jobs whose `resources` already keep
  overlapping jobs apart. Only the same card blocks the run.
- **`ww-enqueue`**: registers cards as hancho-cli jobs, locking `@unity-editor` for Unity cards.
  Card `Mode:` lines `static` and `tools` skip the Unity lock. The card path prefixes it accepts
  (`Assets/`, `Tools/`, `data/`) are fixed in `Get-CardPaths`; change them for another project.
- **`ww-wait`**: blocks until named hancho-cli jobs finish.
- **Unity CPU progress** (`ww-watch`): Unity.exe CPU counts as progress with its own threshold,
  `-MinUnityCpuDelta` (default 30 s), so an idle editor does not hide a stall.
- **Unity Editor check** before every main-checkout run (see `unity.editor` above).
- **uloop**: shared and worktree runs are told not to run `uloop`, because the Unity Editor is
  one instance that the other runs share.
- **Claude default model**: `claude-haiku-5-5` when `-Engine claude` has no `-Model` or `WW_MODEL`.

## Self tests

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ww-unity\ww-run.selftest.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ww-unity\ww-watch.selftest.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ww-unity\ww-wait.selftest.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ww-unity\tests\ww-watch.tests.ps1
```
