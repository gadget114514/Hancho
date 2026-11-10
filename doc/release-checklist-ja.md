# リリースチェックリスト

リリースのたびにこのチェックリストを実行します。`design-v3.md` の 5 章にある卒業指針を最新に保つためのものです。

## 卒業と機能対応表

- [ ] [`design-v3.md` 5.3](design-v3.md) の機能対応表を見直す。Hancho の各機能について、harness 側の候補と、置き換えてよい条件が今も成り立つかを確認する。
- [ ] 変更があれば [`critical-comparison-ja.md`](critical-comparison-ja.md) にも反映する。
- [ ] harness 側でカバーできるようになった機能は「互換」扱いにして新しい投資をやめ、5.3 の条件を満たしてから消す。
- [ ] 5.5 が現在の Hancho の運用と合っているか確認する。
- [ ] 片付けの経路が動くか確認する: `hancho-cli.exe mcp uninstall` で MCP の項目が外れ、`hancho-cli.exe agent uninstall-template` で `hancho-runner.md` が消える。データディレクトリは `--delete-data-dir` を付けない限り残る。
- [ ] README にやめ方が書かれ、データが JSON / JSONL のまま読めることが明記されているか確認する。

## ビルドとリリース

- [ ] `CMakeLists.txt` のバージョンを更新する。
- [ ] Release で `Hancho` と `hancho-cli` をビルドする。
- [ ] `hancho-cli.exe --version` が一致することを確認する。
- [ ] zip とインストーラーを作り、GitHub リリースを公開してインストールする。
