# Excel helper — design & protocol

**Language:** English (below) · [日本語](#日本語)

> **Status: helper implemented** (`jp-pii-helper`, CMake target `jp_pii_helper`).
> The Excel-DNA add-in that talks to it is not written yet. This document is the
> contract the add-in will be built against, plus the reasoning behind its shape.

---

## English

`jp-pii-helper` is the sanitizer core packaged for an **Excel add-in**. The add-in
(Excel-DNA, .NET Framework 4.8) reads cell values from the live workbook, sends them
to the helper, shows candidates in a task pane, and writes the sanitized values into
a new workbook. The helper never touches Excel; it only sees strings.

### Why a separate process, not a DLL in Excel

- **Crash isolation.** A fault in the native parsers or ONNX Runtime kills the helper,
  not Excel and the user's unsaved workbooks.
- **Memory.** The model is ~530 MB on disk and ~1 GB resident. It should not live inside
  `EXCEL.EXE` for the rest of the session; a child process can be started when needed
  and dies when Excel does.
- **No `onnxruntime.dll` collisions.** Office itself and other add-ins may already have a
  different version loaded under the same module name. In-process, `LoadLibrary` would
  hand back theirs.
- **The mapping stays out of Excel's address space.** The token ↔ real value table is the
  most sensitive artifact this tool produces ([SECURITY.md](../SECURITY.md)); Excel hosts
  third-party add-ins and macros. The helper writes the mapping to disk itself and only
  returns a count.
- **Iteration.** A DLL holding ONNX Runtime cannot be unloaded, so every fix would mean
  restarting Excel.

### The one invariant: no match crosses a cell boundary

The helper has to return results **per cell**, but the matcher (`find_mask_spans` /
`Tokenizer::tokenize`) works on one string. Calling it once per cell was rejected: each
confirmed term compiles a regex per call, so cost is cells × terms — 5,000 cells × 50
terms is seconds, paid again on every opt-out toggle.

Instead all non-empty cells are joined with a **sentinel, U+001F** (unit separator), and
the matcher runs **once** on that string:

- The whitespace-insensitive term pattern uses `\s*`; under `PCRE2_UCP`, `\s` matches
  Unicode White_Space only, and U+001F is a control character (`Cc`) — it cannot be
  crossed.
- The phone/postal separator classes (`numpat`) do not contain it.
- Literal (short-term) matching cannot cross it unless the term itself contains it.
- Excel cells cannot normally contain it; if one does, `load` replaces it with a space.

So every span lies inside exactly one cell, and splitting the sanitized string on the
sentinel gives back exactly the original number of cells. Both facts are **checked, not
assumed**: a straddling span makes `preview` fail, and a cell-count mismatch makes
`sanitize` fail, rather than shifting values into the wrong cells.

Detection (NER) runs on a **different** join — cells in a row separated by tab, rows by
newline — which is exactly what the file-based `xlsx-prose` path (`sheet_prose`) produces.
That keeps detection on the verified path and keeps control characters away from the
model. Candidates are *terms*, so which text they came from does not matter downstream.

Consequences the add-in should know about:

- A name split across two cells (`山田` | `太郎`) is **not** matched by the confirmed term
  `山田太郎`. Each cell is masked on its own merits (and `山田` alone may well be a
  candidate in its own right).
- Furigana linking still works across cells: with `山田太郎` confirmed, a cell containing
  `ヤマダタロウ` is highlighted with `term = "山田太郎"` and gets the same token.

### Transport

| | Windows (default) | POSIX / CI |
|---|---|---|
| Channel | named pipe `\\.\pipe\jp-pii-sanitizer-<user SID>` (override: `--pipe NAME`) | `--stdio` |
| Access | DACL `D:P(A;;GA;;;<SID>)` — current user only, inheritance off; `PIPE_REJECT_REMOTE_CLIENTS` | process stdio |
| Clients | one at a time; a new instance is created *before* the served one is closed so the name is never free | n/a |
| On disconnect | session state (cells, candidates, **mapping**) is cleared | process exits |

Framing is the same on both: **4-byte little-endian length + UTF-8 JSON**, max 256 MiB
per frame. Requests carry `id` and `cmd`; responses echo `id` and set `type` to the
command name. A malformed frame gets an `ok:false` response (not a disconnect); an
oversize length closes the connection.

Lifecycle:

- On start the pipe server prints one line to stdout:
  `{"event":"listening","pipe":"\\\\.\\pipe\\…","pid":…,"protocol":1}`.
- `--parent-pid N` makes the helper exit when process `N` exits. The add-in should
  **also** put it in a Job Object with `KILL_ON_JOB_CLOSE`; the flag is the belt to that
  suspenders.
- The first `CreateNamedPipe` uses `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a second helper
  (or anything squatting on the name) fails to start instead of becoming a second instance.
  The add-in should verify the server's process (e.g. `GetNamedPipeServerProcessId` is
  the PID it spawned) before sending anything.
- If spawning from Excel is blocked (e.g. a Defender ASR rule in some environments), the
  add-in connects to an already-running helper instead and tells the user to start it.
  Nothing in the protocol assumes who started the process.

### Protocol (version 1)

Cells are `{"r":row,"c":col,"text":"…"}` with **1-based** row/column. `text` is the
**display string** — formatting numeric cells (a phone number stored as a number with a
leading-zero format) is the add-in's job; the helper only sees strings. Empty and
whitespace-only cells may be sent but are dropped.

| `cmd` | Request fields | Response (besides `id`, `type`, `ok`) |
|---|---|---|
| `hello` | — | `protocol`, `ready` (model loaded?) |
| `init` | — | loads model + dictionary (slow, once); `error` on failure |
| `load` | `cells` | `cell_count`, `candidates:[{text,entity_type,count}]`, `cells:[{r,c,spans:[[begin,end,category,term],…]}]` (only cells with spans; offsets are character offsets within the cell), `warnings` |
| `preview` | `confirmed:[{text,type}]` | `cells` as in `load`, for the given confirmed list |
| `sanitize` | `confirmed`, `reversible` (default true) | `cells:[{r,c,text}]` **changed cells only**, `cell_count`, `changed`, `mapping_count`, `reversible`, `leaks` (empty = safety gate passed) |
| `saveMapping` | `path`, optional `passphrase` | writes the mapping to `path` (plaintext `version 1`, or the encrypted `version 2` envelope); `encrypted`. Fails closed. |
| `reverse` | `mappingPath` **or** `mappingText`; `cells` **or** `text`; optional `passphrase` | `cells:[{r,c,text}]` (changed only) or `restored`; `leftovers`. `needsPassphrase:true` when the mapping is encrypted and no passphrase was given |
| `reset` | — | clears cells, candidates and mapping |
| `shutdown` | — | clears, then the process exits after replying |

`candidates`, `confirmed`, `spans` and the mapping file are **the same shapes the GUI
uses** (`winmain.cpp` / `mapping_io.hpp`), so a mapping saved from Excel restores with
`jp-pii-sanitizer-cli restore --mapping`.

Passphrases travel add-in → helper only, are never returned, and are zeroized after use
(`sodium_memzero`) in the request buffer and the parsed JSON.

### Verification

- `jp-pii-helper --selftest --data models` runs in-process: load/preview/sanitize on a
  small sheet, the boundary invariant (split name not matched, furigana linked across
  cells), exact per-cell round trip through the written mapping, the encrypted envelope,
  and the framing/dispatch layer over an in-memory stream (malformed frame, unknown
  command, oversize frame, shutdown).
- `tools/helper_smoke.py <exe> --data models` drives a **real process** over `--stdio`
  with the full add-in sequence; CI runs both on Linux.
- The named-pipe path was checked by hand on Windows (PowerShell client; the pipe's DACL
  shows a single ACE for the current user; `--parent-pid` exits with its parent).

### Not in this piece

- Idle unload (dropping the model after N minutes) — the add-in can simply `shutdown` and
  respawn.
- Several sheets or workbooks sharing one mapping in one session — `load` replaces the
  document; the confirmed CLI way (`--tables`) remains for batch work.
- Anything Excel-side: number formats, hidden rows, comments, shapes, formulas. The
  add-in's scope is **cell values only** and must say so, as `.docx` says "body only".

---

## 日本語

> **状態: ヘルパー実装済み**（`jp-pii-helper`・CMake ターゲット `jp_pii_helper`）。
> これと話す Excel-DNA アドインは未着手。この文書はアドインを書くときの契約と、
> その形にした理由を残す。

`jp-pii-helper` は、サニタイザの中核を **Excel アドイン**向けに包んだもの。アドイン
（Excel-DNA・.NET Framework 4.8）が開いているブックからセルの値を読み、ヘルパーへ送り、
候補をタスクペインに出し、サニタイズ済みの値を別ブックに書く。ヘルパーは Excel に触らず、
文字列しか見ない。

### なぜ Excel 内の DLL ではなく別プロセスか

- **クラッシュ隔離。** ネイティブパーサや ONNX Runtime の不具合で死ぬのはヘルパーであって、
  Excel と未保存のブックではない。
- **メモリ。** モデルはディスク上 約 530 MB・常駐 約 1 GB。これを `EXCEL.EXE` にセッションの
  間ずっと置くべきではない。子プロセスなら要るときに起動し、Excel と一緒に消える。
- **`onnxruntime.dll` の衝突が無い。** Office 自身や他のアドインが同名の別版を既に読んで
  いることがある。プロセス内なら `LoadLibrary` はそちらを返す。
- **対応表を Excel のアドレス空間に乗せない。** トークン↔実値の表は本ツールが生む最も機微な
  成果物（[SECURITY.md](../SECURITY.md)）で、Excel には他社のアドインやマクロが同居する。
  ヘルパーが自分でディスクに書き、アドインには件数だけ返す。
- **開発の回転。** ONNX Runtime を抱えた DLL は unload できず、直すたびに Excel の再起動に
  なる。

### 唯一の不変条件: セル境界をまたぐマッチを作らない

ヘルパーは**セル単位**で結果を返さなければならないが、マッチャ（`find_mask_spans` /
`Tokenizer::tokenize`）は 1 本の文字列に対して動く。セルごとに呼ぶ案は退けた——確定語
1 つにつき呼び出しごとに正規表現を 1 本コンパイルするので、コストはセル数 × 語数になる
（5,000 セル × 50 語で数秒。しかも opt-out を切り替えるたびに払う）。

代わりに、非空セルをすべて**番兵 U+001F**（Unit Separator）で連結し、その 1 本に
マッチャを**1 回**掛ける。

- 空白無視の確定語パターンは `\s*` を使う。`PCRE2_UCP` の `\s` は Unicode White_Space に
  しか当たらず、U+001F は制御文字（`Cc`）なので越えられない。
- 電話・郵便番号の区切りクラス（`numpat`）にも含まれない。
- 短い語の完全一致は、語自体に番兵が無い限り越えない。
- Excel のセルには通常入力できない。入っていたら `load` が空白に潰す。

したがってスパンは必ず 1 セルに収まり、置換後の文字列を番兵で割れば元のセル数に戻る。
どちらも**仮定ではなく検査する**: またぐスパンが出れば `preview` が失敗し、セル数が
合わなければ `sanitize` が失敗する——値がずれて別のセルに入るのではなく。

検知（NER）は**別の連結**に掛ける——行内はタブ、行間は改行。ファイル経路の `xlsx-prose`
（`sheet_prose`）が作るのと同じ形で、検証済みの経路から外れず、制御文字をモデルに見せない。
候補は「語」なので、どちらのテキストから出たかは以降に影響しない。

アドインが知っておくべき帰結:

- 姓と名が別セルに割れた `山田` | `太郎` は、確定語 `山田太郎` では**マスクされない**。
  各セルはそれ自身で判定される（`山田` 単独が候補に挙がることは普通にある）。
- 読みの名寄せはセルをまたいで効く: `山田太郎` を確定すると `ヤマダタロウ` を含むセルは
  `term = "山田太郎"` としてハイライトされ、同じトークンになる。

### トランスポート

| | Windows（既定） | POSIX / CI |
|---|---|---|
| 経路 | 名前付きパイプ `\\.\pipe\jp-pii-sanitizer-<ユーザーSID>`（`--pipe NAME` で変更） | `--stdio` |
| アクセス | DACL `D:P(A;;GA;;;<SID>)` — 現在のユーザーのみ・継承なし。`PIPE_REJECT_REMOTE_CLIENTS` | プロセスの標準入出力 |
| クライアント | 同時に 1 つ。次の待ち受けを**先に**作ってから今の接続を閉じるので、名前が空く瞬間が無い | — |
| 切断時 | セッション状態（セル・候補・**対応表**）を消す | プロセス終了 |

フレームは両方同じ: **4 バイト LE の長さ ＋ UTF-8 JSON**、1 フレーム最大 256 MiB。要求は
`id` と `cmd` を持ち、応答は `id` を返して `type` にコマンド名を入れる。壊れたフレームには
`ok:false` を返す（切断しない）。上限超過の長さは切断する。

ライフサイクル:

- パイプサーバは起動時に stdout へ 1 行出す:
  `{"event":"listening","pipe":"\\\\.\\pipe\\…","pid":…,"protocol":1}`。
- `--parent-pid N` で、プロセス `N` の終了に追従して終わる。アドインは**加えて**
  `KILL_ON_JOB_CLOSE` の Job Object に入れること。このフラグはその保険。
- 最初の `CreateNamedPipe` は `FILE_FLAG_FIRST_PIPE_INSTANCE` を付けるので、2 つ目の
  ヘルパー（や名前を先取りした何か）は 2 番目のインスタンスにならず起動に失敗する。
  アドインは送信前にサーバのプロセスを確認すること（`GetNamedPipeServerProcessId` が
  自分の起動した PID か）。
- Excel からの起動が塞がれている環境（Defender の ASR 規則など）では、アドインは起動済みの
  ヘルパーに接続し、利用者に手動起動を案内する。プロトコルは誰が起動したかを仮定しない。

### プロトコル（version 1）

セルは `{"r":行,"c":列,"text":"…"}`、行・列は **1 始まり**。`text` は**表示文字列**——
数値セルの表示形式（先頭 0 の電話番号など）の適用はアドインの責務で、ヘルパーは文字列
しか見ない。空セル・空白のみのセルは送ってもよいが落とされる。

| `cmd` | 要求 | 応答（`id`・`type`・`ok` 以外） |
|---|---|---|
| `hello` | — | `protocol`、`ready`（モデル読込済みか） |
| `init` | — | モデルと辞書を読む（重い・1 回）。失敗なら `error` |
| `load` | `cells` | `cell_count`、`candidates:[{text,entity_type,count}]`、`cells:[{r,c,spans:[[begin,end,category,term],…]}]`（スパンのあるセルのみ。オフセットはセル内の文字単位）、`warnings` |
| `preview` | `confirmed:[{text,type}]` | 与えた確定リストでの `cells`（`load` と同形） |
| `sanitize` | `confirmed`、`reversible`（既定 true） | `cells:[{r,c,text}]` **変わったセルのみ**、`cell_count`、`changed`、`mapping_count`、`reversible`、`leaks`（空なら安全ゲート通過） |
| `saveMapping` | `path`、任意 `passphrase` | 対応表を `path` に書く（平文 `version 1`、または暗号化 `version 2` の封筒）。`encrypted`。fail closed |
| `reverse` | `mappingPath` **か** `mappingText`、`cells` **か** `text`、任意 `passphrase` | `cells:[{r,c,text}]`（変わったもののみ）または `restored`、`leftovers`。暗号化済みでパスフレーズが無ければ `needsPassphrase:true` |
| `reset` | — | セル・候補・対応表を消す |
| `shutdown` | — | 消してから応答し、プロセスを終える |

`candidates`・`confirmed`・`spans`・対応表ファイルは **GUI と同じ形**（`winmain.cpp` /
`mapping_io.hpp`）。Excel から保存した対応表は `jp-pii-sanitizer-cli restore --mapping`
でそのまま戻せる。

パスフレーズはアドイン → ヘルパーの一方向のみで、返さない。使用後は要求バッファと
パース済み JSON の両方で `sodium_memzero` する。

### 検証

- `jp-pii-helper --selftest --data models`: プロセス内で、小さなシートの load/preview/
  sanitize、境界の不変条件（分割氏名に当たらない・読みの名寄せはセルをまたぐ）、書き出した
  対応表によるセル単位の完全往復、暗号化封筒、そしてメモリ上ストリームでのフレーミングと
  ディスパッチ（壊れたフレーム・未知コマンド・上限超過・shutdown）を検査する。
- `tools/helper_smoke.py <exe> --data models`: **実プロセス**を `--stdio` で、アドインと
  同じ順序で叩く。CI は Linux で両方を回す。
- 名前付きパイプは Windows で手で確認した（PowerShell クライアント。パイプの DACL は現在の
  ユーザーの ACE 1 つだけ。`--parent-pid` は親と一緒に終了）。

### この段階に含めないもの

- アイドル時のモデル解放 — アドインが `shutdown` して起動し直せば足りる。
- 1 セッションで複数シート・複数ブックが対応表を共有すること — `load` は文書を置き換える。
  バッチには CLI の `--tables` がある。
- Excel 側のこと全部: 表示形式、非表示行、コメント、図形、数式。アドインの対象は
  **セルの値のみ**で、`.docx` が「本文のみ」と書いているのと同じように明記する。
