// 「実名に戻す」ウィンドウ。リボンの「実名に戻す」から開く。
//
// サニタイズとは別の日に・別の対象（AI の返答）に対してやる作業なので、タスクペインには
// 載せず、小さな独立ウィンドウにした。モデルは要らない（ヘルパーの reverse は対応表だけで
// 動く）ので、ペインを開いていなくても使える。
//
// 戻す対象は 2 つ（タブ）:
//   文章 — AI の返答を貼り付けて戻し、結果をコピーする
//   シート — AI が返した表を貼ったシートや、AI が編集して返した .xlsx を開いた状態で、
//            アクティブシート（範囲選択があればその範囲）の伏せ字をその場で書き換える。
//            アドインからの書き込みは Excel の「元に戻す」が効かないので、変わるセルを
//            選択して見せ、確認を取ってから書く。数式セルは触らない。
// 対応表の選択とパスフレーズは両タブで共有する。
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using ExcelDna.Integration;
using Newtonsoft.Json.Linq;

namespace JpPii.Excel
{
    public sealed class RestoreDialog : Form
    {
        private static RestoreDialog _instance;

        /// <summary>1 枚だけ出す（既に開いていれば前面へ）。Excel のメインウィンドウを親にする。</summary>
        public static void ShowSingle()
        {
            if (_instance == null || _instance.IsDisposed)
            {
                _instance = new RestoreDialog();
                _instance.Show(new WindowWrapper(ExcelDnaUtil.WindowHandle));
            }
            else
            {
                if (_instance.WindowState == FormWindowState.Minimized) _instance.WindowState = FormWindowState.Normal;
                _instance.Activate();
            }
        }

        public static void CloseSingle()
        {
            try { _instance?.Close(); } catch (Exception) { }
            _instance = null;
        }

        private sealed class WindowWrapper : IWin32Window
        {
            public WindowWrapper(IntPtr h) { Handle = h; }
            public IntPtr Handle { get; }
        }

        private static HelperClient H => HelperClient.Shared;

        // 共有: 対応表・パスフレーズ・状態
        private readonly Button _btnPick = new Button();
        private readonly Label _lblMapping = new Label();
        private readonly TableLayoutPanel _passPanel = new TableLayoutPanel();
        private readonly TextBox _txtPass = new TextBox();
        private readonly TabControl _tabs = new TabControl();
        private readonly Label _status = new Label();
        // 文章タブ
        private readonly TextBox _txtIn = new TextBox();
        private readonly Button _btnRestoreText = new Button();
        private readonly TextBox _txtOut = new TextBox();
        private readonly Button _btnCopy = new Button();
        // シートタブ
        private readonly Button _btnRestoreSheet = new Button();

        private string _mappingPath;
        private bool _busy;

        private RestoreDialog()
        {
            Text = "実名に戻す — PII サニタイザ";
            Font = new Font(SystemFonts.MessageBoxFont.FontFamily, PaneControl.FontPt);   // ペインと同じ文字サイズ
            StartPosition = FormStartPosition.CenterParent;
            MinimumSize = new Size(520, 480);
            Size = new Size(600, 580);
            ShowInTaskbar = false;
            MinimizeBox = false;

            var table = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(10) };
            table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            // ---- 共有部: 対応表 ----
            var pick = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Margin = new Padding(0) };
            pick.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
            pick.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            _btnPick.Text = "対応表を選ぶ…";
            _btnPick.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _lblMapping.Text = "（未選択）　サニタイズ時に「対応表\\」フォルダへ書かれた .jsonl";
            _lblMapping.AutoEllipsis = true;
            _lblMapping.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _lblMapping.TextAlign = ContentAlignment.MiddleLeft;
            _lblMapping.ForeColor = SystemColors.GrayText;
            pick.Controls.Add(_btnPick, 0, 0);
            pick.Controls.Add(_lblMapping, 1, 0);
            AddRow(table, pick);

            _passPanel.ColumnCount = 2;
            _passPanel.AutoSize = true;
            _passPanel.Margin = new Padding(0);
            _passPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
            _passPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            var lblPass = new Label { Text = "パスフレーズ:", TextAlign = ContentAlignment.MiddleLeft, Anchor = AnchorStyles.Left | AnchorStyles.Right };
            _txtPass.UseSystemPasswordChar = true;
            _txtPass.Anchor = AnchorStyles.Left | AnchorStyles.Right;
            _passPanel.Controls.Add(lblPass, 0, 0);
            _passPanel.Controls.Add(_txtPass, 1, 0);
            _passPanel.Visible = false;   // 保護された対応表だと分かってから出す
            AddRow(table, _passPanel);

            // ---- タブ ----
            _tabs.Margin = new Padding(0, 6, 0, 3);
            _tabs.TabPages.Add(BuildTextTab());
            _tabs.TabPages.Add(BuildSheetTab());
            AddRow(table, _tabs, SizeType.Percent, 100);

            _status.AutoSize = false;
            _status.Height = 36;
            _status.TextAlign = ContentAlignment.MiddleLeft;
            _status.AutoEllipsis = true;
            AddRow(table, _status);

            Controls.Add(table);

            _btnPick.Click += (s, e) => Guard(PickMapping);
            _btnRestoreText.Click += (s, e) => Guard(RestoreText);
            _btnRestoreSheet.Click += (s, e) => Guard(RestoreSheet);
            _btnCopy.Click += (s, e) => { if (_txtOut.Text.Length > 0) Clipboard.SetText(_txtOut.Text); SetStatus("コピーしました。"); };
            _txtPass.KeyDown += (s, e) =>
            {
                if (e.KeyCode != Keys.Enter) return;
                e.SuppressKeyPress = true;
                Guard(_tabs.SelectedIndex == 0 ? (Action)RestoreText : RestoreSheet);
            };
            _tabs.SelectedIndexChanged += (s, e) => SetStatus("");
        }

        private TabPage BuildTextTab()
        {
            var page = new TabPage("文章を戻す") { Padding = new Padding(8) };
            var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1 };
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            AddRow(t, new Label { Text = "AI の返答など、伏せ字のままの文章を貼り付けてください。", AutoSize = true });
            _txtIn.Multiline = true;
            _txtIn.ScrollBars = ScrollBars.Vertical;
            _txtIn.AcceptsReturn = true;
            AddRow(t, _txtIn, SizeType.Percent, 50);

            _btnRestoreText.Text = "実名に戻す";
            AddRow(t, _btnRestoreText, height: 30);

            _txtOut.Multiline = true;
            _txtOut.ReadOnly = true;
            _txtOut.ScrollBars = ScrollBars.Vertical;
            AddRow(t, _txtOut, SizeType.Percent, 50);

            _btnCopy.Text = "結果をコピー";
            _btnCopy.Enabled = false;
            _btnCopy.Anchor = AnchorStyles.Right;
            _btnCopy.Width = 110;
            t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            t.Controls.Add(_btnCopy, 0, t.RowCount++);

            page.Controls.Add(t);
            return page;
        }

        private TabPage BuildSheetTab()
        {
            var page = new TabPage("シートを戻す") { Padding = new Padding(8) };
            var t = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1 };
            t.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

            AddRow(t, new Label
            {
                Text = "AI が返した表を貼り付けたシート、または AI が編集して返したブックを開いた状態で押してください。\n\n" +
                       "アクティブシート（範囲を選択していればその範囲）のセルにある伏せ字を、その場で実名に書き換えます。" +
                       "書き換える前に、変わるセルをシート上で選択して見せ、確認を求めます。\n\n" +
                       "注意: この書き換えは Excel の「元に戻す」で取り消せません。元を残したいときは先にコピーを保存してください。" +
                       "数式のセルは書き換えません。",
                AutoSize = false,
                Height = 150,
                Anchor = AnchorStyles.Left | AnchorStyles.Right,
            });
            _btnRestoreSheet.Text = "シートの伏せ字を実名に戻す…";
            AddRow(t, _btnRestoreSheet, height: 32);

            page.Controls.Add(t);
            return page;
        }

        private static void AddRow(TableLayoutPanel table, Control c, SizeType type = SizeType.AutoSize, float size = 0, int height = 0)
        {
            c.Anchor = AnchorStyles.Left | AnchorStyles.Right | (type == SizeType.Percent ? AnchorStyles.Top | AnchorStyles.Bottom : 0);
            if (c.Margin == new Padding(3)) c.Margin = new Padding(0, 3, 0, 3);
            if (height > 0) c.Height = height;
            table.RowStyles.Add(new RowStyle(type, size));
            table.Controls.Add(c, 0, table.RowCount++);
        }

        // ------------------------------------------------------------ plumbing
        private void Guard(Action a)
        {
            if (_busy) return;
            try { a(); }
            catch (Exception e) { ShowError(e); }
        }

        private void ShowError(Exception e)
        {
            SetBusy(false);
            SetStatus("エラー: " + e.Message, error: true);
            MessageBox.Show(this, e.Message, "実名に戻す", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }

        private void SetStatus(string text, bool error = false)
        {
            _status.Text = text;
            _status.ForeColor = error ? Color.Firebrick : SystemColors.ControlText;
        }

        private void SetBusy(bool busy)
        {
            _busy = busy;
            _btnPick.Enabled = _btnRestoreText.Enabled = _btnRestoreSheet.Enabled = !busy;
            UseWaitCursor = busy;
            if (busy) SetStatus("実名に戻しています…");
        }

        /// <summary>work をバックグラウンドで実行し、done をメインスレッドで呼ぶ（COM はメインスレッドのみ）。</summary>
        private void RunAsync(Func<JObject> work, Action<JObject> done)
        {
            SetBusy(true);
            Task.Run(() =>
            {
                try { return Tuple.Create(work(), (Exception)null); }
                catch (Exception e) { return Tuple.Create((JObject)null, e); }
            }).ContinueWith(t =>
            {
                if (IsDisposed) return;
                BeginInvoke(new Action(() =>
                {
                    SetBusy(false);
                    if (t.Result.Item2 != null) { ShowError(t.Result.Item2); return; }
                    try { done(t.Result.Item1); }
                    catch (Exception e) { ShowError(e); }
                }));
            });
        }

        private void RequireMapping()
        {
            if (string.IsNullOrEmpty(_mappingPath)) throw new InvalidOperationException("対応表を選んでください。");
        }

        private string CurrentPassphrase() => _passPanel.Visible ? _txtPass.Text : "";

        /// <summary>
        /// reverse 応答の共通処理。ok なら true。パスフレーズが要るときは入力欄を出して false、
        /// それ以外の失敗は例外。
        /// </summary>
        private bool CheckReverseResponse(JObject resp)
        {
            _txtPass.Clear();
            if (resp.Value<bool?>("ok") == true) return true;
            if (resp.Value<bool?>("needsPassphrase") == true)
            {
                _passPanel.Visible = true;
                _txtPass.Focus();
                SetStatus("この対応表はパスフレーズで保護されています。入力してもう一度「実名に戻す」を押してください。");
                return false;
            }
            throw new HelperException("逆置換に失敗しました: " + resp.Value<string>("error"));
        }

        private static List<string> Leftovers(JObject resp) =>
            ((JArray)resp["leftovers"])?.Select(x => (string)x).ToList() ?? new List<string>();

        private static string LeftoverNote(List<string> left) =>
            left.Count == 0 ? "" : "　戻せなかった伏せ字（対応表が違う可能性）: " + string.Join(", ", left.Take(5)) + (left.Count > 5 ? " …" : "");

        // ------------------------------------------------------------ 対応表
        private void PickMapping()
        {
            using (var dlg = new OpenFileDialog { Title = "対応表", Filter = "対応表 (JSONL)|*.jsonl|すべて|*.*" })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                _mappingPath = dlg.FileName;
                _lblMapping.Text = _mappingPath;
                _lblMapping.ForeColor = SystemColors.ControlText;
                _passPanel.Visible = false;
                _txtPass.Clear();
            }
        }

        // ------------------------------------------------------------ 文章
        private void RestoreText()
        {
            RequireMapping();
            var text = _txtIn.Text;
            if (text.Length == 0) throw new InvalidOperationException("戻したいテキスト（AI の返答）を貼り付けてください。");
            var pass = CurrentPassphrase();
            var mpath = _mappingPath;
            RunAsync(() => H.ReverseText(text, mpath, pass), resp =>
            {
                if (!CheckReverseResponse(resp)) return;
                _txtOut.Text = (string)resp["restored"];
                _btnCopy.Enabled = _txtOut.Text.Length > 0;
                var left = Leftovers(resp);
                SetStatus("実名に戻しました。" + LeftoverNote(left), error: left.Count > 0);
            });
        }

        // ------------------------------------------------------------ シート
        /// <summary>
        /// シートを読む（メインスレッド）→ ヘルパーで逆置換（背景）→ 変わるセルを選択して確認 → 書き戻す。
        /// 対応表の中身はヘルパー側に留まり、アドインには変わったセルの結果だけが返る。
        /// </summary>
        private void RestoreSheet()
        {
            RequireMapping();
            var snap = SheetIO.ReadTarget();
            if (snap.Cells.Count == 0)
            {
                SetStatus(snap.Description + " に値のあるセルがありません。");
                return;
            }
            var cells = new JArray();
            foreach (var c in snap.Cells)
                cells.Add(new JObject { ["r"] = c.Row, ["c"] = c.Col, ["text"] = c.Text });
            var pass = CurrentPassphrase();
            var mpath = _mappingPath;
            RunAsync(() => H.ReverseCells(cells, mpath, pass), resp =>
            {
                if (!CheckReverseResponse(resp)) return;
                var changed = new List<CellText>();
                foreach (var c in (JArray)resp["cells"])
                    changed.Add(new CellText { Row = (int)c["r"], Col = (int)c["c"], Text = (string)c["text"] });
                var left = Leftovers(resp);
                if (changed.Count == 0)
                {
                    SetStatus(snap.Description + " にこの対応表で戻せる伏せ字はありません。" + LeftoverNote(left), error: left.Count > 0);
                    return;
                }

                // 見せてから書く。選択されたセルがそのまま書き換わる
                SheetIO.SelectCells(snap, changed, maxCells: 2000);
                var answer = MessageBox.Show(this,
                    snap.Description + " の " + changed.Count + " セルを実名に書き換えます（選択したセル）。\n\n" +
                    "この書き換えは Excel の「元に戻す」で取り消せません。続けますか？" +
                    (left.Count > 0 ? "\n\n戻せない伏せ字も残ります: " + string.Join(", ", left.Take(5)) : ""),
                    "実名に戻す", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
                if (answer != DialogResult.Yes)
                {
                    SetStatus("書き換えを中止しました（シートは変更していません）。");
                    return;
                }
                var skipped = SheetIO.WriteBack(snap, changed);
                var written = changed.Count - skipped.Count;
                SetStatus(snap.Description + "　" + written + " セルを実名に戻しました。" +
                          (skipped.Count > 0 ? "　数式セル " + skipped.Count + " 件は変更していません: " + string.Join(", ", skipped.Take(5)) + (skipped.Count > 5 ? " …" : "") : "") +
                          LeftoverNote(left) +
                          "　このシートは実名を含みます。AI には渡さないでください。",
                          error: left.Count > 0 || skipped.Count > 0);
            });
        }
    }
}
