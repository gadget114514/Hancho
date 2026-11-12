# 052 — usage の書き出しと月次表示（U2/U3）

Estimate 85 min
Splittable: no

## 目的

長期の使用量を CSV / JSON で出し、GUI の表示範囲を 12 か月まで広げる（`doc/design-v3.md` 3.6.1 U2/U3、`doc/design-v3-todo.md` 2）。

## やること

- [ ] `hancho-cli usage export --format csv|json` と MCP `usage_export` を追加する。
- [ ] `hancho-cli usage compact` で月内のデルタを日 × エンジン × モデルの 1 行に圧縮する（一時ファイルに書いて置き換え）。月替わりにも自動で行う。
- [ ] GUI の使用量グラフの範囲選択を 24h / 7d / 30d / 12m に広げる。12m は U1 を月で合計して描く。
- [ ] `costUsd` を報告しないエンジンには `costKnown: false` を付ける。
- [ ] スモークテストを追加する。

## 完了条件

- 12 か月分の使用量が CSV と GUI で見られる。
- 保存量が目安（12 か月で 1 MB 未満）を超えない。

## 参照

- `doc/design-v3.md` 3.6.1
- `doc/design-v3-todo.md` 2

## Authorised edits

- `src/llm_stream.cpp`
- `src/llm_stream.h`
- `src/cli.cpp`
- `src/mcp_server.cpp`
- `src/gui_dashboard.cpp`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\llm_stream_smoke.exe
```
