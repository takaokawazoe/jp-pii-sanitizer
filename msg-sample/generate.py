"""Generate PII-redaction test .msg files.

All PII is fabricated. Phone numbers use the 03-xxxx / 090-xxxx ranges,
credit card numbers are Luhn-valid test vectors, and My Number values are
checksum-valid but not issued.
"""
import os
from datetime import datetime, timezone
from msg import Message, P_SUBJECT, P_BODY, P_SENDER_NAME, P_SENDER_EMAIL, \
    P_SENDER_ADDRTYPE, P_DISPLAY_TO, P_DISPLAY_CC, P_HEADERS, \
    P_CREATION_TIME, P_DELIVERY_TIME

OUT = "/mnt/user-data/outputs/msg_pii_testset"
os.makedirs(OUT, exist_ok=True)
NOW = datetime(2026, 3, 12, 9, 41, 0, tzinfo=timezone.utc)

cases = []


def case(fn):
    cases.append(fn)
    return fn


# ---------------------------------------------------------------- 01 baseline
@case
def c01():
    m = Message()
    m.props.unicode(P_SUBJECT, "【契約書送付】山田太郎様 お手続きのご案内")
    m.props.unicode(P_SENDER_NAME, "佐藤 健一")
    m.props.unicode(P_SENDER_EMAIL, "kenichi.sato@example-corp.co.jp")
    m.props.unicode(P_SENDER_ADDRTYPE, "SMTP")
    m.props.unicode(P_DISPLAY_TO, "山田 太郎")
    m.props.time(P_CREATION_TIME, NOW)
    m.props.time(P_DELIVERY_TIME, NOW)
    m.props.unicode(P_HEADERS,
        "Received: from mail.example-corp.co.jp (203.0.113.45)\r\n"
        "From: 佐藤 健一 <kenichi.sato@example-corp.co.jp>\r\n"
        "To: 山田 太郎 <taro.yamada@example.jp>\r\n"
        "Message-ID: <a1b2c3@example-corp.co.jp>\r\n")
    m.add_recipient("山田 太郎", "taro.yamada@example.jp", 1)
    m.props.unicode(P_BODY,
        "山田 太郎 様\r\n\r\n"
        "いつもお世話になっております。株式会社サンプルの佐藤です。\r\n\r\n"
        "ご登録内容の確認をお願いいたします。\r\n"
        "　氏名：山田 太郎（ヤマダ タロウ）\r\n"
        "　生年月日：1985年4月3日\r\n"
        "　住所：〒150-0043 東京都渋谷区道玄坂1-2-3 サンプルビル7F\r\n"
        "　電話：03-1234-5678\r\n"
        "　携帯：090-8765-4321\r\n"
        "　メール：taro.yamada@example.jp\r\n\r\n"
        "ご不明点がございましたらご連絡ください。\r\n\r\n"
        "--\r\n"
        "佐藤 健一 / Kenichi Sato\r\n"
        "株式会社サンプル 営業第二部\r\n"
        "TEL: 03-9876-5432 / Mobile: 080-1111-2222\r\n"
        "kenichi.sato@example-corp.co.jp\r\n")
    return "01_baseline_jp.msg", m


# ------------------------------------------------- 02 format edge cases
@case
def c02():
    m = Message()
    m.props.unicode(P_SUBJECT, "各種フォーマット確認（全角・区切り文字違い）")
    m.props.unicode(P_SENDER_NAME, "検証 用子")
    m.props.unicode(P_SENDER_EMAIL, "yoko.kensho@example.jp")
    m.props.time(P_CREATION_TIME, NOW)
    m.add_recipient("テスト 担当", "test@example.jp", 1)
    m.props.unicode(P_BODY,
        "同一の値を異なる表記で記載しています。\r\n\r\n"
        "■ 電話番号\r\n"
        "  03-1234-5678\r\n"
        "  0312345678\r\n"
        "  03(1234)5678\r\n"
        "  ０３－１２３４－５６７８\r\n"
        "  +81-3-1234-5678\r\n"
        "  +81 90 8765 4321\r\n"
        "  (03) 1234-5678\r\n\r\n"
        "■ 郵便番号・住所\r\n"
        "  〒150-0043\r\n"
        "  〒1500043\r\n"
        "  150-0043\r\n"
        "  東京都渋谷区道玄坂一丁目2番3号\r\n"
        "  東京都渋谷区道玄坂1丁目2-3\r\n"
        "  Tokyo-to, Shibuya-ku, Dogenzaka 1-2-3\r\n\r\n"
        "■ 生年月日\r\n"
        "  1985年4月3日\r\n"
        "  1985/04/03\r\n"
        "  1985.4.3\r\n"
        "  S60.4.3（昭和60年4月3日）\r\n"
        "  昭和６０年４月３日\r\n"
        "  03-Apr-1985\r\n\r\n"
        "■ メールアドレス\r\n"
        "  taro.yamada@example.jp\r\n"
        "  taro.yamada+tag@example.jp\r\n"
        "  TARO.YAMADA@EXAMPLE.JP\r\n"
        "  taro.yamada [at] example [dot] jp\r\n"
        "  \"山田 太郎\" <taro.yamada@example.jp>\r\n")
    return "02_format_variants.msg", m


# ------------------------------------------------- 03 high-sensitivity IDs
@case
def c03():
    m = Message()
    m.props.unicode(P_SUBJECT, "【機密】口座・カード情報の確認依頼")
    m.props.unicode(P_SENDER_NAME, "経理部 鈴木")
    m.props.unicode(P_SENDER_EMAIL, "keiri@example-corp.co.jp")
    m.props.time(P_CREATION_TIME, NOW)
    m.add_recipient("財務 花子", "hanako.zaimu@example.jp", 1)
    m.add_recipient("監査 三郎", "saburo.kansa@example.jp", 2)
    m.props.unicode(P_BODY,
        "以下の内容に誤りがないかご確認ください。\r\n\r\n"
        "【マイナンバー】\r\n"
        "  123456789018\r\n"
        "  1234 5678 9018\r\n"
        "  １２３４５６７８９０１８\r\n\r\n"
        "【銀行口座】\r\n"
        "  みずほ銀行 渋谷支店 普通 1234567\r\n"
        "  口座名義：ヤマダ タロウ\r\n"
        "  金融機関コード：0001 支店コード：123\r\n\r\n"
        "【クレジットカード】\r\n"
        "  4111 1111 1111 1111  有効期限 12/28  セキュリティコード 123\r\n"
        "  5500-0000-0000-0004  exp 03/27  CVV 456\r\n"
        "  378282246310005（AMEX）\r\n\r\n"
        "【パスポート・免許証】\r\n"
        "  旅券番号：TK1234567\r\n"
        "  運転免許証番号：123456789012\r\n\r\n"
        "【その他】\r\n"
        "  健康保険証記号番号：1234・567890\r\n"
        "  社員番号：EMP-004521\r\n")
    return "03_sensitive_ids.msg", m


# ------------------------------------------------- 04 quoted reply chain
@case
def c04():
    m = Message()
    m.props.unicode(P_SUBJECT, "RE: RE: FW: 顧客リスト共有の件")
    m.props.unicode(P_SENDER_NAME, "田中 一郎")
    m.props.unicode(P_SENDER_EMAIL, "ichiro.tanaka@example-corp.co.jp")
    m.props.unicode(P_DISPLAY_TO, "山田 太郎; 佐藤 健一")
    m.props.unicode(P_DISPLAY_CC, "経理部")
    m.props.time(P_CREATION_TIME, NOW)
    m.add_recipient("山田 太郎", "taro.yamada@example.jp", 1)
    m.add_recipient("佐藤 健一", "kenichi.sato@example-corp.co.jp", 1)
    m.add_recipient("経理部", "keiri@example-corp.co.jp", 2)
    m.props.unicode(P_BODY,
        "承知しました。田中です。\r\n"
        "私の連絡先は 090-3333-4444 です。\r\n\r\n"
        "> -----Original Message-----\r\n"
        "> From: 佐藤 健一 <kenichi.sato@example-corp.co.jp>\r\n"
        "> Sent: 2026年3月10日 14:22\r\n"
        "> To: 田中 一郎 <ichiro.tanaka@example-corp.co.jp>\r\n"
        "> Subject: RE: FW: 顧客リスト共有の件\r\n"
        ">\r\n"
        "> 佐藤です。下記のとおりです。\r\n"
        "> 担当：山田 太郎（090-8765-4321）\r\n"
        ">\r\n"
        "> > From: 鈴木 次郎 <jiro.suzuki@example.jp>\r\n"
        "> > Sent: 2026年3月9日 09:05\r\n"
        "> >\r\n"
        "> > 鈴木です。顧客の高橋様（takahashi@example.net / 06-2222-3333）より\r\n"
        "> > 住所変更の連絡がありました。新住所は\r\n"
        "> > 〒530-0001 大阪府大阪市北区梅田4-5-6 です。\r\n")
    return "04_quoted_thread.msg", m


# ------------------------------------------------- 05 with attachments
@case
def c05():
    m = Message()
    m.props.unicode(P_SUBJECT, "【添付あり】社員名簿と請求書")
    m.props.unicode(P_SENDER_NAME, "人事部 高橋")
    m.props.unicode(P_SENDER_EMAIL, "jinji@example-corp.co.jp")
    m.props.time(P_CREATION_TIME, NOW)
    m.add_recipient("部長 渡辺", "watanabe@example-corp.co.jp", 1)
    m.props.unicode(P_BODY,
        "お疲れさまです。人事部の高橋です。\r\n"
        "名簿と請求書を添付します。不明点は 03-5555-6666 まで。\r\n"
        "（添付ファイル名にも氏名が含まれています）\r\n")
    csv = ("社員番号,氏名,カナ,生年月日,住所,電話番号,メール\r\n"
           "EMP-001,山田 太郎,ヤマダ タロウ,1985/04/03,"
           "東京都渋谷区道玄坂1-2-3,090-8765-4321,taro.yamada@example.jp\r\n"
           "EMP-002,鈴木 次郎,スズキ ジロウ,1990/11/20,"
           "大阪府大阪市北区梅田4-5-6,080-2222-3333,jiro.suzuki@example.jp\r\n"
           "EMP-003,John Smith,ジョン スミス,1978/06/14,"
           "神奈川県横浜市西区みなとみらい2-3-4,070-4444-5555,"
           "john.smith@example.com\r\n")
    m.add_attachment("社員名簿_山田太郎_確認用.csv",
                     csv.encode("utf-8-sig"))
    m.add_attachment("invoice_takahashi_202603.txt",
                     ("請求先: 高橋 美咲\r\n"
                      "住所: 〒530-0001 大阪府大阪市北区梅田4-5-6\r\n"
                      "TEL: 06-2222-3333\r\n"
                      "振込先: 三菱UFJ銀行 梅田支店 普通 7654321\r\n"
                      ).encode("utf-8"))
    return "05_attachments.msg", m


# ------------------------------------------------- 06 English / mixed
@case
def c06():
    m = Message()
    m.props.unicode(P_SUBJECT, "Onboarding details for John Smith (SSN on file)")
    m.props.unicode(P_SENDER_NAME, "Emily Carter")
    m.props.unicode(P_SENDER_EMAIL, "emily.carter@example.com")
    m.props.unicode(P_DISPLAY_TO, "HR Team")
    m.props.time(P_CREATION_TIME, NOW)
    m.add_recipient("HR Team", "hr@example.com", 1)
    m.add_recipient("John Smith", "john.smith@example.com", 2)
    m.props.unicode(P_BODY,
        "Hi team,\r\n\r\n"
        "Please find the onboarding details below.\r\n\r\n"
        "  Name: John Smith\r\n"
        "  DOB: 06/14/1978\r\n"
        "  SSN: 123-45-6789\r\n"
        "  Address: 1600 Pennsylvania Ave NW, Washington, DC 20500\r\n"
        "  Phone: (202) 555-0143 / +1 202-555-0187\r\n"
        "  Email: john.smith@example.com\r\n"
        "  Passport: 987654321 (USA)\r\n"
        "  IBAN: GB82 WEST 1234 5698 7654 32\r\n"
        "  IP used at signup: 198.51.100.27\r\n\r\n"
        "日本側の担当は山田 太郎（090-8765-4321）です。\r\n"
        "His Tokyo address: 〒150-0043 東京都渋谷区道玄坂1-2-3\r\n\r\n"
        "Best,\r\nEmily Carter\r\n"
        "Example Inc. | +1 415-555-0122 | emily.carter@example.com\r\n")
    return "06_english_mixed.msg", m


# ------------------------------------------------- 07 negative controls
@case
def c07():
    m = Message()
    m.props.unicode(P_SUBJECT, "誤検知確認用（PIIではない数字・語句）")
    m.props.unicode(P_SENDER_NAME, "品質保証")
    m.props.unicode(P_SENDER_EMAIL, "qa@example-corp.co.jp")
    m.props.time(P_CREATION_TIME, NOW)
    m.add_recipient("開発チーム", "dev@example-corp.co.jp", 1)
    m.props.unicode(P_BODY,
        "以下はPIIではありません。削除されないことを確認してください。\r\n\r\n"
        "・注文番号：4111111111111111 ではなく ORD-20260312-0098\r\n"
        "・製品型番：TK1234567-A\r\n"
        "・バージョン：v1.2.3 / ビルド 123456789012\r\n"
        "・会議室：03-1234（内線ではなく部屋番号）\r\n"
        "・金額：1,234,567円\r\n"
        "・日付：2026年3月12日（生年月日ではない）\r\n"
        "・統計：回答者は1985人、平均年齢は43.2歳\r\n"
        "・ISBN：978-4-1234-5678-9\r\n"
        "・座標：35.6595, 139.7005（渋谷スクランブル交差点・公共施設）\r\n"
        "・公開窓口：info@example-corp.co.jp / 0120-000-000\r\n"
        "・郵便番号に見える数値：150 0043 は在庫数です\r\n"
        "・山田製作所（法人名であり個人名ではない）\r\n"
        "・田中角栄記念館（施設名）\r\n")
    return "07_negative_controls.msg", m


# ------------------------------------------------- 08 tricky placement
@case
def c08():
    m = Message()
    # PII in the subject line and display names only
    m.props.unicode(P_SUBJECT,
        "山田太郎(090-8765-4321) 様の件 / taro.yamada@example.jp")
    m.props.unicode(P_SENDER_NAME, "佐藤 健一 <03-9876-5432>")
    m.props.unicode(P_SENDER_EMAIL, "kenichi.sato@example-corp.co.jp")
    m.props.unicode(P_DISPLAY_TO,
        "山田 太郎 <taro.yamada@example.jp>; 鈴木 次郎 <jiro.suzuki@example.jp>")
    m.props.time(P_CREATION_TIME, NOW)
    m.props.unicode(P_HEADERS,
        "From: 佐藤 健一 <kenichi.sato@example-corp.co.jp>\r\n"
        "To: taro.yamada@example.jp, jiro.suzuki@example.jp\r\n"
        "X-Originating-IP: [203.0.113.45]\r\n"
        "X-Customer-Ref: 山田太郎/1985-04-03\r\n")
    m.add_recipient("山田 太郎 (090-8765-4321)", "taro.yamada@example.jp", 1)
    m.add_recipient("鈴木 次郎", "jiro.suzuki@example.jp", 3)
    m.props.unicode(P_BODY,
        "本文にPIIはありません。件名・差出人名・宛先・ヘッダのみに含まれます。\r\n"
        "本文が空でもメタデータ側が処理されるかの確認用です。\r\n")
    return "08_metadata_only.msg", m


# ------------------------------------------------- 09 dense / long body
@case
def c09():
    m = Message()
    m.props.unicode(P_SUBJECT, "顧客対応ログ（大量PII・4KB超）")
    m.props.unicode(P_SENDER_NAME, "サポート 中村")
    m.props.unicode(P_SENDER_EMAIL, "support@example-corp.co.jp")
    m.props.time(P_CREATION_TIME, NOW)
    m.add_recipient("SVチーム", "sv@example-corp.co.jp", 1)
    names = [("山田 太郎", "taro.yamada", "090-8765-4321", "渋谷区道玄坂1-2-3"),
             ("鈴木 次郎", "jiro.suzuki", "080-2222-3333", "大阪市北区梅田4-5-6"),
             ("高橋 美咲", "misaki.takahashi", "070-4444-5555", "横浜市西区みなとみらい2-3-4"),
             ("渡辺 健", "ken.watanabe", "090-6666-7777", "名古屋市中区栄5-6-7"),
             ("伊藤 さくら", "sakura.ito", "080-8888-9999", "札幌市中央区大通西8-9-10")]
    lines = ["対応履歴一覧（この本文は4KBを超え、ミニストリーム境界を跨ぎます）\r\n"]
    for i in range(40):
        n, e, p, a = names[i % len(names)]
        lines.append(
            "[%04d] %s / %s@example.jp / %s / 東京都%s / "
            "生年月日 19%02d/%02d/%02d / 会員番号 MB-%06d\r\n"
            % (i + 1, n, e, p, a, 70 + (i % 25), (i % 12) + 1, (i % 28) + 1, i * 137))
    m.props.unicode(P_BODY, "".join(lines))
    return "09_large_body.msg", m


# ------------------------------------------------- 10 empty / minimal
@case
def c10():
    m = Message()
    m.props.unicode(P_SUBJECT, "")
    m.props.unicode(P_BODY, "")
    m.props.unicode(P_SENDER_NAME, "")
    m.props.time(P_CREATION_TIME, NOW)
    return "10_empty_edge.msg", m


for fn in cases:
    name, m = fn()
    size = m.save(os.path.join(OUT, name))
    print("%-28s %6d bytes" % (name, size))
