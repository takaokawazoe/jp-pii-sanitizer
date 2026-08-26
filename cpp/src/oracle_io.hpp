// 適合試験の期待値（testdata/*_ref.json）を書き戻すための小道具。
//
// **なぜ要るか**: これらの期待値はもともと Python 実装から生成した「一致の証拠」だった。
// Python 側を休止し、以後は C++ 単独で開発する体制に移したので、期待値の意味が
// 「外部実装と一致する」から「意図せず挙動が変わっていない」＝**回帰試験**に変わる。
//
// 回帰試験として運用するなら、挙動を意図的に変えたときに期待値を更新する手段が要る。
// 手で JSON を編集するのは事故のもとなので、各試験に `--update` を持たせて
// 「試験が落ちる → 差分を読む → 妥当なら受け入れてコミット」を回せるようにする。
//
// **安全装置は差分レビューそのもの。** 期待値を実装自身から作る以上、バグを正解として
// 焼き込めてしまう。だから読める差分が出ることが前提条件で、書き戻しは必ず整形して行う
// （元の期待値は 1 行 JSON のものが多く、そのままでは差分が読めない）。
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "file_io.hpp"
#include "json.hpp"

namespace oracle {

/// argv から `--update` を取り除き、あったかどうかを返す。
/// 位置引数（oracle のパス）と混ざらないよう、ここで抜いてから通常の解析に渡す。
inline bool take_update_flag(int& argc, char** argv) {
  bool found = false;
  int w = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--update") == 0) {
      found = true;
      continue;
    }
    argv[w++] = argv[i];
  }
  argc = w;
  return found;
}

/// 期待値を書き戻す。**必ず整形する**（差分が読めることが回帰試験の前提）。
/// インデントは既存の sudachi_ref.json に合わせて 1 スペース。
/// ensure_ascii は false 相当（nlohmann の既定）なので日本語はそのまま出る。
inline bool write(const std::string& path, const nlohmann::json& j) {
  const std::string body = j.dump(1, ' ', false) + "\n";
  if (!fileio::write_all(path, body)) {
    std::fprintf(stderr, "  期待値を書き戻せません: %s\n", path.c_str());
    return false;
  }
  std::printf("  === 期待値を更新しました: %s ===\n", path.c_str());
  std::printf("  差分を必ず確認してから commit すること（これが唯一の安全装置）。\n");
  return true;
}

}  // namespace oracle
