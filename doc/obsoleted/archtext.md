# Hancho Architecture (ASCII draft)

Hancho v0.9.23 のコード構成にもとづくアスキー図。清書用の下書き。
表記: `-->` は要求・制御、`-.->` はストリーム・イベント、`[ ]` はプロセス/コンポーネント。

---

## 1. プロセス構成と通信経路

```text
+--------+   +--------------+   +-------------+   +---------+   +--------------+
| Human  |   | Supervisor   |   | Scripts /   |   | Browser |   | HanchoTerm   |
| GUI    |   | LLM          |   | agents      |   |         |   | wct.exe      |
+---+----+   +------+-------+   +------+------+   +----+----+   +------+-------+
    |               |                  |               |               |
    | GUI           | MCP / stdio      | CLI           | HTTP+WS       | WCSP
    v               v                  v               | :9138         | :9139
+---------+   +-------------+   +-------------+       |               |
| Hancho  |   | Hancho.exe  |   | hancho-cli  |       |               |
|Dashboard|   |   mcp       |   |   .exe      |       |               |
|  .exe   |   | MCP server  |   | CLI / top   |       |               |
+----+----+   +------+------+   +------+------+       |               |
     |               |                 |              |               |
     +---------------+-----------------+              |               |
                     |                                |               |
                     | Named Pipe (JSON request/response/subscribe)   |
                     v                                |               |
+---------------------------------------------------------------------------------+
|                              HanchoCore.exe                                     |
|                   (1 instance per --data-dir: mutex + pipe)                     |
|                                                                                 |
|  IpcServer      \\.\pipe\Hancho_IPC_v1[_<tag>]                                  |
|  WebUiServer    TCP :9138   HTTP + WebSocket                                    |
|  TermServer     TCP :9139   WCSP v1 (remote) / ephemeral loopback (local)       |
|  RemoteListener TCP :9137   JSON over WebSocket (opt-in, token required)        |
|  IpmsgNode      UDP/TCP :2426  IPMsg (LAN chat, off by default)                  |
|                                                                                 |
|  Engine:  ProcessManager | TaskQueueManager | BtManager | Triage | Memory      |
+---------------------------------------------------------------------------------+
        ^                              ^                        ^
        | WCSP                         | WebSocket              | IPMsg
   HanchoTerm.exe                Remote core                Chat peers
   wct.exe                       (other machine)            (LAN)
```

`Hancho.exe` は入口で、役割ごとに `HanchoCore.exe` / `HanchoDashboard.exe` / `HanchoChat.exe` として起動し直す。実体は同じコア。

---

## 2. コア内部のレイヤ

```text
+============================== HanchoCore.exe ===============================+
|                                                                             |
|  +------------------------------------------------------------------------+ |
|  | IpcServer: action dispatch / long-poll / subscriptions                 | |
|  +---+----------+-----------+------------+-----------+-----------+--------+ |
|      |          |           |            |           |           |          |
|      v          v           v            v           v           v          |
|  ChainGuard  TaskQueue   BtManager   ProcessMgr  Memory      TriageMgr      |
|              Manager                            (L1/L2/L3)                 |
|                 |  ^          |            ^           ^           ^         |
|                 |  |          |            |           |           |         |
|                 |  +----------+            |           |           |         |
|                 |   adds jobs              |           |           |         |
|                 v                          |           |           |         |
|             Scheduler ---------------------+           |           |         |
|             deps / concurrency / retry /               |           |         |
|             resource locks / defer / host              |           |         |
|                 |                                      |           |         |
|                 v                                      |           |         |
|             ProcessManager  (ConPTY + Win32 Job Objects)           |         |
|             timeout / idle / tree kill / stdin / logs / done       |         |
|                 |                                      |           |         |
|                 v                                      |           |         |
|             Worker process (claude/opencode/codex/agy/build)       |         |
|                 |                                      |           |         |
|                 v                                      |           |         |
|             LlmStreamTracker + EventAdapter ----------+-----------+         |
|             raw lines -> hancho.event/1 -> .events.jsonl          |         |
|                 |                                                  |         |
|                 v                                                  |         |
|             GUI monitor / MCP / BT conditions / Memory / Triage    |         |
|                                                                             |
|  +--------------+  +--------------+  +------------+  +-------------------+   |
|  | WebUiServer  |  | TermServer   |  | IpmsgNode  |  | RemoteListener    |   |
|  +--------------+  +--------------+  +------------+  +-------------------+   |
|  +--------------+  +----------------------------------------------+          |
|  | PowerManager |  | WorkerEngineRegistry / adapters (engines.json)|          |
|  +--------------+  +----------------------------------------------+          |
+=============================================================================+
```

---

## 3. ジョブ / 試行 / プロセスのライフサイクル

```text
 caller (MCP / CLI / GUI / BT)
        | taskqueue_add (jobs[])
        v
 +------------------+          +----------------------------+
 | TaskQueueStore   | <------> | Scheduler                  |
 | job + attempts   |          | deps / concurrency / retry |
 | JSON, durable    |          | resource locks / defer     |
 +------------------+          +-------------+--------------+
                                             |
                         job.host empty?     | 1 attempt = 1 managed process
                    +------------------------+------------------------+
                    | local                                           | remote
                    v                                                 v
        +---------------------------+                   +--------------------------+
        | ProcessManager            |                   | RemoteClient             |
        | ConPTY / Job Object       |                   | WS :9137 -> remote core  |
        +------+------+-------------+                   +------------+-------------+
               |      |                                                |
   log/stdout  |      | ConPTY bytes / stdin                             | same job
               v      v                                                v
        worker CLI (claude/opencode/codex/agy/build)             remote ProcessManager
               |
   +-----------+------------------+-------------------+
   v                              v                   v
 .log                    .events.jsonl            .done file / exit code
   |                              |                   |
   +------------------------------+-------------------+
                                  v
                 success check: exitCode AND filesNewer
                 AND outputRegex AND testReport
                                  |
                 +----------------+----------------+
                 v                                 v
        state transition                    failure classifier
        next job / dependents               (failure-rules.json)
        Memory L1 run record                -> TriageManager card
                                            -> retry policy
```

---

## 4. データディレクトリ

```text
<data-dir>/        (default: <install>/logs or %LOCALAPPDATA%\Hancho\logs)
  config.json                    GUI / terminal / webui / triage settings
  engines.json                   worker engine definitions
  hosts.json                     remote host registry (token refs only)
  failure-rules.json             extra failure classifications
  adapters/<name>.json           Tier-1 stream adapters

  taskqueues/<queue>.json        queue + jobs + attempts (source of truth)
  taskqueues/<queue>/instructions/<job>.r<rev>.md
  taskqueue-<queue>.events.jsonl queue events
  taskqueue/<job>.<n>.log        attempt log
  taskqueue/<job>.<n>.events.jsonl
  taskqueue/<job>.<n>.done
  taskqueues/plain.json          runs outside queues

  proc_<id>_<name>_<ts>.log      managed process log
  proc_<id>_<name>_stream.jsonl  raw CLI stream
  proc_<id>_<name>.events.jsonl  normalized hancho.event/1

  memory/records/YYYY-MM.jsonl   L1 run records
  memory/notes.jsonl             L2 notes (append-only + replay)
  memory/knowledge.jsonl         L3 approved project knowledge
  memory/index/                  derived, rebuildable

  triage/cards.json              persisted triage cards
```

---

## 清書時に落とさない要点

- 実行の唯一の入口は `IpcServer`。GUI も MCP も CLI も全部 IPC クライアント。
- `BtManager` はプロセスを直接起動せず、`TaskQueueManager` にジョブを追加する。
- プロセス起動は必ず `ProcessManager` を通る（BT も通常ジョブも同じ）。
- Memory / Triage / GUI / MCP / BT 条件は、正規化イベント `hancho.event/1` を共通の入力にしている。
- プロジェクト分離は `--data-dir` 単位（mutex / pipe / キュー / WebUI / ポートが別）。
