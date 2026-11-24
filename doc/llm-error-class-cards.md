# LLM エラーの構造化分類 — カード 102-1〜102-3

状態: 実装済み（102-1、102-2、テスト追加。ビルドは通過）

親: 会話での議論（文字列パターン判定の限界と、HTTP/エラーオブジェクトの構造化分類の提案）。
ブランチ: `feature/fix-engine-switch`（0.10 系）。

## 背景

- 失敗の分類（`ClassifyTaskQueueFailure`、`src/taskqueue.cpp`）は、エラーイベントの `message` と、ログ本文の正規表現（`kRatePattern` / `kAuthPattern`）で判定している。
- `src/llm_stream.cpp` の `error` 分岐（1693 行付近）は、エラーオブジェクトの `message` だけを残し、`status` / `code` / `type` を捨てている。そのため、トークン切れ（認証）、クォータ切れ、一時的な過負荷が、本文の文言でしか区別できない。
- 構造化情報（HTTP ステータス、`type` / `code`）は、子プロセスの JSON イベントに載っている場合がある。これを分類に使う。

## 仮の決定

| # | 未決定 | 仮の答え |
|---|---|---|
| 1 | 分類の場所 | `llm_stream.cpp` で、エラーイベントを解析するときに構造化情報からクラスを決める。分類できないときは空のままにし、後段の文字列判定に任せる。 |
| 2 | クラス名 | 既存の語彙を使う: `auth_error`、`rate_limited`、`provider_down`。`insufficient_quota` は既存の `rate_limited` に寄せる（新しいクラスは作らない）。 |
| 3 | 優先順位 | 構造化クラスは、ログ本文の文字列ルールより先に使う。ただし権限拒否（4.5 の `permission_denied`）より後にする。 |
| 4 | トークン切れ | `auth_error`。`suggestedAction` は `human`（既存のまま）。BT 側の切り替えは `switchEngineOn` に書いたときだけ起きる（既存の動き。今回は変えない）。 |
| 5 | ww-run.ps1 の切り替え | 今回は変えない。`Get-LimitHint` は本文の文字列のままにする（別カードで扱う）。 |

## 分類の対応表

| 入力（いずれか） | クラス |
|---|---|
| HTTP ステータス 401 / 403、または `type` / `code` が `authentication_error` / `invalid_grant` / `invalid_api_key` / `token_expired` | `auth_error` |
| HTTP ステータス 429、または `type` / `code` が `rate_limit_error` / `rate_limit_exceeded` | `rate_limited` |
| `type` / `code` が `insufficient_quota` / `quota_exceeded` / `billing_hard_limit_reached` | `rate_limited` |
| HTTP ステータス 500 / 502 / 503 / 504 / 529、または `type` / `code` が `overloaded_error` / `server_error` / `service_unavailable` | `provider_down` |

判定の順は表の上から。どれにも当たらなければ空。

---

## 102-1 エラーイベントに構造化情報を残す

- `src/llm_stream.h`（`LlmStreamEvent`）: `std::string errorClass;`（構造化分類。空可）、`std::string errorCode;`（`code` または `type` の値）、`int httpStatus = 0;` を足す。
- `src/llm_stream.cpp`（`ParseGenericEvent` の error 分岐、1693 行付近）: エラーオブジェクトと、トップレベルの `status` / `code` / `type` から値を取る。`status` は数値でも文字列でもよい（数値なら `httpStatus`）。取った値で `ClassifyLlmErrorStructured(httpStatus, errorCode)` を呼び、`errorClass` に入れる。
- `ClassifyLlmErrorStructured` は `llm_stream.h` に宣言し、`llm_stream.cpp` に定義する（上の対応表どおり）。
- `src/event_schema.cpp`（error 分岐、220 行付近）: `errorClass` があれば `data["class"]`、`httpStatus` が 0 より大きければ `data["httpStatus"]`、`errorCode` があれば `data["code"]` を書く。
- `src/event_adapter.cpp`（error 分岐、402 行付近）: 同じ 3 項目を読み戻す。
- テスト（`tests/` の既存の llm/event 系のスモーク。なければ `tests/taskqueue_smoke.cpp` に足す）: 401 の JSON → `auth_error` / 429 → `rate_limited` / 503 → `provider_down` / 構造化情報がないエラー → 空 / `message` の文言が `rate limit` でも、`status` が無ければ構造化クラスは空。

## 102-2 分類器が構造化クラスを先に使う

- `src/taskqueue.cpp` の `ClassifyTaskQueueFailure`: 4.5（permission_denied）の後、5（rate_limited の文字列ルール）の前に、`kind == "error"` のイベントのうち `data.class` を持つ最後のものを探す。見つかったら、そのクラスを `failureClass` にする。
  - `confidence` は `high`。`detail["rule"]` は `structured.<class>`。`detail["httpStatus"]` と `detail["code"]` があれば入れる。
  - `suggestedAction`: `auth_error` → `human`、`rate_limited` → `retry_after`、`provider_down` → `retry`。
  - `summary` は既存の文言に合わせる（認証: `Authentication failed: <message>`、レート: `Rejected by ... rate limit` 相当の一文）。
  - `EnsureEvidence` / `AddEventEvidence` で証拠を残す。
- テスト（`tests/taskqueue_smoke.cpp`）: 構造化 `auth_error` のイベントだけで `auth_error` になる / 構造化 `rate_limited` は、ログに文字列が無くても `rate_limited` / ログに `rate limit` の文字列があるが構造化クラスが `provider_down` なら `provider_down` が勝つ / 構造化情報のない文字列だけのエラーは従来どおり / `permission_denied` のイベントがあれば構造化クラスより優先。

## 102-3 文書と確認

- `doc/llm-error-class-cards.md`（このファイル）の状態を「実装済み」に変える。
- `doc/terminology.md` に `auth_error` の説明が既にあれば合わせる（無ければ足さない）。
- ビルドと既存のスモークテスト（`taskqueue_smoke` / `behavior_tree_smoke`）が通ること。

## 共通の約束

- 変えるのは上に書いたファイルとそのテストだけ。C++ のソースは LF。コミットはしない。
- 既存のテストを弱めたり消したりしない。
- `ww-run.ps1` / `ww-run.lib.ps1` は変えない（範囲外。別カードで扱う）。
