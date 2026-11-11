# 070 — 設計文書の未決事項の決定

Estimate 180 min
Splittable: no

## 目的

`doc/design-v3.md` §7 の未決事項を決め、設計文書と実装に反映する（`doc/design-v3-todo.md` 7）。

## やること

- [ ] イベントスキーマの JSON Schema を `data` の型まで縛るか決める。
- [ ] JSON ストリームのない普通のプロセス（キューの外で起動したもの）にも RunRecord を作るか決める。
- [ ] L3 の昇格を人以外（特定のワーカー）にも許すか決める。
- [ ] リモートの認証をトークン以外（SSH 鍵の流用など）にも広げるか決める。
- [ ] 失敗分類の `incomplete` と `success_check_failed` の境目を決める。
- [ ] 決定ごとに根拠と採用理由を書き、`doc/design-v3.md` §7 を決定済み / 残る未決に整理する。
- [ ] 実装が必要な決定は、`ww-data/tasks/` に新しいカードを起こす。

## 完了条件

- `doc/design-v3.md` §7 に決定の記載があり、設計文書本文も整合する。
- 実装カードの起票と紐づけが済んでいる。

## 参照

- `doc/design-v3.md` §7
- `doc/design-v3-todo.md` 7

## Authorised edits

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `doc/design-v3-todo.md`
- `ww-data/tasks/`

## Read-only

- `src/`
- `doc/schema/hancho.event.v1.json`

## 検証

```powershell
# 文書のみのタスク。ビルドは不要。設計文書の整合を確認する
git diff -- doc/design-v3.md doc/design-v3-todo.md
```
