# POSIX ヘッドレスコアのビルド

Hancho は Linux / macOS を**ヘッドレスコア + CLI**としてサポートします。これらの
プラットフォームには GUI ダッシュボードはなく、画面は `hancho-cli top` だけです
（`doc/design-v3.md` 2.4）。

## 必要なもの

- CMake 3.20 以上
- C++20 コンパイラ（GCC 11+、Clang 14+、Apple Clang 14+）
- PTY サポート（Linux は `libutil`、macOS は libc に含まれる）
- pthreads

## ビルド

```sh
cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
cmake --build build-posix --parallel
```

生成物:

| バイナリ | 用途 |
|---|---|
| `Hancho` | ヘッドレスコア（キュー、BT、Unix ドメインソケットの IPC） |
| `hancho-cli` | クライアント（キュー / BT / プロセス操作と `top`） |
| `posix_core_smoke` | 移植性とコアのスモークテスト |

## 実行

```sh
./build-posix/Hancho --headless --data-dir ./ww-data
./build-posix/hancho-cli --data-dir ./ww-data top
```

コアが起動していなければ `hancho-cli` が自動で起動します。データディレクトリごとに
`flock` のロックとソケットが分かれるため、複数プロジェクトを同時に動かせます。

## テスト

```sh
./build-posix/posix_core_smoke
```

シェルのクォート、インスタンスロック、Unix ソケット、`/bin/sh` を使った実際の
タスクキュー、BT のジョブノードを確認します。

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
  run: ./build-posix/posix_core_smoke
```

## シェル規則

ChainGuard は POSIX 用の規則を別に持ちます（`src/chain_guard_posix.cpp`）。
`&&`、`||`、クォートされていない `&`、`;`、改行、ポーリングループを拒否し、
単一の `|` は許可します。ワーカーのコマンドは `/bin/sh -c` で実行し、
`{summary}` / `{instructions}` は POSIX 規則でクォートします。
