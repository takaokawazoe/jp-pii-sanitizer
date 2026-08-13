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
- **Data leaving the machine.** The app runs fully offline; any outbound network
  request would be a vulnerability.
- Memory-safety issues in the native parsers (OOXML/PDF/MSG) reachable from a
  crafted input document.

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
- **データが端末外へ出ること。** 本アプリは完全オフラインで動作します。外向きのネットワーク
  要求はすべて脆弱性です。
- 細工した入力文書から到達可能な、ネイティブパーサ（OOXML/PDF/MSG）のメモリ安全性の問題。

### 脆弱性の報告

脆弱性の疑いについて**公開 Issue を立てないでください。** GitHub の非公開報告を使ってください:
本リポジトリの **Security → Report a vulnerability**。観測した内容とそれをセキュリティ問題と
考える理由、**合成データ**（実在の個人情報は絶対に使わない）による最小再現手順、
バージョン/コミットと OS を添えてください。数日以内の受領確認を目指します。報奨金制度は
ありません。

検出漏れを報告する際は、**合成データのみ**で再現してください。実文書で漏れを見つけた場合は、
架空の氏名・住所・番号で問題の*形*を再現してください。
