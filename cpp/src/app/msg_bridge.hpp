// .msg 抽出を GUI へ渡すための薄い橋。
//
// **なぜ間に1枚挟むか**: msg.hpp は Aspose.Email を取り込み、そのヘッダは MSVC で
// /bigobj /utf-8 /EHsc、GCC で -Wno-changes-meaning -fpermissive を要求する。
// winmain.cpp の TU に直接入れると、これらのフラグがアプリ全体に掛かってしまう。
// ここだけを別 TU に切り出せばフラグはこの 1 ファイルに閉じ込められ、
// app/core.hpp が「Aspose を持ち込まない facade」であるという設計も保てる。
//
// この宣言には Aspose の型を一切出さないこと（出した瞬間に隔離が壊れる）。
#pragma once

#include <string>
#include <vector>

namespace msgbridge {

struct Extracted {
  std::string text;                       // 本文（末尾に【添付】ファイル名を並べたもの）
  std::vector<std::string> attachments;   // 添付ファイル名。**中身は展開しない**
  std::string error;                      // 空でなければ抽出に失敗している
};

/// .msg を読む。例外は投げず、失敗は error に入れて返す。
Extracted extract(const std::string& path);

}  // namespace msgbridge
