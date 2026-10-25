// Excel との境界。読むのはセルの**表示文字列**、書くのはサニタイズ済みの別ブック。
//
// ここだけが COM（Excel のオブジェクトモデル）に触る。必ず Excel のメインスレッドで
// 呼ぶこと（タスクペインのイベントハンドラはメインスレッド）。
//
// 対象は**セルの値のみ**（docs/excel-helper.md）。コメント・図形・数式・ヘッダー／
// フッターは読まない。だから出力の別ブックにも持ち越さない——読んでいないものを
// 持ち越すと「確認していない内容が送られる」ことになり、DESIGN.md の「画面に出て
// いるものが送られるもの」を一番危ない向きに破る。
//
// サニタイズでは元ブックに**一切書かない**。COM から書くと Excel の「元に戻す」履歴が消える。
// プレビューのハイライトも塗らずに「セルを選択する」で代える（選択は変更ではない）。
// 例外は「実名に戻す」の書き戻し（WriteBack）だけ。これは利用者が「戻す」と言った
// セルをその場で書き換える操作なので、変わるセルを選択して見せ、確認を取ってから書く。
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using ExcelDna.Integration;

namespace JpPii.Excel
{
    public sealed class CellText
    {
        public int Row;   // 1 始まり（シート上の絶対位置）
        public int Col;
        public string Text;
    }

    public sealed class SheetSnapshot
    {
        public dynamic Sheet;                 // 読み元（選択に使う）
        public string SheetName;
        public string Description;            // 例: Sheet1!A1:D20
        public string WorkbookDir = "";       // 元ブックのフォルダ（未保存なら空）。保存ダイアログの既定に使う
        public string WorkbookBaseName = "";  // 元ブックのファイル名（拡張子なし）
        public int TableHeaderRow;            // 範囲がテーブル（ListObject）に掛かっていればその見出し行。無ければ 0
        public List<CellText> Cells = new List<CellText>();
    }

    internal static class SheetIO
    {
        private static dynamic App => ExcelDnaUtil.Application;

        /// <summary>
        /// アクティブシート（複数セルを選択していればその範囲）の非空セルを表示文字列で読む。
        /// </summary>
        public static SheetSnapshot ReadTarget()
        {
            dynamic app = App;
            dynamic sheet = app.ActiveSheet;
            if (sheet == null) throw new InvalidOperationException("アクティブなシートがありません。");
            string sheetName;
            try { sheetName = (string)sheet.Name; }
            catch (Exception) { throw new InvalidOperationException("ワークシートではありません（グラフシート等は対象外です）。"); }

            dynamic used = sheet.UsedRange;
            var areas = new List<dynamic>();
            var selectionUsed = false;
            try
            {
                dynamic sel = app.Selection;
                // 単一セルの選択は「選択なし」とみなしてシート全体。CountLarge は列全体選択でも溢れない。
                if (sel != null && IsRange(sel) && (double)sel.CountLarge > 1)
                {
                    foreach (dynamic area in sel.Areas)
                    {
                        dynamic hit = app.Intersect(area, used);
                        if (hit != null) areas.Add(hit);
                    }
                    selectionUsed = true;
                }
            }
            catch (Exception) { areas.Clear(); selectionUsed = false; }
            if (!selectionUsed) areas.Add(used);

            var snap = new SheetSnapshot { Sheet = sheet, SheetName = sheetName };
            try
            {
                dynamic wb = sheet.Parent;
                snap.WorkbookDir = (string)wb.Path ?? "";
                snap.WorkbookBaseName = System.IO.Path.GetFileNameWithoutExtension((string)wb.Name ?? "");
            }
            catch (Exception) { }
            var desc = new StringBuilder();
            foreach (dynamic area in areas)
            {
                if (desc.Length > 0) desc.Append(",");
                desc.Append(SafeAddress(area));
                ReadArea(area, snap.Cells);
            }
            snap.Description = sheetName + "!" + desc + (selectionUsed ? "（選択範囲）" : "（シート全体）");
            snap.TableHeaderRow = FindTableHeaderRow(app, sheet, areas);
            return snap;
        }

        /// <summary>
        /// 読んだ範囲が Excel のテーブル（ListObject）に掛かっていれば、その見出し行を返す。
        /// テーブルなら見出し行は Excel が知っているので推定しない。
        /// </summary>
        private static int FindTableHeaderRow(dynamic app, dynamic sheet, List<dynamic> areas)
        {
            try
            {
                foreach (dynamic lo in sheet.ListObjects)
                {
                    dynamic hdr = lo.HeaderRowRange;
                    if (hdr == null) continue;
                    foreach (dynamic area in areas)
                        if (app.Intersect(area, lo.Range) != null) return (int)hdr.Row;
                }
            }
            catch (Exception) { }
            return 0;
        }

        private static bool IsRange(dynamic obj)
        {
            try { var _ = obj.Areas; return true; }
            catch (Exception) { return false; }
        }

        private static string SafeAddress(dynamic range)
        {
            try { return (string)range.Address(false, false); }
            catch (Exception) { return "?"; }
        }

        private static void ReadArea(dynamic area, List<CellText> into)
        {
            int row0 = (int)area.Row, col0 = (int)area.Column;
            object raw = area.Value2;
            object[,] vals = raw as object[,];
            if (vals == null)
            {
                // 単一セルは配列ではなくスカラで返る
                vals = new object[2, 2];
                vals[1, 1] = raw;
            }
            int rows = vals.GetLength(0), cols = vals.GetLength(1);
            for (var i = 1; i <= rows; i++)
            {
                for (var j = 1; j <= cols; j++)
                {
                    var v = vals[i, j];
                    if (v == null) continue;
                    string text;
                    if (v is string s)
                    {
                        text = s;
                    }
                    else
                    {
                        // 数値・日付・真偽・エラーは**表示文字列**で読む。先頭 0 の電話番号や
                        // 日付は Value2（double）では別物になる。
                        dynamic cell = area.Cells[i, j];
                        text = DisplayText(cell, v);
                    }
                    if (text.Trim().Length == 0) continue;
                    into.Add(new CellText { Row = row0 + i - 1, Col = col0 + j - 1, Text = text });
                }
            }
        }

        /// <summary>
        /// セルの表示文字列。列幅が足りないと .Text は「####」を返すので、そのときは
        /// 表示形式を使って自分で整形する（TEXT 関数と同じ）。
        /// </summary>
        private static string DisplayText(dynamic cell, object value)
        {
            string t;
            try { t = (string)cell.Text ?? ""; }
            catch (Exception) { t = ""; }
            if (t.Length > 0 && t.Trim('#').Length == 0)
            {
                try
                {
                    string fmt = (string)cell.NumberFormat;
                    t = (string)App.WorksheetFunction.Text(value, fmt);
                }
                catch (Exception)
                {
                    t = Convert.ToString(value, CultureInfo.InvariantCulture);
                }
            }
            if (t.Length == 0) t = Convert.ToString(value, CultureInfo.InvariantCulture) ?? "";
            return t;
        }

        /// <summary>
        /// 候補のセルを選択して見せる（塗らない）。多すぎるときは先頭から maxCells 個。
        /// 同じ列で行が連続するセルは 1 つの矩形にまとめてから Union する（列まるごとのとき
        /// セル数ぶん Union を呼ぶと秒単位で待たされる）。
        /// </summary>
        public static int SelectCells(SheetSnapshot snap, IEnumerable<CellText> cells, int maxCells = 500)
        {
            dynamic app = App;
            dynamic sheet = snap.Sheet;
            try { sheet.Activate(); } catch (Exception) { }
            var list = new List<CellText>();
            foreach (var c in cells)
            {
                if (list.Count >= maxCells) break;
                list.Add(c);
            }
            var n = list.Count;
            list.Sort((a, b) => a.Col != b.Col ? a.Col.CompareTo(b.Col) : a.Row.CompareTo(b.Row));
            dynamic union = null;
            var i = 0;
            while (i < list.Count)
            {
                var j = i;
                while (j + 1 < list.Count && list[j + 1].Col == list[i].Col && list[j + 1].Row == list[j].Row + 1) j++;
                dynamic r = sheet.Range[sheet.Cells[list[i].Row, list[i].Col], sheet.Cells[list[j].Row, list[j].Col]];
                union = union == null ? r : app.Union(union, r);
                i = j + 1;
            }
            if (union != null)
            {
                try { union.Select(); } catch (Exception) { }
            }
            return n;
        }

        /// <summary>
        /// 「実名に戻す」の書き戻し: 変わったセルだけを、読んだシートのその場に文字列として書く。
        /// 数式セルは書かない（値で上書きすると数式が消える）——飛ばしたセルのアドレスを返す。
        /// 呼ぶ前に SelectCells で見せて確認を取ること。Excel の「元に戻す」は効かない。
        /// </summary>
        public static List<string> WriteBack(SheetSnapshot snap, IEnumerable<CellText> changed)
        {
            dynamic sheet = snap.Sheet;
            var skipped = new List<string>();
            foreach (var c in changed)
            {
                dynamic cell = sheet.Cells[c.Row, c.Col];
                bool formula = false;
                try { formula = (bool)cell.HasFormula; } catch (Exception) { }
                if (formula)
                {
                    skipped.Add(ColumnRules.ColumnLetter(c.Col) + c.Row);
                    continue;
                }
                // 文字列として書く。先頭が = や + の実名（まずないが）が数式扱いになるのを避ける
                cell.NumberFormat = "@";
                cell.Value2 = c.Text;
            }
            return skipped;
        }

        /// <summary>矩形範囲を選択して見せる（列の扱いの行をクリックしたとき）。</summary>
        public static void SelectRange(SheetSnapshot snap, int r1, int c1, int r2, int c2)
        {
            dynamic sheet = snap.Sheet;
            try { sheet.Activate(); } catch (Exception) { }
            try { sheet.Range[sheet.Cells[r1, c1], sheet.Cells[r2, c2]].Select(); } catch (Exception) { }
        }

        /// <summary>
        /// サニタイズ済みの別ブックを作る。読んだセルをそのままの位置に**文字列として**書き、
        /// 変わったセルは置換後の値にする。書式・数式・コメント・図形は持ち越さない。
        /// savePath を渡すと .xlsx として保存する（開いたままにする。中身を確認できるように）。
        /// 保存に失敗したら作ったブックを閉じて例外にする——「未保存の別ブックだけ残る」状態を作らない。
        /// </summary>
        public static dynamic WriteOutputWorkbook(SheetSnapshot snap, IDictionary<long, string> replacements, string savePath = null)
        {
            dynamic wb = BuildOutputWorkbook(snap, replacements);
            if (string.IsNullOrEmpty(savePath)) return wb;
            dynamic app = App;
            var prevAlerts = app.DisplayAlerts;
            try
            {
                app.DisplayAlerts = false;   // 上書き確認はダイアログ側で済んでいる
                wb.SaveAs(savePath, 51);     // 51 = xlOpenXMLWorkbook (.xlsx)
            }
            catch (Exception e)
            {
                try { wb.Close(false); } catch (Exception) { }
                throw new InvalidOperationException("サニタイズ済みブックを保存できませんでした: " + savePath + "\n" + e.Message, e);
            }
            finally
            {
                app.DisplayAlerts = prevAlerts;
            }
            return wb;
        }

        private static dynamic BuildOutputWorkbook(SheetSnapshot snap, IDictionary<long, string> replacements)
        {
            dynamic app = App;
            if (snap.Cells.Count == 0) throw new InvalidOperationException("書き出すセルがありません。");
            int minR = int.MaxValue, minC = int.MaxValue, maxR = 0, maxC = 0;
            foreach (var c in snap.Cells)
            {
                if (c.Row < minR) minR = c.Row;
                if (c.Col < minC) minC = c.Col;
                if (c.Row > maxR) maxR = c.Row;
                if (c.Col > maxC) maxC = c.Col;
            }
            var arr = new object[maxR - minR + 1, maxC - minC + 1];
            foreach (var c in snap.Cells)
            {
                string text;
                if (!replacements.TryGetValue(Key(c.Row, c.Col), out text)) text = c.Text;
                // 先頭が = のテキストは数式として解釈されうる。アポストロフィ接頭で文字列に固定する
                // （接頭辞は値には含まれない）。
                if (text.Length > 0 && text[0] == '=') text = "'" + text;
                arr[c.Row - minR, c.Col - minC] = text;
            }

            var prevAlerts = app.DisplayAlerts;
            var prevUpdating = app.ScreenUpdating;
            dynamic wb;
            try
            {
                app.ScreenUpdating = false;
                app.DisplayAlerts = false;
                wb = app.Workbooks.Add();
                dynamic ws = wb.Worksheets[1];
                try { ws.Name = SafeSheetName(snap.SheetName); } catch (Exception) { }
                dynamic rng = ws.Range[ws.Cells[minR, minC], ws.Cells[maxR, maxC]];
                rng.NumberFormat = "@";   // 文字列として固定（"03-1234" を日付にさせない）
                rng.Value2 = arr;
                // 列幅だけは写す（見た目の補助。内容ではない）
                for (var col = minC; col <= maxC; col++)
                {
                    try { ws.Columns[col].ColumnWidth = snap.Sheet.Columns[col].ColumnWidth; }
                    catch (Exception) { }
                }
                try { ws.Cells[minR, minC].Select(); } catch (Exception) { }
            }
            finally
            {
                app.DisplayAlerts = prevAlerts;
                app.ScreenUpdating = prevUpdating;
            }
            return wb;
        }

        public static long Key(int row, int col) => ((long)row << 20) | (uint)col;

        private static string SafeSheetName(string name)
        {
            var sb = new StringBuilder();
            foreach (var ch in name)
                sb.Append("[]:*?/\\".IndexOf(ch) >= 0 ? '_' : ch);
            var s = sb.ToString().Trim('\'');
            if (s.Length == 0) s = "sanitized";
            if (s.Length > 31) s = s.Substring(0, 31);
            return s;
        }
    }
}
