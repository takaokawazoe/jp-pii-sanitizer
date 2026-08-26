# cpp/ — native C++ build

**Language:** English (below) · [日本語](#日本語)

Build instructions. The rationale behind the design is in [../DESIGN.md](../DESIGN.md).

---

## English

The core (NER inference, morphology, detection, masking, reverse) and every input
format (docx/pptx/xlsx/csv/txt/pdf/msg) were ported to C++ and checked stage by
stage against a Python reference implementation. **The conformance tests are
green on both WSL/Linux and Windows/MSVC.**

> **On what the oracles mean now.** `cpp/testdata/*_ref.json` were generated from
> that Python implementation, which reached the numbers below and is now parked —
> development continues in C++ only. The tests therefore no longer *prove* parity
> with a second implementation on every run; they pin the behaviour that was
> verified against it. Read them as regression tests: they catch unintended
> change, and an intentional change updates them (`phase2_pipeline --update`) with
> the diff as the thing under review.

| Executable | Checks | Result |
|---|---|---|
| `phase0_tokenize` | text → input_ids matches transformers | **39/39** |
| `phase0_parity` | input_ids → labels matches torch | **4873/4873** |
| `phase1_sudachi` | morphology / name-core matches SudachiPy | **398/398 · 120/120 · 120/120** |
| `phase2_regex` | 8 regexes match Python `re` | **240/240** |
| `phase2_hf` | HF simple aggregation matches Python | **267/267** |
| `phase2_pipeline` | full detection pipeline candidates match Python | **155/155** |
| `phase3_tokenize` | tokenize / reverse / furigana folding match Python | readings 68/68 |
| `phase4_extract` | extracted text (docx/pptx/xlsx/csv/txt/pdf) matches Python | **8/8** |
| `phase4_msg` | .msg loses none of Python's detected PII | 10/10 |
| `phase5_core` | safety gate / highlight / bundling match Python | spans 5/5 · gate 10/10 |

Candidate-set equality = the eval result (174/175) by construction. The GUI app
(`jp-pii-sanitizer.exe`, CMake target `jp_pii_sanitizer`) is these verified cores hosted in WebView2.

There is also a cross-platform CLI — CMake target `jp_pii_sanitizer_cli`, exe
`jp-pii-sanitizer-cli` — built by the same `cmake --build build`. It reuses the
library headers directly (no WebView2), so it also builds and runs on
Linux/macOS. CI builds it and smoke-tests a `detect → mask → restore`
round-trip, and `package_win.ps1` ships it in the portable ZIP next to the GUI.
Usage and design/implementation notes: [../docs/cli.md](../docs/cli.md).

### Prerequisites

**Linux / WSL** (where the conformance tests run):

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config unzip
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y   # Rust (cargo)
```

**Windows** (to build the shipping app): see "Windows (MSVC) build" below.

Both need Rust (cargo): although this is the "C++ version", the tokenizer and
Sudachi shims are Rust.

### 1. Fetch third-party dependencies (~130 MB, git-ignored)

Not stored in the repo; place them under `cpp/third_party/`. Versions are pinned
(same binary line as the Python side that produced the oracles).

```bash
cd cpp/third_party

# ONNX Runtime 1.27.0 (Windows: onnxruntime-win-x64-1.27.0.zip into the same place)
curl -fsSL -O https://github.com/microsoft/onnxruntime/releases/download/v1.27.0/onnxruntime-linux-x64-1.27.0.tgz
tar xzf onnxruntime-linux-x64-1.27.0.tgz && mv onnxruntime-linux-x64-1.27.0 onnxruntime && rm onnxruntime-linux-x64-1.27.0.tgz

# nlohmann/json (single header)
curl -fsSL -o json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

# PCRE2 (dev headers are not installed system-wide; build from source, no sudo)
curl -fsSL -O https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz
tar xzf pcre2-10.44.tar.gz && mv pcre2-10.44 pcre2 && rm pcre2-10.44.tar.gz

# miniz + pugixml (OOXML = ZIP + XML)
curl -fsSL -O https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
mkdir -p miniz && (cd miniz && unzip -o ../miniz-3.0.2.zip) && rm miniz-3.0.2.zip
curl -fsSL -O https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
tar xzf pugixml-1.14.tar.gz && mv pugixml-1.14 pugixml && rm pugixml-1.14.tar.gz

# PDFium (pinning matters: older builds don't apply ToUnicode and some Japanese
# PDFs return glyph IDs). Windows: pdfium-win-x64.tgz into the same place.
curl -fsSL -o pdfium.tgz https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F7947/pdfium-linux-x64.tgz
mkdir pdfium && tar xzf pdfium.tgz -C pdfium && rm pdfium.tgz

# Aspose.Email FOSS (.msg) — vendor and apply the patch (upstream is unmaintained)
git clone --depth 1 https://github.com/aspose-email-foss/Aspose.Email-FOSS-for-CPP aspose-email
(cd aspose-email && git apply ../../patches/aspose-any_to_string-binary.patch)
```

**libsodium** (mapping encryption — Argon2id + XChaCha20-Poly1305) is handled per
platform rather than vendored for both, because upstream publishes an MSVC build
but expects a package manager elsewhere:

```bash
# Linux / macOS: use the distribution package
sudo apt-get install -y libsodium-dev     # or: brew install libsodium
# No sudo? build it and point CMake at the prefix:
#   ./configure --prefix=$HOME/opt/libsodium --disable-shared && make && make install
#   cmake -S cpp -B cpp/build -DSODIUM_ROOT=$HOME/opt/libsodium
```

```powershell
# Windows: the official MSVC build, into cpp/third_party/libsodium/ (statically
# linked, so no extra DLL lands in the portable ZIP)
curl -fsSL -o libsodium-msvc.zip https://download.libsodium.org/libsodium/releases/libsodium-1.0.20-stable-msvc.zip
Expand-Archive libsodium-msvc.zip -DestinationPath . ; Remove-Item libsodium-msvc.zip
```

The patch fixes `any_to_string()` returning empty for PT_BINARY (a real Outlook
HTML body cannot be read otherwise). To build
the Windows GUI you also need the WebView2 SDK (NuGet `Microsoft.Web.WebView2`
into `third_party/webview2/`).

### 2. Fetch the model and dictionary (~750 MB, from the Release)

Both the tests and the GUI need the NER model, tokenizer, and Sudachi
dictionary. These are large, so they are **attached to a GitHub Release** and
placed into `cpp/models/` by a script:

```bash
# Linux / WSL
JPPII_REPO=takaokawazoe/jp-pii-sanitizer ./cpp/tools/fetch_assets.sh
```
```powershell
# Windows
$env:JPPII_REPO='takaokawazoe/jp-pii-sanitizer'
powershell -ExecutionPolicy Bypass -File cpp/tools/fetch_assets.ps1
```

After extraction, `cpp/models/` holds `model_fp16.onnx` (~530 MB),
`tokenizer.json` (17 MB), `labels.json` / `patterns.json` (also in the repo), and
`sudachi/` (SudachiDict-core, ~210 MB). The conformance oracles
(`testdata/*.json`) are **committed** and need no regeneration; `sudachi_ref.json`
uses relative `models/sudachi/...` paths that resolve when tests run from `cpp/`.

### 3. Build and run (Linux / WSL)

```bash
cd cpp
export PATH="$HOME/.cargo/bin:$PATH"      # cargo (Rust shims)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                       # first build compiles Rust — a few minutes

./build/phase0_tokenize                           # → 39/39
./build/phase0_parity models/model_fp16.onnx      # → 4873/4873
# same for the other phase* — PASS with exit code 0 means success (CI-ready).
```

Run with `cpp/` as the working directory (oracles use relative paths).

### 4. Windows (MSVC) build

The shipping app (`jp-pii-sanitizer.exe`) and all phase tests are green on Windows
with the same oracles.

Toolchain: **VS 2022 Build Tools** (MSVC v143, "Desktop development with C++"),
**CMake / Ninja**, **Rust (msvc)** (`rustup default stable-x86_64-pc-windows-msvc`).
If **Smart App Control** is on, `rustc` dies with `0xc0e90002` (it blocks the
unsigned `rustc_driver-*.dll`) — turn it off. Fetch third-party per §1 (Windows
variants) and the model per §2 (`fetch_assets.ps1`).

To put `cl` and `cargo` on PATH, call `cpp\winenv.bat` (vcvars64 +
`%USERPROFILE%\.cargo\bin`) before cmake. `winenv.bat` is **pure ASCII** (cmd
reads .bat in cp932):

```bat
call winenv.bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

Build the portable ZIP with `powershell -File tools\package_win.ps1`.

**MSVC-specific fixes (guarded with `if(WIN32)`/`#ifdef`, Linux unaffected):**

| # | Gap | Fix |
|---|---|---|
| 1 | MSVC reads UTF-8 sources as cp932 | `if(MSVC) add_compile_options(/utf-8)` (also sets exec charset to UTF-8) |
| 2 | `.so`/`pthread`/`-fpermissive`/rpath are Linux-only | `if(WIN32)` branch: import libs, `ws2_32 bcrypt advapi32 userenv ntdll`, `/bigobj /utf-8`, POST_BUILD DLL copy, Rust `.a`→`.lib` |
| 3 | ONNX `ORTCHAR_T` is `wchar_t` on Windows | `src/ort_path.hpp` (UTF-8→UTF-16) around `Ort::Session` |
| 4 | MSVC `std::ifstream` treats narrow paths as cp932 | `src/file_io.hpp` opens a wide path so Japanese filenames work |

### Gotchas

- **fp16 will not load with `ORT_ENABLE_ALL`** (LayerNorm fusion vs fp16 Cast).
  Use `ORT_ENABLE_EXTENDED`; `phase0_parity` tries strategies top-down.
- **INT8 is not used** — predictions shift toward "entity → O" (mask leaks), the
  worst direction for opt-out.
- **Watch character/byte offsets.** HF `get_offsets()` is bytes, Python
  `offset_mapping` is characters; sudachi.rs `begin()` is bytes, `begin_c()` is
  code points. ids/labels alone won't reveal a mismatch — put offsets in the
  oracle.

---

## 日本語

中核（NER 推論・形態素・検知・マスク・逆置換）＋全入口形式（docx/pptx/xlsx/csv/txt/pdf/msg）を
C++ に移植し、各段を Python 参照実装（oracle）と突き合わせて検証した。**適合試験は
WSL/Linux ＋ Windows(MSVC) の両方で green。**

> **期待値の位置づけ。** `cpp/testdata/*_ref.json` はその Python 実装から生成したもので、
> 下表の数字はそのとき突き合わせた結果。Python 側は現在休止し、開発は C++ 単独で進めている。
> したがって試験は毎回「二つの実装が一致すること」を*証明*しているのではなく、**そのとき
> 検証した挙動を固定している**。回帰試験として読んでほしい——意図しない変化を捕まえるのが
> 役目で、意図的に変えたときは `phase2_pipeline --update` で期待値を更新し、**差分そのものが
> レビュー対象**になる。

| 実行ファイル | 試験内容 | 結果 |
|---|---|---|
| `phase0_tokenize` | テキスト → input_ids が transformers と一致するか | **39/39** |
| `phase0_parity` | input_ids → ラベル が torch と一致するか | **4873/4873** |
| `phase1_sudachi` | 形態素／`person_name_core`／`org_name_core` が SudachiPy と一致するか | **398/398・120/120・120/120** |
| `phase2_regex` | 8パターンの正規表現が Python `re` と同じスパンを返すか | **240/240** |
| `phase2_hf` | HF の simple 集約が Python と同じスパンを返すか | **267/267** |
| `phase2_pipeline` | 検知パイプライン全体の候補が Python と一致するか | **155/155** |
| `phase3_tokenize` | トークン化・逆置換・フリガナ名寄せが Python と一致するか | readings 68/68・各5/5 |
| `phase4_extract` | docx/pptx/xlsx/csv/txt/pdf の抽出テキストが Python と一致するか | **8/8** |
| `phase4_msg` | .msg が Python の検出PIIを1つも落とさないか | 平文 10/10 |
| `phase5_core` | 安全ゲート・ハイライト・束ねが Python と一致するか | spans 5/5・gate 10/10・table OK |

候補一致は、移植時の評価（Python 側 eval 174/175）と同値であることを確認した時点のもの。
**検知精度そのものを毎回測っているわけではない**（eval ハーネスは C++ に未移植）。
GUI アプリ（`jp-pii-sanitizer.exe`・CMake ターゲット `jp_pii_sanitizer`）はこれらの検証済みコアを
WebView2 に載せたもの。

クロスプラットフォームの **CLI** もある — CMake ターゲット `jp_pii_sanitizer_cli`、exe
`jp-pii-sanitizer-cli`。同じ `cmake --build build` で生成される。ライブラリヘッダを直接
再利用し（WebView2 不要）、Linux/macOS でもビルド・実行できる。CI がビルド＋
`detect → mask → restore` のスモークを回し、`package_win.ps1` がポータブル ZIP に GUI と
同梱する。使い方・設計/実装メモは [../docs/cli.md](../docs/cli.md)。

### 必要なもの

**Linux / WSL**（適合試験の主戦場）:

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config unzip
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y   # Rust（cargo）
```

**Windows**（配布アプリのビルド）: 下の「Windows(MSVC) ビルド」節。いずれも Rust（cargo）が
要る。「C++版」だが tokenizer と Sudachi のシムが Rust だから。

### 1. third_party 依存の取得（.gitignore 対象・約 130MB）

リポジトリには入れていないので各自で `cpp/third_party/` へ配置する。バージョンは固定（oracle を
作った Python 側と同一バイナリ系列に揃えるため）。コマンドは上の英語節と同じ。Windows は
onnxruntime/pdfium を win-x64 版で、それ以外はソースで取得。patch は `any_to_string()` が
PT_BINARY の値に空を返すバグ（実物 Outlook の HTML 本文が読めない）を直すもの
。Windows で GUI をビルドするには WebView2 SDK（NuGet
`Microsoft.Web.WebView2` を `third_party/webview2/`）も要る。

### 2. モデルと辞書の取得（Release から・約 750MB）

適合試験にも GUI にも NER モデル・トークナイザ・Sudachi 辞書が要る。これらは巨大なので
**GitHub Release に添付**してあり、取得スクリプトで `cpp/models/` へ展開する（英語節の
`fetch_assets` コマンド参照）。展開後の `cpp/models/` には `model_fp16.onnx`(~530MB)・
`tokenizer.json`(17MB)・`labels.json`/`patterns.json`（リポジトリにも同梱）・`sudachi/`
（SudachiDict-core・~210MB）が入る。適合試験の oracle（`testdata/*.json`）は**リポジトリに
同梱済み**で再生成不要。`sudachi_ref.json` の辞書パスは `models/sudachi/...` の相対パスで、
テストを `cpp/` から実行すれば解決する。

### 3. ビルドと実行（Linux / WSL）

```bash
cd cpp
export PATH="$HOME/.cargo/bin:$PATH"      # cargo（Rust シム）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                       # 初回は Rust のビルドで数分

./build/phase0_tokenize                           # → 39/39
./build/phase0_parity models/model_fp16.onnx      # → 4873/4873
# 他の phase* も同様。PASS かつ終了コード0なら合格（CI 可）。
```

実行は `cpp/` を CWD にして行う（oracle は相対パス参照）。

### 4. Windows(MSVC) ビルド

配布アプリ（`jp-pii-sanitizer.exe`）と全 phase 試験は Windows でも同じ oracle で green。

ツールチェイン: **VS 2022 Build Tools**（MSVC v143・「C++によるデスクトップ開発」）、
**CMake / Ninja**、**Rust(msvc)**（`rustup default stable-x86_64-pc-windows-msvc`）。**Smart App
Control が有効だと rustc が `0xc0e90002` で落ちる**（未署名の `rustc_driver-*.dll` をブロック）
のでオフにする。third_party は §1 の Windows 版、モデルは §2 の `fetch_assets.ps1` を使う。

`cl` と `cargo` に PATH を通すため `cpp\winenv.bat`（vcvars64 + `%USERPROFILE%\.cargo\bin`）を
call してから cmake を呼ぶ。**winenv.bat は純ASCII**（cmd は .bat を cp932 で読むため）。

```bat
call winenv.bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

配布 ZIP は `powershell -File tools\package_win.ps1` で生成。

**移植で必要だった MSVC 固有の修正**（`if(WIN32)`/`#ifdef` で Linux は非破壊）:

| # | ギャップ | 対処 |
|---|---|---|
| 1 | MSVC は UTF-8 ソースを cp932 で読む | `if(MSVC) add_compile_options(/utf-8)`（実行文字セットも UTF-8） |
| 2 | `.so`直リンク・`pthread dl m`・`-fpermissive`・rpath は Linux 専用 | `if(WIN32)` 分岐。import lib・`ws2_32 bcrypt advapi32 userenv ntdll`・`/bigobj /utf-8`・DLL の POST_BUILD コピー・Rust `.a`→`.lib` |
| 3 | ONNX の `ORTCHAR_T` は Windows で `wchar_t` | `src/ort_path.hpp`(UTF-8→UTF-16)を `Ort::Session` で挟む |
| 4 | MSVC の `std::ifstream` はナローパスを cp932 解釈 | `src/file_io.hpp`(ワイドパス)で日本語名ファイルが開けるように |

### 実測メモ（ハマりどころ）

- **fp16 は `ORT_ENABLE_ALL` でロードできない**（LayerNorm fusion が fp16 の Cast と衝突）。
  `ORT_ENABLE_EXTENDED` を使う。`phase0_parity` は通る戦略を上から総当たり。
- **INT8 は使わない**。予測が「エンティティ→O」方向（マスク漏れ）に変わる＝opt-out で最も危険
  。
- **オフセットは文字/バイトの取り違えに注意**。HF `get_offsets()` はバイト、Python の
  `offset_mapping` は文字。sudachi.rs も `begin()` がバイト・`begin_c()` がコードポイント。
  ids やラベルだけ見ていると気づけないので、oracle にオフセットを含めて突き合わせる。
