// 列の扱い（列指定）。表データでは「列＝属性」なので、列単位で種別を決められるようにする。
//
//   自動検出 … 今まで通り NER と番号系の正規表現に任せる（自由記述の列）
//   人名／組織／住所／電話／メール／識別子 … その列の空でないセルは**セル全体を 1 つの
//                トークン**にする（ヘルパーの ColumnRule。docs/excel-helper.md）
//   対象外   … 検知も置換もしない（誤検出の多い商品名・部署名などを黙らせる）
//
// 種別の提案は 3 段: 前回の設定（見出しが一致） → 見出し語 → 検出結果の密度。
// 提案は必ず表に見える形で出し、黙って適用しない。
//
// 設定の保存先は %LOCALAPPDATA%\jp-pii-sanitizer\excel-column-rules.json。中身は
// 見出し名と種別の対応だけで、**セルの値は一切書かない**（対応表とは別物）。
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Newtonsoft.Json.Linq;

namespace JpPii.Excel
{
    internal static class Kinds
    {
        public const string Auto = "AUTO";
        public const string Person = "PERSON";
        public const string Org = "ORGANIZATION";
        public const string Loc = "LOCATION";
        public const string Phone = "PHONE";
        public const string Email = "EMAIL";
        public const string Id = "ID";
        public const string None = "NONE";

        /// <summary>ドロップダウンの並び。</summary>
        public static readonly string[] Order = { Auto, Person, Org, Loc, Phone, Email, Id, None };

        public static string Label(string kind)
        {
            switch (kind)
            {
                case Auto: return "自動検出";
                case Person: return "人名";
                case Org: return "組織";
                case Loc: return "住所";
                case Phone: return "電話";
                case Email: return "メール";
                case Id: return "識別子";
                case None: return "対象外";
                default: return kind;
            }
        }

        public static string FromLabel(string label)
        {
            foreach (var k in Order) if (Label(k) == label) return k;
            return Auto;
        }

        /// <summary>
        /// 列の値を辞書（confirmed）にも載せる種別。人名・組織・住所は他の列（備考など）での
        /// 言及にも同じトークンを当てたい。電話・メール・識別子は載せない——短い識別子
        /// （"1"、"A"）を辞書にすると本文中の無関係な文字まで伏せてしまう。
        /// </summary>
        public static bool IsDictionaryKind(string kind) => kind == Person || kind == Org || kind == Loc;

        /// <summary>セル全体をトークンにする種別か（自動検出・対象外以外）。</summary>
        public static bool IsWholeCellKind(string kind) => kind != Auto && kind != None;

        /// <summary>プレビューのスパンの category → 種別。</summary>
        public static string FromCategory(string category)
        {
            switch (category)
            {
                case "person": return Person;
                case "company": return Org;
                case "address": return Loc;
                case "phone": return Phone;
                case "email": return Email;
                case "id": return Id;
                default: return null;  // postal は列の種別にしない（自動検出で拾える）
            }
        }
    }

    /// <summary>読み込んだ範囲の 1 列。</summary>
    internal sealed class ColumnInfo
    {
        public int Col;
        public string Letter;
        public string Header = "";                       // 見出し行を使わないときは空
        public string Kind = Kinds.Auto;
        public string Reason = "";                       // 提案の根拠（前回の設定／見出し／検出 n/m）
        public List<CellText> DataCells = new List<CellText>();  // 見出し行を除いた非空セル
        public int MaskedCells;                          // 直近のプレビューでスパンを持つセル数

        /// <summary>列の値（前後の空白を除く・重複なし・出現順）。</summary>
        public List<string> DistinctValues()
        {
            var seen = new HashSet<string>();
            var list = new List<string>();
            foreach (var c in DataCells)
            {
                var v = c.Text.Trim();
                if (v.Length > 0 && seen.Add(v)) list.Add(v);
            }
            return list;
        }
    }

    internal static class ColumnRules
    {
        /// <summary>
        /// 列の値のうち、最初から伏せない定型値（記号・「なし」等）。人名列の「-」が [人名3] に
        /// なるのを防ぐ。候補一覧で「列の値も表示」にすればチェックが外れた状態で見え、戻せる。
        /// </summary>
        public static readonly HashSet<string> PlaceholderValues = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "-", "—", "－", "―", "ー", "−", "–", "/", "／", "*", "＊", "?", "？", "…", "・",
            "なし", "無し", "無", "ナシ", "N/A", "NA", "n.a.", "null", "none", "未定", "不明", "未設定", "未入力",
            "未登録", "該当なし", "同上", "〃", "TBD", "TBA", "(空白)", "（空白）", "空欄",
        };

        /// <summary>
        /// 見出し行の推定。テーブルなら Excel の答え（tableHeaderRow）。そうでなければ先頭 10 行を見て、
        /// 「列の 8 割以上が空でない文字列で、数値・日付らしいセルが無い」最初の行。タイトル行
        /// （「顧客一覧 2024 年度」が 1 セルだけ）は埋まりが足りず外れ、データ行は数値・日付で外れる。
        /// 見つからなければ先頭行。
        /// </summary>
        public static int GuessHeaderRow(IList<CellText> cells, int minRow, int tableHeaderRow)
        {
            if (tableHeaderRow >= minRow) return tableHeaderRow;
            var cols = new HashSet<int>();
            foreach (var c in cells) cols.Add(c.Col);
            if (cols.Count == 0) return minRow;
            for (var row = minRow; row < minRow + 10; row++)
            {
                int text = 0, numeric = 0;
                foreach (var c in cells)
                {
                    if (c.Row != row) continue;
                    var v = c.Text.Trim();
                    if (v.Length == 0) continue;
                    if (LooksNumericOrDate(v)) numeric++;
                    else text++;
                }
                if (text == 0 && numeric == 0) continue;
                if (numeric == 0 && text * 10 >= cols.Count * 8) return row;
            }
            return minRow;
        }

        private static readonly Regex NumericLike = new Regex(@"^[\d０-９.,．，/／\-－:：\s年月日時分秒%％¥￥$+±()（）]+$", RegexOptions.Compiled);

        private static bool LooksNumericOrDate(string v) => NumericLike.IsMatch(v) && Regex.IsMatch(v, @"[\d０-９]");

        public static string ColumnLetter(int col)
        {
            var s = "";
            while (col > 0)
            {
                col--;
                s = (char)('A' + col % 26) + s;
                col /= 26;
            }
            return s;
        }

        /// <summary>
        /// 見出し語から種別を推定する。確度の高い語だけ。曖昧な語（顧客名・名称・担当）は
        /// 検出密度に任せる。返り値 null は「分からない」。
        /// </summary>
        public static string SuggestFromHeader(string header)
        {
            if (string.IsNullOrWhiteSpace(header)) return null;
            var h = header.Trim();
            var u = h.ToUpperInvariant().Normalize(System.Text.NormalizationForm.FormKC);  // 全角英数→半角

            if (u.Contains("郵便")) return null;  // 郵便番号は番号系の正規表現で拾う
            if (u.Contains("メール") || u.Contains("MAIL")) return Kinds.Email;
            if (u.Contains("電話") || u.Contains("TEL") || u.Contains("携帯") || u.Contains("FAX") ||
                u.Contains("ファックス") || u.Contains("内線")) return Kinds.Phone;
            if (u.Contains("住所") || u.Contains("所在地") || u.Contains("宛先") || u.Contains("送付先"))
                return Kinds.Loc;
            if (u.Contains("会社") || u.Contains("法人") || u.Contains("取引先") || u.Contains("企業") ||
                u.Contains("団体") || u.Contains("組織名") || u.Contains("勤務先")) return Kinds.Org;
            if (u.Contains("氏名") || u.Contains("名前") || u.Contains("姓名") || u.Contains("担当者") ||
                u.Contains("申請者") || u.Contains("代表者") || u.Contains("責任者") || u.Contains("作成者") ||
                u.Contains("承認者") || u.Contains("依頼者") || u.Contains("紹介者") || u.Contains("フリガナ") ||
                u.Contains("ふりがな") || u == "姓" || u == "名" || u == "氏" || u == "セイ" || u == "メイ")
                return Kinds.Person;
            if (u.Contains("番号") || u.Contains("コード") || u.Contains("ナンバー") ||
                Regex.IsMatch(u, @"(^|[^A-Z])ID($|[^A-Z])") || Regex.IsMatch(u, @"(^|[^A-Z])NO\.?($|[^A-Z])") ||
                u.Contains("№")) return Kinds.Id;
            return null;
        }

        /// <summary>
        /// 検出結果の密度から種別を推定する。データセルの 6 割以上が同じカテゴリで当たっていれば
        /// その種別。2 セル未満では判断しない。
        /// </summary>
        public static string SuggestFromDensity(ColumnInfo col, IDictionary<long, HashSet<string>> categoriesByCell, out string why)
        {
            why = "";
            var n = col.DataCells.Count;
            if (n < 2) return null;
            var count = new Dictionary<string, int>();
            foreach (var c in col.DataCells)
            {
                HashSet<string> cats;
                if (!categoriesByCell.TryGetValue(SheetIO.Key(c.Row, c.Col), out cats)) continue;
                foreach (var cat in cats)
                {
                    var k = Kinds.FromCategory(cat);
                    if (k == null) continue;
                    int v;
                    count[k] = (count.TryGetValue(k, out v) ? v : 0) + 1;
                }
            }
            if (count.Count == 0) return null;
            var best = count.OrderByDescending(kv => kv.Value).First();
            if (best.Value * 10 < n * 6) return null;
            why = "検出 " + best.Value + "/" + n;
            return best.Key;
        }
    }

    /// <summary>
    /// 見出し名 → 種別 の記憶。サニタイズを実行した時点の割り当てを覚え、次に同じ見出しの
    /// 表を読んだときに提案へ反映する。並びが完全に一致する表の記憶が最優先、それが無ければ
    /// 見出し名ごとに引き継ぐ。
    /// </summary>
    internal sealed class ColumnMemory
    {
        private const int MaxTables = 100;
        private readonly string _path;
        private JObject _doc;

        public ColumnMemory() : this(DefaultPath) { }

        public ColumnMemory(string path)
        {
            _path = path;
            Load();
        }

        public static string DefaultPath =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                         "jp-pii-sanitizer", "excel-column-rules.json");

        public string FilePath => _path;

        private void Load()
        {
            try
            {
                _doc = File.Exists(_path) ? JObject.Parse(File.ReadAllText(_path)) : null;
            }
            catch (Exception) { _doc = null; }  // 壊れていれば捨てる（次の保存で作り直す）
            if (_doc == null || _doc["tables"] == null || _doc["headers"] == null)
                _doc = new JObject { ["version"] = 1, ["tables"] = new JArray(), ["headers"] = new JObject() };
        }

        /// <summary>
        /// 見出しの並びから種別を引く。exact=true なら同じ並びの表の記憶。null の要素は「記憶なし」。
        /// </summary>
        public string[] Lookup(IList<string> headers, out bool exact)
        {
            exact = false;
            var result = new string[headers.Count];
            if (headers.All(string.IsNullOrWhiteSpace)) return result;

            foreach (var t in (JArray)_doc["tables"])
            {
                var hs = t["headers"] as JArray;
                var ks = t["kinds"] as JArray;
                if (hs == null || ks == null || hs.Count != headers.Count || ks.Count != headers.Count) continue;
                var same = true;
                for (var i = 0; i < hs.Count && same; i++) same = (string)hs[i] == headers[i];
                if (!same) continue;
                for (var i = 0; i < ks.Count; i++) result[i] = (string)ks[i];
                exact = true;
                return result;
            }

            var byHeader = (JObject)_doc["headers"];
            for (var i = 0; i < headers.Count; i++)
            {
                if (string.IsNullOrWhiteSpace(headers[i])) continue;
                var k = (string)byHeader[headers[i]];
                if (!string.IsNullOrEmpty(k)) result[i] = k;
            }
            return result;
        }

        /// <summary>サニタイズ時の割り当てを覚える。見出しが全部空なら何もしない。</summary>
        public void Remember(IList<string> headers, IList<string> kinds)
        {
            if (headers.Count == 0 || headers.Count != kinds.Count || headers.All(string.IsNullOrWhiteSpace)) return;

            var tables = (JArray)_doc["tables"];
            for (var i = tables.Count - 1; i >= 0; i--)
            {
                var hs = tables[i]["headers"] as JArray;
                if (hs != null && hs.Count == headers.Count && hs.Select(x => (string)x).SequenceEqual(headers))
                    tables.RemoveAt(i);
            }
            tables.Insert(0, new JObject
            {
                ["headers"] = new JArray(headers.ToArray()),
                ["kinds"] = new JArray(kinds.ToArray()),
                ["updated"] = DateTime.Now.ToString("yyyy-MM-ddTHH:mm:ss"),
            });
            while (tables.Count > MaxTables) tables.RemoveAt(tables.Count - 1);

            // 見出し名ごとの記憶は「自動検出以外」だけ。自動検出に戻した見出しは記憶を消す
            // （古い割り当てが提案に居座らないように）。
            var byHeader = (JObject)_doc["headers"];
            for (var i = 0; i < headers.Count; i++)
            {
                var h = headers[i];
                if (string.IsNullOrWhiteSpace(h)) continue;
                if (kinds[i] == Kinds.Auto) byHeader.Remove(h);
                else byHeader[h] = kinds[i];
            }
            Save();
        }

        private void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(_path));
                File.WriteAllText(_path, _doc.ToString());
            }
            catch (Exception) { /* 記憶できなくても本体の動作には影響しない */ }
        }
    }
}
