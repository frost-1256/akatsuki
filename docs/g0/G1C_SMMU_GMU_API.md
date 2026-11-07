# G1c SMMU API と CM3 起動プローブ（v30、2026-11-08）

ソースは MIT。Linux はレジスタ、ビット、ファームウェア形式、順序の事実確認に限って使用し、コードは取り込んでいない。実機未実行 [U]。スマホへの配布・署名・起動フラグ変更は行っていない。

## SMMU API

宣言：`src/epinephrine/epi_smmu.hxx`。実装：`epi_smmu.cxx`。
全 API は PASSIVE_LEVEL、呼び出し側で直列化する。コンテキストごとにページ表を所有し、データページは呼び出し側が所有する。

| 関数 | 契約 |
|---|---|
| `EpiSmmuCreate(params, &ctx)` | MMIO に触れずコンテキストとルート表を作る。params は任意の借用 Device Parameters キー（NULL 可） |
| `EpiSmmuMap(ctx, iova, phys, size, flags)` | 無効時のみ。IOVA/物理/サイズは 4KB 境界。phys は PHYSICAL_ADDRESS、物理連続の通常非キャッシュ RAM。IOVA 32bit、物理 36bit。ファームウェア長は呼び出し側で切り上げ、余白も確保・ゼロ初期化する |
| `EpiSmmuUnmap(ctx, iova, size)` | 無効時のみ。穴を含む要求は全体を拒否。Map の重複拒否も既存の対応付けを変更しない |
| `EpiSmmuEnable(ctx, cc, gmuA)` | CX と G1b の 5 ブランチ ON、CM3 reset、GX OFF。cc は GPU_CC、gmuA は 0x03D7E000 の借用マッピング。IDR、sCR0、全 SMR と CB の未使用状態を確認して CB0 を設定し、ASID 1 の TLB を無効化・同期してストリーム 5 を公開 |
| `EpiSmmuQueryFault(ctx, &fsr, &address, &fsynr0)` | 有効時、CM3 実行中も可。読み取りのみ、フォールトを消さない。FSR/FAR の変化が見えた場合は最大 3 回読み直し、なお不安定なら STATUS_RETRY |
| `EpiSmmuDisable(ctx)` | CM3 を reset に戻し、GX OFF、CX/クロック ON のまま呼ぶ。SMR を無効化、CB を停止、TLB 同期後、保存した設定を復元 |
| `EpiSmmuDestroy(ctx)` | ハードウェアから切り離したコンテキストの表と管理メモリを解放。接続中・復元失敗中は拒否 |

flags は `EPI_SMMU_READ`（必須）、`EPI_SMMU_WRITE`（省略時 RO）、`EPI_SMMU_PRIVILEGED`（省略時は非特権アクセスも可）の組み合わせ。葉は Normal non-cacheable / outer-shareable / executable。テーブルも非キャッシュの 4KB ページ単位で確保し、3 段の AArch64 4KB 表を構築する。nG=1 なので ASID 単位の TLB 無効化が葉にも適用される。CFRE=1 でフォールトを記録し、CFIE=0、CFCFG=0 で IRQ と stall は使わない。sCR0 は書かない。

順番：Create → Map（複数）→ CX ON → Enable → CM3 起動 → CM3 reset → Disable → Unmap → CX OFF → Destroy。Map/Unmap は CX 窓内でもよいが、SMMU が切り離されていることが条件。

**失敗時の寿命**：Enable 失敗後も Disable を呼ぶ。Disable が成功するまでは ctx、データ、cc/gmuA のマッピングと CX 電源を保持する。復元失敗や TLB timeout ではメモリを解放しない。正常な Disable の後なら同じ ctx を次の CX 窓で Enable し直せる。CX 電源断を Disable の代用にはしない。

## GMU 起動プローブ

既存の `epi_gmu.hxx` の `EpiGmuBootTest` 宣言と KeepCx 契約を使い、書きかけの `epi_gmu.cxx` を完成させた。`epi_gpu.cxx` の bit7（`EpiGpuProbe=0x80`）から呼ぶ。フラグはハードウェアアクセス前に解除・flush される。v30 = 20.0.2.30。

入力は既存ローダーが読む `C:\Epinephrine\fw\a660_gmu.bin`。全ブロックを先に検査し、測定済み CORE_FW_VERSION=0x30010008 を持つ画像に限定する。以下の領域を個別に物理連続・非キャッシュで確保・ゼロ初期化する。

| IOVA | 確保サイズ | 用途 / 属性 |
|---|---|---|
| 0x4000 | 0xE000 | コード 0xD4CC バイト、privileged RW |
| 0x100000 | 0x9000 | ゼロ領域、privileged RW |
| 0x200000 | 0x13000 | ゼロ領域、privileged RW |
| 0x300000 | 0x8000 | ゼロ領域、privileged RW |
| 0x60000000 | 0x4000 | dummy、privileged RW。2 つの type7 記述を含む |
| 0x60400000 | 0x7000 | debug、privileged RW |
| 0x60004000 | 0x4000 | log、RW |
| 0x60008000 | 0x4000 | HFI、RW。先頭に 168 バイトの表、+4/8/12KB に各キュー |

CX/CM3/GX の前提を確認し、FAL、retention、HFI アドレス、fence、chip id、log、bus/cache 設定を行う。TCM を書き込んで即時読み戻し、通常メモリのコードを配置し、SMMU を Enable してから CM3 reset を解除する。`FW_INIT_RESULT & 0x1FF == 0x100` を最大 100ms 待つ。成功・timeout とも診断を保存し、CM3 を reset、SMMU を Disable してから解放する。

**範囲**：PDC はマップも読み書きもしない。RSCC 初期化 / rpmh_start、RPMH_CTRL の投票、GBIF QSB、HFI_CTRL_INIT と HFI メッセージ、GX 起動も省略する。この初版で FW_INIT_RESULT まで進むかは未確認 [U]。HFI テーブルを置いたことと、HFI の起動や GPU 起動の成功は別である。

GMU/HFI の割り込みは mask したまま CX を閉じる。CM3 reset の確認失敗、SMMU 復元失敗、または後始末失敗では `KeepCx=TRUE` とし、コンテキスト・データ・GMU マッピングを `g_KeptGmu` に保持する。外側もクロック、CX、GPU_CC マッピングを保持し、追加プローブを実行しない。この状態は次のテストを続けるための状態ではなく、デバイスリセットによる復旧が必要。

## 診断

| EpiDiag 値 | 形式 |
|---|---|
| `GpuGmuStatus` | 0 完了、1 前提不成立、2 firmware 不正、3 メモリ/map 不足、4 SMMU Map 失敗、5 Enable 失敗、6 設定/TCM の書き込み失敗、7 起動 timeout、8 後始末失敗（KeepCx） |
| `GpuGmuNtStatus` / `GpuGmuDisableStatus` | NTSTATUS。Disable 未到達は STATUS_NOT_FOUND |
| `GpuGmuKeepCx` | 1 なら電源・メモリを保持 |
| `GpuGmuRegs` | 6 DWORD：FW_INIT_RESULT、FW_BUSY、GMU2HOST_INTR_INFO、AO_HOST_INTR、GENERAL_0、GENERAL_1 |
| `GpuGmuFaultStatus` / `GpuGmuFault` | QueryFault の NTSTATUS と 3 QWORD `{FSR, FAR, FSYNR0}`。未到達は STATUS_NOT_FOUND。FSR は sticky で、この API は clear しない |
| `GpuGmuLog` | log の先頭 4096 バイト |
| `GpuCxStatus` | 16 は GMU テスト失敗。詳細は GpuGmuStatus。CX が最初から ON の場合は 14 で実行しない |

`EpiGpuProbeStep`：SMMU は 0x500..50E（設定）、0x520..53C（停止・復元）、GMU は 0x600..611（設定）、0x620..622（CM3 起動）、0x630（reset）、0x642..651（設定復元）。TCM 書き込みは `0x80000000 | GMUアドレスの下位29bit`、保持終了は 0x6FF。正常に外側まで戻ると従来どおり 0xFFFF。

## 検証

- ARM64 Release のコンパイル・リンク、INF 検査、カタログ生成成功（エラー・警告なし）。
- `tools\smmu_test\build.cmd H`：実装本体を模擬メモリ/MMIO に接続してテスト。ページ表の独立 walk、RO/RW/privileged、2MB/1GB/4GB 境界、重複・穴拒否、メモリ不足時の rollback、読取専用 fault 照会、TLB timeout、設定・復元失敗を確認。
- GMU も合成画像と手元の実 firmware を入力し、領域構築、配置、起動完了/timeout の模擬結果、reset/Disable 失敗時の保持を確認。実 firmware はテスト引数で読み、リポジトリへ入れない：`build\smmu_test\gmu_test.exe <a660_gmu.binのパス>`。
- **模擬テストは実機での変換、Hypervisor の許可、DDR 到達、CM3 起動成功を証明しない [U]。**

事実の参照：[arm-smmu.h](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.h)、[io-pgtable-arm.c](https://github.com/torvalds/linux/blob/master/drivers/iommu/io-pgtable-arm.c)、[a6xx_gmu.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_gmu.c)、[a6xx_gmu.xml](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml)、[a6xx_hfi.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_hfi.c)、[a6xx_hfi.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_hfi.h)。
