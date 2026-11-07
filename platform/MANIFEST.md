# プラットフォーム設定マニフェスト

Epinephrine が動作するために必要な、ドライバ本体の外側にあるプラットフォーム側設定の一覧。再インストールや環境再構築のときは、このマニフェストの再適用から始める。

「ドライバが依存する設定が外に散らばっていると、欠落に気づけないまま黙って壊れる」— 2026-11-07、公式ドライバがレジストリ設定の欠落で開始不能になった事故の教訓から、この一覧を正本として版管理する。

## 1. DSDT

| 項目 | 内容 | 状態 |
|---|---|---|
| GPU0 `_HID` | `QCOM0A36` → `AKAT0001`（自作用 ID） | 未実施（WP1） |
| GPU0 `_DEP` | 変更なし（MMU0, MMU1, IMM0, IMM1, PEP0, PMIC, RPEN, SCM0 の 8 件） | 現行どおり |
| ベース | 本番系譜 v24（run70）から Akatsuki 用ブランチを切る | 未実施 |

差分ファイルは `platform/dsdt/` に格納予定。コンパイルは出荷用 iasl 20260930、検証は「逆コンパイル差分が HID とヘッダだけ」をゲートとする。

## 2. PEP（Platform Extension Plug-in）

| 項目 | 内容 | 状態 |
|---|---|---|
| PEP0 `Device Parameters\Resources` | Epinephrine 用エントリ（COMPONENT レコード入り） | 未作成。先に実機の現行エントリを読み取り、書式を写す（実測待ち） |

書式が確定したら `platform/pep/` に格納する。

## 3. ドライバ側レジストリ

- Epinephrine 自身の設定値は自ドライバのキー配下に閉じる。他ドライバのキー（例: 公式ドライバの `HKLM\SOFTWARE\QCOM\Drivers\DxKMD`）には依存しないし、書き込まない

## 4. 実機運用の前提（本線の安全規則）

- グラフィックス系はブート時無効が既定（ConfigFlags）。有効化は手動セッションで行い、終了後は無効化と ConfigFlags の読み戻し確認までをひと続きとする
- 公式ドライバと Epinephrine は同じ表示ハードウェアに二重にバインドしない。実験時は公式側を無効化する

## 5. 参考: 公式スタック側の設定（Epinephrine の依存ではない）

再インストール時に欠落すると公式ドライバ側が開始不能になる設定の記録。切り分けの参照用:

- `HKLM\SOFTWARE\QCOM\Drivers\DxKMD\GRAPHICS`: ForceActive=1、PreventPowerCollapse=1
- `HKLM\SOFTWARE\QCOM\Drivers\DxKMD\GRAPHICS\A6x`: DisableGmuACD=0x11、DisableICG=1
