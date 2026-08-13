"""
デモ用サンプル: 誤送信インシデントの一次報告メール（架空データ）。

Web版のデモモードで「ファイルをアップロードせずに精度を確かめる」ための素材。
人名・社名・住所・電話・メール・カナ氏名（フリガナ名寄せの見せ場）・金額
（マスクしない番号＝数字候補除外の確認）を一通り含む。すべて架空。

生成: python samples/make_sample_email.py  → samples/misdirected_email.eml
"""

from __future__ import annotations

import os
from email.message import EmailMessage

BODY = """\
セキュリティ事務局 御中

お世話になっております。営業部の山田 太郎です。
本日、メールの誤送信が発生しましたので、一次報告いたします。

■ 経緯
株式会社みらいテクノロジーズの田中 健一様へお送りするはずの見積書を、
誤って別のお客様である株式会社あおぞら物産の佐藤 美咲様（サトウ ミサキ様）
宛に送信してしまいました。

■ 誤送信の内容
・宛先（誤）: 佐藤 美咲様 <sato.misaki@aozora-bussan.co.jp>
・添付ファイル: 御見積書（金額 ¥1,280,000）、および顧客連絡先の一覧（約120件）
・先方の電話: 03-1234-5678

■ 一次対応
先ほど佐藤様にお電話（03-1234-5678）し、開封前の削除を依頼しました。
当社所在地（東京都渋谷区神南1丁目2番3号）での回収は不要とのことです。
個人情報保護管理者の加藤 真由美（内線2250）にも共有済みです。

至急ご確認をお願いいたします。

--
営業部 山田 太郎
yamada.taro@example-corp.jp / 03-5401-2288（内2214）
"""


def main() -> None:
    msg = EmailMessage()
    msg["Subject"] = "【至急】メール誤送信のご報告"
    msg["From"] = "山田 太郎 <yamada.taro@example-corp.jp>"
    msg["To"] = "セキュリティ事務局 <security-office@example-corp.jp>"
    msg["Cc"] = "渡辺 隆志 <watanabe@example-corp.jp>"
    msg.set_content(BODY)

    out = os.path.join(os.path.dirname(__file__), "misdirected_email.eml")
    with open(out, "wb") as f:
        f.write(msg.as_bytes())
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
