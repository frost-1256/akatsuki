# クレジットと出典

Epinephrine が使ったコード、資料、データの一覧。それぞれ、どう使ったかとライセンスを書く。

## リポジトリに含むコード

| 名前 | 使い方 | ライセンス |
|---|---|---|
| [Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) の `video/KMDOD`（Microsoft） | ドライバ本体の土台。`src/epinephrine/` のうちサンプルに由来するファイルと、その改変版。取得したコミットは `d49bb7d7`（2026-11-05）。出自は [src/epinephrine/UPSTREAM.md](src/epinephrine/UPSTREAM.md) | MS-PL（[LICENSE-MSPL.txt](src/epinephrine/LICENSE-MSPL.txt)） |

これ以外のコード（`epi_*.cxx`／`epi_*.hxx`、`tools/` 以下）は Epinephrine のために新しく書いたもので、MIT（[LICENSE](LICENSE)）に従う。

## 事実の確認だけに使った資料（コードは取り込んでいない）

ハードウェアの事実の確認だけに使った。対象は、レジスタのオフセットとビット、ファームウェアの形式、手順の順番、投票の値など。実装は、これらの事実をもとに一から書いている。どの事実をどこから得たかは、[docs/g0/G0_GPU_SURVEY.md](docs/g0/G0_GPU_SURVEY.md) と各ソースの先頭のコメントに書いてある。

| 資料 | 主に確認したこと | ライセンス |
|---|---|---|
| Linux カーネル `drivers/gpu/drm/msm/adreno/`：`a6xx_gmu.c`、`a6xx_gmu.xml`、`a6xx_hfi.c`、`a6xx_hfi.h`、`a6xx_gpu.c`、`a6xx_catalog.c`、`adreno_gpu.c` | GMU の起動、RSCC、HFI のメッセージ、OOB、slumber、rpmh_start／rpmh_stop、GBIF、A635（7c3）の設定 | GPL-2.0 |
| Linux カーネル `drivers/iommu/arm/arm-smmu/`：`arm-smmu.h`、`arm-smmu.c`、`drivers/iommu/io-pgtable-arm.c` | SMMU のレジスタ配置、ページテーブルの形式 | GPL-2.0 |
| Linux カーネル `drivers/gpu/drm/msm/disp/dpu1/`、`drivers/gpu/drm/msm/dsi/`（`dsi.xml` など） | 表示ブロック（MDSS／DPU の INTF、TE の割り込み）、DSI 6G のコマンドの形式 | GPL-2.0 |
| Linux カーネル `drivers/clk/qcom/`：`gpucc-sc7280.c`、`gcc-sc7280.c` | GPU_CC と GCC のクロック、GDSC | GPL-2.0 |
| Linux カーネル `arch/arm64/boot/dts/qcom/`：`kodiak.dtsi`、`sm7325-nothing-spacewar.dts` | アドレス、割り込み、GPU の OPP 表、ストリーム ID | GPL-2.0 / BSD-3-Clause |
| [Mesa](https://docs.mesa3d.org/drivers/freedreno.html) の freedreno／Turnip、rnndb | Adreno の公開知見とレジスタ定義。ユーザーモード側では Turnip の再利用を予定している（[docs/CONCEPT.md](docs/CONCEPT.md)） | MIT |
| Arm System MMU Architecture Specification（SMMUv2） | `CBAR` の TYPE など、SMMU のレジスタの意味 | Arm の公開仕様 |

## 実機で読んだデータ

- **GMU ファームウェア** `a660_gmu.bin`（Qualcomm）：電話の純正イメージから取り出して、データとして読み込んでいる。**リポジトリには含めない。** 実行時に電話の `C:\Epinephrine\fw\` から読む。
- **純正の表示ドライバ（qcdxkm7325）が出した EDID**：電話の上で記録した出力を、EDID を作るときの見本（`tools/edid_test/golden_shp1c70.hex`）にしている。ドライバのコードは使っていない。
- **cmd-db、DSDT、パネル設定（PCFG）**：電話の上で読み取って値を確かめた。DSDT の差分は [platform/](platform/) で管理する。プロプライエタリなバイナリ、証明書、パスワード類はリポジトリに含めない。

## 協力

- 調査の記録、レビュー、ビルド、実機での試験、文書の作成で、AI アシスタント（Anthropic の Claude、Muse）の手を借りた。SMMU と GMU のハードウェアを扱うコードは、プロジェクトの作者が書いた。
