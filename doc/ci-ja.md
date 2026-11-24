# CI（Jenkins・GitHub Actions など）で Hancho を使う

CI 向けのオプションと環境変数は**すべてオプトイン**です。付けなければ、これまでどおりの対話向けのふるまい（WMI 経由での GUI 自動起動、`run --exit-on-drain` でのキュー全体待ち、最小限の JUnit XML、`hancho-cli` 終了後もジョブが走り続ける）は変わりません。

## 2 つの使い方

| 方式 | 向いている場面 | 仕組み |
|---|---|---|
| **使い捨てコア（`--ephemeral`）** *(推奨)* | ほとんどの CI ビルド（ビルドごとに分離したい） | `hancho-cli run --ephemeral` が `HanchoCore.exe --headless` を Win32 Job Object（`KILL_ON_JOB_CLOSE`）の子プロセスとして起動します。データディレクトリは既定で `<cwd>\.hancho-ci\<pid>`、トリアージは無効（`triage_mode: "off"`）です。終わると残りのジョブをキャンセルしてコアを終了します。CI が `hancho-cli` を強制終了しても、OS がコアと全ジョブを即座に終了します。 |
| **常駐コアを共有する** | Windows サービスやタスクスケジューラでコアを常駐させる場合 | `HanchoCore.exe --headless --data-dir <dir>` を別途起動しておきます。CI 側では `HANCHO_AUTOSTART=off`（または `--no-autostart`）で自動起動を抑止し、`--wait-scope direction --cancel-on-exit` を付けて自分の direction だけを待ち、ビルド中止時は direction をキャンセルします。 |

## オプション・環境変数の早見表

- `run --file <queue.json>` / `run --bt <plan.json>`: キュー登録または BT 起動を行い、完了まで待って終了コード `0`（成功）、`1`（失敗/ブロック）、`2`（キャンセル/中断/タイムアウト）で終了します。
- `--ephemeral`: ビルド内だけで動く使い捨てコアを起動します。
  - `--seed-from <dir>`: `<dir>` から `config.json`、`engines.json`、LLM リスト、`failure-rules.json`、許可設定をコピーします。
  - `--triage`: トリアージを有効のままにします（既定では `--ephemeral` は `triage_mode: "off"` と `triage_llm_mode: "manual"` を設定し、失敗時に LLM が呼ばれないようにします）。
- `--wait-scope <queue|direction>`: 既定は `queue`（キュー全体が空になるまで待つ）。`direction` を指定すると、この `run` が登録した direction だけを待ちます。
- `--cancel-on-exit`: 共有コア使用時、`hancho-cli` の PID を監視し、強制終了されたらその direction をキャンセルします（理由: `client exited`）。
- `--follow`: ジョブの開始・終了（結果・終了コード・所要時間）と失敗時のログ末尾 20 行を標準出力へ 1 行ずつ出力します（`--json` 併用時は NDJSON）。
- `--logs-dir <dir>`: 終了したジョブのログを `<dir>\<jobId>.log`（`--bt` 時は `<dir>\<btId>\<jobId>.log`）にコピーします。
- `--report <junit.xml>`: JUnit XML を出力します（`--bt` 時は各ジョブ 1 件ずつ＋BT 結果 1 件を出力）。
- `--report-output` / `--report-output-bytes <n>`: 各 `<testcase>` の `<system-out>` にログ末尾（既定 64 KB）を、`<failure>` に失敗クラスと終了コードを含めます。
- `--timeout <sec>`: 全体の制限時間（秒）。超過すると direction をキャンセルし、終了コード `2` で終了します。
- `--task <id>`: direction を既存のタスクに紐付けます。
- `--no-autostart` / `HANCHO_AUTOSTART=headless|off|gui`: コア未起動時の自動起動方法を指定します（既定: `gui`）。
- `HANCHO_NO_DIALOGS=1`: セッション 0 や画面なし環境で `MessageBoxW` を出さず、標準エラーとログに書きます（`HANCHO_AUTOSTART` 指定時も有効）。

## Jenkins の例（Windows エージェント）

```groovy
pipeline {
    agent { label 'windows' }
    stages {
        stage('Build & Test') {
            steps {
                bat '''
                    hancho-cli.exe run --ephemeral --file ci/jobs.json ^
                        --follow --logs-dir build/hancho-logs ^
                        --report build/junit.xml --report-output ^
                        --timeout 1800
                '''
            }
        }
    }
    post {
        always {
            junit allowEmptyResults: true, testResults: 'build/junit.xml'
            archiveArtifacts artifacts: 'build/hancho-logs/**', allowEmptyArchive: true
        }
    }
}
```

BT を使う場合は `--file ci/jobs.json` を `--bt ci/plan.json` に置き換えます。

## GitHub Actions の例（セルフホスト Windows ランナー）

```yaml
jobs:
  build:
    runs-on: self-hosted
    steps:
      - uses: actions/checkout@v4
      - name: Run Hancho CI queue
        shell: cmd
        run: |
          hancho-cli.exe run --ephemeral --file ci/jobs.json --follow --logs-dir artifacts\logs --report artifacts\junit.xml --report-output --timeout 1800
      - name: Upload logs
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: hancho-logs
          path: artifacts/logs
```

## 共有コアをサービス / タスクスケジューラで動かす

1. `HanchoCore.exe --headless --data-dir C:\hancho-ci-data` を Windows サービスまたはタスクスケジューラ（スタートアップ時）に登録します。
2. CI ステップからは次のように呼び出します:
   ```bat
   set HANCHO_AUTOSTART=off
   set HANCHO_NO_DIALOGS=1
   hancho-cli.exe --data-dir C:\hancho-ci-data run --file ci/jobs.json --wait-scope direction --cancel-on-exit --follow --logs-dir build\hancho-logs --report build\junit.xml --report-output
   ```
3. **セッション 0 での注意**: サービスはデスクトップのないセッション 0 で動きます。GUI の自動起動は使わず、`--ephemeral`、`HANCHO_AUTOSTART=headless`、または常駐 `HanchoCore.exe --headless` を使い、`HANCHO_NO_DIALOGS=1` で `MessageBoxW` による停止を防いでください。
4. **LLM エンジンの認証**: CI で `agent` ノード（`claude`、`opencode`、`codex`、`agy` など）を動かす場合、サービスや CI エージェントを実行する**同じ Windows アカウント**であらかじめログインしておくか、API キーを環境変数で渡す必要があります。
5. **使い捨てコアでトリアージを切る理由**: トリアージが有効だと失敗時に LLM が呼ばれます。CI では確定的な終了コードと JUnit レポートだけが必要で、意図しない LLM 呼び出しを防ぐため `--ephemeral` では既定で無効にしています（必要な場合のみ `--triage` を付けます）。
