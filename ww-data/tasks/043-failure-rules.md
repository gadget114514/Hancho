# 043 — failure-rules.json と任意の LLM 見立て

Estimate 160 min
Splittable: no

## 目的

分類規則をデータディレクトリで拡張し、決まらないときだけ LLM の見立てを参考にする（`doc/design-v3.md` 4.4）。

## やること

- [ ] `<data-dir>/failure-rules.json` で規則を追加・上書きする。
- [ ] 規則で決まらない（`unknown` / `low`）ときだけ、既存の `escalate` で見立てを頼む。
- [ ] LLM には証拠（イベント末尾、ログ末尾、試行情報）だけを渡す。
- [ ] LLM の見立ては `confidence: low` の参考欄に置き、自動再試行の判断には使わない。
- [ ] 034 / 042 の集計に反映する。

## 完了条件

- 外部規則だけで新しい分類を足せる。
- LLM の見立てが無効でも分類器が決定的に動く。

## 参照

- `doc/design-v3.md` 4.4
- `doc/design-v3-en.md` Section 4

## Authorised edits

- `src/`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
