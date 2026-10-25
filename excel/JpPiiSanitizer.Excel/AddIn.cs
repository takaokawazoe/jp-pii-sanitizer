// Excel アドインの入口（docs/excel-helper.md）。
//
// 役割は 3 つだけ: リボンにボタンを出す・タスクペインを開く・終了時にヘルパーを畳む。
// 検知もマスクもここではやらない——全部ヘルパー（別プロセス）に送る。
using System.Runtime.InteropServices;
using ExcelDna.Integration;
using ExcelDna.Integration.CustomUI;

namespace JpPii.Excel
{
    public class AddIn : IExcelAddIn
    {
        // 起動時にヘルパーを立てない。モデルは約 1 GB 常駐するので、使うと決めた
        // とき（ペインを開いて「読み込んで検出」）に初めて起こす。
        public void AutoOpen() { }

        public void AutoClose()
        {
            Pane.Close();
            RestoreDialog.CloseSingle();
            HelperClient.DisposeShared();
        }
    }

    [ComVisible(true)]
    public class Ribbon : ExcelRibbon
    {
        public override string GetCustomUI(string ribbonId)
        {
            return @"<customUI xmlns='http://schemas.microsoft.com/office/2009/07/customui'>
  <ribbon>
    <tabs>
      <tab id='jppiiTab' label='PII'>
        <group id='jppiiGroup' label='PII サニタイザ'>
          <button id='jppiiOpen' label='サニタイズ' size='large' imageMso='ReviewProtectSheet'
                  screentip='個人情報をマスクして別ブックを作る'
                  supertip='アクティブシート（範囲を選択していればその範囲）のセルの値を検出・マスクし、サニタイズ済みの別ブックを作ります。'
                  onAction='OnOpenPane'/>
          <button id='jppiiRestore' label='実名に戻す' size='large' imageMso='ReviewUnprotectSheet'
                  screentip='伏せ字の文章を実名に戻す'
                  supertip='AI の返答など伏せ字のままの文章を貼り付け、サニタイズ時に書かれた対応表で実名に戻します。'
                  onAction='OnOpenRestore'/>
        </group>
      </tab>
    </tabs>
  </ribbon>
</customUI>";
        }

        public void OnOpenPane(IRibbonControl control)
        {
            Pane.Show();
        }

        public void OnOpenRestore(IRibbonControl control)
        {
            RestoreDialog.ShowSingle();
        }
    }
}
