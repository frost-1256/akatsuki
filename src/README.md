# src

Epinephrine のソースコード置き場。

- `epinephrine/`：WDK サンプル KMDOD（microsoft/Windows-driver-samples の video/KMDOD、**MS-PL**）を土台にしたドライバ本体。出自は `epinephrine/UPSTREAM.md`
- ビルド：EWDK の ISO をマウントして `tools/build_epinephrine.ps1`（ARM64 Release、未署名）→ `tools/sign_epinephrine.ps1`（テスト署名）。出力は `build/Release/SampleDisplay/`（Epinephrine.sys、Epinephrine.inf、epinephrine.cat）
- 新しく書いたファイル（MIT）：
  - `epi_hw`：表示ブロックの読み取り
  - `epi_panel`：パネル設定（PCFG）の読み込み
  - `epi_edid`：PCFG から EDID を作る
  - `epi_dsi`：DSI でパネルの明るさを変える
  - `epi_vsync`：VSync
  - `epi_gpu`：GPU の試験用 `EpiGpuProbe`
  - `epi_smmu`：adreno SMMU
  - `epi_gmu`：GMU の起動、HFI、GX の電源
- 机上の試験：`tools/smmu_test/`（SMMU／GMU を模擬して動かす）、`tools/edid_test/`（EDID）
- 使った資料とライセンスは [CREDITS.md](../CREDITS.md)
- 構成の計画は [docs/m1/AKATSUKI_M1_PLAN.md](../docs/m1/AKATSUKI_M1_PLAN.md) の WP2 を参照
