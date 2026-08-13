# Contributing / コントリビューション

**Language:** English (below) · [日本語](#日本語)

---

## English

Thanks for your interest. This project ports a Japanese PII sanitizer to native
C++; correctness of detection and masking is the priority.

### Ground rules

- **Never commit real personal information.** All sample and test data must be
  synthetic (fictional names, `example-corp.jp`-style addresses, non-routable
  numbers). PRs that add real PII will be rejected.
- **Keep the conformance tests green.** Detection/masking behavior is pinned
  against oracles in `cpp/testdata/`. If you change detection logic, explain the
  intended change and update the affected oracle with a clear rationale. See
  [DESIGN.md](DESIGN.md) and [cpp/README.md](cpp/README.md).
- Match the surrounding code style. Comments in this codebase are in Japanese;
  keeping them consistent is fine.

### Building and testing

See [cpp/README.md](cpp/README.md). The phase tests (`cpp/build/phase*`) must
pass against their oracles before a change is considered done.

### Security issues

See [SECURITY.md](SECURITY.md) — do not use public issues for vulnerabilities.

---

## 日本語

ご関心ありがとうございます。本プロジェクトは日本語 PII サニタイザをネイティブ C++ へ移植する
もので、検知とマスクの**正しさ**を最優先します。

### 基本ルール

- **実在の個人情報を絶対にコミットしない。** サンプル・テストデータはすべて合成（架空の氏名・
  `example-corp.jp` 形式の住所・到達不能な番号）にしてください。実 PII を追加する PR は却下します。
- **適合試験を green に保つ。** 検知/マスクの挙動は `cpp/testdata/` の oracle に固定されています。
  検知ロジックを変える場合は、意図した変更を説明し、影響する oracle を明確な根拠とともに更新
  してください。[DESIGN.md](DESIGN.md) と [cpp/README.md](cpp/README.md) を参照。
- 周囲のコードスタイルに合わせてください。本コードベースのコメントは日本語です（統一のため
  日本語のままで構いません）。

### ビルドとテスト

[cpp/README.md](cpp/README.md) を参照。変更が完了とみなされる前に、phase テスト
（`cpp/build/phase*`）が各 oracle に対して通っている必要があります。

### セキュリティ問題

[SECURITY.md](SECURITY.md) を参照 — 脆弱性に公開 Issue を使わないでください。
