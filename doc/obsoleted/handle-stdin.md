# pipe モードの stdin の扱い（`--open-stdin` / `--close-stdin`）

## 現在の動作（2026-10-21 時点）

pipe モードでは、既定で **stdin の書き込み側を開いたまま**にする。`hancho-cli.exe send` / MCP の
`send_process_input` で入力を送れる。`--close-stdin` を指定すると、子プロセス起動直後に
書き込み側を閉じ、子プロセスへ EOF を届ける。

| 層 | 指定 | 既定値 | 実装箇所 |
|---|---|---|---|
| 内部 | `StartProcessRequest::openStdin` | `true` | `src/process_manager.h` |
| CLI | `--open-stdin` / `--close-stdin` | open | `src/cli.cpp` |
| IPC | `open_stdin` | `true` | `src/ipc_server.cpp` |
| MCP | `start_process` の `open_stdin` | `true` | `src/mcp_server.cpp` |

stdin を閉じる処理は 1 か所だけ:

```cpp
// src/process_manager.cpp
if (req.mode == ExecMode::Pipe && !req.openStdin && proc->hStdinWrite) {
    CloseHandle(proc->hStdinWrite);
    proc->hStdinWrite = nullptr;
}
```

### フラグの意味
- `--open-stdin`: stdin の書き込み側を開いたままにする（既定）。`hancho-cli send` で入力する場合に必要。
- `--close-stdin`: 起動直後に stdin を閉じ、子プロセスに EOF を届ける。`opencode run` のような
  非対話プログラムが stdin 待ちで止まるのを防ぐ。
- 2 つのフラグは**後に書いたほうが勝つ**（`--open-stdin --close-stdin` なら閉じる）。
- **pipe モード専用**。`--pty` と `--mode console` では、どちらのフラグを付けても何も変わらない。
- stdin を閉じたプロセスに `hancho-cli send` しても入力は届かない（`hStdinWrite` が `nullptr` のため）。

## 使い分け

| 用途 | 起動方法 |
|---|---|
| 対話シェルを pipe モードで使う | 既定（`--open-stdin`）のまま。`hancho-cli send` で入力 |
| `opencode run "<task>"` などの非対話コマンド | `--close-stdin` を付ける |
| `opencode` の TUI | `--pty`（推奨）。pipe モードは TTY がないため TUI には不向き |

```powershell
# 対話シェル: stdin は開いたまま
hancho-cli.exe start --name "pipe-shell" --cmd "cmd.exe"

# 非対話: stdin を閉じて EOF を届ける
hancho-cli.exe start --name "oc-run" --close-stdin --cmd "opencode run OK" --timeout 90
```

MCP では `open_stdin: false` が `--close-stdin` に相当する。

## 検証結果（opencode 1.19.3 / 2026-10-21）

- `opencode run OK` を既定（stdin open）で pipe 起動
  → 出力が始まらず running のまま。stdin の EOF 待ちで停止することを確認。
- `opencode run OK` を `--close-stdin` 付きで pipe 起動
  → 約 7 秒で exit 0。出力を受信して完了することを確認。
- `opencode`（TUI）を pipe モードで起動
  → stdin open では running のまま応答なし。stdin close では EOF により即終了する
  （ユーザー報告と一致。TUI は `--pty` を使う）。

### 再現コマンド

```powershell
# EOF 待ちで止まる
hancho-cli.exe start --name oc-open --cmd "opencode run OK" --timeout 15 --wait
# EOF を届けて完了
hancho-cli.exe start --name oc-close --close-stdin --cmd "opencode run OK" --timeout 90 --wait
```
