// PDF 抽出。PDFium(BSD) を使う。
//
// スパイクで pymupdf4llm と 32/32 一致・43倍速・AGPL回避を確認済み。C++ 版が PDFium を使うので
// oracle も pypdfium2 で作る（pymupdf4llm ではない）。ここは pypdfium2 の get_text_bounded を
// バイト単位で再現する:
//   - ページ bbox = FPDF_GetPageBoundingBox → (left, bottom, right, top)
//   - FPDFText_GetBoundedText(tp, left, top, right, bottom, ...) で UTF-16LE を取得
//   - "\n".join(pages) して _strip_ruby
#pragma once

#include <string>
#include <vector>

#include "extractors.hpp"  // strip_ruby
#include "fpdf_text.h"
#include "fpdfview.h"
#include "utf8.hpp"

namespace pdf {

// PDFium は プロセスで1回だけ Init/Destroy する必要がある（RAII で握る）。
struct Library {
  Library() {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
  }
  ~Library() { FPDF_DestroyLibrary(); }
};

/// UTF-16LE（コードユニット列）を UTF-8 に。サロゲートペアも処理する。
inline std::string utf16le_to_utf8(const std::vector<unsigned short>& u16) {
  std::string out;
  for (std::size_t i = 0; i < u16.size(); ++i) {
    unsigned int cp = u16[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < u16.size()) {  // 上位サロゲート
      const unsigned int lo = u16[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

/// 1ページのテキスト（pypdfium2 の get_text_bounded 相当・ページ全体 bbox）。
inline std::string page_text(FPDF_PAGE page) {
  FS_RECTF bbox{};
  if (!FPDF_GetPageBoundingBox(page, &bbox)) return {};
  FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
  if (!tp) return {};
  // args = (left, top, right, bottom)（pypdfium2 と同じ順）
  const int n = FPDFText_GetBoundedText(tp, bbox.left, bbox.top, bbox.right, bbox.bottom,
                                        nullptr, 0);
  std::string out;
  if (n > 0) {
    std::vector<unsigned short> buf(n);
    FPDFText_GetBoundedText(tp, bbox.left, bbox.top, bbox.right, bbox.bottom, buf.data(), n);
    // pypdfium2 は n_chars 分をそのまま utf-16-le デコード（末尾NULは実測で無し）
    out = utf16le_to_utf8(buf);
  }
  FPDFText_ClosePage(tp);
  return out;
}

/// テキストPDFを抽出（extract_pdf 相当・ただしエンジンは PDFium）。
inline std::string extract_pdf(const std::string& path) {
  static Library lib;  // プロセス内で1回だけ初期化
  FPDF_DOCUMENT doc = FPDF_LoadDocument(path.c_str(), nullptr);
  if (!doc) return {};
  std::vector<std::string> pages;
  const int n = FPDF_GetPageCount(doc);
  for (int i = 0; i < n; ++i) {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (page) {
      pages.push_back(page_text(page));
      FPDF_ClosePage(page);
    }
  }
  FPDF_CloseDocument(doc);
  std::string joined;
  for (std::size_t i = 0; i < pages.size(); ++i) {
    if (i) joined += "\n";
    joined += pages[i];
  }
  return extractors::strip_ruby(joined);
}

}  // namespace pdf
