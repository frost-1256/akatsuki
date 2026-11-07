# Akatsuki 向け引き継ぎ：spacewar（Nothing Phone (1)）の Adreno／qcdxkm7325 表示問題（2026-11-07 時点）

## 要約
- GPU（qcdxkm7325）は、**レジストリの設定値と numproc 4 がそろえば起動する**。問題は表示だけ（全画面ノイズまたは黒画面）。
- 起動しない原因（解決済み）：`HKLM\SOFTWARE\QCOM\Drivers\DxKMD` が欠けていると、GMU が HFI の seq5 GX_BW_PERF_VOTE（id 0x1E）に応答せず、qcdxkm が 10 秒待ってから DdiStartDevice を 0xC0000001 で失敗させる。
- 表示：dsc2b では全画面ノイズ。再起動後の最初の有効化では黒画面。
- dsc3（DSI STREAM0_TOTAL を 180 に固定）では、GPU は起動するが画面は黒のまま。その後、WinRM が応答しなくなった（OS が半分固まった）。
- 次の候補：DSC と DSI のタイミング（qcdxkm は TOTAL 360、ブートローダは 180、widebus の有無）、DSI PLL（ブートローダは VCO 687 MHz、旧 qcdxkm は 854.89 MHz）。

## GPU 起動に必須の条件
1. `HKLM\SOFTWARE\QCOM\Drivers\DxKMD`、`DxKMD\DISPLAY`、`DxKMD\GRAPHICS`：それぞれ `WriteEDIDToReg=1`（DWORD）。
2. `HKLM\SOFTWARE\QCOM\Drivers\DxKMD\GRAPHICS\A6x`：`DisableGmuACD=0x11`、`DisableICG=1`。**ACD を切らないと起動しない。**
3. `DxKMD\DISPLAY\PanelEdid`（128 バイト）は qcdxkm が自分で書くので、用意しなくてよい。
4. BCD の `numproc 4` は必須。8 コアでは bugcheck 0x101 CLOCK_WATCHDOG_TIMEOUT で固まる。
5. zap ファイル：DriverStore の `qcdxkmsuc7325.mbn` は `a660_zap.elf` と同じもの（SHA256 ADB53DED…）。
6. qcdxkm のパッチ：HFI タイムアウトを 1 秒から 10 秒へ延長し、初期の性能インデックスを 2（315 MHz）にする（dsc2b に含まれる）。
   - 署名済み dsc2b の SHA256：FCD87D22797D738D9931EAFF56F54BAD662968C027C2A064908F86C76BE7FA71
   - 署名済み dsc3 の SHA256：A908C63236705160CC5D38EAD0538B8EBB371C97B71D40F507B093641A2A567D

## 運用上の注意
- 有効化の途中でクラッシュしたりフリーズしたりすると、GPU の ConfigFlags が 0 のまま残る。その場合、次の起動はくるくるで止まる。リカバリのガジェットモードから `winboot/livesetup/phone_op.ps1 -Ops online,cf-fix,offline` で 1 に戻す。
- 電話のライブダンプやクラッシュダンプを、ホスト PC の kd.exe や WinDbg で開くと、**ホスト PC が落ちた**（3 回）。
- qcscm、qcsecapp、qcrpen を安易に足さないこと。qcsecapp は今回の問題とは無関係だった。
- 2026-11-07 18:50 の時点で、電話の DriverStore は **dsc3** のまま。戻すには、起動後に GPU を無効にしてから `winboot/livesetup/dsc_online_swap.ps1 -To dsc2b` を実行する。dsc2b のバックアップは電話の `C:\DscSwap\qcdxkm7325.sys.bak-dsc2b` にある。

## zip の中身
- `opus-session/`
  - `HANDOVER_QCDX.md`：旧環境での解析の全体像
  - `NEXT_STEPS_20261106.md`：11/06〜07 の作業ログ
  - `report_*.md`、`brief_*.md`：Codex による静的解析と動的解析
    - 起動問題：gx2〜gx40、`qcdx_10s_wait`、`gx20_acd_path`、`gx22_a6x_knobs`
    - 表示：gx43、gxnoise、gxr1〜8、`panel_nodsc`、`dscoff`、`dsc0_followup`
  - `mdsssnap/`：実機データ
    - MdssSnap のスナップショット：旧環境は `run3`／`run4_v12`、新環境の dsc2b は `run5_v13`
    - ETW、GmuSnap、enable テスト、evtx、kd のテキスト出力
  - `work/`：解析の作業ファイル。`qcdscoff_build/` に dsc0〜3 のパッチスクリプト、未署名のバイナリ、レポートがある。
- `winboot/`
  - `gmusnap/`、`livedumpnow/`
  - `livesetup/`：WinRM 経由で実機を操作するスクリプト
- `muse-workfiles/drivers/`：パッチ版 qcdxkm（署名済み）など
- `muse-workfiles/mdsssnap/`：MdssSnap ドライバ
- `memory/`：Claude の作業メモ（英語。`dsc0-test-20261107.md` に今日の結論がある）
- `EXCLUDED_LARGE_FILES.txt`：大きすぎて入れなかったファイルの一覧（メモリダンプなど。PC の元の場所にある）
