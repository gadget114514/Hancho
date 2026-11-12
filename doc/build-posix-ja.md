# POSIX ヘッドレスコアのビルド

Hancho は Linux / macOS を**ヘッドレスコア + CLI**としてサポートします。操作画面として
端末向けの `hancho-cli top` に加え、コア内蔵の **ブラウザ UI ダッシュボード**（`hancho-cli web`、
詳細仕様は [browser-ui-ja.md](browser-ui-ja.md)）を提供します（`doc/design-v3-todo.md` §8）。

## 必要なもの

- CMake 3.20 以上
- C++20 コンパイラ（GCC 11+、Clang 14+、Apple Clang 14+）
- PTY サポート（Linux は `libutil`、macOS は libc に含まれる）
- pthreads
- *※ フロントエンド開発環境（Node.js / npm）は不要です。UI 資産はバイナリに自動埋め込みされます。*

## ビルド

```sh
cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
cmake --build build-posix --parallel
```

生成物:

| バイナリ | 用途 |
|---|---|
| `Hancho` | ヘッドレスコア（キュー、BT、Unix ドメインソケットの IPC、組み込み WebUI サーバー） |
| `hancho-cli` | クライアント（キュー / BT / プロセス操作、`top`、`web` / `ui` ブラウザ起動） |
| `posix_core_smoke` | 移植性とコアのスモークテスト |
| `webui_smoke` | ブラウザ UI（HTTP 資産配信・トークン認証・WebSocket IPC）のスモークテスト |

## 実行

```sh
# コアプロセスの起動（WebUI HTTP+WebSocket サーバーが localhost:9138 で自動起動）
./build-posix/Hancho --headless --data-dir ./ww-data

# ブラウザ UI ダッシュボードの表示（既定ブラウザで安全なトークン付き URL を開く）
./build-posix/hancho-cli --data-dir ./ww-data web

# 端末向け対話型 TUI ダッシュボードの表示（タブ切替・監視・診断・ジョブ操作）
./build-posix/hancho-cli --data-dir ./ww-data top

# 非対話スナップショット出力（スクリプト・パイプ連携）
./build-posix/hancho-cli --data-dir ./ww-data top --once
```

コアが起動していなければ `hancho-cli` が自動で起動します。データディレクトリごとに
`flock` のロックとソケットが分かれるため、複数プロジェクトを同時に動かせます。
TUI のキー操作や詳細な確認チェックリストは [tui-design-ja.md](tui-design-ja.md) §5 を参照してください。

## テスト

```sh
# POSIX コアおよび TUI 出力成型の自動テスト
./build-posix/posix_core_smoke

# ブラウザ UI サーバーと WebSocket IPC のテスト
./build-posix/webui_smoke
```

シェルのクォート、インスタンスロック、Unix ソケット、`/bin/sh` を使った実際の
タスクキュー、BT のジョブノード、WebUI の資産配信と認証・復元、および TUI の
全 5 画面レンダリング・キー解析・ジョブ操作リクエスト生成を確認します。

## 境界

| 境界 | POSIX 実装 |
|---|---|
| `IPty` | `forkpty` |
| `IProcessTree` | `setsid` のプロセスグループ。書き込める場合は cgroup v2 |
| `IIpcTransport` | Unix ドメインソケット |
| `IInstanceLock` | `flock` |
| `IShellQuote` | POSIX のシングルクォート規則 |

Windows は従来の ConPTY、Job Object、Named Pipe、ミューテックスの経路をそのまま使います。

## CI

```yaml
- name: Build
  run: |
    cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
    cmake --build build-posix --parallel
- name: Test
  run: |
    ./build-posix/posix_core_smoke
    ./build-posix/webui_smoke
```

## シェル規則

ChainGuard は POSIX 用の規則を別に持ちます（`src/chain_guard_posix.cpp`）。
`&&`、`||`、クォートされていない `&`、`;`、改行、ポーリングループを拒否し、
単一の `|` は許可します。ワーカーのコマンドは `/bin/sh -c` で実行し、
`{summary}` / `{instructions}` は POSIX 規則でクォートします。
