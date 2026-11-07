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

---

# WP2b-2 レポート：PCFG からの EDID 合成（2026-11-07）

## 変更
- `src/epinephrine/epi_edid.hxx` / `epi_edid.cxx`（新規、MIT）
  - パネル XML（PCFG）から EDID 1.x の基本ブロック（128 バイト）を作る。カーネル用ヘッダに依存しないので、ドライバにもホスト側のテストにも同じファイルが入る。
  - 構成は qcdxkm7325 が作ったもの（モニタ SHP1C70）と同じ。
    - ヘッダ、ベンダ、色度：`EDID Configuration` グループの値をそのまま使う。
    - 確立タイミングと標準タイミング：使わない。
    - DTD1：Active Timing。ピクセルクロックは htotal×vtotal×DSIRefreshRate を 10 kHz 単位で切り捨てた値。
    - DTD3：名前（PanelName の先頭 13 文字）。DTD2 と DTD4 は空き。
- `src/epinephrine/epi_panel.cxx`（新規、MIT）
  - StartDevice で GPU0 の `_ROM(offset, 0x1000, 0)` を `DxgkCbEvalAcpiMethod`（`DISPLAY_ADAPTER_HW_ID`）で繰り返し呼び、PCFG 全体を読む。
  - EpiDiag に記録する値：PanelStatus、PanelXmlLen、Edid、PanelTiming。
  - 止め方は `EpiDisablePanel=1`。提示する内容は `EpiPanelFeatures` で選ぶ（bit0＝EDID、bit1＝実際のタイミング、既定は 1）。
- `bdd.cxx` の GetEdid：合成した EDID を `QueryDeviceDescriptor` で返す。
- `bdd_dmm.cxx`：`EpiApplyPanelSignal` を追加した。ターゲットモードとモニタモードに実際のタイミングを入れるもので、bit1 のときだけ有効（下記の未解決事項）。
- `tools/edid_test/`：ホスト側のテスト。`build.cmd <EWDKドライブ>` でビルドし、`edid_test.exe <panel.xml> golden_shp1c70.hex` で実行する。黄金値は電話のレジストリ（`Enum\DISPLAY\SHP1C70\...\EDID`）から取った。
- `Sample/SampleDisplay.vcxproj`：**DriverVer を固定した**（`EpiDriverVersion`、現在 20.0.2.12。下記の判明事項）。

## 結果
- ホスト側のテスト：PCFG から合成した EDID が、黄金値と 128 バイトすべて一致した。
- 実機（oem61、v33-akat）
  - PanelStatus=0、PanelXmlLen=5756。電話の `_ROM` から読んで合成した EDID も、黄金値と一致した。
  - モニタは `Default_Monitor` から `SHP1C70`「Generic Monitor (RM692E5 Visio)」に変わった。
  - 1080×2400 で、**画面は正常に映った**。

## 判明事項
1. **DriverVer はビルド時刻から付いていた**
   - 原因：stampinf の既定では「時.分.秒.ミリ秒」が DriverVer になる。表示ドライバでは先頭の数字が WDDM のバージョンとして扱われる。
   - 症状：21 時以降にビルドした版（21.x）は、AddDevice が `CM_PROB_FAILED_ADD`（problem status 0xC0000182 `STATUS_DEVICE_CONFIGURATION_ERROR`）で失敗した。その場での入れ替えも「再起動が必要」になり、再起動しても失敗した。
   - 対処：`EpiDriverVersion` で 20.x に固定した。
2. **実際の周波数を提示すると真っ黒になる（原因判明）**
   - 症状：ターゲットモードに PixelRate、HSync、VSync を入れると、ドライバは OK のまま表示経路が有効にならず、画面が真っ黒になった。
   - 原因：VidPn の DDI に一時的なトレース（`EpiDiag\VidPnLog`）を入れて調べた。ターゲットの `pfnAddMode` が `STATUS_GRAPHICS_INVALID_FREQUENCY`（0xC01E030A）を返し、その結果 EnumVidPnCofuncModality が失敗して、経路が一度もコミットされていなかった。
     - レジストリのスイッチで項目ごとに入れ、デバイスの再起動だけで切り分けた。3 つのうちどれか 1 つだけでも拒否された。値の形（mHz/1000、整数 Hz、EDID と完全に一致する有理数）には関係がない。
     - 次のことも試したが、効かなかった。
       - `VSyncFreqDivider=1`（モニタモードまで拒否されるようになった。0 が正しい）
       - `MinimumVSyncFreq` を VSync と同じ値にする（初期値は 0xFFFFFFFF）
       - EDID に Range Limits 記述子（0xFD）を加える（dxgkrnl が読み直していることはトレースで確認した）
     - 受け付けられたもの：ターゲットモードの TotalSize（1181×2453、ブランキング込み）、モニタモードの周波数。
   - 根拠：Microsoft の資料（Saving energy with VSync control）に次のとおりある。VSync 制御（`DxgkDdiControlInterrupt`、`DxgkDdiGetScanLine`、ISR／DPC で VSync を通知すること）を持たない表示専用ドライバは、この 3 つを `D3DKMDT_FREQUENCY_NOTSPECIFIED` にする。VSync 制御を持つドライバでは、逆に指定が必須になる。Epinephrine（KMDOD 由来）は VSync 制御を持たない。
   - 対処：`EpiPanelFeatures` の bit1 は、TotalSize（ブランキング込み）だけを入れるようにした。周波数は未指定のままにする。実機（20.0.2.12、features=3）では、AddTgt が st0 になり、Present が通り、1080×2400 で動いた。既定値は 1 のまま。
   - 実際の周波数は、VSync 割り込み（IRQ 0x73）を実装してから入れる（そのときは必須になる）。
   - 途中で試した、古い表示構成（qcdxkm 時代の SHP1C70 構成と MSNIL 構成）の削除は関係がなかった（バックアップは電話の `C:\Epinephrine\diag\GraphicsDrivers_before_wp2b2fix.reg`）。
3. **DDI トレース**：`EpiDiag\VidPnLog` は残した。VidPn、子デバイスの状態、電源、EDID の読み出し、Present（初回、失敗時、256 回ごと）の DDI と、その戻り値が記録される。

## レビュー
- Codex の残量が少ないため、別主体レビューは未実施（Claude の自己確認のみ）。

---

# WP2b-3a レポート：VSync の読み取り専用観察（2026-11-07）

## 目的
VSync 割り込みを入れる前に、表示のタイミングを読み取りだけで観察する。何回目のフレームか、いま何ライン目を走査しているかを示すレジスタを特定し、GetScanLine と VSync の実装に使えるかを確かめる。ハードウェアには書き込まない。

## 変更
- `epi_hw.cxx` / `epi_hw.hxx`：`EpiVsyncProbe` を追加した。StartDevice の EpiHwSnapshot の直後に呼ぶ。
  - INTF1 の +0xAC、+0xB0、+0x298、+0x2B0 と、MDSS HW_INTR_STATUS（+0x10）を 250 µs おきに 400 回読み、QPC の時刻と一緒に `EpiDiag\ProbeSamples` に記録する。
  - INTF1 全体（0x300 バイト）を前後 2 回読み、変化したレジスタを `ProbeIntfDiff` に記録する。
  - 既定では動かない。`Device Parameters\EpiVsyncProbe` を 0 以外にしたときだけ動く。その場合、開始処理が約 0.1〜0.15 秒止まる。状態は `ProbeStatus`（0＝完了、1＝無効、2＝MDP なし、3＝メモリ不足、4＝マップ失敗）。

## 結果（20.0.2.13、oem70、2 回）
| レジスタ | 振る舞い | 解釈 |
|---|---|---|
| INTF1 +0xAC | 約 16.72 ms ごとに 1 増える（0x2799C→0x279A2） | フレームカウンタ（mainline の INTF_FRAME_COUNT） |
| INTF1 +0xB0 | 1→2400 と増えて、1 に戻る。戻るのはフレームカウンタが増えるときと同じ | 走査ライン（INTF_LINE_COUNT）。2400 を超える値は観測されなかった（帰線期間の約 53 ライン、約 0.36 ms は、サンプル間隔とほぼ同じ） |
| INTF1 +0x2B0 / +0x298 | +0xB0 より約 1.5 ms 遅れて、同じ周期で戻る。+0x298 は +0x2B0 に約 0x960 を足した値 | tear-check（TE 側）の読み出しポインタ系 |
| MDSS +0x10 | ずっと 0 | 割り込みは保留中のものがない（有効になっていない） |

- フレーム周期は 16.72 ms 前後（約 59.8 Hz）で、間隔のばらつきは数 µs だった。ただし 1 回目の採取では、7.1 ms や 24.7 ms といった不規則な間隔が 2 回あった。フレームが 1 回遅れたか、とんだものと考えられる。
- 静的な値：INTF1 +0x2B4 = 0x80000001、+0x288 = 0x180085、+0x294/+0x2A0 = 0x960（2400）、+0x2A4 = 0x961、+0x1C4 = 0x800（割り込み状態と見られる。有効化ビットは 0）。
  - パネルはコマンドモードで、bootloader がオートリフレッシュ（TE ごとに MDP がフレームバッファを読み出す）を有効にしたまま渡していると考えられる。表示専用ドライバがフレームバッファに書くだけで画面が更新されるのは、このため。
  - レジスタの名前は mainline DPU からの推定で、まだ確認していない。

## 結論
- 走査ライン（+0xB0）とフレームカウンタ（+0xAC）は、読み取りだけで取れる。`DxgkDdiGetScanLine` は +0xB0 から実装できる。2400 以上のライン、またはライン 0 を帰線期間として扱う。
- VSync の通知には、次の 2 つの方法がある。
  1. **ソフトウェア VSync**：タイマ DPC で +0xAC の変化を見て、`DxgkCbSynchronizeExecution` の中から `DxgkCbNotifyInterrupt(DXGK_INTERRUPT_DISPLAYONLY_VSYNC)` を呼ぶ。ハードウェアの割り込み設定には書き込まない。
  2. **ハードウェア割り込み**：INTF1 の割り込み有効化（+0x1C0 付近と推定）に書き込み、IRQ 0x73 で受ける。消費電力と精度では有利だが、書き込みを伴い、割り込みが止まらなくなるおそれがある。

## レビュー
- Codex の残量が少ないため、別主体レビューは未実施（Claude の自己確認のみ）。

---

# WP2b-3b レポート：ソフトウェア VSync と実際のリフレッシュレート（2026-11-07）

## 変更
- `epi_vsync.cxx` / `epi_vsync.hxx`（新規、MIT）
  - `DxgkDdiControlInterrupt`（`DXGK_INTERRUPT_DISPLAYONLY_VSYNC`）と `DxgkDdiGetScanLine` を登録した。
  - 1 ms の高分解能タイマ（`ExAllocateTimer(EX_TIMER_HIGH_RESOLUTION)`）で INTF1 +0xAC（フレームカウンタ）を見る。値が変わったら、`DxgkCbSynchronizeExecution` の中から `DxgkCbNotifyInterrupt` と `DxgkCbQueueDpc` を呼ぶ。KMDOD が PRESENT_PROGRESS に使っている経路と同じ。
  - GetScanLine は INTF1 +0xB0 から返す。ライン 0、または VActive を超えるラインを帰線期間として扱う。
  - ハードウェアには書き込まない。INTF1 は読み取り専用でマップする。
  - 止め方：`Services\Epinephrine\EpiSoftVsync=0`（DriverEntry で読む。デバイスの再起動が必要）。0 にすると、KMDOD と同じ動作（周波数は未指定）に戻る。
- `bdd_dmm.cxx`：VSync 制御が有効なときは、ターゲットモードとモニタモードに実際の周波数を入れる。値は PCFG の DSIRefreshRate（60000/1000、HSync 147180000/1000、PixelRate 173,819,580）。パネル情報がないときは 60 Hz を入れる。
- トレース：VidPnLog が満杯になっても、PresentCount、VsyncNotifies、VsyncSkipped、TraceTimeMs、VidPnLast は更新を続ける。Present のトレースは 64 回ごとにした。

## 結果（20.0.2.15 と 20.0.2.16）
- 周波数付きのターゲットモードが受け付けられた（AddTgt st0）。OS の報告は 1080×2400 @ 60 Hz。
- 約 74 秒の操作中に Present 506 回、VSync 通知 2484 回、フレームのとびは 0。画面が止まっているあいだは、dxgkrnl が VSync を無効にする。
- システムログに表示関係のエラーはない。ユーザーの感想は「普通に動く。単純な動作の遅さは感じるが、ましに感じる」。描画は CPU（WARP）のままなので、遅さそのものは別の問題。

# WP2b-3c レポート：割り込み状態の読み取り専用観察（2026-11-07）

## 変更
- `EpiVsyncProbe` を拡張した。INTF1 +0x1C4 と、INTF の TE 割り込みブロック（MDP + 0x36800。mainline の `MDP_INTF_REV_7xxx_TEAR_OFF`）の +0x4 もサンプルする。
- TE ブロックは初めて読むアドレスなので、`EpiVsyncProbe` の bit1 で 1 回だけ読む。読む前にそのビットを落とし、記録（`EpiProbeTearAttempt`）を `ZwFlushKey` で確実にディスクへ書いてから読む。

## 結果（20.0.2.17、oem74）
- TE ブロックは問題なく読めた（`EpiProbeTearAttempt=2`、WHEA なし）。
- 有効化レジスタはすべて 0 だった。状態ビットは一度立つと消すまで残る方式で、観察のあいだ値は変わらなかった。

| レジスタ | 値 |
|---|---|
| INTF1 +0x1C0（有効化） | 0 |
| INTF1 +0x1C4（状態） | 0x800（bit11） |
| TE +0x0（有効化） | 0 |
| TE +0x4（状態） | 0xF（bit0〜3） |
| MDSS +0x10 | 0 |

- MDSS +0x10 は、有効になっている割り込みだけをまとめて示すようで、ずっと 0 だった。
- 読むだけでは、フレームごとに立つビットは見分けられない。見分けるには、クリアレジスタ（+0x8）に書いて消し、また立つ様子を見る必要がある。有効化は 0 のままにするので、割り込みは発生しない。
