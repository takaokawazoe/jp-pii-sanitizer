// xlsx の抽出（ooxml.hpp から include される）。
//
// openpyxl(read_only, data_only) の iter_rows(values_only) ＋ _rows_to_prose を再現する。
// 幸い対象サンプルには日付/float の型変換痕が無く（実測）、全セルを文字列として扱える。
// もし datetime 書式のセルが来たら openpyxl は "2026-04-03 00:00:00" 等に変換するので、
// そのときは styles.xml の numFmt を見て変換が要る（リスク）。
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "miniz.h"
#include "numparse.hpp"
#include "pugixml.hpp"

namespace ooxml {

/// `<row>` の子がセル要素 `<c>` か。名前空間接頭辞（"x:c" 等）は落として比べる。
///
/// **`name.find("c") != npos` で判定してはいけない。** 'c' を含む別の要素
/// （拡張の <extLst> 配下など）まで通ってしまう。ここは完全一致で見る。
inline bool is_cell_node(const std::string& name) {
  const auto colon = name.find_last_of(':');
  return (colon == std::string::npos ? name : name.substr(colon + 1)) == "c";
}

/// セル参照 "AB12" の列部分を 0始まりの列番号に（A=0, Z=25, AA=26…）。
inline int col_of(const std::string& ref) {
  int col = 0;
  for (char c : ref) {
    if (c >= 'A' && c <= 'Z')
      col = col * 26 + (c - 'A' + 1);
    else
      break;
  }
  return col - 1;
}

/// sharedStrings.xml を読む。各 <si> は直下または <r> 内の全 <t> を連結。
inline std::vector<std::string> read_shared_strings(const std::string& path) {
  std::vector<std::string> out;
  const std::string xml = zip_read(path, "xl/sharedStrings.xml");
  if (xml.empty()) return out;
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return out;
  for (const auto& si : doc.select_nodes("//*[local-name()='si']")) {
    std::string s;
    for (const auto& t : si.node().select_nodes(".//*[local-name()='t']"))
      s += t.node().text().get();
    out.push_back(s);
  }
  return out;
}

/// workbook.xml の sheet 順（表示順）で worksheets/sheetX.xml のエントリ名を返す。
/// openpyxl の worksheets 順＝workbook.xml の <sheet> 順。r:id → rels でファイルを引く。
inline std::vector<std::string> sheet_entries_in_order(const std::string& path) {
  // r:id → target
  std::map<std::string, std::string> rid2target;
  {
    const std::string rels = zip_read(path, "xl/_rels/workbook.xml.rels");
    pugi::xml_document doc;
    if (doc.load_buffer(rels.data(), rels.size())) {
      for (const auto& r : doc.select_nodes("//*[local-name()='Relationship']")) {
        const std::string id = r.node().attribute("Id").value();
        std::string tgt = r.node().attribute("Target").value();
        if (tgt.rfind("/xl/", 0) == 0)
          tgt = tgt.substr(1);  // 絶対 → ルート相対
        else if (tgt.rfind("worksheets/", 0) == 0)
          tgt = "xl/" + tgt;
        rid2target[id] = tgt;
      }
    }
  }
  std::vector<std::string> out;
  const std::string wb = zip_read(path, "xl/workbook.xml");
  pugi::xml_document doc;
  if (doc.load_buffer(wb.data(), wb.size())) {
    for (const auto& s : doc.select_nodes("//*[local-name()='sheet']")) {
      // r:id 属性（名前空間つき）。local-name で拾う
      std::string rid;
      for (const auto& a : s.node().attributes())
        if (std::string(a.name()).find(":id") != std::string::npos) rid = a.value();
      const auto it = rid2target.find(rid);
      if (it != rid2target.end()) out.push_back(it->second);
    }
  }
  return out;
}

/// (シート名, worksheet エントリ名) を workbook 順に返す（--sheet の名前解決用）。
inline std::vector<std::pair<std::string, std::string>> sheets_in_order(const std::string& path) {
  std::map<std::string, std::string> rid2target;
  {
    const std::string rels = zip_read(path, "xl/_rels/workbook.xml.rels");
    pugi::xml_document doc;
    if (doc.load_buffer(rels.data(), rels.size())) {
      for (const auto& r : doc.select_nodes("//*[local-name()='Relationship']")) {
        const std::string id = r.node().attribute("Id").value();
        std::string tgt = r.node().attribute("Target").value();
        if (tgt.rfind("/xl/", 0) == 0) tgt = tgt.substr(1);
        else if (tgt.rfind("worksheets/", 0) == 0) tgt = "xl/" + tgt;
        rid2target[id] = tgt;
      }
    }
  }
  std::vector<std::pair<std::string, std::string>> out;
  const std::string wb = zip_read(path, "xl/workbook.xml");
  pugi::xml_document doc;
  if (doc.load_buffer(wb.data(), wb.size())) {
    for (const auto& s : doc.select_nodes("//*[local-name()='sheet']")) {
      const std::string name = s.node().attribute("name").value();
      std::string rid;
      for (const auto& a : s.node().attributes())
        if (std::string(a.name()).find(":id") != std::string::npos) rid = a.value();
      const auto it = rid2target.find(rid);
      if (it != rid2target.end()) out.emplace_back(name, it->second);
    }
  }
  return out;
}

/// 1シートを列位置を保った grid にする（sheet_prose と違い空セルを詰めない＝表マスク用）。
/// 行は `r` 属性（1始まりのシート行番号）で位置づける。これにより --header-row N が
/// スプレッドシートの行番号と一致する（空行・タイトル行があっても正しく引ける）。
inline std::vector<std::vector<std::string>> sheet_rows(const std::string& xml,
                                                        const std::vector<std::string>& shared) {
  pugi::xml_document doc;
  std::vector<std::vector<std::string>> grid;
  if (!doc.load_buffer(xml.data(), xml.size())) return grid;
  int maxcol = -1, maxrow = 0, seq = 0;
  std::map<int, std::map<int, std::string>> byrow;  // シート行(1始まり) → (列 → 値)
  for (const auto& rownode : doc.select_nodes("//*[local-name()='row']")) {
    ++seq;
    int rnum = rownode.node().attribute("r").as_int(0);
    if (rnum <= 0) rnum = seq;
    std::map<int, std::string> cells;
    for (const auto& c : rownode.node().children()) {
      if (!is_cell_node(c.name())) continue;
      const std::string ref = c.attribute("r").value();
      const std::string type = c.attribute("t").value();
      std::string val;
      if (type == "s") {
        const auto v = c.child("v").text().get();
        const int idx = numparse::to_int(v, -1);  // 壊れた <v> でも例外を投げない
        if (idx >= 0 && idx < static_cast<int>(shared.size())) val = shared[idx];
      } else if (type == "inlineStr") {
        for (const auto& t : c.select_nodes(".//*[local-name()='t']"))
          val += t.node().text().get();
      } else {
        val = c.child("v").text().get();
      }
      const int col = col_of(ref);
      if (col < 0) continue;
      cells[col] = val;
      if (col > maxcol) maxcol = col;
    }
    if (!cells.empty()) { byrow[rnum] = std::move(cells); if (rnum > maxrow) maxrow = rnum; }
  }
  if (maxrow == 0 || maxcol < 0) return grid;
  grid.assign(static_cast<std::size_t>(maxrow),
              std::vector<std::string>(static_cast<std::size_t>(maxcol + 1), ""));
  for (const auto& rc : byrow)
    for (const auto& cv : rc.second)
      if (rc.first >= 1 && rc.first <= maxrow && cv.first <= maxcol)
        grid[static_cast<std::size_t>(rc.first - 1)][static_cast<std::size_t>(cv.first)] = cv.second;
  return grid;
}

/// シート名指定で grid を読む（空指定・不一致なら先頭シート）。
inline std::vector<std::vector<std::string>> read_xlsx_sheet_rows(const std::string& path,
                                                                  const std::string& sheet_name) {
  const auto shared = read_shared_strings(path);
  const auto sheets = sheets_in_order(path);
  if (sheets.empty()) return {};
  std::string entry = sheets.front().second;
  if (!sheet_name.empty())
    for (const auto& ns : sheets)
      if (ns.first == sheet_name) { entry = ns.second; break; }
  return sheet_rows(zip_read(path, entry), shared);
}

/// 1シートを行ごとの「非空セルを列順で tab 連結」した文字列にする（_rows_to_prose 相当）。
inline std::string sheet_prose(const std::string& xml, const std::vector<std::string>& shared) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return {};
  std::vector<std::string> lines;
  for (const auto& rownode : doc.select_nodes("//*[local-name()='row']")) {
    // (列番号, 値) を集める。結合セルは左上のみ値を持つので自然に扱える。
    std::vector<std::pair<int, std::string>> cells;
    for (const auto& c : rownode.node().children()) {
      if (!is_cell_node(c.name())) continue;
      const std::string ref = c.attribute("r").value();
      const std::string type = c.attribute("t").value();
      std::string val;
      if (type == "s") {  // sharedStrings のインデックス
        const auto v = c.child("v").text().get();
        const int idx = numparse::to_int(v, -1);  // 壊れた <v> でも例外を投げない
        if (idx >= 0 && idx < static_cast<int>(shared.size())) val = shared[idx];
      } else if (type == "inlineStr") {
        for (const auto& t : c.select_nodes(".//*[local-name()='t']"))
          val += t.node().text().get();
      } else {  // 数値・日付（data_only なら計算済み値が <v> に）
        val = c.child("v").text().get();
      }
      // _rows_to_prose は非空セルのみ（str(c).strip() が真）。空白のみは落とす。
      if (!utf8::trim(val).empty()) cells.emplace_back(col_of(ref), val);
    }
    std::sort(cells.begin(), cells.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (!cells.empty()) {
      std::string line;
      for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i) line += "\t";
        line += cells[i].second;
      }
      lines.push_back(line);
    }
  }
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i) out += "\n";
    out += lines[i];
  }
  return out;
}

inline std::string read_xlsx_prose(const std::string& path) {
  const auto shared = read_shared_strings(path);
  std::vector<std::string> parts;
  for (const auto& entry : sheet_entries_in_order(path)) {
    const std::string p = sheet_prose(zip_read(path, entry), shared);
    if (!p.empty()) parts.push_back(p);  // "\n".join(p for p in parts if p)
  }
  std::string joined;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) joined += "\n";
    joined += parts[i];
  }
  return extractors::strip_ruby(joined);
}

}  // namespace ooxml
