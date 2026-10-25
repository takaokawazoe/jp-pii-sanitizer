# excel/ — Excel add-in (Excel-DNA)

**Language:** English (below) · [日本語](#日本語)

Design, the helper protocol and the reasons behind the shape live in
[../docs/excel-helper.md](../docs/excel-helper.md). This file is just how to build and run.

---

## English

The add-in is a thin Excel-DNA XLL (.NET Framework 4.8, 64-bit) that reads cell values,
talks to `jp-pii-helper.exe` over a named pipe, and writes a sanitized copy of the sheet
into a new workbook. All detection and masking happens in the helper.

### Build

Needs the .NET SDK (8.x is fine) and network access to NuGet on the first build.
The .NET Framework 4.8 reference assemblies come from NuGet automatically.

```powershell
cd excel\JpPiiSanitizer.Excel
dotnet build -c Release
# → bin\Release\net48\publish\JpPiiSanitizer.Excel-AddIn64-packed.xll  (single file, dependencies packed)
```

### Run during development

The XLL looks for `jp-pii-helper.exe` and `models\` next to itself. While developing,
put a `jp-pii-sanitizer.config.json` next to the XLL instead (git-ignored):

```json
{"helper": "C:\\tools\\jp-pii-sanitizer\\cpp\\build\\jp-pii-helper.exe",
 "data":   "C:\\tools\\jp-pii-sanitizer\\cpp\\models"}
```

Then open the `.xll` in Excel (File → Open, or Options → Add-ins → Go… → Browse).
A **PII** tab appears on the ribbon with two buttons: **サニタイズ** opens the task pane,
**実名に戻す** opens a small separate window for restoring real names. It has two tabs that
share the mapping file and passphrase: **文章を戻す** (paste an AI reply, copy the result) and
**シートを戻す** (rewrite the placeholders in the active sheet, or the selected range, in place —
for a table the AI returned that you pasted into a sheet, or a workbook the AI edited). The sheet
tab first selects the cells that will change and asks for confirmation, because writes from an
add-in cannot be undone in Excel; formula cells are never overwritten.

### Pane layout

The pane shows only the main path: **読み込んで検出** → the column grid (with the header row
box on the same line) → the free-text candidate list → 可逆 / パスフレーズで保護 → a link that
selects every cell that will change → **サニタイズして別ブックに保存…**. Rarely used things
(adding a term by hand, showing the values of kind columns one by one, reset) are folded under
**詳細**. In the candidate list, one click selects the cells where that term would be masked
(so you can see the context), a second click on the same row unchecks / rechecks it.

### Output

**サニタイズして別ブックに保存…** asks once where to save the sanitized `.xlsx` (default:
next to the source workbook as `<name>_sanitized.xlsx`). In reversible mode the mapping is
written automatically to a `対応表\` subfolder next to it (`対応表\<name>_sanitized.jsonl`),
so it is always produced together with the workbook but never shows up in the file list when
you pick the `.xlsx` to attach. Nothing is created if the dialog is cancelled or the mapping
cannot be written.

### Column rules and their memory

After **読み込んで検出** the pane lists the columns of the range with a kind each
(自動検出 / 人名 / 組織 / 住所 / 電話 / メール / 識別子 / 対象外). Kinds are proposed from
the header text, from the detection density and from what you chose last time; you confirm
or change them. The header row is taken from the Excel table if the range is one, otherwise
guessed (first of the top 10 rows that is mostly text and has no numbers/dates) and shown in
**見出し行**, where you can override it (0 = no header). Column rules apply below the header
row; the header row and anything above it (title rows) only get the normal detection.
Values of person/organization/location columns are hidden from the candidate list by default
(**列指定の列の値も候補一覧に出す** under **詳細** shows them, so a single value can be kept);
placeholder values such as `-` or `なし` are left unmasked from the start. The link
**マスク対象 N セルをシートで選択して確認する** selects every cell the sanitize step would
change — that is the preview. On **サニタイズ** the header→kind assignment is remembered in
`%LOCALAPPDATA%\jp-pii-sanitizer\excel-column-rules.json` (header names and kinds only,
never cell values) and proposed again for the next table with the same headers.

### Installer (per-user, no admin)

`installer/build_installer.ps1` builds the helper and the XLL, stages them with the models
and runs Inno Setup 6 (`winget install -e --id JRSoftware.InnoSetup`):

```powershell
powershell -ExecutionPolicy Bypass -File excel\installer\build_installer.ps1            # version from the latest git tag
powershell -ExecutionPolicy Bypass -File excel\installer\build_installer.ps1 -Version 0.7.0
# → excel\dist\jp-pii-sanitizer-excel-<version>-setup.exe  (≈520 MB, the model dominates)
```

The setup (`installer/jp-pii-sanitizer-excel.iss`) needs no admin rights and touches only
the current user:

```
%LOCALAPPDATA%\jp-pii-sanitizer\
 ├ jp-pii-sanitizer.xll
 ├ jp-pii-helper.exe
 ├ onnxruntime.dll, onnxruntime_providers_shared.dll
 ├ models\            (model_fp16.onnx, tokenizer.json, labels.json, patterns.json, sudachi\)
 ├ README.txt
 └ uninstall\
```

- registers the folder as a **Trusted Location** (`HKCU\…\Excel\Security\Trusted Locations\LocationN`)
  so the unsigned XLL loads without a prompt;
- registers the XLL for **automatic loading** at Excel start (`HKCU\…\Excel\Options\OPENn`, kept
  contiguous), so the **PII** tab is simply there;
- refuses 32-bit Office, asks to close Excel while the files are replaced;
- the uninstaller removes both registrations and the files, but leaves
  `excel-column-rules.json` (the user's column memory).

CI (`.github/workflows/ci.yml`, job `windows`) runs the same script on every push and keeps
the setup as a workflow artifact for 14 days; on a `v*` tag it attaches the setup to a
**draft** Release, which is published by hand after checking it.

### Without the installer (portable zip)

`build_installer.ps1 -Portable` (CI always passes it) also produces
`jp-pii-sanitizer-excel-<version>-portable.zip`: the same files plus a step-by-step
`README.txt` (`installer/README-portable.txt`). The user does by hand what the setup does:
unblock the zip before extracting (Mark of the Web, otherwise Excel refuses the XLL),
extract to a writable folder, add that folder as a Trusted Location in the Trust Center,
and pick the XLL under Options → Add-ins → Excel Add-ins → Browse. No registry editing,
no admin, no executable to run.

The setup is unsigned, so a downloaded copy shows SmartScreen once (詳細情報 → 実行).
For development, uninstall it first (or point `jp-pii-sanitizer.config.json` at the build
tree) — otherwise Excel loads both the installed and the development XLL.
Silent install for scripting: `setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.

---

## 日本語

アドインは薄い Excel-DNA の XLL（.NET Framework 4.8・64 ビット）。セルの値を読み、
`jp-pii-helper.exe` と名前付きパイプで話し、サニタイズ済みのシートを別ブックに書く。
検知もマスクも全部ヘルパー側。

### ビルド

.NET SDK（8.x で可）と、初回は NuGet へのネットワークが要る。
.NET Framework 4.8 の参照アセンブリは NuGet から自動で来る。

```powershell
cd excel\JpPiiSanitizer.Excel
dotnet build -c Release
# → bin\Release\net48\publish\JpPiiSanitizer.Excel-AddIn64-packed.xll（1 ファイル・依存を同梱）
```

### 開発中の動かし方

XLL は自分の隣の `jp-pii-helper.exe` と `models\` を探す。開発中は代わりに XLL の隣へ
`jp-pii-sanitizer.config.json` を置く（git 管理外）:

```json
{"helper": "C:\\tools\\jp-pii-sanitizer\\cpp\\build\\jp-pii-helper.exe",
 "data":   "C:\\tools\\jp-pii-sanitizer\\cpp\\models"}
```

Excel で `.xll` を開く（ファイル → 開く、またはオプション → アドイン → 設定 → 参照）。
リボンに **PII** タブが出る。ボタンは 2 つ: **サニタイズ** でタスクペインが開き、
**実名に戻す** で実名に戻すための小さな別ウィンドウが開く。タブは 2 つで、対応表と
パスフレーズは共有: **文章を戻す**（AI の返答を貼って結果をコピー）と **シートを戻す**
（アクティブシート、範囲選択があればその範囲の伏せ字をその場で書き換える。AI が返した表を
貼ったシートや、AI が編集して返したブック向け）。シートのほうは先に変わるセルを選択して
見せ、確認を取ってから書く——アドインからの書き込みは Excel の「元に戻す」で取り消せない
ため。数式セルは書き換えない。

### ペインの構成

ペインに出すのは主経路だけ: **読み込んで検出** → 列の表（同じ行に見出し行の指定）→
自由記述の候補 → 可逆／パスフレーズで保護 → 変わるセルを全部選択するリンク →
**サニタイズして別ブックに保存…**。たまにしか使わないもの（語の手動追加、列指定の列の値の
個別表示、リセット）は **詳細** に畳んである。候補一覧は 1 回クリックでその語が伏せられる
セルをシートで選択（文脈を見る）、同じ行をもう 1 回クリックでチェックを外す／戻す。

### 出力

**サニタイズして別ブックに保存…** は、サニタイズ済み `.xlsx` の保存先を 1 回だけ聞く
（既定は元ブックの隣に `<名前>_sanitized.xlsx`）。可逆モードなら対応表はその隣の
`対応表\` フォルダに自動で書く（`対応表\<名前>_sanitized.jsonl`）。ブックと必ず対で
できる一方、添付する `.xlsx` を選ぶ一覧には並ばない。ダイアログをキャンセルしたとき、
対応表が書けなかったときは何も作らない。

### 列の扱いとその記憶

**読み込んで検出** のあと、ペインに範囲の列が並び、列ごとに種別を選べる
（自動検出／人名／組織／住所／電話／メール／識別子／対象外）。種別は見出し語・検出結果の
密度・前回の選択から提案され、利用者が確認・修正する。見出し行は、範囲が Excel の
テーブルならその見出し行、そうでなければ推定（先頭 10 行のうち、ほぼ文字列で数値・日付の
無い最初の行）で **見出し行** に入り、数値で上書きできる（0 = 見出しなし）。列指定が
掛かるのは見出し行より下。見出し行とその上（タイトル行）は通常の検出だけ。
人名・組織・住所の列の値は候補一覧に既定では出さない（**詳細** の
**列指定の列の値も候補一覧に出す** で出して 1 件だけ残すことができる）。`-` や `なし` の
ような定型値は最初から伏せない。リンク **マスク対象 N セルをシートで選択して確認する** は
サニタイズで変わるセルを全部選択する＝これがプレビュー。**サニタイズ** を実行すると
見出し → 種別 の対応を `%LOCALAPPDATA%\jp-pii-sanitizer\excel-column-rules.json` に
覚え（見出し名と種別だけ。セルの値は書かない）、次に同じ見出しの表を読んだときの提案に使う。

### インストーラ（per-user・管理者不要）

`installer/build_installer.ps1` がヘルパーと XLL をビルドし、モデルと一緒に集めて
Inno Setup 6（`winget install -e --id JRSoftware.InnoSetup`）で固める:

```powershell
powershell -ExecutionPolicy Bypass -File excel\installer\build_installer.ps1            # 版は直近の git タグ
powershell -ExecutionPolicy Bypass -File excel\installer\build_installer.ps1 -Version 0.7.0
# → excel\dist\jp-pii-sanitizer-excel-<version>-setup.exe（約 520 MB。ほぼモデル）
```

setup（`installer/jp-pii-sanitizer-excel.iss`）は管理者権限を要らず、現在のユーザーにしか触らない:

```
%LOCALAPPDATA%\jp-pii-sanitizer\
 ├ jp-pii-sanitizer.xll
 ├ jp-pii-helper.exe
 ├ onnxruntime.dll, onnxruntime_providers_shared.dll
 ├ models\            （model_fp16.onnx, tokenizer.json, labels.json, patterns.json, sudachi\）
 ├ README.txt
 └ uninstall\
```

- このフォルダを **信頼できる場所** に登録する（`HKCU\…\Excel\Security\Trusted Locations\LocationN`）
  ——未署名の XLL をダイアログ無しで読ませるため
- XLL を Excel の **起動時読み込み** に登録する（`HKCU\…\Excel\Options\OPENn`。連番を保つ）
  ——起動すれば **PII** タブがある状態になる
- 32 ビット Office は拒否。ファイルを置き換える間は Excel を閉じてもらう
- アンインストーラは両方の登録とファイルを外すが、`excel-column-rules.json`（列の記憶）は残す

CI（`.github/workflows/ci.yml` の `windows` ジョブ）が push のたびに同じスクリプトを回し、
setup をワークフローの成果物として 14 日残す。`v*` のタグでは Release の**ドラフト**に
添付する——公開は中身を確かめてから手で行う。

### インストーラを使わない（ポータブル zip）

`build_installer.ps1 -Portable`（CI は常に付ける）は `jp-pii-sanitizer-excel-<version>-portable.zip`
も作る。中身は同じファイルと、手順書の `README.txt`（`installer/README-portable.txt`）。
setup がやることを利用者が手で行う: 展開前に zip のブロックを解除（Mark of the Web が
付いたままだと Excel が XLL を読まない）、書き込める場所に展開、トラストセンターで
そのフォルダを信頼できる場所に追加、オプション → アドイン → Excel アドイン → 参照 で
XLL を選ぶ。レジストリ編集なし・管理者不要・実行する exe なし。

setup は未署名なので、ダウンロードした直後は SmartScreen が 1 回出る（詳細情報 → 実行）。
開発中は先にアンインストールしておく（または `jp-pii-sanitizer.config.json` でビルドツリーを
指す）。そうしないと Excel がインストール済みと開発中の XLL を両方読む。
スクリプトから入れるなら `setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`。
