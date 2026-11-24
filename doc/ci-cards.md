# CI（Jenkins など）で使えるようにする — タスクカード 105-1〜105-6

状態: タスクカードまで書いた（2026-11-24）。未着手。0.10 系（`main`）。105-3 以降は direction のレコード（タスクカード 103-1・103-2、`doc/direction-cards.md`）を前提にする。

用語は `doc/terminology.md` に合わせる（task / direction / job、コア = `Hancho.exe --headless` / `HanchoCore.exe`）。

## なぜやるか

Hancho のコアは常駐する。CI のビルド（Jenkins の 1 回の実行）は、始まって終わり、終わったら何も残さないことを前提にしている。この 2 つの寿命が別々なので、次のことが起きる（2026-11-24 にコードで確認）。

| # | 起きること | 原因 |
|---|---|---|
| 1 | ビルドが終わってもコアが残る | `hancho-cli` はコアがないと自動で起動する。起動は WMI 経由で、呼び出し元のプロセスツリーの外に出す（`src/ipc_client.cpp` の `LaunchGuiOutsideCallerTree`。Claude の Bash ツールを止めないための作り）。Jenkins の後始末（ProcessTreeKiller）は環境変数でビルドのプロセスを探すが、WMI で起動したプロセスはそれを引き継がないので、おそらく対象にならない。ワークスペースの exe が使用中のまま残り、更新・削除もしにくい |
| 2 | 自動で起動するのが `Hancho.exe gui` | サービスとして動く Jenkins（セッション 0、画面なし）ではダッシュボードは無駄で、起動待ちで詰まるおそれもある |
| 3 | 失敗が見えないまま止まる | `src/main.cpp` の `StartCoreHeadless` は、失敗を `MessageBoxW` で出す。画面のないセッションでは誰も閉じられず、そこで止まる |
| 4 | 中止したビルドのジョブが走り続ける | Jenkins の Abort は `hancho-cli` を強制終了する（Ctrl+C の処理は呼ばれない）。ジョブはコアの中で動き続け、次のビルドとぶつかる。コアが終わるときも、ジョブのプロセスを放して残すことがある（`platform_windows.cpp` の `Release`） |
| 5 | 同時に走るビルドが混ざる | 既定のデータディレクトリとキュー `main` を共有する。`run --exit-on-drain` はキュー全体が空になるまで待つので、ほかのビルドのジョブまで待ち、JUnit にも混ざる |
| 6 | ジョブの出力が CI のコンソールに出ない | `run` が出すのは最後の集計 1 行だけ。ログはデータディレクトリ（ワークスペースの外）にあり、成果物として残せない |
| 7 | 時間の上限がない、起動の失敗が尾を引く | `run` に全体の制限時間がない。自動起動に一度失敗すると、しばらく自動起動しない（`RecentAutoStartFailure`） |
| 8 | 失敗の判断で LLM が呼ばれる | ふだんの設定のままだと、失敗したジョブのトリアージで本物の LLM が呼ばれる（102-9 の負荷試験で 138 回呼ばれたのと同じ） |

## 方針

**CI 対応はすべてオプション。今のふるまいは変えない（2026-11-24、ユーザー）。** 新しいオプションか環境変数を付けたときだけ動きが変わる。付けなければ、自動起動（WMI 経由の `gui`）、ダイアログ、`run --exit-on-drain` の待ち方（キュー全体）、終了コード、JUnit の中身、`hancho-cli` が死んだときのジョブの扱い（走り続ける）は今のまま。各タスクカードのテストに「オプションなしでは今と同じ」ことの確認を入れる。

CI 向けに、使い方を 2 つ用意する。

- **使い捨てコア（CI ではこちらをすすめる。`--ephemeral` を付けたときだけ）**: `run` が、そのビルドの中だけで動くコアを自分の子として起動し、終わったら止める。Job Object で包むので、`hancho-cli` が強制終了されても、コアとジョブが全部一緒に消える。データディレクトリはワークスペースの中。ビルドごとにまっさらで、ほかのビルドと混ざらない。
- **常駐コアを共有する**: コアはサービスやスケジュールタスクで別に動かし、CI からは自動起動しない。`run` は自分が出した direction だけを待ち、`hancho-cli` が死んだらコアがその direction をキャンセルする。

どちらでも、ジョブの経過をコンソールに流し、ログをワークスペースに写し、全体の制限時間を付けられるようにする。

## 仮の決定

| 項目 | 仮の答え |
|---|---|
| オプション名 | `run --ephemeral`。`--data-dir` を省いたら `<今のフォルダ>\.hancho-ci\<pid>` を使う（終わったら残す。CI のワークスペースの掃除に任せる） |
| 使い捨てコアの設定 | ふだんのデータディレクトリからは何も写さない。`--seed-from <dir>` を付けたときだけ、設定のファイル（`config.json`、`engines.json`、LLM リスト、`failure-rules.json`、許可）を写す。どちらの場合も `config.json` に `triage_mode: "off"` と `triage_llm_mode: "manual"` を書く（`--triage` で外せる） |
| 終わるときのジョブ | 使い捨てコアは、止める前に残っているジョブを全部キャンセルする（放して残すことはしない）。止める IPC の `quit` に `cancel_jobs: true` を足す |
| 自動起動の種類 | 既定は今のまま（WMI 経由で `gui`）。環境変数 `HANCHO_AUTOSTART=headless` のときだけ headless のコアを起動する。`HANCHO_AUTOSTART=off` と `--no-autostart` で自動起動しない。セッション 0 かどうかで自動的に切り替えることはしない |
| ダイアログ | 既定は今のまま。`HANCHO_AUTOSTART` を指定したとき、または環境変数 `HANCHO_NO_DIALOGS=1` のときだけ、`MessageBoxW` の代わりに標準エラーとログに書く |
| `run` の待ち方 | 既定は今のまま（キュー全体が空になるまで待つ）。`--wait-scope direction` を付けたときだけ自分の direction を待つ。`--ephemeral` では毎回まっさらなので、どちらでも同じ |
| `hancho-cli` が死んだとき（共有のコア） | 既定は今のまま（ジョブは走り続ける）。`--cancel-on-exit` を付けたときだけ、`run` は自分の pid を渡し、コアはその pid が終わったらその direction をキャンセルする |
| JUnit | 既定は今の中身のまま。`--report-output` を付けたときだけ出力を `<system-out>` に入れる |
| 終了コード | 今と同じ（0 成功、1 失敗、2 キャンセル・中断）。`--timeout` で時間切れになったら 2 |
| POSIX | 使い捨てコアは Windows を先に作る。POSIX はプロセスグループで同じことをする（同じタスクカードの中で、テストは Windows だけでもよい） |

## タスクカード

| カード | 内容 | 前提 |
|---|---|---|
| 105-1 | `HANCHO_AUTOSTART` / `--no-autostart`、ダイアログを出さない指定 | — |
| 105-2 | 使い捨てコア（`run --ephemeral`） | 105-1 |
| 105-3 | `run --wait-scope direction`、`run --bt` | 103-1、103-2 |
| 105-4 | `run --cancel-on-exit` | 105-3 |
| 105-5 | `--follow`、`--logs-dir`、`--report-output`、`--timeout` | 105-3 |
| 105-6 | 文書（`doc/ci.md`）・ヘルプ・skill | 105-1〜105-5 |

- 105-1・105-2 は 103-x を待たずに流せる。105-2 だけでも「混ざる」「残る」「中止しても走り続ける」の 3 つは片付く。

---

各タスクカードとも（100-x〜104-x と同じ）:

- 指定したファイル以外はなるべく触らない。触ったら結果ファイルにその理由を書く。
- 各ファイルの改行コードを保つ。
- ビルド・テストは自分で実行しない（BT の次のノードが行う）。コミットしない。
- 既存のテストを弱めたり消したりしない。新しい振る舞いにはテストを足す。
- ユーザーに見える文言は `Tr(L"日本語", L"English", L"Español")` の形で 3 言語にする。

## 105-1: 自動起動を headless にする・ダイアログを出さない

ファイル: `src/ipc_client.cpp`、`src/ipc_client.h`、`src/main.cpp`、`src/cli.cpp`、`tests/headless_smoke.cpp`。

- 自動起動の種類を、環境変数 `HANCHO_AUTOSTART`（`gui` / `headless` / `off`）と全体のオプション `--no-autostart` から決める関数を足す。どちらもなければ `gui`（今のまま）。
- `headless` のときは `LaunchGuiOutsideCallerTree` を使わず、`HanchoCore.exe --headless`（`--data-dir` を引き継ぐ）を起動する。プロセスツリーの外に出すのは今と同じ（共有のコアはビルドより長く生きるのが正しい。ビルドの中に閉じたいときは 105-2 を使う）。
- `off` / `--no-autostart` のときは、コアがなければエラーにする。エラーの文言に、コアの起動方法（`Hancho.exe --headless --data-dir ...`）を書く。
- `HANCHO_AUTOSTART` を指定したとき、または `HANCHO_NO_DIALOGS=1` のときだけ、`StartCoreHeadless` などの `MessageBoxW` を出さず、標準エラーとログに書く。
- `RecentAutoStartFailure` の待ちは、`HANCHO_AUTOSTART` を指定したときだけ使わない（CI のやり直しで止めない）。
- テスト: 判定関数の単体テスト（環境変数の各値と、なにもないとき `gui`）、`--no-autostart` でコアがなければ終了コード 1 とエラー文、`headless` でダッシュボードが起動しない、環境変数がなければ今と同じ経路を通る。

## 105-2: 使い捨てコア（`run --ephemeral`）

ファイル: `src/cli.cpp`（`RunTaskQueueFileCommand`）、新規 `src/ephemeral_core.h` / `.cpp`、`CMakeLists.txt`、`src/ipc_server.cpp`（`quit` の `cancel_jobs`）、`src/main.cpp`（止めるときにジョブをキャンセルする）、`src/platform/platform_windows.cpp`（必要なら）、新規 `tests/ephemeral_smoke.cpp`。

- `run --ephemeral [--data-dir <dir>] [--seed-from <dir>] [--triage]`:
  1. データディレクトリを作る（上の「仮の決定」）。`--seed-from` があれば設定のファイルを写す。`config.json` にトリアージを切る設定を書く。
  2. Job Object を作り、`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` を付ける。`hancho-cli` 自身はこの Job に入らない。
  3. `HanchoCore.exe --headless --data-dir <dir>` を `CREATE_SUSPENDED` で起動して Job に入れてから動かす。`DETACHED_PROCESS` と WMI は使わない。コアが起動するジョブのプロセスも、入れ子の Job としてこの Job に入る（`CREATE_BREAKAWAY_FROM_JOB` を使っている所がないことを確かめる）。
  4. ping で起動を待つ（30 秒）。以降の IPC は、このデータディレクトリのパイプに送る。
  5. いつもの `run` と同じに登録して待ち、レポートを書く。
  6. `quit`（`cancel_jobs: true`）を送り、10 秒待つ。残っていれば Job を閉じて終わらせる。
- `hancho-cli` が強制終了されたときは、OS が Job のハンドルを閉じるので、コアとジョブが全部終わる（何もしなくてよいことをテストで確かめる）。
- 同じデータディレクトリですでにコアが動いていたらエラー（使い捨てにならないため）。
- `quit` の `cancel_jobs`: 実行中・待機中のジョブをキャンセルし、プロセスを放さずに終わらせてから止まる。省いたとき（`Hancho.exe quit` を含む今の呼び方すべて）は今のまま。
- `--ephemeral` を付けない `run` は、今と同じ経路（既定のデータディレクトリ、自動起動）を通る。
- POSIX: `setpgid` で新しいプロセスグループにし、終わるときとシグナルを受けたときにグループごと止める。Linux では `PR_SET_PDEATHSIG` も付ける。
- テスト: `quit` を `cancel_jobs` なしで送ると今と同じ、成功・失敗・キャンセルの終了コード、終わったあとコアのプロセスが残らない、`hancho-cli` を `TerminateProcess` したらコアと実行中のジョブ（`ping -n 60` など）が 5 秒以内に消える、すでにコアがあるとエラー、`--seed-from`、トリアージの設定が書かれる。

## 105-3: `run` が自分の direction だけを待つ（`--wait-scope direction`）

ファイル: `src/cli.cpp`、`src/ipc_server.cpp`、`src/taskqueue.cpp`、`src/bt_manager_api.cpp`、`tests/taskqueue_smoke.cpp`、`tests/bt_integration_smoke.cpp`。

- `run --file ... --wait-scope direction`: 登録で、IPC の `taskqueue_add` が返す `directionId`（103-1）を受け取る。待つのはその direction の job だけ。終了コードと JUnit も、その direction の job から決める（系譜の扱いは今の `--count-superseded` のまま）。
- `--wait-scope` を省いたとき（`queue`）は今のまま、キュー全体が空になるまで待ち、キュー全体で終了コードと JUnit を決める。
- `run --bt <plan.json>`: BT を起動し、その BT の direction が終わるまで待つ。終了コードは BT の結果（`succeeded` 0、`failed` 1、`cancelled` 2）。JUnit には BT が出した job を 1 件ずつ、BT の結果を 1 件として書く。
- `--task <id>` を受け付け、direction をタスクに入れる。
- テスト: `--wait-scope direction` なら、同じキューに別の direction の job（終わらないもの）があっても自分の分で終わり、レポートに他の direction の job が入らない。省けば今と同じにキュー全体を待つ。`run --bt` の成功・失敗。

## 105-4: `hancho-cli` が死んだら direction をキャンセルする（`--cancel-on-exit`）

ファイル: `src/ipc_server.cpp`、`src/taskqueue.cpp`、`src/bt_manager.cpp`、`src/direction_store.cpp`、`src/cli.cpp`、テスト（`tests/bt_ipc_smoke.cpp` か新規）。

- direction の登録（`taskqueue_add`、`bt_start`）に `client_pid` と `cancel_on_client_exit` を足す。コアはその pid を `OpenProcess(SYNCHRONIZE)` で見張り（スレッド 1 本で `WaitForMultipleObjects`、64 件を超えたら分ける）、終わったらその direction の job と BT をキャンセルする。理由は `client exited`。タスクの経過ログにも書く。
- 既定は無効（今のまま、ジョブは走り続ける）。`run --cancel-on-exit`（`--file` / `--bt`）を付けたときだけ有効にする。MCP・`taskqueue add` には足さない。
- direction が終わったら見張りをやめる。コアを再起動したときは、保存した pid がもう生きていなければキャンセルする（pid の使い回しを避けるため、プロセスの開始時刻も一緒に保存して比べる）。
- POSIX は `pidfd_open`（なければ 1 秒ごとの `kill(pid, 0)`）。
- テスト: `--cancel-on-exit` 付きの `run` を強制終了すると direction がキャンセルされ、ジョブのプロセスが消える、付けなければ今と同じに残る、ほかの direction は影響を受けない、再起動後の判定。

## 105-5: `--follow`、`--logs-dir`、JUnit の出力、`--timeout`

ファイル: `src/cli.cpp`（`RunTaskQueueFileCommand`、`TaskQueueRunJUnitXml`）、テスト。

- `--follow`: 待っているあいだ、job の開始・終了（結果、終了コード、かかった時間）・失敗の抜粋（ログの最後の 20 行）を、1 行ずつ標準出力に書く。`taskqueue_wait` の差分から作る。`--json` と一緒なら 1 行 1 件の JSON にする。
- `--logs-dir <dir>`: 終わった job のログを `<dir>\<jobId>.log` に写す（BT なら `<dir>\<btId>\<jobId>.log`）。
- JUnit: `--report-output` を付けたときだけ、各 testcase の `<system-out>` に job の出力の末尾（64 KB、`--report-output-bytes` で変える）、失敗なら `<failure>` に失敗クラスと終了コードを書く。付けなければ今の中身のまま。
- どのオプションも、付けなければ今の出力（最後の集計 1 行）と同じ。
- `--timeout <秒>`: 超えたら、その direction（105-3 の前なら登録した job）をキャンセルし、レポートを書いて終了コード 2。
- テスト: `--follow` の行の形、`--logs-dir` にファイルができる、`--report-output` で JUnit に出力と失敗が入り、なければ今と同じ中身、`--timeout`。

## 105-6: 文書・ヘルプ・skill

ファイル: 新規 `doc/ci.md`・`doc/ci-ja.md`、`src/cli.cpp`（help）、`skills/hancho/SKILL.md`、`README.md`、`README-ja.md`。

- `doc/ci.md`: 2 つの使い方（使い捨て・共有）と選び方、Jenkins（Windows エージェント。`bat` ステップの例、`junit` での取り込み、`archiveArtifacts` でログを残す）と GitHub Actions のセルフホストランナーの例、共有のコアをサービスやスケジュールタスクとして動かす方法、セッション 0 で起きること、LLM の engine の認証（サービスのアカウントで `claude` などにログインしておく必要がある）、トリアージを切る理由。
- help と skill に `--ephemeral`、`--no-autostart`、`HANCHO_AUTOSTART`、`HANCHO_NO_DIALOGS`、`--wait-scope`、`--cancel-on-exit`、`--follow`、`--logs-dir`、`--report-output`、`--timeout`、`run --bt` を書く。どれもオプションで、付けなければ今のふるまいだと明記する。
- テスト: help に新しいオプションが出る。
