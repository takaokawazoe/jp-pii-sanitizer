# Security Policy / セキュリティポリシー

**Language:** English (below) · [日本語](#日本語)

---

## English

### Scope

`jp-pii-sanitizer` is a privacy tool: its whole job is to keep personal
information from leaking. We therefore treat the following as security issues, in
addition to ordinary vulnerabilities:

- **Detection failures that leak PII** — an input where real personal
  information survives into the "sanitized" output while the safety gate reports
  success. (*Over*-masking, and misses that the opt-out design expects a human to
  catch, are quality issues, not security issues — see the README disclaimer.)
- **The mapping (対応表) escaping its intended boundary.**
- **Data leaving the machine.** This application makes no network calls, and that is
  now enforced rather than merely true: navigation is blocked, new windows are refused,
  and the UI page carries a `default-src 'none'` CSP. Any outbound request originating
  from this application's code would be a vulnerability. (The WebView2 *runtime* is a
  Microsoft component and may contact Microsoft on its own; that is outside our control
  and out of scope.)
- Memory-safety issues in the native parsers (OOXML/PDF/MSG/EML) reachable from a
  crafted input document. Resource exhaustion counts: ZIP entries are capped and the
  regex engine has match and depth limits, so a crafted file should not be able to
  exhaust memory or hang the process.

### Reporting a vulnerability

Please **do not open a public issue** for a suspected vulnerability. Use GitHub's
private reporting: **Security → Report a vulnerability** on this repository.
Include what you observed and why it is a security issue, a minimal reproducer
using **synthetic** data (never real personal information), and the
version/commit and your OS. We aim to acknowledge within a few days. There is no
bug-bounty program.

When reporting a detection leak, reproduce with **synthetic** data only — if you
found a real document that leaks, recreate the *shape* of the problem with
fictional names/addresses/numbers.

---

## 日本語

### 対象

`jp-pii-sanitizer` はプライバシーツールであり、個人情報を漏らさないことが役割そのものです。
そのため、通常の脆弱性に加えて次を**セキュリティ問題**として扱います:

- **PII を漏らす検出失敗** — 実在の個人情報が「サニタイズ済み」の出力に残っているのに、
  安全ゲートが成功と報告する入力。（*過剰*マスクや、opt-out 設計が人によるチェックを前提と
  している検出漏れは、品質の問題でありセキュリティ問題ではありません。README の免責参照。）
- **対応表（マッピング）が意図した境界の外へ出ること。**
- **データが端末外へ出ること。** 本アプリはネットワーク通信を行いません。これは
  「そう書いてある」だけでなく**実装で強制**しています——遷移を全拒否し、新規ウィンドウを
  拒み、UI に `default-src 'none'` の CSP を入れてあります。本アプリのコードから出る外向きの
  要求はすべて脆弱性です。（WebView2 の**ランタイム自体**は Microsoft の部品で独自に通信し得ます。
  そこは制御外で対象外です。）
- 細工した入力文書から到達可能な、ネイティブパーサ（OOXML/PDF/MSG/EML）のメモリ安全性の問題。
  資源の枯渇も含みます——ZIP の展開サイズに上限を置き、正規表現には照合回数と深さの上限を
  設けてあるので、細工したファイルでメモリを食い潰したり固まったりしないはずです。

### 脆弱性の報告

脆弱性の疑いについて**公開 Issue を立てないでください。** GitHub の非公開報告を使ってください:
本リポジトリの **Security → Report a vulnerability**。観測した内容とそれをセキュリティ問題と
考える理由、**合成データ**（実在の個人情報は絶対に使わない）による最小再現手順、
バージョン/コミットと OS を添えてください。数日以内の受領確認を目指します。報奨金制度は
ありません。

検出漏れを報告する際は、**合成データのみ**で再現してください。実文書で漏れを見つけた場合は、
架空の氏名・住所・番号で問題の*形*を再現してください。
