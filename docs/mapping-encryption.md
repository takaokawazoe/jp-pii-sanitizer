# Mapping encryption — design note

**Language:** English (below) · [日本語](#日本語)

> **Status: design only. Nothing here is implemented yet.**
> The mapping format is currently written in plaintext JSONL (`version 1`). This
> document records the decisions taken before writing any code, so the shape —
> and the reasoning behind it — survives.

The mapping (token ↔ real value) is the most sensitive artifact this tool
produces, and [SECURITY.md](../SECURITY.md) already names "the mapping escaping
its intended boundary" as a security issue. This note covers protecting that file
at rest with a user-chosen passphrase.

Format and I/O basics live in [cli.md](cli.md#design-decisions) (decision 8);
`cpp/src/mapping_io.hpp` is the single serializer both the GUI and the CLI use.

---

## English

### Threat model

The choices below follow from what this actually defends against. The asset is
**one file at rest**, and the attacker is someone who obtains a copy and can
grind against it offline, unthrottled, for as long as they like.

| Scenario | Covered |
|---|---|
| Mapping carried out by mail / share / USB, or mis-sent | **yes — this is the point** |
| Lost or stolen machine; device not wiped on offboarding | **yes** |
| Mapping lingering in backups or on a file server | **yes** |
| Someone reading the screen on the same PC | no — the GUI displays the mapping |
| Malware running on that PC | no — the plaintext is in process memory |

The tool is fully offline (a stated invariant), so there is no key server and no
device binding: the key can only come from a passphrase a human chose. Because
the attack is offline and unlimited, **the KDF matters far more than the cipher.**

### Design decisions

1. **Opt-in, never the default.** A reversible run writes the mapping in plaintext
   unless the user explicitly asks for protection. Encryption that gets in the way
   is encryption people turn off; the default stays out of the way, and the
   protected path is one deliberate action.

2. **Argon2id, m = 256 MiB, t = 3, p = 1, 16-byte salt.** Memory-hardness is what
   denies an attacker GPU parallelism — capacity, not compute, becomes the limit.
   PBKDF2 is rejected outright (CPU-only, so GPUs win by orders of magnitude);
   scrypt would be acceptable but has no advantage here. 256 MiB is unremarkable
   for an app that already loads a ~530 MB ONNX model.

3. **XChaCha20-Poly1305 (IETF), 24-byte nonce.** The 192-bit nonce means a random
   nonce per file is safe with no counter to manage — AES-GCM's 96-bit nonce is a
   footgun for no benefit at this size. It is also constant-time in software,
   which matters for a binary shipped to unknown hardware.

4. **libsodium.** It provides Argon2id, the AEAD, a CSPRNG, base64 and
   `sodium_memzero` — the glue is all that has to be written. It is also the
   boring, recognisable choice, which matters when a corporate security review
   looks at the dependency list. Monocypher was considered (two vendored files,
   zero build friction) but deliberately ships **no CSPRNG**, which would mean
   hand-rolling entropy collection — the last place to want bespoke code.

5. **Encrypted files are `version 2`; plaintext stays `version 1`.** The reader
   already rejects unknown versions outright rather than parsing them into an
   empty, innocent-looking mapping, so older builds refuse an encrypted file with
   a clear message. Keeping plaintext at `version 1` means **new plaintext output
   is still readable by v0.1.2** — this change is purely additive.

6. **The envelope wraps a complete `version 1` document.** Decryption yields
   exactly what the plaintext format is, so it can be handed to the existing
   strict parser untouched. Encryption stays a pure outer layer.

7. **Passphrase entry stays in the WebView.** A native Win32 dialog was
   considered and rejected: its only real security gain is keeping the passphrase
   out of the renderer process, which is worth little under the threat model
   above (anyone who can read renderer memory can read the plaintext mapping in
   ours). Against that sit DPI/theme/font/`.rc` work, lost accessibility, an
   untestable modal in a codebase whose `--selftest` exists precisely to avoid
   GUI dialogs, and a UI defined in two places. The features that make passphrase
   UX good — generation, a reveal toggle — are cheap in HTML and tedious in Win32.

   One genuine risk the native dialog would have avoided **must be closed
   explicitly**: WebView2's `IsGeneralAutofillEnabled` defaults to *true*, so form
   values can be persisted under `%LOCALAPPDATA%`. Both it and
   `IsPasswordAutosaveEnabled` are set to `FALSE` rather than trusted to defaults.
   That turns the only at-rest exposure into two API calls.

8. **No strength enforcement — but the strong option is the easy one.** No
   minimum length, no composition rules, no strength meter, no blocking. Modern
   guidance (NIST SP 800-63B) already discourages composition rules, and friction
   here has a specific failure mode: a rejected passphrase means the user saves
   the mapping in plaintext instead. (That guidance assumes a verifier that can
   rate-limit, which does not transfer to offline attack — so the answer is to
   change the *default*, not to add a rule.) A **generate** button puts a strong
   passphrase one click away; anyone who wants their own types it and is not
   stopped.

9. **Empty passphrases are refused — the one hard rule.** An empty passphrase
   still derives a key and still produces a well-formed encrypted file, i.e. a
   file that *looks* protected and opens for anyone. That is a fail-open dressed
   as a feature, and it is the same failure shape this codebase has repeatedly
   removed elsewhere. Refusing it is not a strength policy.

10. **Fail closed.** If protection is requested and anything fails — libsodium
    error, empty passphrase, a 256 MiB allocation that does not succeed — the
    tool writes nothing and exits non-zero. It never falls back to plaintext.

### File format

    {"_meta":{"version":2,
              "kdf":{"alg":"argon2id","v":19,"m":262144,"t":3,"p":1,"salt":"<b64:16B>"},
              "aead":"xchacha20poly1305-ietf",
              "nonce":"<b64:24B>"}}
    <base64 ciphertext, single line>

- `m` is in KiB (262144 = 256 MiB). Parameters are recorded **as numbers, not as a
  symbolic level**, so they can be raised later without orphaning existing files.
- The `_meta` line is bound as **associated data**. Tampering with the parameters
  already fails authentication (a different key is derived), so this is hygiene
  rather than a fix for a specific break — but it is free.
- Ciphertext is base64 on **one line**, which keeps the mapping pasteable into the
  restore textarea. At mapping sizes the 33 % overhead is irrelevant.
- **Read-side sanity limits.** A hostile file could specify `m = 4 TiB` and
  exhaust the reader's memory, so `m` is capped (1 GiB) and unknown `alg`/`aead`
  values are rejected before any allocation.

### API

`mapping_io` gains an encrypted path. The parser cannot prompt, so the passphrase
requirement is surfaced to the caller instead:

```cpp
std::vector<Entry> parse(const std::string& body);                        // throws NeedsPassphrase
std::vector<Entry> parse(const std::string& body, std::string_view pass); // plaintext ignores pass
std::string to_jsonl(const std::vector<Entry>&);                          // plaintext, version 1
std::string to_jsonl_encrypted(const std::vector<Entry>&, std::string_view pass);
```

### GUI

**Saving.** A checkbox next to the save button — *"パスフレーズで保護する"*, off by
default — reveals a passphrase field, a **generate** button and a **reveal
toggle**. Saving with an empty field is refused; nothing else is. The notice
shown is exactly:

> このパスフレーズがないと、サニタイズした内容を元に戻せません

A reveal toggle is preferred over a confirm-entry field: it catches the same typo
with less friction.

**Generated passphrases.** 20 characters from a ~32-character alphabet with
confusables removed (`I`/`l`/`1`, `O`/`0`), in four hyphenated groups of five —
e.g. `K7QM3-P4XR9-2FTBH-6YN8D`, about 100 bits. Grouping and the reduced alphabet
exist so the passphrase can be **written down**, since copy-paste should not be
the only way to keep it. Randomness comes from `crypto.getRandomValues()` or
libsodium's `randombytes_uniform()` — **never `Math.random()`**, which is
predictable and would quietly make the generator the weakest part of the feature.

**Restoring.** Paste keeps working, and a **file picker is added alongside it**.
The picker is native-held: the chosen mapping is read and retained on the native
side and referenced by the restore command, mirroring how `saveMapping` already
works, so a mapping opened this way never enters the WebView at all. If the
mapping turns out to be encrypted, native replies `{ok:false,
needsPassphrase:true}`, the UI reveals a passphrase field, and the request is
re-sent.

**Passphrase handling.** One-way JS → native, never returned. JS clears the field
and its variable immediately after sending; native zeroizes the passphrase and
derived key with `sodium_memzero` once the key is derived. Copies left in the JSON
parse tree are cleared where reachable; swap and hibernation are out of reach and
treated as accepted residual risk.

### CLI

    mask    … --encrypt-mapping [--passphrase-file F]
    restore --mapping m.jsonl   [--passphrase-file F]

With no `--passphrase-file`, the passphrase is read from the terminal with echo
disabled. There is deliberately **no `--passphrase` flag**: it would leak into
process listings and shell history. Defaults are unchanged — without
`--encrypt-mapping`, `mask` writes plaintext exactly as before.

### Verification

- `mapping_io` unit checks extended to cover: encrypt/decrypt round-trip, wrong
  passphrase, tampered ciphertext, tampered `_meta`, parameter sanity limits,
  empty-passphrase refusal, and plaintext still round-tripping. Run on **Linux
  (GCC) and MSVC**, as the existing checks are.
- CI smoke gains `mask --encrypt-mapping` → `restore --passphrase-file`.
- The GUI gets a `--selftest` hook that bypasses the save dialog, writes an
  encrypted mapping and feeds it to the CLI — the same technique used to prove
  the GUI and CLI share one mapping format.

### Out of scope

- **DPAPI / machine-bound protection.** Windows-only, so it would split the
  GUI/CLI parity that the shared serializer exists to protect, and it fails
  destructively when a profile or machine changes. Not planned, now or later.
- **Provenance and mismatch detection.** Re-running the sanitizer with different
  file order or opt-out choices yields a differently numbered mapping; applying
  it to an older AI reply would silently swap names, and the leftover-token check
  would not catch it because the token *set* is unchanged. Recording sources in
  `_meta` was considered and dropped: this is left to operational practice.
- Strength meters, minimum lengths, composition rules, breach-list checks.
- Encrypting the sanitized text. It is the artifact meant to be sent out.

### Note on losing a passphrase

Worth stating plainly, because it shapes how loud the warning needs to be: losing
the passphrase does **not** lose source data — the original documents are
untouched. What becomes unreadable is the *AI's reply*, which exists nowhere else.
Detection is deterministic, so re-running the same files with the same choices
reproduces the same mapping; different choices produce different numbering.

---

## 日本語

> **状態: 設計のみ。まだ何も実装されていません。**
> 対応表は現在、平文の JSONL（`version 1`）で書かれています。この文書は、コードを
> 書く前に決めた内容とその理由を残すためのものです。

対応表（トークン ↔ 実値）は本ツールが生む最も機微な成果物で、
[SECURITY.md](../SECURITY.md) も「対応表が意図した境界の外へ出ること」を
セキュリティ問題として挙げています。ここでは、そのファイルを利用者が選んだ
パスフレーズで at rest 保護する設計を扱います。

フォーマットと I/O の基本は [cli.md](cli.md#日本語) の設計判断 8 にあります。
`cpp/src/mapping_io.hpp` が GUI と CLI 共通の唯一のシリアライザです。

### 脅威モデル

以下の選択はすべてここから導かれます。守る対象は **at rest のファイル 1 個**で、
攻撃者は**そのファイルを入手し、オフラインで無制限に試行できる**人です。

| 想定 | 防げるか |
|---|---|
| 対応表をメール・共有フォルダ・USB で持ち出す／誤送信 | **防げる（本命）** |
| PC の紛失・盗難、退職時の端末回収漏れ | **防げる** |
| バックアップやファイルサーバに残り続ける | **防げる** |
| 同じ PC で画面を覗かれる | 防げない（GUI は対応表を表示する） |
| その PC でマルウェアが動いている | 防げない（平文がプロセス内にある） |

本ツールは完全オフライン動作（不変条件）なので、鍵サーバも端末束縛もありません。
鍵は**人が選んだパスフレーズ**からしか得られません。攻撃がオフラインかつ無制限で
ある以上、**暗号アルゴリズムより KDF の方が圧倒的に重要**です。

### 設計判断

1. **既定はオフ、明示操作でのみ暗号化。** 可逆実行の対応表は、利用者が明示的に保護を
   求めない限り平文で書きます。邪魔になる暗号化は「使われない暗号化」になります。
   既定は邪魔をせず、保護は意図的な 1 操作にします。

2. **Argon2id・m = 256 MiB・t = 3・p = 1・salt 16 バイト。** メモリハード性が GPU の
   並列度を潰します（律速が演算ではなくメモリ容量になる）。PBKDF2 は CPU only で
   GPU に桁違いに稼がれるため不採用。scrypt でも成立しますが、Argon2id を取れる状況で
   選ぶ利点がありません。約 530 MB の ONNX を読むアプリにとって 256 MiB は過大では
   ありません。

3. **XChaCha20-Poly1305 (IETF)・nonce 24 バイト。** nonce が 192 bit あるので、
   ファイルごとに乱数 nonce を使うだけで安全です（カウンタ管理が要らない）。
   AES-GCM の 96 bit nonce はこの規模で得るものが無いのに罠だけがあります。
   ソフトウェア実装でも定数時間なのも、未知のハードに配る binary では効きます。

4. **libsodium。** Argon2id・AEAD・CSPRNG・base64・`sodium_memzero` が揃っていて、
   書くのは繋ぎだけです。加えて**企業のセキュリティレビューで説明しやすい**という
   実利があります。Monocypher（2 ファイル・ビルド摩擦ゼロ）も検討しましたが、
   **CSPRNG を意図的に提供しない**ため乱数収集を自作することになり、暗号で最も
   自作したくない部分が残るので見送りました。

5. **暗号化は `version 2`、平文は `version 1` のまま。** リーダーは既に「未知の
   version を空の対応表として静かに成功させず、明示的に拒否する」実装になっている
   ので、旧ビルドは暗号化ファイルを明確なメッセージで撥ねます。平文を `version 1` に
   据え置くことで、**新版が書いた平文は v0.1.2 でもそのまま読めます**。この変更は
   純粋な追加です。

6. **封筒の中身は「完全な `version 1` 文書」。** 復号すると平文フォーマットそのものが
   出てくるので、既存の厳格パーサへ無改造で渡せます。暗号化は純粋な外側の層に
   留まります。

7. **パスフレーズ入力は WebView のまま。** ネイティブの Win32 ダイアログを検討して
   見送りました。得られるのは「パスフレーズがレンダラプロセスに乗らない」ことだけで、
   上の脅威モデルではその価値は小さい（レンダラのメモリを読める攻撃者は、こちらの
   平文の対応表も読める）。対して払うのは DPI・テーマ・フォント・`.rc` の文字コード、
   失われるアクセシビリティ、GUI ダイアログを避けるために置いた `--selftest` で
   叩けないモーダル、そして UI 定義が 2 箇所に割れることです。パスフレーズ UX を
   良くする機能（生成・伏字トグル）は HTML なら安く、Win32 では高くつきます。

   ただしネイティブ化で避けられたはずの**本物のリスクは明示的に潰します**。WebView2 の
   `IsGeneralAutofillEnabled` は既定が有効で、フォーム値が `%LOCALAPPDATA%` 配下に
   永続化されうるためです。`IsPasswordAutosaveEnabled` と併せて、既定に頼らず
   `FALSE` を明示設定します。唯一の at-rest 露出が API 2 つで消えます。

8. **強度の強制はしない。ただし「強い方が楽」にする。** 最小長・文字種・強度メーター・
   保存ブロックのいずれも設けません。現代のガイダンス（NIST SP 800-63B）も構成ルールを
   推奨しておらず、ここでの摩擦には固有の失敗モードがあります——弾かれた人は、
   代わりに平文で保存します。（同ガイダンスはレート制限できる検証側を前提としており、
   オフライン攻撃には前提が移りません。だから加えるべきは「規則」ではなく「既定」の
   変更です。）**生成ボタン**で強いパスフレーズを 1 クリックの位置に置き、自分で
   決めたい人は何にも止められずに入力できます。

9. **空パスフレーズだけは拒否する（唯一の固い規則）。** 空でも鍵は導出され、形式的に
   正しい暗号化ファイルができます。つまり**保護されているように見えて誰でも開ける
   ファイル**です。これは機能の顔をした fail-open で、本コードベースが繰り返し
   潰してきた失敗の形そのものです。これは強度ポリシーではありません。

10. **fail closed。** 保護を要求されて何かが失敗したら——libsodium のエラー、空の
    パスフレーズ、256 MiB の確保失敗——**何も書かずに異常終了**します。平文への
    フォールバックはしません。

### ファイル形式

    {"_meta":{"version":2,
              "kdf":{"alg":"argon2id","v":19,"m":262144,"t":3,"p":1,"salt":"<b64:16B>"},
              "aead":"xchacha20poly1305-ietf",
              "nonce":"<b64:24B>"}}
    <base64 の暗号文・1行>

- `m` の単位は KiB（262144 = 256 MiB）。パラメータは**記号名ではなく数値で記録**し、
  後で強度を上げても既存ファイルが開けなくならないようにします。
- `_meta` 行を **AAD** として束ねます。パラメータを改竄しても導出される鍵が変わって
  認証に失敗するので厳密には冗長ですが、無料なので入れます。
- 暗号文は **1 行の base64**。復元画面への貼り付けが成立し続けます。対応表の大きさなら
  33 % の増加は無視できます。
- **読み込み時のパラメータ上限。** 悪意あるファイルが `m = 4 TiB` を指定して読み手の
  メモリを枯渇させられるので、`m` に上限（1 GiB）を設け、未知の `alg`/`aead` は
  確保前に拒否します。

### API

パーサは対話できないので、パスフレーズの要否を呼び出し側に返します。

```cpp
std::vector<Entry> parse(const std::string& body);                        // NeedsPassphrase を投げる
std::vector<Entry> parse(const std::string& body, std::string_view pass); // 平文なら pass は無視
std::string to_jsonl(const std::vector<Entry>&);                          // 平文 version 1
std::string to_jsonl_encrypted(const std::vector<Entry>&, std::string_view pass);
```

### GUI

**保存。** 保存ボタンの隣にチェックボックス「パスフレーズで保護する」（既定オフ）を
置き、チェックするとパスフレーズ欄・**生成ボタン**・**伏字トグル**が現れます。
空のまま保存しようとしたときだけ止め、それ以外は一切止めません。表示する文言は：

> このパスフレーズがないと、サニタイズした内容を元に戻せません

確認再入力欄ではなく伏字トグルを採ります。同じタイプミスを、より少ない摩擦で防げます。

**生成されるパスフレーズ。** 紛らわしい文字（`I`/`l`/`1`、`O`/`0`）を除いた約 32 文字の
アルファベットから 20 文字、5 文字 × 4 グループのハイフン区切り
（例 `K7QM3-P4XR9-2FTBH-6YN8D`）＝ 約 100 bit。グループ化と文字の選別は、
**紙に書き写せる**ようにするためです（退避手段がクリップボード一択にならないように）。
乱数は `crypto.getRandomValues()` または libsodium の `randombytes_uniform()` を使い、
**`Math.random()` は使いません**——予測可能で、生成ボタンが機能全体の最弱点になります。

**復元。** 貼り付けは従来どおり動かしつつ、**ファイルピッカーを併存**させます。
ピッカーは native 保持型で、選ばれた対応表を native 側が読んで保持し、復元コマンドは
それを参照します（既存の `saveMapping` と対称）。この経路で開いた対応表は WebView に
一切乗りません。対応表が暗号化されていた場合、native が `{ok:false,
needsPassphrase:true}` を返し、UI がパスフレーズ欄を出して再送します。

**パスフレーズの扱い。** JS → native の一方向のみで、native から返しません。JS は送信
直後に入力欄と変数をクリアし、native は鍵導出後に `sodium_memzero` でパスフレーズと
鍵を消します。JSON パース木に残るコピーも届く範囲で消しますが、スワップと休止状態は
手が届かないため、受容する残存リスクとして扱います。

### CLI

    mask    … --encrypt-mapping [--passphrase-file F]
    restore --mapping m.jsonl   [--passphrase-file F]

`--passphrase-file` が無ければ、端末からエコーを止めて読みます。**`--passphrase` の
ような平文フラグは意図的に作りません**——プロセス一覧とシェル履歴に残るためです。
既定は変わらず、`--encrypt-mapping` が無ければ `mask` は従来どおり平文を書きます。

### 検証

- `mapping_io` の単体試験を拡張：暗号化往復、パスフレーズ違い、暗号文の改竄、
  `_meta` の改竄、パラメータ上限、空パスフレーズ拒否、平文が従来どおり往復すること。
  既存の試験と同じく **Linux (GCC) と MSVC の両方**で実行します。
- CI のスモークに `mask --encrypt-mapping` → `restore --passphrase-file` を追加。
- GUI には保存ダイアログを迂回する `--selftest` フックを足し、暗号化した対応表を
  書き出して CLI に食わせます（GUI と CLI が同一フォーマットであることを示したのと
  同じ手法）。

### スコープ外

- **DPAPI・端末束縛の保護。** Windows 専用なので、共有シリアライザで守っている
  GUI/CLI の対等性が割れます。プロファイルや端末が変わると破壊的に失敗もします。
  現在も将来も予定しません。
- **出所記録と取り違え検出。** ファイルの読み込み順や opt-out の選択が変われば番号の
  違う対応表ができ、それを古い AI 出力に当てると人名が静かに入れ替わります。しかも
  トークンの集合は同じなので残骸チェックは警告しません。`_meta` に出所を持たせる案は
  検討のうえ見送り、**運用に委ねます**。
- 強度メーター・最小長・文字種の強制・流出パスワード辞書照合。
- サニタイズ済みテキストの暗号化（外へ出すためのものなので不要）。

### パスフレーズを失った場合について

警告の強さを決めるために明記しておきます。パスフレーズを失っても**原本は失われません**
（元の文書は無傷です）。読めなくなるのは *AI の返答* で、これはどこにも原本がありません。
検知は決定論的なので、同じファイルを同じ選択で再実行すれば同じ対応表が再現します。
選択が変われば番号も変わります。
