# WP2b-1 レポート：表示系ブロックの読み取り専用スナップショット（2026-11-07）

## 目的
StartDevice で一度だけ、GPU0 の `_CRS` から MDP_REGS（0x0AE00000、長さ 0x200000）を見つけて表示系のレジスタを読み、レジストリに残す。ハードウェアには一切書き込まない。後の WP2b（モード、VSYNC、Escape）の前に、Epinephrine がどの状態で POST フレームバッファを受け取っているかを固定しておくため。

## 変更
- `src/epinephrine/epi_hw.hxx` / `epi_hw.cxx`（新規、MIT）
  - 翻訳後のリソースリストを走査する。メモリ、IRQ、接続の各リソースを種類とアドレスで分類し、MDP_REGS と DP_PHY_REGS を見分ける（並び順には頼らない）。
  - MdssSnap v1.1 以降が bootloader の状態で問題なく読めたブロックだけを `MmMapIoSpaceEx(PAGE_READONLY|PAGE_NOCACHE)` でマップし、`READ_REGISTER_ULONG` で読む。対象は mdss、mdp_top、ctl0、intf1、dsc0_enc、dsc0_ctl、dsi0、dsi0_phy、dsi0_pll。pp0 と lm0 は入れない（0x0AE69000 を読んで WHEA が出た前例がある）。
  - 出力先は `<デバイスキー>\Device Parameters\EpiDiag`（揮発キー）。値は SnapVersion=1、Resources（文字列）、MdpFound、DpPhyFound、各リソースの件数、ブロックごとの REG_BINARY、MapFailures、SnapStatus（0＝完了、1＝無効化、2＝MDP なし、3＝メモリ不足）。
  - 止め方：`Device Parameters\EpiDisableHwSnap`（DWORD）を 0 以外にする。
- `bdd.cxx`：StartDevice の `RegisterHWInfo()` の直後、`DxgkCbAcquirePostDisplayOwnership` の前で `EpiHwSnapshot()` を呼ぶ。失敗しても開始処理には影響しない。
- `bdd.hxx`：`epi_hw.hxx` を include し、メンバ `m_EpiRes` を追加した。
- `tools/sign_epinephrine.ps1`：ハッシュの計算を `Get-FileHash` から .NET の SHA256 に替えた（`-NoProfile` の素のホストで Utility モジュールが読めないことがあるため）。

## ビルド
- エラー 0、警告 0。Epinephrine.sys（署名後）の SHA256 は `8978565766CE98DC65736A575C5A55AE43763139850BE64B6ABFA5E2D5DAA6B1`。

## 実機確認（2026-11-07 20:11、v33-akat、numproc 4）
- 配置：`pnputil /add-driver` で oem56.inf として追加し、`UpdateDriverForPlugAndPlayDevicesW(FORCE)` で割り当てた。状態は `OK/CM_PROB_NONE`、"Epinephrine Display (Akatsuki)" 1080×2400。直後 15 分の System ログに、エラーも警告もない。
- EpiDiag の値：SnapStatus=0、MapFailures=0、MdpFound=1、DpPhyFound=1、メモリ 10 件、IRQ 3 件、接続 1 件。
- Resources：
  `mem AE00000+200000; mem 88E0000+10000; irq v926; mem 3D00000+3F010; mem 3D60000+3F000; irq v826; mem B290000+10000; mem B490000+10000; mem 3D90000+9000; mem 3DE0000+10000; mem C200000+FFFF; mem AA00000+200000; irq vB27; conn c1 t2 id0:19`
  - 並びは DSDT の `_CRS` どおり。IRQ の値は GIC の番号ではなく、翻訳後のベクタ。接続 1 件は GPIO 44。
- mdss（0x0AE00000）の HW_VERSION は 0x70020000（MDSS 7.2、mdp_top+0 と同じ値）。それ以外は 0。

### MdssSnap pre（run5_v13_pre_20261107、qcdxkm 読み込み前の状態）との比較
比較した 1264 ワードのうち 1238 ワードが一致した。違いは次のとおりで、いずれも動作中に変わるカウンタや状態の値と考えられる。設定値の違いは見つかっていない。

| ブロック | 違い | 内容（推定） |
|---|---|---|
| ctl0, dsi0_phy | 0 | — |
| intf1 | 4 | +0xAC、+0xB0、+0x298、+0x2B0。フレームカウンタやライン位置の類 |
| dsi0 | 5 | +0x8、+0xC、+0x48、+0xA8、+0x120。状態、割り込み、エラーの各状態レジスタ |
| dsi0_pll | 6 | +0x1B8 から +0x218。PLL の校正結果の読み返し |
| dsc0_enc / dsc0_ctl | 3 / 1 | 状態の読み返し |
| mdp_top | 7 | +0x2B0 から +0x3B8。クロックやセーフ制御の状態 |

## 結論
- Epinephrine は v33-akat 上で MDP_REGS を正しく受け取り、読み取り専用のアクセスでは WHEA もフリーズも起きなかった。
- Epinephrine が受け取る表示系の状態は、qcdxkm より前に MdssSnap が見た bootloader の状態と同じ（設定レジスタは一致）。

## レビュー
- Codex の残量が少ないため、別主体レビューは未実施（Claude の自己確認のみ）。自己確認で入れた修正は 2 点：ブロックの範囲チェックを NT_ASSERT から実行時の判定に変えたことと、ZwQueryValueKey のバッファを 8 バイト境界に揃えたこと。

## 次（WP2b-2 以降）
- PCFG（`_ROM`）からの EDID 合成と、モード 1080×2400@60 の通知。
- VSYNC：IRQ 0x73 は MDSS が共有する線。割り込みが止まらなくなるおそれがあるため、まずは ISR で数えるだけにし、マスク方法を決めてから有効にする。
- 診断用の Escape／Ioctl（EpiDiag をユーザーモードから取れるようにする）。
