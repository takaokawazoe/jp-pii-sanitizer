// docx / pptx / xlsx の抽出。OOXML = ZIP + XML。
//
// Python 版は python-docx / python-pptx / openpyxl を使う。それぞれの「段落テキストの作り方」
// 「シェイプ走査順」「結合セルの値展開」を再現する必要がある。ここが Phase 4 の量の本体。
//
// pugixml は名前空間を展開しないので、要素名は "w:p" 等プレフィックス込みで扱える。
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "extractors.hpp"  // strip_ruby / trim / Table / rows_to_prose 相当
#include "miniz.h"
#include "pugixml.hpp"

namespace ooxml {

/// ZIP 内の1エントリを文字列で取り出す。無ければ空。
inline std::string zip_read(const std::string& path, const std::string& entry) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return {};
  std::string out;
  const int idx = mz_zip_reader_locate_file(&zip, entry.c_str(), nullptr, 0);
  if (idx >= 0) {
    std::size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
    if (p) {
      out.assign(static_cast<char*>(p), sz);
      mz_free(p);
    }
  }
  mz_zip_reader_end(&zip);
  return out;
}

/// ZIP 内のエントリ名一覧（glob 的な用途に）。
inline std::vector<std::string> zip_entries(const std::string& path) {
  std::vector<std::string> out;
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return out;
  const mz_uint n = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat(&zip, i, &st)) out.emplace_back(st.m_filename);
  }
  mz_zip_reader_end(&zip);
  return out;
}

// ---- docx ----

/// 要素の子孫から指定タグのテキストを順に連結（python-docx の paragraph.text 相当）。
inline std::string concat_texts(const pugi::xml_node& node, const char* tag) {
  std::string out;
  for (const auto& n : node.select_nodes(std::string(".//").append(tag).c_str()))
    out += n.node().text().get();
  return out;
}

/// docx セルのテキスト（python-docx cell.text は段落を \n で連結）。
inline std::string docx_cell_text(const pugi::xml_node& tc) {
  std::vector<std::string> paras;
  for (const auto& p : tc.select_nodes(".//w:p"))
    paras.push_back(concat_texts(p.node(), "w:t"));
  std::string out;
  for (std::size_t i = 0; i < paras.size(); ++i) {
    if (i) out += "\n";
    out += paras[i];
  }
  return out;
}

inline std::string extract_docx(const std::string& path) {
  const std::string xml = zip_read(path, "word/document.xml");
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return {};

  std::vector<std::string> parts;
  auto body = doc.select_node("//w:body").node();
  for (const auto& child : body.children()) {
    const std::string name = child.name();
    if (name == "w:p") {
      const std::string t = concat_texts(child, "w:t");
      if (!utf8::trim(t).empty()) parts.push_back(t);
    } else if (name == "w:tbl") {
      for (const auto& tr : child.children("w:tr")) {
        std::vector<std::string> cells;
        for (const auto& tc : tr.children("w:tc")) {
          const std::string t = utf8::trim(docx_cell_text(tc));
          if (!t.empty()) cells.push_back(t);
        }
        if (!cells.empty()) {
          std::string line;
          for (std::size_t i = 0; i < cells.size(); ++i) {
            if (i) line += "\t";
            line += cells[i];
          }
          parts.push_back(line);
        }
      }
    }
  }
  std::string joined;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) joined += "\n";
    joined += parts[i];
  }
  return extractors::strip_ruby(joined);
}

// ---- pptx ----

/// pptx の1段落テキスト（a:p 内の a:t を連結）。
inline std::string pptx_para_text(const pugi::xml_node& p) { return concat_texts(p, "a:t"); }

/// シェイプツリーを XML 順に走査（python-pptx の shapes 順＝spTree の子順）。
inline void pptx_walk(const pugi::xml_node& tree, std::vector<std::string>& out) {
  for (const auto& sp : tree.children()) {
    const std::string name = sp.name();
    if (name == "p:grpSp") {
      pptx_walk(sp, out);  // グループは再帰
    } else if (sp.select_node(".//a:tbl")) {
      // 表: セル単位（a:tc の各 a:p を連結して1片）
      for (const auto& tc : sp.select_nodes(".//a:tc")) {
        std::string cell;
        for (const auto& p : tc.node().select_nodes(".//a:p")) cell += pptx_para_text(p.node());
        const std::string t = utf8::trim(cell);
        if (!t.empty()) out.push_back(t);
      }
    } else if (sp.select_node(".//p:txBody") || sp.select_node(".//a:txBody")) {
      // テキストフレーム: 段落単位で1片
      for (const auto& p : sp.select_nodes(".//a:p")) {
        const std::string t = utf8::trim(pptx_para_text(p.node()));
        if (!t.empty()) out.push_back(t);
      }
    }
  }
}

inline std::string extract_pptx(const std::string& path) {
  // スライドは ppt/slides/slideN.xml。番号順に処理（presentation.xml の順が正確だが、
  // ファイル名の数値順で概ね一致する。厳密化は presentation.xml.rels 経由。TODO）
  std::vector<std::pair<int, std::string>> slides;
  for (const auto& e : zip_entries(path)) {
    if (e.rfind("ppt/slides/slide", 0) == 0 && e.size() > 4 &&
        e.compare(e.size() - 4, 4, ".xml") == 0) {
      const std::string num = e.substr(16, e.size() - 20);  // "ppt/slides/slide{N}.xml"
      if (!num.empty() && std::all_of(num.begin(), num.end(), ::isdigit))
        slides.emplace_back(std::stoi(num), e);
    }
  }
  std::sort(slides.begin(), slides.end());

  std::vector<std::string> parts;
  for (const auto& [n, entry] : slides) {
    const std::string xml = zip_read(path, entry);
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) continue;
    auto tree = doc.select_node("//p:cSld/p:spTree").node();
    pptx_walk(tree, parts);
    // ノート: ppt/notesSlides/notesSlideN.xml（対応は rels 経由が正確。番号一致で近似）
    const std::string note_entry =
        "ppt/notesSlides/notesSlide" + std::to_string(n) + ".xml";
    const std::string nxml = zip_read(path, note_entry);
    if (!nxml.empty()) {
      pugi::xml_document nd;
      if (nd.load_buffer(nxml.data(), nxml.size())) {
        // python-pptx の notes_text_frame は **body プレースホルダのみ**。notesSlide には
        // sldImg（スライド画像）・body（ノート本文）・sldNum（番号「1」）の ph があり、
        // 全 a:p を拾うと番号まで混ざる（実測で「[ノート] 1」になった）。body だけ選び、
        // 段落を \n で連結する（notes_text_frame.text の仕様）。
        std::vector<std::string> paras;
        for (const auto& p :
             nd.select_nodes("//p:sp[.//p:ph[@type='body']]//a:p"))
          paras.push_back(pptx_para_text(p.node()));
        std::string note;
        for (std::size_t i = 0; i < paras.size(); ++i) {
          if (i) note += "\n";
          note += paras[i];
        }
        if (!utf8::trim(note).empty()) parts.push_back("[ノート] " + note);
      }
    }
  }
  std::string joined;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) joined += "\n";
    joined += parts[i];
  }
  return extractors::strip_ruby(joined);
}

// ---- xlsx ----
// 結合セル・空セル埋め・sharedStrings を扱う。ooxml_xlsx.hpp に実装。
std::string read_xlsx_prose(const std::string& path);

}  // namespace ooxml

#include "ooxml_xlsx.hpp"
