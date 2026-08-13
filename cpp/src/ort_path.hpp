// ONNX Runtime のモデルパス型はプラットフォームで異なる（移植性ギャップ）。
//   Linux/POSIX: ORTCHAR_T = char    → Ort::Session(env, const char*, opts)
//   Windows    : ORTCHAR_T = wchar_t → Ort::Session(env, const wchar_t*, opts)
// UTF-8 の std::string を ORT が要求するパス型へ変換する。Linux では素通し（挙動不変）。
#pragma once

#include <string>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
namespace ortpath {
// UTF-8 → UTF-16。モデルパスに非ASCIIが混じっても正しく開ける。
inline std::wstring to_ort(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}
}  // namespace ortpath
#else
namespace ortpath {
inline const std::string& to_ort(const std::string& s) { return s; }
}  // namespace ortpath
#endif
