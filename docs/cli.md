# CLI — design & implementation notes

**Language:** English (below) · [日本語](#日本語)

User-facing usage lives in the [README](../README.md#command-line-interface-cli).
This document records *why* the CLI is shaped the way it is, and the
implementation details worth knowing before changing it.

---

## English

`jp-pii-sanitizer-cli` is a scriptable interface over the same verified cores as
the GUI. It exposes three commands:

- `detect <files…>` → candidate PII as JSONL (and a table-column skeleton).
- `mask <files…>` → sanitized text (+ a mapping when reversible).
- `restore --mapping … ` → real values back from token text.

### Design decisions

1. **The candidate file is the authority — review is the user's workflow, not a
   tool feature.** `mask --candidates f` masks exactly the terms in `f` (plus the
   always-automatic e-mail/phone/postal). We deliberately did **not** build an
   interactive review/confirm mechanism into the CLI: editing the JSONL between
   `detect` and `mask` *is* the review step. This keeps the CLI small and
   composable. With no `--candidates`, `mask` auto-detects and masks everything
   (opt-out).

2. **JSONL, not a JSON array.** One object per line survives hand-editing:
   deleting a line drops a candidate, adding a line adds one, and there are no
   array commas/brackets to break. `#` comments and blank lines are ignored.

3. **Irreversible by default.** A reversible run writes a mapping, which is the
   most sensitive artifact the tool produces (token ↔ real value). The default
   therefore writes nothing to disk; you opt into reversibility explicitly with
   `--reversible --mapping map.jsonl`.

4. **Data tables share ONE mapping with prose.** This is the load-bearing
   decision. Masking a table in a *separate* invocation restarts token numbering,
   so `{{PERSON_1}}` in a docx and `{{PERSON_1}}` in an xlsx would denote
   *different* people — restore would collide. So prose documents and data tables
   are masked in **one** invocation through **one** `Tokenizer`: the same person
   in a document and a spreadsheet gets the same token, and one mapping restores
   everything consistently. (`step5::bundle_blocks` already threads a single
   tokenizer through both prose blocks and `tokenize_table`, so once the
   `FileResult`s are populated this falls out for free.)

5. **Two authority files, each homogeneous.** Prose terms (`{"type","text"}`) and
   table columns (`{"file","sheet","header_row","name_cols","company_cols"}`) are
   different shapes. Rather than mixing them, prose lives in `--candidates` and
   tables in `--tables`, so each file stays trivially editable.

6. **Dependency-minimal.** `restore` needs neither the model nor the dictionary
   (pure token substitution). `mask --candidates` loads only Sudachi. Only
   `detect` and one-shot `mask` load the ~530 MB NER model. This makes the common
   masking path fast and light.

7. **Reuses the library headers, not the GUI facade.** `core.hpp` is the
   WebView2-specific facade and deliberately pulls in no Aspose. The CLI instead
   includes the library headers directly — so it can ingest `.msg` (its
   translation unit pulls in Aspose) — and links no WebView2, which is why it
   builds and runs on Linux/macOS as well as Windows.

8. **One mapping serializer, shared with the GUI.** `mapping_io.hpp` owns both the
   writer and the reader; the GUI calls the same functions instead of formatting
   the mapping in JavaScript. Two implementations meant two formats — the app used
   to save CSV, so a mapping saved in the GUI made `restore` fail on line 1. The
   reader is deliberately **strict**: a truncated, half-filled or duplicated entry
   raises an error with its line number instead of being skipped, because a
   silently dropped entry leaves a live `{{PERSON_n}}` in text the user believes
   was restored. Files start with a `{"_meta":{"version":1}}` line — readers that
   predate it ignore it (it carries no `token`/`original`), and an unknown version
   is rejected outright rather than parsed into an empty, innocent-looking mapping.
   That gate is what a future passphrase-encrypted mapping would ride on — see
   [mapping-encryption.md](mapping-encryption.md).


9. **Attachments are listed, never opened.** For `.msg` and `.eml`, the body and the
   *attachment file names* are processed — names carry PII often enough to matter
   (`社員名簿_山田太郎_確認用.csv`) — but the attachment bytes are not parsed.
   This is deliberate, not missing work. Expanding them would mean writing the
   attachment plaintext to a temp file (miniz and PDFium both want a path), and
   feeding untrusted, externally-delivered bytes straight into the native parsers
   — exactly the surface [SECURITY.md](../SECURITY.md) singles out. Not expanding
   them also loses nothing: an attachment that is never bundled is never sent to
   the AI, so this is a convenience trade, not a leak. Users who want an
   attachment processed save it and load it as a normal file. Both the GUI (a
   modal dialog) and the CLI (a stderr warning) say so explicitly, because
   silence would read as "attachments were handled".

### Pipeline

- **detect** splits inputs: prose docs go through NER → candidate JSONL; csv/xlsx
  files produce a per-file (per-sheet for xlsx) column skeleton to `--tables-out`,
  with `header_row` auto-detected and columns guessed from the header text.
  Sheets whose columns can't be guessed (forms) are emitted commented-out.
- **mask** ingests every file, applies the confirmed terms (from `--candidates`,
  else auto-detected) plus the table columns through one tokenizer, runs the
  safety gate, and exits non-zero if any raw value survived. Table output is a
  count summary only — raw rows never reach the AI.
- **restore** loads the mapping, reverse-substitutes token text, and warns on any
  leftover tokens.

### Implementation notes / gotchas

- **Windows `argv` is cp932.** Japanese `--sheet` / `--name-cols` (and Japanese
  file paths) arrive mis-encoded through `char** argv`, so column matching
  silently produced nothing. `main()` rebuilds `argv` as UTF-8 from
  `GetCommandLineW` / `CommandLineToArgvW` (the target links `shell32`).
- **`ooxml::sheet_rows` preserves structure.** Unlike `sheet_prose` (which
  compacts non-empty cells and so loses column positions), `sheet_rows` keeps
  every column position and indexes rows by the sheet's `r` attribute — so
  `--header-row N` is the real spreadsheet row number and works on 方眼紙 forms
  whose header is not on row 1.
- **Columns match by header name.** `tokenize_table` compares against header
  strings; the CLI resolves 1-based column indices to header names first.
- **Data-root resolution:** `--data` > `<exe_dir>/models` > `./models`, so the
  CLI inside the portable ZIP finds `models/` next to the exe with no flags.

### Known limitations

- **`.docx` reads the body only** (`word/document.xml`). Headers, footers, footnotes
  and comments are not extracted — so they are never sent to the AI, but they are also
  never reviewed. Do not read a clean result as "the whole file was checked".

- `.eml` bodies are decoded from UTF-8, ISO-2022-JP, Shift_JIS and EUC-JP only.
  Any other charset is reported as an error rather than silently mangled — an
  unreadable body must not look like "no PII found".
- One table sheet per xlsx **file** per invocation — `--tables` keys entries by
  basename, so multiple sheets of the same workbook can't all be active at once.
- No Linux/macOS binaries in the Release yet — build from source.

### Verification

`.github/workflows/ci.yml` builds the whole tree on Linux and runs the
conformance suite plus a detect → mask → restore smoke test, so the
cross-platform claim is checked on every push, not just asserted.

---

## 日本語

`jp-pii-sanitizer-cli` は、GUI と同じ検証済みコアをスクリプトから叩くための
インターフェース。3 つのコマンドを持つ。

- `detect <files…>` → 候補 PII を JSONL 出力（＋表の列雛形）。
- `mask <files…>` → サニタイズ済みテキスト（可逆時は対応表も）。
- `restore --mapping …` → トークン入りテキストを実名に戻す。

### 設計判断

1. **候補ファイルが権威 — レビューはツールの機能ではなく利用者のワークフロー。**
   `mask --candidates f` は `f` の語（＋常時自動のメール／電話／郵便）だけをマスクする。
   対話的な確認機構は**あえて入れていない**。`detect` と `mask` の間で JSONL を編集すること
   *そのもの*がレビュー工程。これで CLI は小さく・合成可能に保てる。`--candidates` 無しの
   `mask` は自動検出して全マスク（opt-out）。

2. **JSON 配列ではなく JSONL。** 1 行 1 オブジェクトなら手編集に強い。行を消せば候補が落ち、
   行を足せば増える。配列の `,`/`[]` で壊れる要素が構造的に無い。`#` コメント・空行は無視。

3. **既定は不可逆。** 可逆にすると対応表（トークン↔実名）を書く。これは本ツールが生む最も機微な
   成果物なので、既定ではディスクに何も書かず、`--reversible --mapping map.jsonl` で明示的に
   可逆を選ぶ。

4. **データ表も prose と対応表を 1 つ共有する。** ここが要の判断。表を*別呼び出し*でマスクすると
   トークン番号が振り直され、docx の `{{PERSON_1}}` と xlsx の `{{PERSON_1}}` が*別人*を指して
   復元が衝突する。だから prose と表は**1 回**の呼び出しで**1 つ**の `Tokenizer` を通す。文書でも
   表でも同一人物は同一トークンになり、対応表 1 つで全体を一貫復元できる。
   （`step5::bundle_blocks` が prose ブロックと `tokenize_table` の両方に同じトークナイザを貫通させて
   いるので、`FileResult` さえ組めば自動で成立する。）

5. **権威ファイルは 2 つ・各々均質。** prose の語（`{"type","text"}`）と表の列
   （`{"file","sheet","header_row","name_cols","company_cols"}`）は形が違う。混ぜずに、prose は
   `--candidates`、表は `--tables` に分けることで、どちらも編集しやすいまま保つ。

6. **依存は最小。** `restore` はモデルも辞書も不要（純粋なトークン置換）。`mask --candidates` は
   Sudachi のみ。~530MB の NER モデルを積むのは `detect` とワンショット `mask` だけ。よく使う
   マスク経路が軽く速い。

7. **GUI facade ではなくライブラリヘッダを直接再利用。** `core.hpp` は WebView2 用の facade で
   あえて Aspose を取り込まない。CLI はライブラリヘッダを直接 include し（＝`.msg` を取り込める・
   TU に Aspose が入る）、WebView2 をリンクしない。これが Linux/macOS でもビルド・実行できる理由。

8. **対応表のシリアライザは 1 本・GUI と共有。** `mapping_io.hpp` が読み書きの両方を持ち、
   GUI も JavaScript で組み立てずに同じ関数を呼ぶ。実装が 2 つあれば書式も 2 つになり、
   実際 GUI は CSV を保存していたため、その対応表を `restore` に渡すと 1 行目で落ちていた。
   リーダーは意図的に**厳格**で、壊れた行・片側だけの行・重複を読み飛ばさず行番号を添えて
   落とす。黙って落とすと、利用者が「実名に戻した」と思っているテキストに `{{PERSON_n}}` が
   生き残るため。ファイル先頭には `{"_meta":{"version":1}}` を置く。この行は
   `token`/`original` を持たないので旧リーダーは素通りし、逆に未知バージョンは
   「空の対応表として正常終了」させず明示的に拒否する。将来のパスフレーズ暗号化は
   この gate に乗る形になる — [mapping-encryption.md](mapping-encryption.md)。


9. **添付は一覧に出すだけで、開かない。** `.msg` と `.eml` は本文と*添付ファイル名*を対象にする
   （`社員名簿_山田太郎_確認用.csv` のようにファイル名自体が PII を持つため）が、添付の
   中身は解析しない。**未実装ではなく意図的な判断**。展開するには添付の平文を一時ファイルへ
   書く必要があり（miniz も PDFium もパスを要求する）、外部から届いた信頼できないバイト列を
   そのままネイティブパーサに食わせることになる——[SECURITY.md](../SECURITY.md) が名指しで
   挙げている面そのもの。しかも展開しなくても失うものは無い: 束ねに入らない添付は AI にも
   送られないので、これは漏洩ではなく利便性の取引でしかない。添付を処理したい利用者は、
   保存して普通のファイルとして読み込む。黙っていると「添付も処理された」と読まれるので、
   GUI はモーダルダイアログで、CLI は stderr で明示する。

### パイプライン

- **detect** は入力を分ける。prose 文書は NER →候補 JSONL、csv/xlsx はファイル単位（xlsx は
  シート単位）の列雛形を `--tables-out` に出力（`header_row` 自動検出・見出し語から列を推測）。
  列を推測できないシート（フォーム）はコメント行で出す。
- **mask** は全ファイルを取り込み、確定語（`--candidates`、無ければ自動検出）＋表の列を 1 つの
  トークナイザで処理し、安全ゲートを走らせ、生の値が残れば非ゼロ終了する。表の出力は件数要約のみ
  ＝生の行は AI に渡らない。
- **restore** は対応表を読み、トークンを逆置換し、未置換トークンがあれば警告する。

### 実装メモ（ハマりどころ）

- **Windows の `argv` は cp932。** 日本語の `--sheet`／`--name-cols`（と日本語ファイルパス）が
  `char** argv` 経由で誤エンコードされ、列マッチが静かに空振りした。`main()` で
  `GetCommandLineW`／`CommandLineToArgvW` から `argv` を UTF-8 で作り直す（`shell32` をリンク）。
- **`ooxml::sheet_rows` は構造を保つ。** `sheet_prose`（非空セルを詰めて列位置を失う）と違い、
  `sheet_rows` は列位置を保ち、行をシートの `r` 属性で位置づける。だから `--header-row N` は実際の
  スプレッドシート行番号として機能し、見出しが 1 行目に無い方眼紙フォームでも効く。
- **列は見出し名で照合。** `tokenize_table` は見出し文字列と比較する。CLI は 1 始まり列番号を先に
  見出し名へ解決する。
- **データルート解決:** `--data` > `<exe_dir>/models` > `./models`。ポータブル ZIP 内の CLI は
  exe 隣の `models/` をフラグ無しで見つける。

### 既知の未対応

- **`.docx` は本文だけ**（`word/document.xml`）。ヘッダー・フッター・脚注・コメントは
  抽出しない。**AI に送られることも無い代わりに、確認もされない。** 結果が綺麗だった
  ことを「ファイル全体を見た」と読み替えないこと。

- `.eml` の本文は UTF-8 / ISO-2022-JP / Shift_JIS / EUC-JP のみ復号する。それ以外の
  charset は黙って化けさせず**エラーとして報告する**——読めなかった本文が
  「PII が無かった」に見えるのが一番まずいため。
- xlsx **ファイル**あたり 1 呼び出しで表シート 1 枚 — `--tables` は basename でキーを引くため、
  同一ブックの複数シートを同時に有効化できない。
- Linux/macOS バイナリは Release に未添付 — ソースからビルドする。

### 検証

`.github/workflows/ci.yml` が Linux で全ツリーをビルドし、適合試験一式＋
detect → mask → restore のスモークを回す。クロスプラットフォーム性は主張ではなく
毎 push で検証している。
