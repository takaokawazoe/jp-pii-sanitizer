# PII削除ツール テスト用 .msg ファイル

本物の OLE 複合ファイル形式（CDFV2 / MS-CFB）で生成。`file` コマンドで
`CDFV2 Microsoft Outlook Message` と認識され、Outlook・olefile・extract-msg
のいずれでも開けます。

**全PIIは架空の値です。** 電話番号は 03-xxxx / 090-xxxx のテスト用レンジ、
住所は実在しない番地、クレジットカードは各ブランドの公開テスト番号、
マイナンバーはチェックディジットのみ有効な未発行値です。

## ファイル一覧

| ファイル | 目的 | 主な確認点 |
|---|---|---|
| `01_baseline_jp.msg` | 基本形 | 本文・署名・ヘッダの標準的な日本語PII |
| `02_format_variants.msg` | 表記ゆれ | 全角/半角、区切り文字違い、和暦、`[at]`難読化 |
| `03_sensitive_ids.msg` | 高機微ID | マイナンバー、口座、カード＋CVV、旅券、免許証 |
| `04_quoted_thread.msg` | 引用返信 | `>` `>>` 多段引用内のPII、元メッセージヘッダ |
| `05_attachments.msg` | 添付 | 添付**ファイル名**内の氏名、CSV/TXT本体のPII |
| `06_english_mixed.msg` | 英日混在 | SSN、IBAN、US電話/住所、IPアドレス＋日本語PII |
| `07_negative_controls.msg` | 誤検知 | **削除されてはいけない**値のみ |
| `08_metadata_only.msg` | メタデータ | 本文は無害、件名/差出人名/宛先/ヘッダのみPII |
| `09_large_body.msg` | 大容量 | 200件のPII、本文8888バイト（通常FATチェーン経由） |
| `10_empty_edge.msg` | 空 | 全プロパティ空。クラッシュしないこと |

## 検出すべきPIIカテゴリ

氏名（漢字・カナ・ローマ字）／メールアドレス／固定・携帯電話番号／
郵便番号・住所／生年月日（西暦・和暦）／マイナンバー／銀行口座／
クレジットカード番号・有効期限・CVV／旅券番号／運転免許証番号／
健康保険証番号／社員番号／SSN／IBAN／IPアドレス

## 特に注意すべき箇所

PIIは本文だけでなく以下にも配置しています。ツールが本文しか見ていない場合、
`08_metadata_only.msg` と `05_attachments.msg` で漏れが出ます。

- 件名（`PidTagSubject`）
- 差出人表示名・アドレス（`PidTagSenderName` / `PidTagSenderEmailAddress`）
- 宛先表示文字列（`PidTagDisplayTo` / `PidTagDisplayCc`）
- 受信者ストレージ（`__recip_version1.0_#XXXXXXXX`）
- トランスポートヘッダ（`PidTagTransportMessageHeaders`、`X-` ヘッダ含む）
- 添付ファイル名（`PidTagAttachLongFilename`）
- 添付ファイルの中身（`PidTagAttachDataBinary`）

## 誤検知テスト（07）

以下は削除対象ではありません。過剰にマスクしていないか確認してください。

- 注文番号 `ORD-20260312-0098`、製品型番 `TK1234567-A`
- カード番号に見える注文ID、`123456789012` というビルド番号
- 会議室番号 `03-1234`、在庫数 `150 0043`
- 金額 `1,234,567円`、統計値 `1985人`、`ISBN 978-4-1234-5678-9`
- 法人名「山田製作所」、施設名「田中角栄記念館」
- 公開連絡先 `info@example-corp.co.jp` / `0120-000-000`

## 検証済み

```
olefile      : 10/10 構造OK
extract-msg  : 10/10 件名・差出人・受信者・添付・本文すべて復元
file(1)      : CDFV2 Microsoft Outlook Message
```

`09_large_body.msg` は本文が4096バイトを超えるため通常のFATチェーンを、
それ以外はミニFAT経由を通ります。両方の読み取り経路をカバーしています。
