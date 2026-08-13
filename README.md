# jp-pii-sanitizer

A native, offline Japanese PII sanitizer for Windows.
Windows 向けの、ネイティブ・オフラインの日本語 PII サニタイザ。

**Language:** English (below) · [日本語](#日本語)

---

## English

It detects personal information in office documents, replaces it with
placeholders before you send text to an external AI, and can restore the real
values afterward. It runs fully on-device — no network calls, no data leaves the
machine. The detection core is a single Japanese NER model (ONNX) plus
rule-based recognizers.

> ⚠️ **This tool reduces the risk of leaking personal information. It does not
> guarantee that every piece of PII is removed.** See [Disclaimer](#disclaimer).

### What it does

1. **Load** one or more files (`.docx`, `.pptx`, `.xlsx`, `.pdf`, `.csv`, `.txt`).
2. **Detect** candidate PII — person names, organizations, addresses, e-mail
   addresses, phone numbers, postal codes.
3. **Confirm** (opt-out): everything is masked by default; you only *uncheck*
   what should stay, and can *manually add* anything the detector missed.
4. **Sanitize** — replace detected PII with placeholders and download the result.
   - *Reversible*: `{{PERSON_1}}` with a mapping CSV you keep private.
   - *Irreversible*: redaction like `[人名1]` (no mapping kept).
   - A **safety gate** re-scans the output and warns if any raw value survived.
5. **Restore** — paste the AI's reply plus the mapping CSV to get real values back.

The confirmation UI highlights exactly what will be masked (the same matcher the
sanitizer uses), so the preview and the result always agree.

### Design in brief

- Detection uses `tsmatz/xlm-roberta-ner-japanese` (MIT) exported to ONNX (fp16),
  plus PCRE2 recognizers and Sudachi morphology.
- **Opt-out, not opt-in.** Over-masking is safe; under-masking is dangerous.

### Download and run (Windows 11+)

1. Download `jp-pii-sanitizer-win-x64.zip` from [Releases](../../releases).
2. Extract it to a writable folder (Desktop, Downloads, …).
3. Double-click `jp-pii-sanitizer.exe`.

- **Windows 11 or later** — the WebView2 runtime ships with Windows 11.
- **No administrator rights required** — it writes only to `%LOCALAPPDATA%`.
- **Unsigned build.** SmartScreen may show "Windows protected your PC" on first
  launch; choose **More info → Run anyway** (no admin needed), or build it
  yourself.

### Command-line interface (CLI)

Besides the GUI there is a scriptable CLI, `jp-pii-sanitizer-cli`, for batch and
headless use. Unlike the GUI (Windows/WebView2), the CLI builds and runs on
Linux/macOS too. It ships in the portable ZIP next to the GUI.

```sh
# 1. detect candidates for review
jp-pii-sanitizer-cli detect report.docx notes.pdf -o candidates.jsonl

# 2. edit candidates.jsonl (delete false positives, add misses), then mask
jp-pii-sanitizer-cli mask report.docx notes.pdf --candidates candidates.jsonl \
    --reversible --mapping mapping.jsonl -o masked.txt

# 3. after the AI replies, restore real values
jp-pii-sanitizer-cli restore --mapping mapping.jsonl -i ai_reply.txt -o final.txt
```

- **The candidate file is the authority.** `mask --candidates f` masks exactly
  the terms in `f` (plus always-automatic e-mail/phone/postal) and needs no
  model. Human review is your workflow: edit the JSONL between `detect` and
  `mask`. With no `--candidates`, `mask` auto-detects and masks everything
  (opt-out).
- **Default is irreversible** (`[人名1]`-style, no mapping written). Add
  `--reversible --mapping map.jsonl` to keep a private mapping for `restore`.
- **Candidate JSONL** is one object per line —
  `{"type":"PERSON","text":"田中 太郎"}` (`type` ∈ PERSON/ORGANIZATION/LOCATION).
- **Data tables (csv/xlsx).** Prose and tables are masked in one invocation so
  they share one mapping (the same person in a document and a spreadsheet gets
  the same token). Give columns per file via a `--tables` JSONL —
  `{"file":"list.csv","header_row":1,"name_cols":["氏名"],"company_cols":["取引先"]}`
  — or for a single table use flags: `--table --header-row N --sheet NAME
  --name-cols A,B --company-cols C` (columns by header name or 1-based index).
  `detect … --tables-out tables.jsonl` writes a starter you can edit. Table
  output is a count summary only; raw rows never reach the AI.
- Formats: `.docx .pptx .xlsx .pdf .csv .txt .msg`. Run `--help` for all options.
  The mapping file is your most sensitive artifact — treat it like a password
  and delete it when done.

### Build from source

See [cpp/README.md](cpp/README.md). In short: Linux/WSL (for the conformance
tests) or Windows/MSVC (for the shipping app); fetch the C++ dependencies and
the model/dictionary bundle, then `cmake --build`. The large assets (~530 MB
ONNX model, tokenizer, Sudachi dictionary) are **not** in this repository — they
are attached to the Release and fetched by a script.

### Disclaimer

This software is provided under the Apache License 2.0 on an "AS IS" basis,
without warranties of any kind. It assists with masking personal information but
**does not guarantee that all PII is detected or removed.** False negatives and
false positives both occur. Structured identifiers (account numbers, employee
IDs, dates of birth, card numbers) are **out of scope.** The default is to mask
everything and let the operator opt out — **a human must review the output
before it is sent anywhere.** You are solely responsible for verifying the
result.

### License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Third-party
components and the NER model remain under their own licenses; see [NOTICE](NOTICE).
All sample and test data is synthetic — no real personal information is included.

---

## 日本語

オフィス文書に含まれる個人情報を検出し、社内外の AI に渡す前にプレースホルダへ置き換え、
後から実名に戻せます。すべて端末内で完結し、ネットワーク通信は行わず、データは外に出ません。
検知の中核は単一の日本語 NER モデル（ONNX）＋ルールベース認識器です。

> ⚠️ **本ツールは個人情報の漏えいリスクを下げますが、すべての PII の除去を保証しません。**
> [免責事項](#免責事項)を参照してください。

### できること

1. **読み込み**: 1つ以上のファイル（`.docx` / `.pptx` / `.xlsx` / `.pdf` / `.csv` / `.txt`）。
2. **検出**: 人名・組織名・住所・メールアドレス・電話番号・郵便番号の候補。
3. **確定（opt-out）**: 既定は全マスク。残すものだけ*チェックを外し*、検出漏れは*手動で追記*できます。
4. **サニタイズ**: 検出した PII をプレースホルダに置換して結果をダウンロード。
   - *可逆*: `{{PERSON_1}}` ＋ 対応表 CSV（厳重管理）。
   - *不可逆*: `[人名1]` の墨消し（対応表なし）。
   - **安全ゲート**が出力を再走査し、生の値が残っていれば警告します。
5. **逆置換**: AI の返答と対応表 CSV を貼り付けて実名に戻します。

確定画面は、実際にマスクされる箇所（サニタイザと同じマッチャ）をハイライトするので、
プレビューと結果が常に一致します。

### 設計の要点

- 検知は `tsmatz/xlm-roberta-ner-japanese`（MIT）を ONNX(fp16) 化
  したもの＋ PCRE2 認識器＋ Sudachi 形態素。
- **opt-in ではなく opt-out。** 過剰マスクは安全側、取りこぼしは危険側。

### ダウンロードと実行（Windows 11 以降）

1. [Releases](../../releases) から `jp-pii-sanitizer-win-x64.zip` をダウンロード。
2. 書き込みできる場所（デスクトップ、ダウンロード等）に展開。
3. `jp-pii-sanitizer.exe` をダブルクリック。

- **Windows 11 以降**（WebView2 ランタイムが標準搭載）。
- **管理者権限は不要**（`%LOCALAPPDATA%` にのみ書き込み）。
- **未署名**のため初回に SmartScreen 警告が出ることがあります。［詳細情報］→［実行］で起動
  できます（管理者権限不要）。あるいは自分でビルドしてください。

### コマンドラインインターフェース（CLI）

GUI とは別に、バッチ・ヘッドレス用途向けの CLI `jp-pii-sanitizer-cli` があります。
GUI（Windows/WebView2）と違い、CLI は Linux/macOS でもビルド・実行できます。
ポータブル ZIP に GUI と同梱されています。

```sh
# 1. 候補を検出（レビュー用）
jp-pii-sanitizer-cli detect report.docx notes.pdf -o candidates.jsonl

# 2. candidates.jsonl を編集（誤検知を削除・漏れを追記）してマスク
jp-pii-sanitizer-cli mask report.docx notes.pdf --candidates candidates.jsonl \
    --reversible --mapping mapping.jsonl -o masked.txt

# 3. AI の返答を実名に戻す
jp-pii-sanitizer-cli restore --mapping mapping.jsonl -i ai_reply.txt -o final.txt
```

- **候補ファイルが権威。** `mask --candidates f` は `f` の語（＋常時自動のメール／電話／
  郵便）だけをマスクし、モデルは不要。レビューはあなたのワークフロー＝`detect` と `mask`
  の間で JSONL を編集します。`--candidates` 無しの `mask` は自動検出して全マスク（opt-out）。
- **既定は不可逆**（`[人名1]` 形式・対応表を書かない）。`--reversible --mapping map.jsonl`
  で `restore` 用の対応表を残します。
- **候補 JSONL** は 1 行 1 オブジェクト —
  `{"type":"PERSON","text":"田中 太郎"}`（`type` は PERSON／ORGANIZATION／LOCATION）。
- **データ表（csv／xlsx）。** prose と表を 1 回の呼び出しでマスクするので対応表を共有します
  （文書と表で同じ人物は同じトークン）。列はファイルごとに `--tables` JSONL で指定 —
  `{"file":"list.csv","header_row":1,"name_cols":["氏名"],"company_cols":["取引先"]}`
  — 単一表ならフラグ `--table --header-row N --sheet NAME --name-cols A,B
  --company-cols C`（列は見出し名 or 1始まり番号）。`detect … --tables-out tables.jsonl`
  で編集用の雛形を出力できます。表の出力は件数要約のみで、生の行は AI に渡りません。
- 対応形式: `.docx .pptx .xlsx .pdf .csv .txt .msg`。全オプションは `--help`。
  対応表ファイルは最も機微な成果物です。パスワードのように扱い、不要になったら削除してください。

### ソースからのビルド

[cpp/README.md](cpp/README.md) を参照。要約: Linux/WSL（適合試験）または Windows/MSVC
（配布アプリ）で、C++ 依存とモデル/辞書を取得して `cmake --build`。容量の大きいアセット（約 530MB の
ONNX モデル・トークナイザ・Sudachi 辞書）は**リポジトリに含めず**、Release に添付して取得
スクリプトで配置します。

### 免責事項

本ソフトウェアは Apache License 2.0 のもと「現状有姿」で提供され、いかなる保証もありません。
個人情報のマスクを支援しますが、**すべての PII の検出・除去を保証するものではありません。**
検出漏れ（誤陰性）も過検出（誤陽性）も起こり得ます。口座番号・社員番号・生年月日・カード番号
などの構造的な識別子は**対象外**です。既定は全マスク＋opt-out ですが、**送信前に必ず人が出力を
確認してください。** 結果の検証責任は利用者にあります。

### ライセンス

Apache License 2.0（[LICENSE](LICENSE) / [NOTICE](NOTICE)）。第三者コンポーネントと NER モデルは
各自のライセンスに従います（[NOTICE](NOTICE)）。サンプル・テストデータは全て合成で、実在の
個人情報は含みません。
