# G0：GPU コア起動の机上調査（2026-11-07）

位置づけ：GPU 描画（構想文書 M2 以降）の最初の段階。ハードウェアには触れず、既存の資料だけで「GPU コアを動かすまでに何が要るか」「どこが危ないか」を確定させる。根拠には次の印を付ける。
- [P]：実測、またはコードで確かめたこと
- [I]：推定
- [U]：わかっていないこと

## 0. 結論（先に）

1. GPU コアを動かすための資料と部品は、ほぼ揃っている。
   - レジスタの配置：本家 Linux と、この機種の _CRS で突き合わせた。
   - ファームウェア：SQE、GMU、zap がある。
   - 起動手順：本家 Linux の a6xx を手本にできる。
   - qcdxkm を解析した GX 系レポートもある。
   - 公式ドライバ（qcdxkm）では、条件を絞れば GMU の起動までは成功している [P]。
2. 大きな壁は三つある。
   - **TrustZone の呼び出し**：zap シェーダの読み込み（PAS）と、SMMU アパーチャの設定。
   - **SMMU**：GPU からメモリへのアクセスは、すべて adreno SMMU を通る。
   - **GX のクロック供給**：qcdxkm でも、812 MHz では周波数リミッタが働いて止まった。
3. PEP0 の GPU0 の定義には、表示系のコンポーネントしかない [P]。GPU コア側の電源とクロック（GPU_CC、RSCC、PDC）は、ドライバが直接操作する前提になっている [I]。
4. デスクトップ（DWM）を実際に速くするには、D3D11 のユーザーモードドライバが必要になる。自作の道（Turnip は Vulkan）では、そこまでの距離が非常に長い。別の道として、qcdxkm を描画専用の GPU として使い、表示は Epinephrine が受け持つ構成がある（§7 B 案）。これは方針に関わるので、ユーザーが判断する。

## 1. ハードウェアの配置

| ブロック | 物理アドレス / 大きさ | 出所 |
|---|---|---|
| GPU（KGSL 3D0） | 0x03D00000 / 0x40000 | kodiak.dtsi、_CRS の GFX_REGS [P] |
| CX_MEM | 0x03D9E000 / 0x1000 | kodiak.dtsi [P] |
| CX_DBGC | 0x03D61000 / 0x800 | kodiak.dtsi [P] |
| GMU | 0x03D6A000 / 0x34000 | kodiak.dtsi [P]。qcdxkm は GFX_REG_CONT（0x03D60000 / 0x3F000）として 1 つに束ねてマップする（GX7）[P] |
| GPU_CC | 0x03D90000 / 0x9000 | kodiak.dtsi、_CRS の GPU_CC、GX7 [P] |
| RSCC | 0x03DE0000 / 0x10000 | kodiak.dtsi、_CRS の GPU_RSCC [P] |
| GMU_PDC | 0x0B290000 / 0x10000 | kodiak.dtsi、_CRS の GPU_PDC_REGS / SEQ_MEM [P] |
| adreno SMMU | 0x03DA0000 / 0x20000 | kodiak.dtsi [P]。Windows では MMU0 / MMU1（GPU0 の _DEP）のどちらかが持っている [I] |
| GPU 割り込み | GIC SPI 300 = GSIV 332（0x14C） | kodiak.dtsi、_CRS の GFX_INTERRUPT。Epinephrine に今割り当てられている IRQ 332 がこれにあたる [P] |
| GMU 割り込み | SPI 304（HFI）/ 305（GMU） | kodiak.dtsi [P]。_CRS にあるかは未確認 [U] |

- GPU は、本家 Linux の catalog の chip id 0x06030500（`qcom,adreno-635.0`、ADRENO_6XX_GEN4、GMEM 512 KB）にあたる [P]。
- 使うファームウェアは `a660_sqe.fw`、`a660_gmu.bin`、`a660_zap.mbn`（spacewar.dts でも同じ指定）[P]。
- 手元のファームウェアは `stock-rom/vx/firmware/` にある：`a660_sqe.fw`、`a660_gmu.bin`、`a660_zap.mdt` と `.b00`〜`.b02`。また qcdxkm は、GMU イメージを .sys の RVA 0x1A4540 に内蔵している [P]。

## 2. 電源とクロックの持ち主

- PEP0 の GPU0 の定義は、G0MD（コンポーネント 0）、G4MD（4）、G5MD（5）の 3 つだけ。中身はすべて表示系（disp_cc、MDSS の GDSC、MDP / EBI のバス帯域、eDP / DP の LDO）で、gpu_cc や gcc_gpu の名前は一つも出てこない [P]。
- このため、GPU コアの電源とクロックは PoFx からは要求できない。ドライバが _CRS の GPU_CC、RSCC、PDC を直接操作する設計になっている [I]（qcdxkm が GPU_CC+0x1534 などを直接読み書きしている事実、GX7 [P] と合う）。
- GCC 側のクロック（gcc_gpu_cfg_ahb、memnoc_gfx、snoc_dvm_gfx、ddrss_gpu_axi）の持ち主はわかっていない [U]。GCC は _CRS にないので、正規には触れられない。UEFI や他のドライバが既定で入れているかどうかも未確認。
- GX の電源（GDSC、VDD_GFX の電圧）は、GMU のファームウェアが RSCC / PDC を通して RPMh に投票する [P]（本家 Linux、GX8 §2.2）。ホストが直接入れるのは CX 側だけ。

## 3. 起動手順（本家 Linux の a6xx を手本にする）

| # | 段階 | 本家 Linux | TZ | 備考 |
|---|---|---|---|---|
| 1 | CX 側を立ち上げる | `a6xx_gmu_resume`：CX GDSC、GMU クロック（gmu 200 MHz、hub 150 MHz、cxo、axi、memnoc、ahb、smmu_vote） | – | GPU_CC を直接操作する。手順は gpucc-sc7280.c を参照 |
| 2 | secure init | `a6xx_gmu_secure_init`（A750 以降のみ） | (SCM) | A635 には不要 [P] |
| 3 | RPMh / PDC | `a6xx_rpmh_start`、`a6xx_gmu_rpmh_init`（PDC のシーケンスと RSC の設定） | – | qcdxkm にも同じ処理があるはず [I] |
| 4 | GMU を起動する | `a6xx_gmu_fw_load` → CM3_BOOT_CONFIG=2、HFI テーブルのアドレス、FENCE_RANGE、CM3_CFG=0x4052、chipid → FW_INIT_RESULT が 0xBABEFACE になるのを待つ | – | GMU 用のメモリは SMMU のストリーム 5 を通る [P] |
| 5 | HFI | INIT、性能表、帯域表、TEST、CORE_FW_START、性能の投票 | – | qcdxkm での 10-31 の失敗地点（GX22 / GX23）[P] |
| 6 | GX を入れる | OOB_GPU、または HFI で GX の電源を要求する | – | 812 MHz では周波数リミッタが働き、GX のクロックが届かない [P] |
| 7 | hw_init | SMMU アパーチャ（`qcom_scm_set_gpu_smmu_aperture(0)`）、hwcg、protect、GBIF | **SCM** | アパーチャ設定の SCM が使えなければ省略される（本家）[P] |
| 8 | SQE / CP | SQE の命令を読み込む、リングバッファ、CP_SQE_CNTL=1、CP_ME_INIT | – | ここで初めて「GPU が命令を実行する」 |
| 9 | 安全モードを解除する | zap の PAS 読み込み（`qcom_mdt_load` + `qcom_pas_auth_and_reset(13)`）→ CP_SET_SECURE_MODE 0 | **SCM** | 量産機では zap が必須。ない場合の SECVID_TRUST_CNTL=0 は拒否される見込み [I] |

## 4. qcdxkm から得ている実績（GX 系レポート）

- GMU の起動は、ACD をオフにし、最初の性能レベルを 315 MHz（表の 2 番目）に下げると、最後（HFI START）まで成功した（KD 第 8 セッション）[P]。この機種で GMU ファームウェアが動くことは証明済み。
- 812 MHz（表の 7 番目）では、GPU_CC+0x1534 の bit0（周波数リミッタ）が立つ。すると OOB 要求 0 の ACK が来ず、GMU の起動が失敗する [P]。GX のブランチクロックと SPTPRAC はクロックオフのまま [P]（GX7 / GX8）。
- GMU の起動が成功した後も、qcdxkm の StartDevice は別の箇所で失敗した [P]。原因はわかっていない [U]。
- 0x101（CPU 同士の待ち合いで止まる）は、qcdxkm の連続メモリ確保の段階でも起きた [P]。Epinephrine の明るさ処理は 1 ページの連続メモリ確保を使っており、今のところ問題は出ていない [P]。それでも、大きな確保やマッピングの変更には注意する。

## 5. 壁と危険（重い順）

1. **TZ の呼び出し**：zap の PAS 読み込みと SMMU アパーチャの設定。
   - Windows には、ドライバが SCM を呼ぶための公開 API がない [I]。
   - 自分で SMC を出すと、QcTrEE / qcscm との競合が起き、Hyper-V の介在もありうる [U]。
   - qcscm は「気軽に戻さない」と決めている（ユーザーの方針）。
   - zap がないと安全モードを抜けられず、CP は普通のメモリを読めない見込み [I]。
2. **SMMU**：GPU からのアクセスはすべて adreno SMMU を通る。Windows では SMMU ドライバ（MMU0 / MMU1）がこれを持っていると見られる [I]。
   - 自分でコンテキストを組むと、持ち主と競合する。
   - バイパスで使えるかどうかはわかっていない [U]。
   - 表示（MDSS）は物理アドレスのまま動いている。ただし、これは別の SMMU のストリームの話 [P]。
3. **GX のクロック供給と周波数リミッタ**：低い周波数なら通ることはわかっている。正式なレジスタの意味はわかっていない [U]。
4. **電源が落ちたブロックへのアクセス**：GPU_CC、GMU、GPU のレジスタを、クロックや電源が入る前に読むと、バスが固まる（同期外部アボート）おそれがある [I]。読み取りだけでも安全とは限らない。
5. **0x101**：大きなメモリ操作や、TLB フラッシュを伴う操作で起きる可能性がある [I]。

## 6. G1 案（実機での最初の一歩）

- **G1a：GPU_CC の状態を読む。** CX GDSC、GX GDSC、GMU の CBCR、0x1534 を、ブレッドクラム付きで 1 回だけ読む。読むだけなので書き込みはしないが、§5.4 の危険がある。止まった場合は再起動で戻る。
- **G1b：CX 側を立ち上げる。** 本家 Linux の gpucc-sc7280 の手順で CX GDSC と GMU のクロックを入れ、GMU のレジスタを読み返す。
- **G1c：GMU を起動する（315 MHz 相当）。** HFI まで。qcdxkm で成功した条件に合わせる。
- **G2 以降**：zap / SMMU の壁（§5.1 と §5.2）を越える方法を決めてから進む。

## 7. 方針の分かれ道（ユーザーが判断する）

| 案 | 内容 | 長所 | 短所 |
|---|---|---|---|
| A：自作（構想どおり） | Epinephrine を本格的な WDDM ドライバに育て、Turnip（Vulkan）につなぐ。D3D は DXVK、または Mesa の D3D10 UMD（gallium d3d10umd）を使う | オープンで、権利面がきれい。手順を自分で説明できる | 年単位。TZ / SMMU の壁を自分で越える必要がある。DWM が速くなるのは最後 |
| B：組み合わせ | 描画は qcdxkm（描画専用アダプタとして）、表示は Epinephrine が受け持つ。Windows には、表示専用アダプタと描画専用アダプタを組み合わせる仕組みがある [I] | 公式の D3D11/12 UMD がそのまま使えるので、DWM とアプリが GPU で描ける可能性がある。zap と SMMU も公式が処理する | qcdxkm は表示と描画を 1 つのノードで扱う。表示を持たない構成で動くかどうかはわかっていない [U]。既知の起動失敗（リミッタ、StartDevice、0x101）もそのまま残る。「qcdxkm の改造は止める」という過去の決定との関係も整理が要る |

おすすめ：A の G1a〜G1c（GMU の起動まで）は、どちらの案にも役立つ観測になる。B を選ぶ場合でも、GX のクロックと GMU の挙動を自分で確かめたことは、qcdxkm の失敗を切り分ける材料になる。

## 参照

- 本家 Linux（master）：`drivers/gpu/drm/msm/adreno/{a6xx_gmu.c, a6xx_gpu.c, a6xx_hfi.c, a6xx_catalog.c, adreno_gpu.c}`、`drivers/clk/qcom/gpucc-sc7280.c`、`arch/arm64/boot/dts/qcom/{kodiak.dtsi, sm7325-nothing-spacewar.dts}`
- DSDT v33-akat：`\_SB.PEP0.G0MD / G4MD / G5MD`、`\_SB.GPU0._CRS / RESI`
- silicium/opus-session/report_qcdx_gx*.md（特に GX6、GX7、GX8、GX22、GX23）、session-logs/kd_gx_measure_2026-10-31.txt
- goals/.../GPU_STATUS_AND_OPTIONS.md、ANALYSIS_RESULTS.md（Kodiak パックの WDDM スタック）
