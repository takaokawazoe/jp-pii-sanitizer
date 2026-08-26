// msg_bridge.hpp の実装。**この TU だけが Aspose を取り込む**（理由はヘッダの注記）。
#include "app/msg_bridge.hpp"

#include <exception>

#include "msg.hpp"

namespace msgbridge {

Extracted extract(const std::string& path) {
  Extracted out;
  try {
    const auto r = msg::extract_msg(path);
    // 本文＋添付ファイル名。ファイル名自体が PII を持つ（社員名簿_山田太郎_確認用.csv）ので
    // 検知対象に載せる。組み立ては CLI と共通の msg:: ヘルパを使い、両者で挙動を揃える。
    out.text = msg::body_with_attachment_names(r);
    out.attachments = msg::attachment_names(r);
  } catch (const std::exception& e) {
    out.error = e.what();
  } catch (...) {
    out.error = "不明なエラー";
  }
  return out;
}

}  // namespace msgbridge
