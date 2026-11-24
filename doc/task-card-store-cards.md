# タスクカードを Hancho に保存する — タスクカード 104-1〜104-6

状態: タスクカードまで書いた（2026-11-24）。未着手。0.10 系（`main`）。前提は direction のレコード（タスクカード 103-1・103-2、`doc/direction-cards.md`）。

用語は `doc/terminology.md` に合わせる。タスクカード = direction = トリアージのカード（同じ direction を、まだ実行していない・実行中・失敗した、の状態で呼び分けたもの）。

## なぜやるか

- タスクカードは今、`doc/` の Markdown（カード 100-x、101-x、102-x、103-x）にしかなく、Hancho は知らない。task を開いても、どのカードが済んで、どれが依存待ちで、どれがまだ流していないのかが見えない。
- lineage-all5 はカード 10 枚を 1 本の BT に詰めたため、カードの区切りが direction に現れず、失敗した 1 枚だけをやり直せなかった。
- カードの本文から BT を作る手順を、supervisor が毎回手で書いている。

## 方針

タスクカードを、まだ実行していない direction（状態 `planned`）として task の中に保存する。実行すると同じレコードが BT の direction になり、`running` → `succeeded` / `failed` と進む。失敗すればそれがトリアージのカードになる。

```
task  タスク一覧の作り直し
 ├ direction 102-2  succeeded
 ├ direction 102-3  running
 ├ direction 102-4  failed（= トリアージのカード）
 │   └ direction 102-4-r1  planned（救済。supersedes 102-4）
 └ direction 102-6  planned（102-3、102-5 待ち）
```

## 仮の決定

| 項目 | 仮の答え |
|---|---|
| 保存場所 | direction のレコード（103-1 の `DirectionStore`）に `card` を足す。task のレコードの中には持たない（direction と 1 か所で管理する） |
| カードの項目 | `card.number`（`102-3`）、`card.title`、`card.body`（Markdown の本文。上限 64 KB）、`card.template`（BT を作る雛形の名前）、`card.params`（雛形に渡す値）、`after`（同じ task の中のカード番号の配列） |
| `taskId` | 必須。カードは task の中にしか作れない |
| 番号 | task の中で一意。救済は元の番号に `-r1`、`-r2` を付ける（102-10 と同じ） |
| BT の作り方 | 雛形 `card`（agent → build → test → 失敗なら fix ループ）。agent ノードの指示文にカードの本文を入れる。build・test のコマンドは `card.params`（なければ task の既定、`task create --card-defaults <json>`）から取る |
| 依存の判定 | `after` のカードの direction が `succeeded` か `closed`（result succeeded）なら満たす。失敗・`ignored` で閉じたものは満たさない |
| 自動で流すか | 流さない。`card run --ready` で、依存を満たした `planned` のカードをまとめて流す。自動で流す設定（`task update --auto-run`）は後で考える |
| CLI の名前 | `card`（`hancho-cli card add` など）。トリアージのカードも同じ direction なので、`card show` で失敗した direction も見られる |
| 編集 | `planned` のあいだは本文・依存を変えられる。流したあとは変えられない（やり直しは救済の direction にする） |

## タスクカード

| カード | 内容 | 前提 |
|---|---|---|
| 104-1 | `planned` の direction とカードの CLI / MCP / IPC | 103-2 |
| 104-2 | カードを流す（雛形 `card`、`card run`、`--ready`） | 104-1 |
| 104-3 | 救済のカード（`card redo`） | 104-2 |
| 104-4 | Markdown からの取り込み・書き出し | 104-1 |
| 104-5 | 表示（`task show`、ダッシュボード、Web UI、TUI） | 104-2 |
| 104-6 | 文書・ヘルプ・skill、`doc/` のカードの取り込み | 104-1〜104-5 |

- 依存: 103-2 → 104-1 → 104-2 → 104-3、104-1 → 104-4、104-2 → 104-5、104-1〜104-5 → 104-6。
- 102-10 の救済は 104-3 があればそれを使う（救済の direction もカードになる）。104-3 より先に 102-10 を流すときは、救済を BT の direction として作り、104-3 でカードの形にそろえる。

---

各タスクカードとも（100-x・102-x・103-x と同じ）:

- 指定したファイル以外はなるべく触らない。触ったら結果ファイルにその理由を書く。
- 各ファイルの改行コードを保つ。
- ビルド・テストは自分で実行しない（BT の次のノードが行う）。コミットしない。
- 既存のテストを弱めたり消したりしない。新しい振る舞いにはテストを足す。
- ユーザーに見える文言は `Tr(L"日本語", L"English", L"Español")` の形で 3 言語にする。

## 104-1: `planned` の direction とカードの操作

ファイル: `src/direction_store.h`、`src/direction_store.cpp`、`src/taskqueue.h`、`src/taskqueue.cpp`、`src/task_store.cpp`、`src/ipc_server.cpp`、`src/mcp_server.cpp`、`src/cli.cpp`、`tests/taskqueue_smoke.cpp`。

- 前提: 103-2。
- direction のレコードに `card`（上の項目）と `after` を足し、`kind` に `card`（まだ BT を作っていない）を足す。状態は `planned`（job がなく、流していない）。一覧 API（`card_list` / `direction_list` / `taskqueue_changes`）ではペイロード肥大化を防ぐため `card.body` を省き、単体取得（`card_show` / `direction_show`）で `body` を返す。
- 操作:
  ```
  hancho-cli card add --task <id> --number 102-3 --title "..." --body-file <md> [--after 102-2,102-4] [--template card] [--params <json>]
  hancho-cli card list --task <id>          # 番号順。状態と、依存待ちなら何を待っているか
  hancho-cli card show <number|directionId> [--task <id>]
  hancho-cli card edit <number> --task <id> [--title] [--body-file] [--after] [--params]   # planned のときだけ
  hancho-cli card remove <number> --task <id>   # planned のときだけ
  ```
  MCP（`card_add` / `card_list` / `card_show` / `card_edit` / `card_remove`）と IPC にも同じ操作。`card show` で番号が指定され `--task` が省略されたときは、全 task から探して 1 件だけならそれを返し、複数 task に同じ番号があれば `--task` を求めるエラーにする。
- 検査: 番号の重複、`after` に存在しない番号、依存の循環、閉じた task への追加はエラー。
- タスクの状態: `planned` のカードがあり、ほかに `running` や `failed` がなければ、タスクは `open`（まだ終わっていない）。
- タスクの経過ログに `card_added` / `card_edited` / `card_removed` を書く。
- テスト: 追加・一覧・編集・削除、各検査、`planned` が残っているとタスクが `succeeded` にならない、再起動後も残る。

## 104-2: カードを流す

ファイル: `src/bt_templates.h`、`src/bt_manager.h`、`src/bt_manager.cpp`、`src/bt_manager_api.cpp`、`src/direction_store.cpp`、`src/taskqueue.cpp`、`src/cli.cpp`、`src/mcp_server.cpp`、`src/ipc_server.cpp`、`tests/bt_integration_smoke.cpp`。

- 前提: 104-1。
- 雛形 `card` を足す: agent（id `impl`、指示文 = カードの本文、`card.params.engines` があればその候補）→ build（`card.params.build`、`blame: "impl"`）→ test（`card.params.test`、`blame: "impl"`）→ 失敗なら `catch`（`maxRetries: 2`）の fix ループ（101-x の切り替えを使う）。`bt template card` で中身を見られること。
- `card run <number> --task <id>`: 依存を満たしていればカードの本文と雛形から BT を作って始める。BT 定義にカードの `directionId` を渡し、新しい direction は作らずにその `planned` の direction レコードを `kind: "bt"` / `btId: "<btId>"` に昇格させる。依存を満たしていなければエラー（`--force` で無視）。
- `card run --ready --task <id>`: 依存を満たした `planned` のカードを流す。`--max <n>` で同時に流す数を絞る（同じワークツリーでのビルド競合を防ぐため、既定の `--max` は 1 とし、明示指定で並列化できるようにする）。
- task の既定（`task create` / `task update` の `--card-defaults <json>`）: カードの `params` にない値（build・test のコマンド、`engines`）を補う。
- テスト: 流すと同じ direction が `running` になり BT の job がその direction に属する、依存待ちはエラー、`--ready` が依存を満たしたものだけを流す、雛形の検証が通る、既定の補完。

## 104-3: 救済のカード

ファイル: `src/direction_store.cpp`、`src/taskqueue.cpp`、`src/cli.cpp`、`src/mcp_server.cpp`、`src/ipc_server.cpp`、`src/triage_manager.cpp`、`tests/taskqueue_smoke.cpp`。

- 前提: 104-2。
- `card redo <number> --task <id> [--body-file] [--params] [--reason "..."]`: 失敗した（または閉じた）カードを引き継ぐ新しいカード `<number>-r<n>` を `planned` で作る。direction のレコードに `supersedes`（元の direction）を入れ、元には `supersededBy` を付ける。本文・params は指定しなければ元のものを写す。`--run` を付ければそのまま流す。
- 依存: 元のカードに依存していたカードは、救済のカードが成功すれば満たされる（job の系譜と同じく、最新の direction で判定する）。
- 救済の連鎖の上限（102-10 と同じ。既定 2 段）を超えたらエラー（`--force` で無視）。
- トリアージの判断「内容を変えてやり直す」「エンジンを切り替える」は、102-10 が入っていればこれを呼ぶ。
- テスト: 番号の付け方、系譜、依存の付け替え、上限。

## 104-4: Markdown からの取り込み・書き出し

ファイル: 新規 `src/card_markdown.h` / `.cpp`、`CMakeLists.txt`、`src/cli.cpp`、`src/mcp_server.cpp`、`src/ipc_server.cpp`、`tests/taskqueue_smoke.cpp`。

- 前提: 104-1。
- `card import --task <id> --file doc/xxx-cards.md [--dry-run]`: `## <番号>: <title>` または `## <番号> <title>`（コロンあり・なし両対応）の見出しごとに 1 枚のカードにする。本文は次の見出しまで。依存は、①文書の「依存:」の行（`102-2 → 102-3 → 102-6` の矢印チェーンを隣接ペアに分解）、②カード一覧の Markdown 表の「前提」列（`103-1〜103-7` の範囲表記も展開）、③本文の「前提:」の行からカード番号を抽出して読む。同じ task 内にない外部のカード番号（例: 102-x の文書にある `103-2`）や読めなかった依存は警告にして、取り込みは続ける。
- 文書の共通の約束（`---` の後の「各タスクカードとも」または「## 共通の約束」）は、各カードの本文の末尾に付ける。
- 同じ番号のカードがすでにあれば、`planned` なら上書き、流したものなら飛ばして警告（`--dry-run` で事前に確かめられる）。
- `card export --task <id> [--file <md>]`: 同じ形の Markdown に書き出す（状態も見出しの横に書く）。
- テスト: 102-x と 103-x の文書を取り込んで、番号・title・依存が正しい、取り込み → 書き出し → 取り込みで同じになる。

## 104-5: 表示

ファイル: `src/task_store.cpp`（`task show`）、`src/taskqueue.cpp`、`src/task_lineage_view.h`、`src/cli.cpp`、`src/cli_top.h`、`src/gui_dashboard.cpp`、`src/webui/app.js`、`src/webui_assets.cpp`（埋め込みも両方更新）、テスト。

- 前提: 104-2。
- `task show`: カードを番号順に、状態（`planned` / 依存待ち / `running` / `succeeded` / `failed` / `closed`）と、依存待ちなら待っている番号を出す。
- ダッシュボード: タスクを開くと、103-3 の direction の行としてカードが並ぶ。`planned` と依存待ちは薄い色で、番号と title を出す。カードの行を選ぶと、詳細ペインに本文を出す。右クリックに「流す」「やり直す（救済）」「閉じる」。
- タスクの見出し行の結果列（100-5 / 103-3 の `✗✗✓`）に、まだ流していない `planned` のカードも `·` として出す。
- Web UI と TUI も同じ並び（色と右クリックは Web UI だけ）。
- 102-x の件数の上限は、最上位の行（タスク）で数える。カードはタスクの中なので数えない。
- テスト: 表示の行を作る関数の単体テスト（`planned`、依存待ち、救済の字下げ）、TUI の表示。

## 104-6: 文書・ヘルプ・skill、`doc/` のカードの取り込み

ファイル: `skills/hancho/SKILL.md`、`src/cli.cpp`（help）、`doc/obsoleted/cli*.md`、`doc/obsoleted/mcp*.md`、`doc/terminology.md`。

- 前提: 104-1〜104-5。

- skill に supervisor の手順を書く: 大きな依頼は `task create` → カードを `card add`（または `card import`）→ `card run --ready` → 終わったら次の `--ready`。判断と理由は `task note`。カードを 1 本の BT に詰めない。
- `doc/terminology.md` の「direction のレコード」の「案（未決定）」を決定に変える。
- 0.10 の作業そのものを Hancho に入れる: task「0.10.0」を作り、`doc/task-list-redesign-cards.md`（102-x、102-1 は除く）、`doc/direction-cards.md`（103-x の残り）を `card import` で取り込む。取り込んだあとの進み具合は Hancho の `card list` で見て、`doc/` の文書の状態の表は更新しない（文書には「Hancho の task <id> を見る」と書く）。
- テスト: help に `card` の操作が出る、skill の例が `bt validate` を通る。
