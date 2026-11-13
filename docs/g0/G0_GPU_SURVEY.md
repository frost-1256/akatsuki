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

### 7.1 決定（2026-11-07）：A

ユーザーの判断で A（自作のオープンソース KMD）に決めた。B（qcdxkm を描画専用で使う案）は取らない。

クリーンルームの方針：
- 本家 Linux（GPL）は、レジスタの位置や手順の順番といった**事実を調べるため**にだけ使う。コードは写さない。Epinephrine は MIT のまま保つ。
- qcdxkm の解析レポート（GX 系）は、**どこで失敗するか、どこが危ないか**を知るためにだけ使う。コードや手順の出どころにはしない。

## 8. G1a の実装（v24、20.0.2.24）

`src/epinephrine/epi_gpu.cxx`（MIT）。読むだけで、書き込みはしない。既定では動かない。

- 起動：`Device Parameters\EpiGpuProbe` を 0 以外にする。ハードウェアに触る前に 0 に戻して書き込みを確定（flush）するので、止まっても次の起動では繰り返さない。
- 順番：
  1. GCC（0x100000）の GPU 用ブランチクロック 4 つ（0x71004 `GPU_CFG_AHB`、0x7100C、0x71018、0x71154）を読む。GCC は常にクロックが入っている。`GPU_CFG_AHB` は本家 Linux が「常に ON」にしているクロック。
  2. `GPU_CFG_AHB` の CBCR bit31（CLK_OFF）が 0 のときだけ、GPU_CC（0x3D90000）の 20 個のレジスタを読む。PLL0/1 の MODE、GX/CX の GDSCR、各 CBCR、GMU/HUB の RCG、0x1534（リミッタの状態）、0x5000 が対象。0x153C（リミッタの解除）は読まない。
- ブレッドクラム：読む直前ごとに `Device Parameters\EpiGpuProbeStep` に番号を書いて flush する。番号の意味は、0x1xx が GCC、0x2xx が GPU_CC の何番目か、0xFFFF が最後まで戻ったこと。
- 結果は EpiDiag に書く：`GpuProbeStatus`（0 完了、1 無効、2 _CRS に GPU_CC がない、3 メモリ不足、4 マップ失敗、5 `GPU_CFG_AHB` が OFF なので GPU_CC は読まなかった）、`GpuProbe`（{物理アドレス, 値} の組）、`GpuProbeText`。

### 8.1 G1a の結果（2026-11-07、v24）

最後まで読めた。固まらなかった（`EpiGpuProbeStep=0xFFFF`、`GpuProbeStatus=0`）。

| レジスタ | 値 | 読み方 |
|---|---|---|
| GCC `GPU_CFG_AHB` (0x71004) | 0x00000009 | ON。このため GPU_CC が読めた |
| GCC `GPU_MEMNOC_GFX` (0x7100C) | 0x80004221 | 有効にはなっているが OFF（CLK_OFF） |
| GCC `GPU_SNOC_DVM` (0x71018) | 0x80000001 | 有効にはなっているが OFF |
| GCC `DDRSS_GPU_AXI` (0x71154) | 0x00300001 | ON |
| PLL0 / PLL1 MODE | 0 / 0 | 両方とも停止、ロックしていない |
| GX GDSCR / CX GDSCR | 0x00222001 / 0x00222001 | SW_COLLAPSE=1、PWR_ON=0。**両方とも電源が落ちている** |
| `cx_gmu` / `gx_gmu` | 0x80006221 / 0x80000000 | OFF（cx_gmu は有効になっているが、CX GDSC が落ちているので止まっている） |
| GMU RCG CMD | 0x80000000 | ROOT_OFF |
| HUB RCG CMD/CFG | 0 / 0 | XO を分周なしで使っている |
| `hub_aon` | 0x00000001 | ON |
| `ahb` / `crc_ahb` / `cxo_aon` | 0 | OFF とは出ていない |
| `cxo`、`hub_cx_int`、`cx_snoc_dvm`、`sleep`、`hlos1_vote_smmu` | bit31=1 | OFF |
| 0x1534（リミッタ） | 0 | 立っていない |

まとめ：
- GPU コアは完全に冷えた状態にある。CX と GX のどちらの電源も落ちていて、GMU のクロックも PLL も止まっている。UEFI も他のドライバも、GPU コアには触れていない。
- GPU_CC への読み取りは安全だった（`GPU_CFG_AHB` が ON だったため）。
- G1b（CX GDSC を入れ、GMU クロックを有効にする）は、まっさらな状態から始められる。

### 8.2 G1b：CX 側の電源投入テスト（v25、20.0.2.25、`EpiGpuProbe` bit1）

`epi_gpu.cxx` の `EpiGpuCxTest`。書き込むのは GPU_CC だけ。GCC の投票、PLL、TZ、SMMU には触らない。GMU と HUB の RCG は、リセット直後と同じ XO（19.2 MHz）のまま使う。

手順：
1. CX GDSC の SW_COLLAPSE を外し、PWR_ON を待つ。
2. ahb、cxo、cx_gmu、hub_cx_int、hlos1_vote_smmu の各 CBCR の enable を立てる。
3. GMU のレジスタを読む。
4. CBCR と GDSCR を元の値に戻す。

結果（2026-11-07）：成功した。固まらず、元の状態に戻った（`GpuCxStatus=0`）。
- CX GDSC は 10 µs 以内に ON になった。GDSCR は 0x00222001 から 0xF8222000、GDS_HW_CTRL は 0x5FF4 から 0x80001FE0 に変わった。
- 5 つのブランチがすべて動いた（CLK_OFF=0）。GMU の RCG の ROOT_OFF も外れた。
- GMU のレジスタ：
  - CM3_SYSRESET=1：CM3 はリセットで止まったまま。起動前としては正しい。
  - FW_INIT_RESULT=0。HFI_CTRL_STATUS=0。
  - SPTPRAC_PWR_CLK_STATUS=0xC0：GX_HM の GDSC もクロックも OFF。
  - RPMH_POWER_STATE=0xF。
- GMU の常時カウンタ（AO counter）は進んでいた（0x408E4 から 0x4759A）。GMU のクロックと CX のバスは生きている。
- 元に戻したあとの値：GDSCR=0x00222001、HW_CTRL=0x5FF4。G1a のときと同じ。

次（G1c、GMU の起動）の前に調べること：
- GMU は adreno SMMU（0x3DA0000）のストリーム 5 を通してメモリを使う（`iommus = <&adreno_smmu 5 0x400>`）。HFI のキューやログのバッファを GMU に見せるには、この SMMU を誰が持っていて、どう設定するかを先に決める必要がある（§5.2）。
- RSCC と PDC の初期化（rpmh_start）は、AOSS や RPMh とのやり取りになる。どこまで安全に書けるかを、机の上で確認する。

## 9. G1c の前の机上調査（2026-11-07）

### 9.1 adreno SMMU（MMU1）の持ち主

- DSDT v33-akat の `\_SB.MMU1`（`QCOM0A09`、UID 1）が adreno SMMU を受け持っている。_CRS は 0x3DA0000 / 0x20000 [P]。
- スマホ上では **qcsmmu.sys（Qualcomm 製）** が `ACPI\VEN_QCOM&DEV_0A09...` として動いている（Status OK）[P]。もう 1 つの ` ` は MMU0（apps SMMU）。
- `IMM0`/`IMM1`（`QCOM068F`）には qciommu.sys が付いている [P]。
- **MMU1 を _DEP に持っているのは GPU0 だけ**（行 13522–13523：MMU0、MMU1、IMM0、IMM1、PEP0…）[P]。MMU0 は多くのデバイスが使っている。
- IORT はない（HKLM\HARDWARE\ACPI に DSDT、FADT、RSDT、SSDT だけ）[P]。Windows 標準の DMA リマップではなく、Qualcomm のドライバが SMMU を握る構成になっている。
- GMU はストリーム 5 を使う（`iommus = <&adreno_smmu 5 0x400>`）。GMU のファームウェアは決まった IOVA を前提にしている（0x60000000 から始まる uncached 領域、HFI と log のバッファ）[P]。このためバイパスでは動かせず、変換テーブルが必要になる。
- adreno SMMU 自体も、GPU_CC の `hlos1_vote_gpu_smmu` と CX の電源の内側にある [I]。G1a の時点ではこのクロックが OFF だったので、qcsmmu は今は MMU1 を実際には動かしていないと考えられる [I]。

選択肢：

| 案 | 内容 | 評価 |
|---|---|---|
| S1：自分で持つ | MMU1 を Windows から外し（DSDT で GPU0 の _DEP から MMU1 を抜き、MMU1 の _STA を 0 にする）、Epinephrine が SMMU-500 のコンテキストバンクを自分で設定する。手本は本家 Linux の arm-smmu（qcom の adreno-smmu 向け対策を含む）で、事実だけを使う | オープンな方針に合う。MMU1 を使うのは GPU0 だけなので、他への影響は小さい。ハイパーバイザが SMMU の一部の書き込みを横取りする可能性があり、それを確かめる必要がある [U] |
| S2：qcsmmu を使う | qcsmmu / qciommu のインターフェースを解析して呼ぶ | プロプラ依存が残る。解析も必要になる。方針に合わない |

おすすめは S1。注意点として、MMU1 をデバイスマネージャーで無効にするだけだと、_DEP が満たされず GPU0（Epinephrine）が起動しなくなるおそれがある [I]。外すときは DSDT（v34）でまとめて変える。

### 9.2 RSCC と PDC

- A635 は a660 ファミリーに入る。本家 Linux では、この場合 `pdc_in_aop = true` になり、**PDC の TCS コマンドは AOP（ファームウェア）側が設定済み**として書かない [P]。
- このためドライバが書くのは次の範囲だけ：
  - GPU PDC（0xB290000、_CRS にある）：`PDC_GPU_SEQ_START_ADDR = 0` と `PDC_GPU_ENABLE_PDC = 0x80000001` の 2 つ。
  - RSCC（GPU 専用の RSC。kodiak では "rscc" リソース。DSDT の _CRS では 0x3DE0000 / 0x10000）：約 20 個の初期化値（シーケンスメモリと TCS 設定）。そのあと rpmh_start（`RSCC_CONTROL_REQ` を立てて ACK と SEQ_BUSY を待つ）。
- どちらも GPU 専用のブロックで、他のサブシステムの投票には触れない [I]。実際に電圧（GX/CX のレール）を投票するのは、GMU のファームウェアが動き出してから [I]。
- GMU に渡す ARC レベル表（gfx.lvl、cx.lvl、mx.lvl）は、cmd-db から読む。cmd-db は予約メモリ 0x80860000 / 0x20000 にあり、読むだけで済む [P]。形式は本家 Linux の cmd-db.c で確認する（事実だけ）。

### 9.3 G1c の分け方（案）

1. **G1c-0（読むだけ）**：cmd-db からレベル表を読む。G1b と同じ手順で CX を入れ、RSCC と GPU PDC の現在値を読む。
2. **G1c-1（DSDT v34）**：MMU1 を Windows から外す（S1）。Epinephrine から adreno SMMU を読めるかを確かめる。
3. **G1c-2**：SMMU のコンテキストバンク（ストリーム 5）、RSCC と PDC の初期化、GMU ファームウェアの読み込み、CM3 の起動。`FW_INIT_RESULT` に 0xBABEFACE が出るまで。

### 9.4 G1c-0 の結果（v26 / v27、2026-11-07）

- **v26（`EpiGpuProbe=7`）：スマホが再起動した。** ブレッドクラムは `0x338` で、GPU PDC の `GPU_ENABLE_PDC`（0xB294500）を最初に読んだところだった。cmd-db、GMU、RSCC の読み取りは、その前に終わっていた。
  - 固まったのではなく再起動したので、ハイパーバイザの stage-2 保護（HLOS に GPU PDC が見せられていない）の可能性が高い [I]。
  - **GPU PDC（0xB290000）には触らない。** v27 では、bit3 を立てたときだけ読むようにした。
- **v27（`EpiGpuProbe=7`、PDC なし）：成功した。** `GpuProbeStatus=0`、`GpuCxStatus=0`、`CmdDbStatus=0`。
- **cmd-db**（0x80860000、magic `db30030c`、version 1）が読めた。ARC（slave 3）のおもな値：

| リソース | アドレス | レベル（vlvl） |
|---|---|---|
| gfx.lvl | 0x30050 | 0, 64, 128, 192, 224, 256, 320, 384, 416 |
| cx.lvl | 0x30000 | 0, 16, 64, 128, 192, 256, 384, 416 |
| mx.lvl | 0x30010 | 0, 16, 64, 128, 192, 224, 256, 320, 384, 416, 432 |

  `gmxc.lvl` はない。本家 Linux では、その場合 GX の 2 つ目の投票に mx.lvl を使う [P]。VRM（slave 4）に `vrm.gfx`（0x42a00）がある。
- **RSCC**（0x3DE0000、CX を入れた状態で読んだ）：
  - 一部はすでに設定されていた。`PDC_SEQ_START_ADDR=0x4520`、`MATCH_VALUE_LO/HI=0x4510/0x4514`、`PDC_SLAVE_ID=1` で、本家 Linux の初期化値と同じ。HIDDEN_TCS の ADDR には 0x82204514 と 0x82204510 が入っていた。
  - `RSC_STATUS0_DRV0=0`。
  - `SEQ_MEM_0..5=0`：シーケンスはまだ読み込まれていない。
  - TCS0〜3 の STATUS はどれも 1（空き）。
  - GMU 側の `RSCC_CONTROL_REQ/ACK` は 0 / 0。
- 意味すること：
  - G1c-2 では、PDC に書かずに RSCC の初期化と rpmh_start だけを試す。a660 ファミリーでは PDC を AOP が持っているので、それで足りるかを確かめる [U]。
  - GMU に渡す ARC の投票表は、この cmd-db から作れる。

### 9.5 G1c-1 結果（2026-11-08、v34-akat、Epinephrine v28、`EpiGpuProbe=0x10`）

- v34 で MMU1 が隠れ、qcsmmu が adreno SMMU を掴んでいない状態で、CX 窓の中から読んだ。リセットなし（Step 0xFFFF、GFSR 0）。
- `sCR0=0x002C0406`：CLIENTPD=0（SMMU 有効）、GFRE/GFIE=1、**USFCFG=1**（どの SMR にも当たらないストリームはバイパスでなくフォールト）。上位 0x2C0000 は未解読。
- `IDR0=0x4C017E06`：S1TS=1・S2TS=0（ステージ 1 のみ）、SMS=1（ストリームマッチ）、NUMIRPT=1、NUMSIDB=15、**NUMSMRG=6**。
- `IDR1=0x30000007`：4KB ページ、NUMPAGENDXB=3（グローバル 16 ページ = 64KB、コンテキストバンクは 0x3DB0000 から）、NUMS2CB=0、**NUMCB=7**。
- `IDR2=0x5511`：IAS/OAS とも 36 ビット、UBS=64 ビット、AArch64 4KB と 64KB の表形式に対応。
- `IDR7=0x24`：r2p4（SMMU-500）。
- SMR0〜5 はどれも 0（VALID なし）。S2CR は意味のない初期値。**誰もストリームを設定していない**。
- 意味すること：
  - 今のままでは GMU（ストリーム 5）の DDR アクセスはすべてフォールトする。
  - G1c-2 では SMMU を自分で設定する。SMR0 に ID 5 / マスク 0x400、S2CR0 を CB0 への変換、CB0 に AArch64 4KB のステージ 1 表を置いて GMU の IOVA（0x60000000〜）を自前のメモリに張る。
  - ハイパーバイザ（ステージ 2）がこの SMMU への書き込みを許すかは未確認 [U]。最初は sCR0 を変えずに SMR/S2CR/CB0 だけ書き、読み戻しで確かめる。

### 9.6 G1c-2a 実装メモ（手作業で実装する）

G1c-2a（adreno SMMU の書き込みと読み戻し）は手で実装する。2026-11-08 に Claude で実装しようとしたが、途中で止まった。この節では、既存のプローブの枠組みへの組み込み方と、テストの約束事だけを書く。SMMU の設定内容は §9.5 の方針どおりで、事実の出典は下の参照に限る。

- 組み込み先：`src/epinephrine/epi_gpu.cxx`
  - `EpiGpuCxTest` の「3a. G1c-1」の直後に入れる。ここなら GMU レジスタの読み取りが済んでいて、CX GDSC とブランチが ON の状態になっている。
  - 有効にするフラグは `EpiGpuProbe` の bit5（0x20）。CX 窓を開く条件 `flags & 0x16` は 2 か所ある（GPU_CC を RW でマップする条件と、`EpiGpuCxTest` を呼ぶ条件）。両方を `0x36` にする。
  - ブレッドクラム（`EpiGpuProbeStep`）は 0x360〜0x36F を使う。これは未使用。書き込みのたびに 1 ステップ進める。リセットしたら、次の起動で最後のステップを読めば、どの書き込みで落ちたかがわかる。
  - 結果は EpiDiag に `GpuSmmuW`（REG_BINARY）として、「元の値・書いた値・読み戻した値」の並びで置く。PC 側で解読しやすい。主要な値は `GpuCxText` にも `EpiCat` で足す。
  - `Sample/SampleDisplay.vcxproj` の DriverVer を上げる。20.x の範囲を守る（例：20.0.2.29）。
- テストの約束事：
  - 全体：
    - sCR0 には書かない。
    - GMU の CM3 は reset のまま（`cm3_sysreset=1`）にする。GX も OFF のまま。この 2 つで、SMMU を通るアクセスが起きない状態を保つ。
    - 元の値を全部読んでから書く。書いたらすぐ読み戻す。最後は逆順で元の値に戻す。
  - CX 窓を閉じるところ：
    - CX GDSC を落とすと、SMMU の状態も消えるはず [U]。G1c-2a の最後には CX GDSC を落としたあと、もう一度 CX を ON にして SMR0 を読み、値が消えたかを確かめる。消えたかどうかは、そこで初めてわかる。
    - コンテキストバンクのフォールト割り込みは有効にしない（ハンドラがないため）。
  - リスク：
    - リスクは GPU PDC のときと同じ種類で、ハイパーバイザのステージ 2 で止められる可能性がある [U]。落ちたらブレッドクラムで位置を特定し、その書き込みは打ち切る。
    - 実機で走らせるのは毎回 1 回だけ（ワンショットで、`EpiGpuProbe` は自動で 0 に戻る）。
- 参照（事実のみ。コードは持ち込まない）：Arm IHI 0062 SMMU Architecture Specification v2（グローバル空間 0/1、コンテキストバンク）、Arm SMMU-500 TRM（r2p4）、本家 Linux `drivers/iommu/arm/arm-smmu/arm-smmu.h` と `arm-smmu-qcom.c`（adreno SMMU の扱い）、`kodiak.dtsi` の `adreno_smmu` と GMU の `iommus`。

### 9.6.1 G1c-2a の実装（v29、20.0.2.29、実機未実行）

`EpiGpuProbe=0x20`（bit5）で単独実行できる。既存の bit6（GMU TCM ロード）も保持し、CX 窓を開くマスクは両方のビットを含む `0x76` にした。ワンショットの解除・flush は既存どおり。

- `EpiSmmuWriteTest` を G1c-1 の直後に追加。CM3_SYSRESET=1、GX GDSC の collapse、SMMU クロック ON、§9.5 と同じ IDR0/1/2/7 を確認する。全 SMR の VALID と全 CB の SCTLR.M/CFIE/CFRE が 0 でなければ書かない。CX が最初から ON の場合も試験しない。
- 書く順番は CB0 SCTLR（0x106：M/CFIE/CFRE は 0）、CBAR0（0x1F300）、CBA2R0（1：AArch64）、S2CR0（0：変換先 CB0）、SMR0（0x84000005：ID 5、mask 0x400、VALID）。全対象の元の値を先に読み、各書き込みを即時読み戻しする。不一致なら後続の設定を止め、書いた対象すべてを逆順で復元・読み戻しする。sCR0 は書かない。
- ページテーブルは作らず、CB0 の変換も開始しない。これは書き込み可否の試験であり、GMU メモリをマップする `EpiSmmuMap/Unmap` や、運用用の `EpiSmmuEnable/Disable` ではない。それらには別途、表の確保、キャッシュ整合性、TLB 無効化と同期の実装が必要。
- ブレッドクラム：`0x35E` は事前確認、`0x360..364` は上記順の書き込み、`0x365..369` は逆順の復元。CX collapse 完了後、`0x36A` で再投入し、`0x36B..36F` で 5 ブランチを有効化、`0x35F` で SMR0 を読む。各 CX 窓の後始末は `0x341..345` でブランチを逆順復元、`0x346` で GDSCR を復元する。
- **電源断で値が消えるかは未確定 [U]。** SMR0 は電源断の前に元に戻してある。そのため、再投入後が 0 でも、試験値が電源断によって消えた証拠にはならない。復元後の値と再投入後の値を別々に記録する。

EpiDiag の診断形式：

| 値 | 内容 |
|---|---|
| `GpuSmmuW` (REG_BINARY) | 上記書き込み順の 5 レコード、各 5 DWORD（little endian）：SMMU ベースからのバイト offset、元の値、設定予定値、即時読み戻し値、復元読み戻し値。合計 100 バイト。未実行フィールドは 0、必ず status と併読する |
| `GpuSmmuWStatus` | 0 完了、1 map 失敗、2 CM3/GX/クロックの前提不成立、3 ID/layout 不一致、4 既存 stream/context あり、5 書き込み不一致、6 復元不一致、7 SMMU 試験未到達 |
| `GpuSmmuWCycleStatus` | 0 再投入後の読み取りと再 collapse 完了、1 再投入 timeout、2 branch timeout、3 map 失敗、4 最終 collapse timeout、7 再投入試験未到達 |
| `GpuSmmuWAfterSmr0` | 再投入後の SMR0。未取得は 0xFFFFFFFF |
| `GpuCxStatus` | 従来値に加え、14 最初から CX が ON（試験せず）、15 再投入試験失敗。SMMU 自体の成否は `GpuSmmuWStatus` を見る |

主要値は `GpuCxText` にも残す。文字列バッファを 3072 WCHAR に広げ、既存の GMU 診断と同時に記録できるようにした。ARM64 Release のコンパイル・リンク、INF 検査、カタログ生成が成功（エラー・警告なし）。実機には配布・実行していない。

レジスタ定義の照合元（事実のみ、コードは取り込んでいない）：[本家 arm-smmu.h](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.h)、[adreno SMMU の扱い](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c)。ハイパーバイザによる書き込み許可と実機の読み戻し結果は未検証 [U]。

### 9.7 G1c-2b 結果（2026-11-08、v34-akat、Epinephrine v29、`EpiGpuProbe=0x40`）

- ファームウェア：stock vendor の `a660_gmu.bin`（55,256 バイト、SHA-256 6bb9360a…）。Adreno 635 は a660 のファームウェアを使う。
  - 先頭の 11 ブロックは size 0 の領域記述。そのうち 2 つは type 7（0x60000000 と 0x60001000、各 0x1000）。
  - データを持つのは 4 ブロック：
    | ブロック | GMU アドレス | サイズ | 中身 |
    |---|---|---|---|
    | blk11 | 0x0 | 0x1DC | ITCM、ベクタテーブル。先頭は初期 SP = 0x10007F80 |
    | blk12 | 0x4000 | 0xD4CC | icache、**コード本体。SMMU 経由** |
    | blk13 | 0x10007F98 | 0x28 | DTCM |
    | blk14 | 0x10007FDC | 0x18 | DTCM。CORE_FW_VERSION = 0x30010008 を含む |
- 結果：GpuFwStatus 0。リセットなし。
  - ITCM 0x1DC バイトと DTCM 0x40 バイトを書き、読み戻しの不一致は 0。
  - `CORE_FW_VERSION` は書く前が 0x9E58CE6C（TCM のごみ）、書いた後は **0x30010008**（v3.1.8）。
  - CM3 は reset のまま（`cm3_sysreset=1`）。CX は元どおり落ちた。
- 意味すること：
  - CX 窓の中で、GMU の TCM は AHB から読み書きできる。
  - CM3 を起動するには、icache（IOVA 0x4000）に blk12 を置いて SMMU で対応付ける必要がある。0x60000000 の dummy 領域も同じ。
  - バージョンが 0x20010004 より大きいので、起動完了の判定は `(FW_INIT_RESULT & 0x1FF) == 0x100` になる（0xBABEFACE ではない）。

### 9.8 SMMU API と CM3 起動プローブ（v30、実機未実行）

Map/Unmap・Enable/Disable・QueryFault と、bit7（0x80）の CM3 起動・停止プローブを実装した。API の契約、領域配置、診断値、検証結果は [G1C_SMMU_GMU_API.md](G1C_SMMU_GMU_API.md) を参照。PDC/RSCC 初期化と HFI メッセージは初版では省略しており、実機の起動成功は未確認 [U]。

### 9.9 G1c-2a / G1c-3 の実機結果（2026-11-08、v34-akat）

**G1c-2a（v30、`EpiGpuProbe=0x20`）**：`GpuSmmuWStatus=6`。書き込みはすべてハイパーバイザに止められずに通った [P]。

| レジスタ | 元の値 | 書いた値＝読み戻し | 復元 |
|---|---|---|---|
| CB0 SCTLR | 0x31A56116 | 0x106 | OK |
| CBAR0 | 0 | 0x1F300 | **0x1F300 のまま** |
| CBA2R0 | 0 | 1 | OK |
| S2CR0 | 0x1BFA5BFF | 0 | OK |
| SMR0 | 0 | 0x84000005 | OK（0） |

CBAR0 が 0 に戻らないのは、stage 1 しかない SMMU で TYPE=0（stage 2）を受け付けないためと見られる [U]。害はない。

**G1c-3（`EpiGpuProbe=0x80`）**：読み戻しの不一致を 1 つずつ潰した。

| 版 | 止まった場所 | 書いた値 → 読み戻し | 対処 |
|---|---|---|---|
| v30 / v31 | GMU setup[0] `AO_HOST_INTERRUPT_MASK`（0x23B06） | 0xFFFFFFFF → 0x3F（リセット時から 0x3F） | 比較マスク 0x3F |
| v32 | SMMU CB0 `TCR2`（ステップ 0x503） | 0x38011 → 0x38071（bit5/6、TBI0/TBI1 [U] が 1 固定） | 比較マスク 0xFFFFFF9F |
| v34 | SMMU CB0 `SCTLR`（ステップ 0x50D） | 0x1127 → 0x0127（bit12 ASIDPNE が RAZ） | 比較マスク 0xFFFFEFFF |
| **v35** | **CM3 解放後の時間切れ**（status 7） | — | — |

- その他の実測：`GMU2HOST_INTR_MASK`（0x1F993）は 32 ビットすべて読み戻せる。リセット時の値で 0 以外だったのは `CM3_CFG=0x4052`、`PWR_COL_INTER_FRAME_CTRL=0x09C40402`。
- **v35（3312b49）：SMMU が有効になり、CM3 が解放され、ファームウェアが動いた** [P]。
  - `FW_INIT_RESULT=0x800`（期待値は `(x & 0x1FF) == 0x100`）。`FW_BUSY=0`、`GMU2HOST_INTR_INFO=0x4`、`AO_HOST_INTR=0`、`GENERAL_0=0x30000009`、`GENERAL_1=0`。
  - SMMU の `FSR=0x400`：FORMAT フィールドだけで、fault ビットは 0。icache（IOVA 0x4000）からの命令取り込みも、ログ領域への書き込みも、SMMU 越しに通った。
  - ログ領域（IOVA 0x60004000）に 16 バイトの記録が 4 件。形式は `{ID, タイムスタンプ, 番号, 値}` と見られる [U]：(0x21, 0xCF6FC9, 1, 0)、(0x23, 0xD126D8, 1, 0xDEADBA55)、(0x23, 0xD131DD, 2, 0xDEADBA55)、(0x21, 0xD13759, 3, 0x1264)。19.2 MHz とすると約 6 ms で止まっている。
  - 後片付け（CM3 を reset、SMMU Disable、CX を落とす）はすべて成功。
  - 足りない候補：RSCC の初期化、`GMU_RPMH_CTRL`、GMU のクロック（§9.10）[U]。

### 9.10 RSCC / RPMh の手順（本家 Linux、a660 ファミリー）

事実の出典は本家 Linux の `a6xx_gmu.c` と `a6xx_gmu.xml`。行番号は手元の参照コピーのもの。コードは持ち込まない。

- **`a6xx_rpmh_start`（620 行）は初回の起動では何もしない。** `GMU_STATUS_PDC_SLEEP` が立っているときだけ動き、それを立てるのは `a6xx_rpmh_stop`（673 行）だけ。初回に効くのは、プローブ時に 1 回呼ばれる `a6xx_gmu_rpmh_init`（681 行、呼び出しは 2530 行）[P]。
- **`a6xx_gmu_rpmh_init`**：RSCC のベースは 0x3DE0000（kodiak.dtsi の "rscc"）。オフセットは dword 単位で、バイト位置は ×4。

| 順 | レジスタ（dword） | バイト | 値 | 実機の値（v27） |
|---|---|---|---|---|
| 1 | `GPU_RSCC_RSC_STATUS0_DRV0`（0x0004） | +0x010 | 0x01000000（SDE のクロックゲーティング無効化） | 0 |
| 2 | `RSCC_PDC_SLAVE_ID_DRV0`（0x000B） | +0x02C | 1 | 1（設定済み） |
| 3, 4 | `HIDDEN_TCS_CMD0_DATA` / `ADDR`（0x000E / 0x000D） | +0x038 / +0x034 | 0 / 0 | ※ |
| 5, 6 | 同 +2（0x0010 / 0x000F） | +0x040 / +0x03C | 0 / 0 | ※ |
| 7, 8 | 同 +4（0x0012 / 0x0011） | +0x048 / +0x044 | 0x80000000 / 0 | ※ |
| 9 | `RSCC_OVERRIDE_START_ADDR`（0x0100） | +0x400 | 0 | |
| 10 | `RSCC_PDC_SEQ_START_ADDR`（0x0008） | +0x020 | 0x4520 | 設定済み |
| 11, 12 | `RSCC_PDC_MATCH_VALUE_LO` / `HI`（0x0009 / 0x000A） | +0x024 / +0x028 | 0x4510 / 0x4514 | 設定済み |
| 13〜17 | `RSCC_SEQ_MEM_0_DRV0` +0〜+4（0x0180〜） | +0x600〜+0x610 | 0xEAAAE5A0、0xE1A1EBAB、0xA2E0A581、0xECAC82E2、0x0020EDAD | すべて 0 |

  - そのあと PDC（0xB290000）に `PDC_GPU_SEQ_START_ADDR=0` と `PDC_GPU_ENABLE_PDC=0x80000001` を書く（799〜800 行）。a660 ファミリーは `pdc_in_aop=true` なので、PDC の TCS とシーケンスは書かない。**PDC は読むだけで電話がリセットされるので触らない**（§9.4）。今の状態は確かめられない [U]。
  - ※ HIDDEN_TCS の 3 つの ADDR のうち 2 つに、v27 の時点で 0x82204514 と 0x82204510 が入っていた。Linux はここを 0 にするが、ブートローダか AOP が入れた値の可能性がある [U]。まずは設定済みの値（2、10〜12、ADDR）を上書きしない形で試す。
- **起動のたびの処理で、今の実装（3312b49）にないもの**：
  - GMU のコアクロックを 200 MHz、hub を 150 MHz に設定（1340〜1342 行）。今は RCG の既定値（XO と見られる [U]）。
  - 起動用のバス帯域の投票（1355 行）。DDR へのアクセスは SMMU 越しに通っているので、急ぎではない。
  - `AO_HOST_INTERRUPT_CLR = ~0`（1358 行）。
  - **`GMU_RPMH_CTRL`（0x1F8E8）\|= 0xF11**（846〜852 行）：RPMH_INTERFACE（bit0）、LLC（bit4）、DDR（bit8）、MX（bit9）、CX（bit10）、GFX（bit11）の投票を有効にする。
- **`rpmh_start` / `rpmh_stop`**（2 回目以降の起動、停止時）：
  - start：`GMU_RSCC_CONTROL_REQ`（0x23B07）に bit1 を書く。`GMU_RSCC_CONTROL_ACK`（0x23B08）の bit1 を待つ（100 µs 間隔で最大 10 ms）。RSCC の `SEQ_BUSY_DRV0`（0x0101、+0x404）が 0 になるのを待つ。REQ を 0 に戻す。
  - stop：REQ に 1 を書く。`RSC_STATUS0_DRV0` の bit16 を待つ。REQ を 0 に戻す。
- **次の進め方（案）**：
  1. RSCC への書き込み試験（CM3 は reset のまま）：表の 1、3、5、7、9、13〜17 を書いて読み戻す。ADDR はすべて保持する（順8も対象外）。書き込みごとにブレッドクラムを進める。RSCC への書き込みがハイパーバイザに止められないかを、独立した 1 回で確かめる。
  2. RSCC の初期化をしたうえで、`RPMH_CTRL=0xF11` を足して `0x80` を実行する。`FW_INIT_RESULT` とログの記録が v35 からどう変わるかを見る。
  3. 変わらなければ、GMU のクロックを 200 MHz にする（GPU_CC の RCG の設定）。
  - GMU 起動のコードはユーザーが書く。

### 9.10.1 RSCC 単独書き込み試験の実装（実機未実行）

`EpiGpuProbe=0x100`（bit8）で実行する。実装は `src/epinephrine/epi_gpu.cxx` の既存 CX 窓に組み込んだ。フラグはハードウェアアクセス前に 0 に戻して flush し、解除に失敗した場合は試験しない。bit0/1 以外の別試験との併用は拒否するため、`0x80` の CM3 起動は同時に走らない。ファームウェアも不要。

- CX が開始時点で collapse していることを要求し、CX と5ブランチを有効にする。CM3_SYSRESET=1、GX GDSC collapse と GMU 側の GX OFF、5ブランチの CLK_OFF=0、RSCC_CONTROL_REQ/ACK=0、RSCC SEQ_BUSY=0 を確認する。前提不成立なら RSCC には書かない。
- 対象は dword 順に `0x004, 0x00E, 0x010, 0x012, 0x100, 0x180..0x184` の10項目。設定値は上表のとおり。すべての元値を先に読み、各書き込みを即時読み戻しして完全一致で比較する。失敗した項目を含め、書き込み済みの項目すべてを逆順に復元して読み戻す。復元不一致でも残りの復元を試みる。その後、既存の処理でブランチと CX を元に戻す。
- HIDDEN_TCS の全 ADDR（`0x00D, 0x00F, 0x011`）、PDC_SLAVE_ID、PDC_SEQ_START/MATCH は保持する。GPU PDC のマップ・読み書き、RPMH_CTRL、RSCC_CONTROL_REQ、CM3_SYSRESET への書き込みは行わない。
- ブレッドクラムは `0x900` が前提確認、`0x901` が RSCC idle 確認と元値の保存、`0x910+i` が項目 i の設定、`0x930+i` が項目 i の復元。i は0始まり。設定時はブレッドクラムの保存・flush に失敗したら次の書き込みを中止する。復元は保存に失敗しても実行する。CX の後始末は既存の `0x340..0x346`、正常に戻れば最終ステップは `0xFFFF`。

EpiDiag の診断形式：

| 値 | 内容 |
|---|---|
| `GpuRsccW`（REG_BINARY） | 上記順の10レコード、各5 DWORD（little endian）：`{Dword, Original, Value, Readback, Restored}`。合計200バイト。Dword は 0x03DE0000 からの dword offset（バイト位置は×4）。未読の Readback/Restored は 0xFFFFFFFF、元値保存前の Original は0。試験関数未到達なら空データ |
| `GpuRsccWStatus`（REG_DWORD） | 0 設定・復元の全比較成功、1 RSCC map失敗、2 前提不成立、3 設定読み戻し不一致、4 復元読み戻し不一致、5 ブレッドクラム保存・flush失敗、6 別試験との併用を拒否、7 試験関数未到達。後始末で追加失敗した場合の優先順位は4、5、3 |
| `GpuRsccWCount`（REG_DWORD） | 書き込みを試みた項目数（0〜10）。不一致の項目も含む |
| `GpuRsccWBadIndex`（REG_DWORD） | 設定の不一致、または設定直前のブレッドクラム失敗の項目番号。該当なしは0xFFFFFFFF |
| `GpuRsccWRestoreBadIndex`（REG_DWORD） | 最初の復元不一致の項目番号（復元は逆順）。該当なしは0xFFFFFFFF |
| `GpuCxStatus` | 既存値に加え17が RSCC 試験失敗。14は開始時に CX が collapse していない。CX の復元結果も併読する |
| `GpuProbeStatus` | 既存値に加え6が別試験との併用、7がワンショット解除の保存・flush失敗 |

`GpuCxText` にも `rscc_w_status`、`rscc_w_count`、`rscc_w_bad` を追記する。`GpuRsccWStatus=0` でも CX の後始末は別なので `GpuCxStatus` を必ず併読する。今回の変更ではビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat、Epinephrine v36 / 20.0.2.36、`EpiGpuProbe=0x100`）：成功** [P]。`GpuRsccWStatus=0`、`GpuRsccWCount=10`、`GpuRsccWBadIndex` と `GpuRsccWRestoreBadIndex` はどちらも 0xFFFFFFFF、`GpuCxStatus=0`。

- 10 項目とも、書く前の値は 0（v27 と同じ）。書いた値はそのまま読み戻せ、すべて 0 に戻った。
- ハイパーバイザは RSCC（0x3DE0000）への書き込みを止めない。GPU PDC（0xB290000）とは扱いが違う。
- CM3 は最後まで reset のまま（`cm3_sysreset=1`）。CX も正常に落ちた。
- 次は §9.10 の 2 番目：`0x80` の起動に、RSCC の初期化と `GMU_RPMH_CTRL=0xF11` を足す。

### 9.10.2 0x80 起動試験への RSCC / RPMH_CTRL 追加（実機未実行）

`EpiGpuProbe=0x80` の `EpiGmuBootTest` に §9.10 の2番目を追加した。独立試験 `0x100` と同じ10項目・同じ値を使い、HIDDEN_TCS の全 ADDR と PDC slave/sequence/match は保持する。GPU PDC、RSCC_CONTROL_REQ、クロック設定には書かない。

- CM3 reset、GX OFF、CX と5ブランチ ON の既存前提を確認する。RSCC_CONTROL_REQ/ACK と RSCC SEQ_BUSY が0であることも確認し、RSCC の元値をすべて保存してから初期化する。各書き込みを即時読み戻しし、不一致やブレッドクラム失敗なら CM3 を解放せず後始末する。
- 続いて GMU setup と TCM コピー、SMMU Enable、CM3 解放へ進む。GMU setup の末尾（index 18）に `GMU_RPMH_CTRL`（dword `0x1F8E8`）への `0xF11` を追加し、比較マスクは `0xFFFFFFFF`。`GpuGmuSetup` は19レコード×20バイト＝380バイトとなる。
- 起動完了・時間切れ後の `GpuGmuRegs` と `GpuGmuLog`（先頭0x1000バイト）の採取は従来どおり。`GpuGmuRegs` の先頭 DWORD が FW_INIT_RESULT。v35 の `0x800` とログ4件から変化したかを実機で確認する。
- RSCC は CM3 起動中には復元しない。CM3 reset と SMMU Disable の成功後、GMU setup（RPMH_CTRL を含む）を復元し、その後 RSCC を逆順に復元・読み戻しする。CM3 reset、SMMU Disable、GMU 設定復元に失敗した場合は RSCC の設定とマップも保持する。RSCC 復元は途中で失敗しても残りを試み、失敗時は既存の teardown 失敗と同じく CX・マップ・メモリを保持する。

追加診断（EpiDiag）：

| 値 | 内容 |
|---|---|
| `GpuGmuRsccSetup`（REG_BINARY） | 独立試験と同じ順の10レコード×20バイト。`{Dword, Original, Value, Readback, Restored}`。Dword は RSCC 0x03DE0000 からの dword offset。未読の Readback/Restored は0xFFFFFFFF。元値保存前は空データ |
| `GpuGmuRsccBadIndex`（REG_DWORD） | 初期化の書き込み不一致、または書き込み直前のブレッドクラム失敗の0始まりの番号。該当なしは0xFFFFFFFF |
| `GpuGmuRsccRestoreBadIndex`（REG_DWORD） | 復元不一致または復元直前のブレッドクラム失敗の最初の番号。復元は逆順。該当なしは0xFFFFFFFF |
| `GpuGmuRsccRestoreStatus`（REG_DWORD） | 復元の NTSTATUS。0なら成功、未試行なら STATUS_NOT_FOUND。復元不一致は STATUS_IO_DEVICE_ERROR。ブレッドクラム保存に失敗しても復元書き込み自体は試みる |
| `GpuGmuStatus` | 新規9＝RSCC 初期化の書き込み・読み戻し・ブレッドクラム失敗（CM3 未解放）。RSCC idle の前提不成立は既存の1、復元失敗は既存の8。`GpuGmuNtStatus` は後始末開始時の状態を保持する |

ブレッドクラムは `0x680` が RSCC idle 確認・元値保存、`0x690+i` が初期化、`0x6B0+i` が復元（i は0始まり）。GMU_RPMH_CTRL は setup index 18 なので設定時 `0x612`、復元時 `0x652`。今回の変更ではテスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat、Epinephrine v37 / 20.0.2.37、`EpiGpuProbe=0x80`）：GMU の起動に成功** [P]。

| 項目 | v35（RSCC なし） | v37 |
|---|---|---|
| `GpuGmuStatus` | 7（時間切れ） | **0** |
| `FW_INIT_RESULT` | 0x800 | **0x100** |
| `GMU2HOST_INTR_INFO` | 0x4 | 0 |
| SMMU `FSR` | 0x400（fault なし） | 0x400（fault なし） |
| ログの記録 | 4 件 | 6 件 |
| `GpuCxStatus` | 16 | 0 |

- RSCC の 10 項目は書いた値のまま読め、終了後にすべて 0 へ戻った（`GpuGmuRsccRestoreStatus=0`）。`GMU_RPMH_CTRL` も 0xF11 のまま読めた。
- Disable、CM3 の reset、CX を落とす処理はすべて成功（`KeepCx=0`、`cm3_sysreset=1`、`rpmh_power_state=0xF`）。
- ログ：(0x21, 番号 1, 0)、0x23 が 4 件（番号 1、2、3、0x3A、値はどれも 0xDEADBA55）、(0x21, 番号 5, **0x100**)。最後の 0x21 は初期化の結果と見られる [U]。v35 では最後が (0x21, 番号 3, 0x1264) だった。どちらも約 6 ms の間に書かれている。
- わかったこと：PDC に触れなくても、RSCC の初期化と `RPMH_CTRL` だけで GMU のファームウェアの初期化は通る。a660 ファミリーで PDC を AOP が持つという見立て（§9.2）と合う [I]。
- 次は HFI（§9.11）。

### 9.11 HFI の手順（本家 Linux、A635 = 7c3、非 legacy）

事実の出典は本家 Linux の `a6xx_hfi.c`、`a6xx_hfi.h`、`a6xx_gmu.c`、`a6xx_gmu.xml`、`a6xx_catalog.c`、`kodiak.dtsi`。行番号は手元の参照コピーのもの。コードは持ち込まない。

**A635 の扱い** [P]：chip id 0x06030500 は `adreno_is_7c3`。カタログ（`a6xx_catalog.c` 1124 行）には `bcms`、`clx_tbl`、`thinmem_config`、`gmu_chipid`、IFPC の quirk がない。kodiak.dtsi の GPU の OPP には `qcom,opp-acd-level` がない。したがって ACD、CLX、IFPC、THINMEM のメッセージは送られない。`gbif_cx = a640_gbif`（GBIF_QSB_SIDE0〜3 = 0x00071620）は `fw_start` で書かれるが、今の実装にはない。

**1. キュー表**（`a6xx_hfi_init`、1155〜1198 行）：今の `GmuQueues()`（3312b49）と同じ。表のヘッダは 6 dword（size は 168 バイト）。キューのヘッダは 12 dword が 3 つで、HFI 領域の +0x1000（コマンド、id 0）、+0x2000（応答、id 1）、+0x3000（デバッグ、id 2）に置く。各キューは 0x400 dword。

**2. HFI の開始**（`a6xx_gmu_hfi_start`、346 行。`gmu_start` の成功直後に呼ばれる）：
- `GMU_HFI_CTRL_INIT`（0x1F986）= 1。
- `GMU_HFI_CTRL_STATUS`（0x1F980）の bit0 を待つ（100 µs 間隔、最大 10 ms）。

**3. 送信**（`a6xx_hfi_send_msg` 212 行、`a6xx_hfi_queue_write` 72 行）：
- 先頭 dword は `(seqnum << 20) | (0 << 16) | (dwords << 8) | id`。seqnum は 1 から始まり、`% 0xFFF`。
- コマンドキューの `write_index` から書き、`size`（0x400）で折り返す。書いたあと、4 dword 境界まで 0xFAFAFAFA で埋める。空き（CIRC_SPACE）が足りなければ送らない。
- バリアのあと `write_index` を更新し、`GMU_HOST2GMU_INTR_SET`（0x1F994）= 1。

**4. 応答の待ち方**（`a6xx_hfi_wait_for_ack` 149 行）：
- `GMU_GMU2HOST_INTR_INFO`（0x1F992）の bit0（MSGQ）を待つ（100 µs 間隔、最大 1 s）。マスク（0x1F993）が全部立っていても INFO は読める。
- `GMU_GMU2HOST_INTR_CLR`（0x1F991）= 1。
- 応答キューを読む。`read_index == write_index` なら空なので、`rx_request = 1` にしてもう一度待つ。先頭 dword の bit15:8 が dword 数。読んだあと `read_index` を 4 dword 境界に揃えて更新する。
- 応答の形：`{header, ret_header（送った header の写し、seqnum は bit31:20）, error, payload[16]}`。id 126 は ACK の定義値だが、応答判定では必須にしない。id 100 は FW のエラー通知 `{header, code, payload[2]}` として先に処理し、それ以外は3 DWORD以上かつ ret_header の seqnum が一致すれば応答候補とする。成功には ret_header 全体の一致と error=0 を要求する。条件外のパケットは記録して読み飛ばす。

**5. メッセージの順番**（`a6xx_hfi_start` 1060 行。A635 で実際に送られるもの）：

| 順 | メッセージ | id | 大きさ | 中身 |
|---|---|---|---|---|
| 1 | PERF_TABLE | 4 | 59 dword | 下の 6 |
| 2 | BW_TABLE | 3 | 160 dword | 下の 7 |
| 3 | CORE_FW_START | 14 | 2 dword | handle = 0 |
| 4 | START | 10 | 1 dword | なし |

そのあと Linux は `GMU2HOST_INTR_CLR = ~0`、`GMU2HOST_INTR_MASK = ~CM3_FAULT`（bit23 だけ有効）にして、`a6xx_gmu_set_initial_freq` で `GX_BW_PERF_VOTE`（id 30）を送る。**これは GX の電源を入れる要求なので、最初の試験には入れない。**

**6. PERF_TABLE**（`a6xx_hfi_send_perf_table` 329 行、投票の作り方は `a6xx_gmu.c` 1772〜1917 行）：
- 形：`{header, num_gpu_levels, num_gmu_levels, gx_votes[16] = {vote, acd, freq}, cx_votes[4] = {vote, freq}}`。freq は kHz。acd はすべて 0xFFFFFFFF。0 番は「off」（freq 0、vote 0）。
- vote = `(primary のレベル値 << 16) | (secondary の番号 << 8) | primary の番号`。primary は OPP のレベル以上で最初の cmd-db のレベル、secondary はその primary の値以上で最初のレベル（なければ最大）。
- GX：primary は gfx.lvl、secondary は mx.lvl（gmxc.lvl がないため）。CX：primary は cx.lvl、secondary は mx.lvl。cmd-db の値は §9.4 の実測。
- kodiak.dtsi の OPP と、計算した vote：

| 対象 | 周波数 | OPP レベル | vote | opp-supported-hw |
|---|---|---|---|---|
| GX 0 | 0 | — | 0x00000000 | — |
| GX | 315 MHz | LOW_SVS（64） | 0x00400201 | 0x17 |
| GX | 450 MHz | SVS（128） | 0x00800302 | 0x17 |
| GX | 550 MHz | SVS_L1（192） | 0x00C00403 | 0x01 / 0x16 |
| GX | 608 MHz | SVS_L2（224） | 0x00E00504 | 0x16 |
| GX | 700 MHz | NOM（256） | 0x01000605 | 0x06 |
| GX | 812 MHz | NOM_L1（320） | 0x01400706 | 0x06 |
| GX | 840 MHz | TURBO（384） | 0x01800807 | 0x02 |
| GX | 900 MHz | TURBO_L1（416） | 0x01A00908 | 要確認 |
| CX 0 | 0 | — | 0x00000000 | — |
| CX | 200 MHz | MIN_SVS（48） | 0x00400202 | — |

- この電話の speedbin（ヒューズ）はまだ読んでいない [U]。315 MHz と 450 MHz は supported-hw が 0x17 で、カタログの speedbin（0、1、2、4 番）のどれでも有効なので、最初の試験は GX を 3 段（0、315、450）、CX を 2 段（0、200）にするのが安全 [I]。

**7. BW_TABLE**（`adreno_7c3_build_bw_table` 649 行。bcms がないので固定値）：
- 形：`{header, bw_level_num, cnoc_cmds_num, ddr_cmds_num, cnoc_wait_bitmask, ddr_wait_bitmask, cnoc_cmds_addrs[6], cnoc_cmds_data[2][6], ddr_cmds_addrs[8], ddr_cmds_data[16][8]}`。
- 値：`bw_level_num = 1`、`ddr_cmds_num = 3`、`ddr_wait_bitmask = 0x07`、ddr のアドレスは 0x50004、0x50000、0x50088、ddr_cmds_data[0][0..2] はすべて 0x40000000。`cnoc_cmds_num = 1`、`cnoc_wait_bitmask = 0x01`、cnoc のアドレスは 0x5006C、cnoc_cmds_data[0][0] = 0x40000000（off）、[1][0] = 0x60000001（on）。
- アドレスは cmd-db の BCM のアドレス。この電話の cmd-db と照合できる [U]。

**8. 進め方（案）**：
1. HFI の開始と PERF_TABLE、BW_TABLE だけを送る。応答（error、ret_header）と 3 つのキューの read/write index を記録する。そのあと今までどおり CM3 を reset に戻す。
2. 通ったら CORE_FW_START と START を足す。
3. GX の投票（`GX_BW_PERF_VOTE`）は、GX 側の準備（GX GDSC など）を調べてから。
- 注意 [U]：BW_TABLE には CNOC の「on」投票（0x60000001）が入っている。GMU が動いている間に RPMh へ投票し、CM3 の reset だけでは取り消されないかもしれない。Linux は止める前に `PREPARE_SLUMBER`（id 33）と OOB の手順を踏む。試験のあとは `rpmh_power_state` などを読んで確かめる。
- GMU を扱うコードはユーザーが書く。

### 9.11.1 HFI 開始・PERF_TABLE・BW_TABLE の実装（実機未実行）

`EpiGpuProbe=0x80` を拡張した。FW_INIT_RESULT の完了確認後に HFI を開始し、PERF_TABLE（id 4、seq 1、59 DWORD）と BW_TABLE（id 3、seq 2、160 DWORD）を1件ずつ送って ACK を待つ。`0x80` 単独ではここまで。CORE_FW_START と START は追加の bit9 を指定した `0x280` で送る（§9.11.2）。GBIF QSB と GX_BW_PERF_VOTE はさらに bit10 を指定した `0x680` のみ（§9.12.1）。PREPARE_SLUMBER は追加していない。キュー ABI の照合元は [本家 a6xx_hfi.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_hfi.h) と [a6xx_hfi.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_hfi.c)（事実のみ、コードの取り込みなし）。

- PERF_TABLE は GX が off / 315000 / 450000 kHz の3段、CX が off / 200000 kHz の2段。投票値は上記の実測 cmd-db に基づく固定値。BW_TABLE は上記7c3固定値を使う。両メッセージとも未使用の投票・アドレス・データは0、GX の ACD は0xFFFFFFFF。
- HFI_CTRL_INIT=1 の後、HFI_CTRL_STATUS bit0 を100 µs間隔で最大10 ms待つ。共有 RAM は volatile とメモリバリアで読み書きし、4 DWORDへのパディングを含む空きを確認してからコマンドを公開する。キュー位置・サイズ・index・メッセージ長を検証し、破損時は読み進めない。
- 各応答の待ち時間は100 µs間隔で最大1 s。MSGQ 割り込みをクリアし、応答を読む。id 100 以外で3 DWORD以上、packet[1] の seqnum が一致すれば応答とみなし、ret_header 全体が送信 header と一致し、error=0なら成功。id 126 の条件は設けない。読み飛ばしたパケットは件数・最後の長さ・先頭8 DWORDを記録する。FWエラー通知（id 100）は別途件数と最後の4 DWORDも記録して読み続ける。異常や時間切れでも既存のレジスタ・SMMU fault・ログの採取を行ってから CM3 を reset に戻す。
- BW_TABLE の write_index 公開後は、doorbell や応答待ちが失敗した場合も投票済みの可能性があると扱う。CM3 reset 後に最大10 ms待ち、CM3 reset の維持、RSCC SEQ_BUSY=0、RSCC_CONTROL_REQ/ACK=0、GX GDSC collapse / PWR_ON=0、GX HM OFF を確認する。rpmh_power_state は診断として記録し、0xFとの一致を条件にしない。上記条件を確認できなければ status 8 / KeepCx=1 とし、SMMU・RSCC・GMU設定・マップ・バッファ・CXを保持する。RSCC が停止していても CNOC投票が取り消された証明にはならない [U]。PREPARE_SLUMBER/OOB での停止は今回の対象外。

EpiDiag の追加診断（REG_BINARY は little endian）：

| 値 | 内容 |
|---|---|
| `GpuGmuHfiStatus`（REG_DWORD） | HFI 開始と指定された全メッセージの交換の NTSTATUS。0は全 ACK 成功、STATUS_NOT_FOUNDは未到達。`GpuGmuStatus=10` は HFI 試験失敗。teardown 失敗で status 8 になってもこの値は保持する |
| `GpuGmuHfiCtrlStatus`（REG_DWORD） | 開始待ちで最後に読んだ HFI_CTRL_STATUS。未読は0xFFFFFFFF |
| `GpuGmuHfiBwPublished`（REG_DWORD） | BW_TABLE の write_index を公開したら1 |
| `GpuGmuHfiIdleStatus`（REG_DWORD） | BW公開後、reset後の停止状態確認の NTSTATUS。0は上記条件を観測、STATUS_IO_TIMEOUTは未確認、STATUS_NOT_FOUNDは未試行 |
| `GpuGmuHfiMessages`（REG_BINARY） | PERF、BW、CORE_FW_START、START、GX_BW_PERF_VOTE、PREPARE_SLUMBERの順で各148バイト、合計888バイト。各37 DWORD：`{SentHeader, NtStatus, AckDwords, Ack[19], Skipped, FwErrors, LastFwError[4], LastSkippedDwords, LastSkipped[8]}`。応答は `{header, ret_header, error, payload[最大16]}`。Skipped はFWエラー通知も含む読み飛ばし件数。LastSkippedDwords は最後に読み飛ばしたパケットの全長（DWORD数）、LastSkipped は先頭最大8 DWORD（短い場合の残りは0）。未送信の NtStatus は STATUS_NOT_FOUND。AckDwords=0は未取得。サイズ不正時は申告された長さと先頭headerを残す場合があるため、NtStatus・AckDwordsを併読する |
| `GpuGmuHfiQueues`（REG_BINARY） | 9レコード×28バイト＝252バイト。各 `{Stage, CmdRead, CmdWrite, RspRead, RspWrite, DebugRead, DebugWrite}`。Stage 0=開始前、1=HFI ready、2=PERF交換終了、3=BW交換終了、4=CORE_FW_START交換終了、5=START交換終了、6=VOTE交換終了、7=slumber/BUSY/キュー確認後、8=CM3 reset直後。デバッグキューは消費しない |
| `GpuGmuHfiPower`（REG_BINARY） | 4レコード×24バイト＝96バイト。各 `{Stage, RpmhPowerState, GxGdscr, GxHm, HfiCtrlStatus, Gmu2HostIntrInfo}`。Stage 0=開始前、1=HFI試験終了、2=CM3 reset後（BW公開時は停止待ちの後）、3=GMU/RSCC設定復元後 |
| `GpuGmuHfiRscc`（REG_BINARY） | 4レコード×20バイト＝80バイト。各 `{Stage, Status0, SeqBusy, ControlReq, ControlAck}`。Stage は Power と同じ。Status0 は RSCC dword 0x004、SeqBusy は0x101、ControlReq/ControlAck は GMU 側0x23B07/0x23B08。未取得フィールドは0xFFFFFFFF |

キュー・電源の未取得フィールドは0xFFFFFFFF。FW起動に失敗して HFI 未到達なら HFI バイナリ診断は空。`GpuGmuStatus=0` は FW起動・指定された全メッセージの ACK・後始末の成功を意味する。`GpuGmuHfiStatus=0` だけでは後始末の成功を意味しないため、`GpuGmuStatus`、`GpuGmuKeepCx`、`GpuCxStatus` を併読する。`GpuCxText` には `gmu_hfi_status` も追記する。

ブレッドクラム：`0x700`=開始前のMSGQクリア、`0x701`=HFI_CTRL_INIT、`0x702`=ready待ち、`0x710/720`=PERF/BWキュー書き込み、`0x711/721`=doorbell、`0x712/722`=ACK待ち、`0x713/723`=応答割り込みクリア、`0x730`=reset後の停止確認。書き込み・doorbell・割り込みクリアの前に保存とflushを確認する。テスト用 shim には最小限のHFI応答を追加したが、今回テスト・ビルド・署名・配置・実機実行は行っていない。

**結果 1（2026-11-08、v34-akat、Epinephrine v38 / 20.0.2.38、`EpiGpuProbe=0x80`）：PERF_TABLE の ACK を読み飛ばして時間切れ** [P]。

- `HFI_CTRL_STATUS=1`。PERF_TABLE（header 0x00103B04）はファームウェアが読み（コマンドキュー r/w = 0x3C/0x3C）、4 dword の応答を書いた（応答キュー w=4）。
- 実装が応答の id に 126 を要求していたため、読み飛ばした（skipped=1）。1 秒で時間切れになり `GpuGmuStatus=10`。BW_TABLE は送っていない。
- GMU のログに応答の header 0x00010304 が残っていた：id は **元のコマンドの id（4）**、大きさ 3、種類（bit19:16）1。本家 Linux の `a6xx_hfi_wait_for_ack` は id を見ず、id 100 かどうかと ret_header の seqnum だけを見る。
- `GMU_RPMH_POWER_STATE`（0x1F8EC）は、ファームウェアが動き出すと 0 になり、CM3 を reset に戻したあとも 0 のまま。CX 窓を開いた直後は 0xF。slumber の手順を踏まないかぎり 0xF には戻らないと見られる [U]。そこでアイドル確認は、CM3 の reset、RSCC の `SEQ_BUSY`／`REQ`／`ACK` が 0、GX OFF で判断するように変えた。

**結果 2（v39 / 20.0.2.39、ACK の判定を Linux と同じにした版）：PERF_TABLE と BW_TABLE の両方に ACK（error 0）** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuStatus` / `GpuGmuHfiStatus` / `GpuGmuHfiIdleStatus` | 0 / 0 / 0 |
| `GpuGmuKeepCx` / `GpuCxStatus` | 0 / 0 |
| PERF_TABLE（送信 0x00103B04） | ACK {0x00010304, 0x00103B04, 0} |
| BW_TABLE（送信 0x0020A003） | ACK {0x00110303, 0x0020A003, 0} |
| コマンドキュー r/w | 0xDC / 0xDC（60 + 160 dword） |
| 応答キュー r/w | 0x8 / 0x8 |
| デバッグキュー r/w | 0 / 0 |
| RSCC `SEQ_BUSY` / `REQ` / `ACK` | 最初から最後まで 0 |
| GX GDSC | OFF のまま |
| SMMU `FSR` | 0x400（fault なし） |

- ファームウェアの ACK の header は `(ファームウェア側の seq << 20) | (1 << 16) | (3 << 8) | 元のコマンドの id`。seq は 0 から数える。2 dword 目は送った header の写し、3 dword 目は error。
- BW_TABLE を受け取っても、RSCC のシーケンサは動かなかった。
- 次は CORE_FW_START（id 14）と START（id 10）。GX の投票はまだ入れない。

### 9.11.2 CORE_FW_START / START の追加試験

`EpiGpuProbe` の bit9（0x200）を追加した。`0x280 = 0x80 | 0x200` のときだけ、BW_TABLE の応答成功後に CORE_FW_START、START を順に送る。`0x80` は PERF/BW までの従来動作を維持する。bit9 単独は `GpuProbeStatus=6` としてハードウェアアクセス前に拒否する。bit9 使用時はワンショット解除・flush の成功を確認してから進み、保存に失敗した場合は `GpuProbeStatus=7` で止める。

| メッセージ | id | seq | DWORD数 | 内容 | 送信header | ブレッドクラム |
|---|---|---|---|---|---|---|
| CORE_FW_START | 14 | 3 | 2 | header、handle=0 | 0x0030020E | 0x740=キュー書き込み、0x741=doorbell、0x742=応答待ち、0x743=割り込みクリア |
| START | 10 | 4 | 1 | headerのみ | 0x0040010A | 0x750=キュー書き込み、0x751=doorbell、0x752=応答待ち、0x753=割り込みクリア |

どちらも既存の `GmuHfiExchange()` を使い、4 DWORD境界まで0xFAFAFAFAで埋める。失敗した場合は後続を送らず、状態・ログを採取して既存の後始末へ進む。FW側の応答headerは予測値を条件にせず、従来どおり ret_header の一致と error=0で成功を判定する。

HFI 到達時の診断サイズは、現在は全モードとも Messages が888バイト、Queues が252バイト（§9.13.1 のSLUMBER追加で拡張）。指定されないメッセージは NtStatus=STATUS_NOT_FOUND、対応する Queue stage は未取得（0xFFFFFFFF）。CM3 reset 後のキューは stage 8に記録する。Power / Rscc の形式は維持し、stage 1は指定された試験全体の終了後（0x1E80はslumber後）、stage 2はreset・停止確認後となる。v40 の実測時点では592バイト・196バイトで、reset は stage 6だった。

実機ではコマンドキュー index が 0xDC→0xE0→0xE4 と進むか、応答・デバッグキューに自発メッセージが増えるか、GX GDSC と RPMH_POWER_STATE、GMUログの変化を確認する。START が GX の電源状態を変えるかは未確認 [U]。GX_BW_PERF_VOTE は送らず、後始末は従来どおり CM3 reset、RSCC/GXの停止確認、SMMU Disable、GMU/RSCC復元を使う。条件不成立なら status 8 / KeepCx=1となる。今回テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat、Epinephrine v40 / 20.0.2.40、`EpiGpuProbe=0x280`）：4 つのメッセージすべてに ACK（error 0）** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuStatus` / `GpuGmuHfiStatus` / `GpuGmuHfiIdleStatus` | 0 / 0 / 0 |
| `GpuGmuKeepCx` / `GpuCxStatus` / `GpuProbeStatus` | 0 / 0 / 0 |
| CORE_FW_START（送信 0x0030020E） | ACK {0x0021030E, 0x0030020E, 0} |
| START（送信 0x0040010A） | ACK {0x0031030A, 0x0040010A, 0} |
| コマンドキュー r/w | 0xDC → 0xE0 → 0xE4（どちらも 4 dword ずつ） |
| 応答キュー r/w | 0x8 → 0xC → 0x10（ACK の分だけ） |
| デバッグキュー r/w | 0 / 0 のまま |
| GX GDSCR / GX HM | 0x00222001 / 0xC0（OFF のまま、全 stage） |
| `RPMH_POWER_STATE` | 0（stage 0〜3） |
| RSCC `SEQ_BUSY` / `REQ` / `ACK` | 最初から最後まで 0 |
| SMMU `FSR` | 0x400（fault なし） |

- ACK の header は予想どおり `(ファームウェア側の seq << 20) | (1 << 16) | (3 << 8) | id`。自発的なメッセージは届かず、読み飛ばしも 0 件。
- GMU のログ（19.2 MHz とすると）：
  - CORE_FW_START を受けたあと（0x21, 2, 0x0030020E）、0x23 の記録が 5 件（番号 4、5、6、0x12、7、値はどれも 0xDEADBA55）と、新しい ID 0x30 の記録（番号 0、値 0x7476A2A1）が 1 件。そのあと ACK（0x21, 3, 0x0021030E）。受信から ACK まで約 0.7 ms。0x23 の続き番号は起動時の 1〜3、0x3A に続くもので、CORE_FW_START で残りの初期化が進んだと見られる [U]。0x30 の値の意味は不明 [U]。
  - START を受けたあと（0x21, 2, 0x0040010A）、ID 0x1E の記録（番号 0、値 0）が 1 件。そのあと ACK（0x21, 3, 0x0031030A）。
- START を受けても、ファームウェアは GX の電源を入れにいかず、RSCC のシーケンサも動かなかった。後片付けはすべて成功した。
- 次の候補：GX の投票（`GX_BW_PERF_VOTE`、id 30）。その前に、GX GDSC を入れるための準備（GBIF QSB の設定、GMU のクロック、止めるときの `PREPARE_SLUMBER`／OOB）を調べる。

### 9.12 GX を入れる前に必要なこと（本家 Linux、A635 = 7c3、非 legacy）

事実の出典は本家 Linux の `a6xx_gmu.c`、`a6xx_gpu.c`、`a6xx_hfi.c/.h`、`a6xx_gmu.xml`、`a6xx.xml`、`a6xx_catalog.c`、`gpucc-sc7280.c`、`gcc-sc7280.c`、`kodiak.dtsi`。行番号は手元の参照コピーのもの。コードは持ち込まない。

**1. GX の電源を入れるのは OOB の GPU_SET であり、GX_BW_PERF_VOTE ではない** [P]
- `a6xx_gmu_resume`（1322 行）は、HFI の開始のあと `set_initial_freq` で `GX_BW_PERF_VOTE` を送る。この時点では GX はまだ入っていない。
- GX が入るのは `hw_init`（a6xx_gpu.c 1179 行）の最初の `a6xx_gmu_set_oob(GMU_OOB_GPU_SET)`。「GPU の設定が終わるまで GMU に GPU を入れたままにさせる」という意味。最後に `clear_oob(GPU_SET)`（1533 行）で、GMU に電源管理を返す。
- `gxpd`（GX GDSC の genpd）は復旧のときにしか使わない（1282 行のコメント）。ふだん GX GDSC を操作するのは GMU のファームウェア。
- 非 legacy の OOB の GPU_SET（364〜468 行）：
  - `HOST2GMU_INTR_SET`（0x1F994）に bit30 を書く。
  - `GMU2HOST_INTR_INFO`（0x1F992）の bit31 を待つ（100 µs 間隔、最大 10 ms）。
  - `GMU2HOST_INTR_CLR`（0x1F991）に bit31 を書く。
  - 解除は `HOST2GMU_INTR_SET` に bit31。
  - 7c3 は非 legacy なので、BOOT_SLUMBER と DCVS の OOB（`set_new` がない）は使えない。

**2. GX_BW_PERF_VOTE（id 30）**（a6xx_hfi.c 1003 行）
- 形は 4 dword：`{header, ack_type = 1（応答を待つ）, freq = PERF_TABLE の GX の番号, bw = 0}`。
- 7c3 にはカタログの `bcms` がないので、bw はいつも 0。そのため Linux は、CPU 側でも `dev_pm_opp_set_opp` を呼ぶ（228〜237 行、1209 行）。これは interconnect `gfx-mem`（MASTER_GFX3D → EBI1）への帯域の投票で、315 MHz の OPP の値は peak 1804000 kBps。Windows で同じことをする方法はまだない [U]。今は、ほかのマスターが DDR を起こしているので通っていると見られる [I]。
- PERF_TABLE の GX の番号は 1 = 315 MHz、2 = 450 MHz（§9.11 の 6）。

**3. 7c3 のアイドル状態は ACTIVE（0）** [P]
- `idle_level` は、IFPC の quirk がなければ `GMU_IDLE_STATE_ACTIVE`（0）になる（2349 行）。7c3 には IFPC の quirk がない。
- `a6xx_gmu_wait_for_idle` は `RPMH_POWER_STATE == idle_level` を待つ。v38〜v40 で見えた `RPMH_POWER_STATE=0` は、この ACTIVE と同じ値で、異常ではない。
- `a6xx_gmu_power_config` は、IFPC／SPTP の設定をせずに `PWR_COL_INTER_FRAME_CTRL=0x9C40400` と `RPMH_CTRL` だけを書く。`SYS_BUS_CONFIG`／`ICACHE_CONFIG`／`DCACHE_CONFIG`（0x1F40F／0x1F400／0x1F401）にはどれも 1 を書く。

**4. 止め方（`a6xx_gmu_shutdown`、1427 行）** [P]
1. **GPU の初期化（`hw_init`）をしないまま止める場合は、先に OOB の GPU_SET と解除を 1 回行う。** Linux のコメントは「最後に起動してから OOB の GPU_SET をしないまま PREPARE_SLUMBER を送ると、ファームウェアの電源の状態が狂う」と言っている。今の試験はまさにこの状態にある。
2. `wait_for_idle`（`RPMH_POWER_STATE == 0`）。
3. `bus_halt`：GBIF を止める（GX 側のレジスタ）。
4. `notify_slumber`（577 行）：
   - `POWER_COUNTER_ENABLE`（0x1F840）= 0。
   - `PREPARE_SLUMBER`（id 33、3 dword `{header, bw = 0, freq = 0}`）を送る。
   - `AO_AHB_FENCE_CTRL`（0x23B10）= 0。
   - 7c3 では `CX_FALNEXT_INTF`（0x1F8F1）= 1（GEMNoC の対策。これがないと CX GDSC が落ちないことがある）。
5. `AO_GPU_CX_BUSY_STATUS`（0x23B0C）の bit23（GPUBUSYIGNAHB）が 0 になるのを待つ（100 µs 間隔、最大 10 ms）。
6. HFI を止める。割り込みのマスク（0x23B06、0x1F993）を全部立てる。`CM3_SYSRESET=1`。`rpmh_stop`。
- うまくいかないときは `force_off`（1139 行）に進む。`PWR_COL_KEEPALIVE=0`、SPTP を落とす、`rpmh_off`、`AHB_FENCE_STATUS_CLR=7`、`AO_AHB_FENCE_CTRL=0`、CM3 の reset、GPU のソフトリセット、`rpmh_stop` の順。
- `rpmh_off`（1099 行）：RSCC の `TCS0`〜`TCS3_DRV0_STATUS`（RSCC の dword 0x346、0x3EE、0x496、0x53E）の bit0（idle）を待つ。これは「RPMh への投票が残っていない」ことの確認なので、今のアイドル確認にそのまま足せる（読むだけ）。

**5. GBIF の QSB**（`a6xx_gmu_fw_start`、1044〜1046 行）[P]
- 7c3 のカタログは `gbif_cx = a640_gbif`。`GBIF_QSB_SIDE0`〜`SIDE3`（GPU の dword 0x3C03〜0x3C06、バイトで +0xF00C〜+0xF018）に、どれも 0x00071620 を書く。
- 書くのは `a6xx_gmu_start`（CM3 の解放）の直前。「CX の GBIF」なので、GX が OFF でも書けると見られる [I]。
- `GBIF_HALT`（0x3C45）と `RBBM_GBIF_HALT`（0x0016）を 0 に戻すのは、`hw_init` の中（OOB の GPU_SET のあと）。

**6. GMU のクロック**（`a6xx_gmu_resume` 1340 行、`gpucc-sc7280.c` 110〜143 行）[P]
- GMU は 200 MHz：`gpu_cc_gmu_clk_src`（GPU_CC +0x1120、parent map 0）。GCC の GPLL0 の DIV 出力（源の番号 6）を 1.5 で割る。RCG の CFG の値は `(6 << 8) | 2` = 0x602 [I]。
- hub は 150 MHz：`gpu_cc_hub_clk_src`（GPU_CC +0x117C、parent map 1）。同じ源を 2 で割る。CFG は `(6 << 8) | 3` = 0x603 [I]。
- RCG は、CFG を書いたあと CMD の bit0（UPDATE）を立て、0 に戻るのを待つ（RCG2 の一般の手順）[I]。
- 源の `gcc_gpu_gpll0_div_clk_src` は、GCC の投票レジスタ 0x52000 の bit16（`gcc_gpu_gpll0_clk_src` は bit15）。このレジスタはほかのクロックの投票と共有なので、書く前に今の値を読む。Windows の GCC ドライバとぶつからないかは不明 [U]。
- 今の実測（v40）：`gmu_rcg_cmd=0x80000000`（bit31 ROOT_OFF）、`gmu_rcg_cfg=0`（XO、19.2 MHz）、hub も CMD／CFG ともに 0。今は XO のまま、ファームウェアの起動と HFI が通っている。

**7. 進め方（案）**
1. **読むだけ**：次の実行で GCC の 0x52000、RSCC の TCS0〜3 の STATUS、`AO_GPU_CX_BUSY_STATUS`（0x23B0C）を記録する。
2. **GX は OFF のまま**：GBIF QSB の 4 つの書き込みと、`GX_BW_PERF_VOTE`（freq = 1、315 MHz）を足す。Linux も GX が OFF の状態で送っている。GX GDSC と RSCC の TCS が動くかを見る。
3. **GMU のクロックを 200／150 MHz にする**：1 の結果で GCC の bit16 がもう立っているかを確かめてから。
4. **GX を入れる**（OOB の GPU_SET）：止めるのは 4 の手順（OOB の解除 → idle → PREPARE_SLUMBER → BUSY 待ち → CM3 reset）。CM3 の reset だけで済ませると GX が入ったまま残るおそれがある [U]。今の「GX が OFF でなければ KeepCx=1」の安全策は残す。GBIF の halt（`bus_halt`）は GX 側のレジスタを触るので、手順を別に調べる。
- GMU を扱うコードはユーザーが書く。

### 9.12.1 GBIF QSB・GX_BW_PERF_VOTE と状態採取の実装

新しい bit10（0x400）を追加し、`EpiGpuProbe=0x680 = 0x80 | 0x200 | 0x400` で GBIF 設定と GX_BW_PERF_VOTE を実行する。bit10 に bit9 または bit7 が伴わなければ `GpuProbeStatus=6` でハードウェアアクセス前に拒否する。ワンショット解除・flush の成功を確認してから進む。`0x80` / `0x280` の書き込み・送信・後始末の手順は維持し、新規の GCC/GBIF 写像と追加の読み取りも `0x680` だけで行う。

- RSCC 写像は既存の0x2000バイトを維持し、TCS3（+0x14F8）まで含むことをコンパイル時にも確認する。GBIF は物理0x03D0F000の1ページをRWで、GCC vote は0x00152000の1ページを読み取り専用で追加する。GCC、GPUのクロック源、OOB、PDCには書かない。
- GMU setup のログアドレス（PWR_COL_CP_MSG）を設定した直後、残りの bus/cache/IFRAME/RPMH 設定へ進む前に GBIF を設定する。CM3 reset と GX OFF を再確認し、4つの元値をすべて読み、0xFFFFFFFFまたは0xDEADxxxxが1つでもあればGBIFには書かず中止する。正常なら GPU dword 0x3C03〜0x3C06へ各0x00071620を書き、即時読み戻しを完全一致で比較する。失敗時はCM3を解放しない。GMU setup の19レコード・380バイトは変更せず、GBIFには別の診断表を使う。
- START の応答成功後、id=30、seq=5、4 DWORDの `{header, ack_type=1, freq=1, bw=0}` を既存の GmuHfiExchange() で送る。header は0x0050041E、追加のパディングは不要。VOTE直前と交換終了後の状態を記録する。失敗・時間切れは HFI status 10（後始末にも失敗すれば最終 status 8）。応答の判定は従来どおり ret_header の一致と error=0。
- 後始末の GX OFF と KeepCx 判定は維持する。CM3 reset、RSCC/GXの停止確認、SMMU Disable、GMU設定の復元後、書き込み済みGBIFを逆順に復元・読み戻しし、その後RSCCを復元する。GBIF復元は途中で失敗しても残りを試みる。失敗なら既存のteardown失敗と同じくCX・マップ・メモリを保持する。GXがOFFに戻らず既存のidle確認に失敗した場合は、GBIFも復元せず設定を保持する。
- TCSのbit0とAO CX busyのbit23は記録のみとし、今回idle条件には追加しない。TCSがbusyのままなら記録をもとに停止手順の追加を検討する。PREPARE_SLUMBER、OOB GPU_SET、GMUクロック変更は今回の対象外。

追加診断（EpiDiag、REG_BINARYはlittle endian）：

| 値 | 内容 |
|---|---|
| `GpuGmuGbifSetup`（REG_BINARY） | 4レコード×20バイト＝80バイト。各 `{Reg, Original, Value, Readback, Restored}`。RegはGPUベースからのdword（0x3C03〜0x3C06）、Valueは0x00071620。未読の元値・読み戻し・復元値は0xFFFFFFFF。GBIF試験未到達なら空 |
| `GpuGmuGbifStatus` / `GpuGmuGbifRestoreStatus`（REG_DWORD） | 設定・復元のNTSTATUS。未試行はSTATUS_NOT_FOUND。`GpuGmuStatus=11` はGBIFの前提・元値検査・書き込み・読み戻し・ブレッドクラム失敗、復元失敗は8 |
| `GpuGmuGbifBadIndex` / `GpuGmuGbifRestoreBadIndex`（REG_DWORD） | 設定・復元で最初に失敗した0始まりの項目番号、該当なしは0xFFFFFFFF。CM3/GXの前提不成立では項目番号なし |
| `GpuGmuHfiExtra`（REG_BINARY） | 5レコード×48バイト＝240バイト。各 `{Stage, GccVote, Tcs0, Tcs1, Tcs2, Tcs3, CxBusy, CxBusy2, Gbif0, Gbif1, Gbif2, Gbif3}`。Stage 0=HFI開始前（GBIF設定後）、1=HFI終了後（VOTE交換後）、2=reset/停止確認後、3=設定復元後、4=VOTE直前。未取得は0xFFFFFFFF、0x80/0x280またはHFI未到達なら空 |
| `GpuGmuHfiExtraStatus`（REG_DWORD） | 追加読み取り前のブレッドクラム保存・flushの最初の失敗。成功は0、未到達はSTATUS_NOT_FOUND。開始前またはVOTE直前の保存失敗では送信を進めず、後始末中の失敗は復元を妨げない |

ExtraのGccVoteは物理0x00152000（bit15/16を観察）、TCSはRSCC +0xD18/+0xFB8/+0x1258/+0x14F8（bit0を観察）、CxBusy/2はGPU dword 0x23B0C/0x23B0D（bit23を観察）、Gbifはdword 0x3C03〜0x3C06。Power/Rsccの4段階は従来どおりで、VOTE前後のTCSはExtra stage 4と1で比較できる。

HFI診断は全モードで Messages が5件・740バイト、Queues が8段・224バイト。VOTEは Messages[4]、Queue stage 6、reset後はstage 7。指定されないメッセージはSTATUS_NOT_FOUND、キューstageは0xFFFFFFFF。ブレッドクラムは GBIF元値読み取り0x6A0+i、設定0x6C0+i、復元0x6D0+i、VOTEは0x760〜0x763。Extraの各読み取りは `0x770 + Stage*0x10 + i`（i=0がGCC、1〜4がTCS、5/6がCX busy、7〜10がGBIF）で保存・flushを確認する。今回テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat、Epinephrine v41 / 20.0.2.41、`EpiGpuProbe=0x680`）：GBIF の設定と GX_BW_PERF_VOTE の両方が成功** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuStatus` / `GpuGmuHfiStatus` / `GpuGmuHfiIdleStatus` | 0 / 0 / 0 |
| `GpuGmuKeepCx` / `GpuCxStatus` / `GpuProbeStatus` | 0 / 0 / 0 |
| GBIF QSB SIDE0〜3 | 元の値 0x00070620 → 書いた値 0x00071620（そのまま読めた）→ 戻した値 0x00070620。違いは bit12 だけ |
| GX_BW_PERF_VOTE（送信 0x0050041E） | ACK {0x0041031E, 0x0050041E, 0} |
| コマンドキュー r/w | 0xE4 → 0xE8 |
| 応答キュー r/w | 0x10 → 0x14 |
| デバッグキュー r/w | 0 / 0 |
| GCC 0x52000（投票） | 0x00E0057F（全段階で同じ）。**bit15 も bit16 も 0** |
| RSCC TCS0〜3 STATUS | すべての段階で 1（idle）。VOTE の後も RPMh への投票は残っていない |
| `AO_GPU_CX_BUSY_STATUS` / `STATUS2` | 0x30000000 / 0x12（全段階で同じ）。bit23（GPUBUSYIGNAHB）は 0 |
| GX GDSCR / GX HM | 0x00222001 / 0xC0（OFF のまま） |
| RSCC `SEQ_BUSY` / `REQ` / `ACK` | 最初から最後まで 0 |
| SMMU `FSR` | 0x400（fault なし） |

- GBIF QSB は、GX が OFF でも読み書きできた。CX 側のレジスタであることが確かめられた。
- GMU のログ：VOTE を受けてから ACK を返すまで約 0.9 ms（19.2 MHz とする）。その間に約 50 件の記録がある。新しい ID は 0x05、0x24、0x25、0x26、0x11。0x23 の番号には 0x108、0x15C、0x15D、0x20D、0xFF00F109 などがあり、値はすべて 0xDEADBA55。0x24 の値は (0xFF000005, 0xFFFF0000) と (0xFF000001, 0xFFFFFFFF)。中身の意味は不明 [U]。ファームウェアは投票を内部で処理したが、TCS も GX も動いていないので、RPMh には何も送っていないと見られる [I]。GX が OFF の間は投票を覚えておくだけ、という Linux の使い方と合う [I]。
- GCC の bit16（`gcc_gpu_gpll0_div_clk_src`）が立っていないので、GMU のクロックを 200 MHz にするには、先にこの投票を立てる必要がある。

### 9.12.2 GMU 200 MHz / hub 150 MHz の追加試験

bit11（0x800）を追加し、`EpiGpuProbe=0xE80 = 0x80 | 0x200 | 0x400 | 0x800` のときだけ、GCCのGPLL0_DIV投票とGMU/hubのRCG切り替えを実行する。bit11にbit7/9/10が伴わない指定は、ハードウェアアクセス前に `GpuProbeStatus=6` で拒否する。ワンショット解除・flushの失敗は7。`0x80` / `0x280` / `0x680` の手順・HFI診断形式は変更しない。

値の出典は本家 `gpucc-sc7280.c`、`gcc-sc7280.c`、`a6xx_gmu.c` のレジスタ・周波数設定の事実のみ。コードは取り込んでいない。GMU/hubの源はどちらもsource 6（GCC GPLL0_DIV）、CFGはGMUが0x602（分周1.5）、hubが0x603（分周2）。MND modeは0。源が300 MHzなら、それぞれ200 MHz / 150 MHzとなる [I]。PLL自体、GCC bit15、HFIメッセージには変更を加えない。

- 既存のRSCC・GMU設定とTCMコピーの後、SMMU Enableの直前に設定する。CM3 reset、GX GDSCとGX HMのOFF、cx_gmuブランチのCLK_OFF=0を再確認する。両RCGのCMD/CFGを保存し、試験の前提としてCFG=0（XO）かつUPDATE=0を要求する。前提不成立ならGCCにもRCGにも書かない。
- GCC vote（物理0x00152000）の写像は、このモードだけRWとする。ブレッドクラムのflush後、書く直前に共有レジスタを読み直し、bit16が0ならそのビットだけを立てて1回書く。直後に読み戻し、bit16が立ったことと、他ビットの変化を記録して10 µs待つ。元からbit16が1なら書かず、解除対象にも含めない。
- GMU、hubの順にCFGを書き、各CMDへ現在値にUPDATE bit0を足して書く。UPDATE直後から10 µs間隔で最大500 µs待ち、CMDのROOT_OFF=0とCFGの完全一致を確認する。UPDATEとpollの間にはレジストリflushを挟まず、待ち回数を記録する。最後に両RCGと子ブランチを再読する。成功した場合だけSMMU Enable、CM3解放、従来のHFIへ進む。
- 後始末では、CM3 resetと既存のidle確認、SMMU Disableの成功後に、hub→GMUの順で元CFG=0へ戻す。途中で書き込みが止まったshadow CFGも復元対象に含める。設定時のUPDATEが残っている場合は先に最大500 µs待ち、終わらなければCFGを書き換えずKeepCxに進む。両RCGの復元を試みた後、XOとUPDATE完了を新しい読み取りで確認できた場合だけ、GCC bit16を解除する。
- GCC解除でも書く直前にレジスタを読み直し、自分が立てたbit16だけをクリアする。既存の他ビットや、元から有効だったbit16は戻さない。解除直後の読み戻しと他ビットの差分も記録する。復元失敗時は追加した源の投票を残し、既存のstatus 8 / KeepCx=1でCX・マップ・メモリを保持する。CM3 resetやSMMU Disableで先に失敗した場合もクロック復元を試みず保持する。

共有GCCレジスタは外部ドライバとの原子的な更新を保証できない。他ビットのXORは読み戻しで観測した変化であり、競合や更新消失がなかった証明にはならない [U]。全体の元値を書き戻す処理やリトライは入れない。

追加診断（EpiDiag、REG_BINARYはlittle endian）：

| 値 | 内容 |
|---|---|
| `GpuGmuClockStatus` / `GpuGmuClockRestoreStatus`（REG_DWORD） | 設定・復元のNTSTATUS。未試行はSTATUS_NOT_FOUND。`GpuGmuStatus=12` はクロック設定失敗（CM3未解放）。復元にも失敗した場合は最終status 8。HFI側のstatusは従来どおり |
| `GpuGmuClockGcc`（REG_BINARY） | 9 DWORD＝36バイト。`{Before, AfterEnable, BeforeDisable, AfterDisable, EnableOtherXor, DisableOtherXor, AddedByUs, EnableWritten, DisableWritten}`。XORはbit16を除いた他ビットの差。最後の3値は0/1（AddedByUsは追加した履歴で、解除成功後も1）。未読の前半6値は0xFFFFFFFF。元からbit16が立っていればEnableWritten=0、AddedByUs=0、DisableWritten=0 |
| `GpuGmuClockRcgs`（REG_BINARY） | GMU、hubの順に各48バイト、合計96バイト。各12 DWORD：`{CmdOffset, CfgOffset, OriginalCmd, OriginalCfg, TargetCfg, SetCmd, SetCfg, SetPolls, RestoreCmd, RestoreCfg, RestorePolls, PendingPolls}`。offsetはGPU_CCからのバイト。SetPolls/RestorePollsはUPDATE後の10 µs待ち回数（0〜50）、PendingPollsは復元前に残っていたUPDATEの待ち回数（0〜50）。未取得は0xFFFFFFFF。タイムアウト時も最後のCMD/CFGを保持する |
| `GpuGmuClockBranches`（REG_BINARY） | 3行×3 DWORD＝36バイト。行は設定前、設定後、復元後。各行は `{cx_gmu (+0x1098), hub_cx_int (+0x1204), hub_aon (+0x1178)}` の読み戻し（CLK_OFFはbit31）。未読は0xFFFFFFFF |

クロック未到達ならバイナリ診断は空。既存の `GpuGmuHfiExtra` は0xE80でも採取し、stage 0/1/2/4のGCC voteでbit16の有効化、stage 3で復元後の値を確認できる。Messagesは740バイト、Queuesは224バイトのまま。GMUのログ採取も従来どおりで、AOタイムスタンプからCORE_FW_START/VOTEの受信→応答時間をv40/v41と比較する。速くなったことは実機で確認するまで未確定 [U]。

ブレッドクラム：0x6E0=前提と元RCGの保存、0x6E1=GCC投票、0x6E2/6E3/6E4=GMUのCFG/UPDATE/結果検証、0x6E5/6E6/6E7=hubの同処理、0x6E8=最終確認。復元は0x6F0/6F1/6F2がhub、0x6F3/6F4/6F5がGMU、0x6F6がXO確認とGCCの解除。UPDATE待ち中は直前のUPDATE番号を保持し、処理が戻れば後片付けの番号へ更新する。0x6FFは既存のKeepCx終了記録に予約済み。今回テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat、Epinephrine v42 / 20.0.2.42、`EpiGpuProbe=0xE80`）：GMU 200 MHz／hub 150 MHz で全段階が成功** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuStatus` / `GpuGmuClockStatus` / `GpuGmuClockRestoreStatus` | 0 / 0 / 0 |
| `GpuGmuHfiStatus` / `GpuGmuHfiIdleStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | 0 / 0 / 0 / 0 |
| GCC 0x52000 | 0x00E0057F → bit16 を立てて 0x00E1057F → 戻して 0x00E0057F。ほかのビットの変化（XOR）は前後とも 0 |
| GMU の RCG | CFG 0 → 0x602（UPDATE はすぐに 0、待ち 0 回）→ 0（待ち 1 回）。CMD は 0 のまま |
| hub の RCG | CFG 0 → 0x603（待ち 0 回）→ 0（待ち 0 回） |
| ブランチ（cx_gmu／hub_cx_int／hub_aon） | 0x6221 / 1 / 1 のまま（すべて ON） |
| HFI の 5 つのメッセージ | すべて ACK、error 0（v41 と同じ値） |
| GX / RSCC / TCS0〜3 / SMMU | OFF / idle / 1 / fault なし |

処理時間（GMU のログのタイムスタンプ。AO カウンタ 19.2 MHz とする）：

| 区間 | v41（XO） | v42（200 MHz） | 比 |
|---|---|---|---|
| 最初の記録から初期化の結果（0x21, 5, 0x100）まで | 122120 tick（約 6.4 ms） | 22137 tick（約 1.15 ms） | 約 5.5 倍 |
| PERF_TABLE の受信から ACK まで | 1200 tick | 200 tick | 6 倍 |
| CORE_FW_START の受信から ACK まで | 13911 tick（約 0.72 ms） | 1913 tick（約 0.10 ms） | 約 7.3 倍 |
| GX_BW_PERF_VOTE の受信から ACK まで | 17578 tick（約 0.92 ms） | 1721 tick（約 0.09 ms） | 約 10 倍 |

- GMU のクロックは 200 MHz で動いていると見られる [I]。XO との比は 10.4 倍。初期化の区間は待ち時間を含むので、比が小さい。
- VOTE の処理中のログの記録は、v41 の約 50 件から約 25 件に減った。v41 では同じ並び（0x23 の 0x0A／0x15C／0x10B、0x25、0x26 など）が 2 回出ていた。何かを待つ繰り返しの回数が減ったと見られる [U]。
- GCC の共有レジスタへの書き込みで、ほかのビットは変わらなかった。Windows の GCC ドライバとぶつかった様子もない。
- これで §9.12 の 7 の 1〜3 が終わった。残りは 4（OOB の GPU_SET で GX を入れ、slumber で止める）。

### 9.13 GX を初めて入れる手順と止め方（本家 Linux、A635 = 7c3、非 legacy）

事実の出典は本家 Linux の `a6xx_gmu.c`、`a6xx_gpu.c`、`a6xx_hfi.c/.h`、`a6xx_gmu.xml`、`a6xx.xml`、`gpucc-sc7280.c`、`kodiak.dtsi`。行番号は手元の参照コピーのもの。コードは持ち込まない。

**1. GX を入れるのは GMU のファームウェア** [P]
- `gpucc-sc7280.c` の `gx_gdsc`（GDSCR 0x100C、CLAMP_IO 0x1508）の `power_on` は `gdsc_gx_do_nothing_enable`。CPU 側は GX GDSC を操作しない。GMU の DT ノードは `power-domains = cx, gx` だが、`gx` は復旧のときにしか使わない（§9.12 の 1）。
- GX の電源レール（RPMh の gfx.lvl）への投票も GMU が RSCC の TCS を通して行う。Linux が CPU から GX のレールに投票することはない。
- **未確認の前提** [U]：Linux は `a6xx_gmu_rpmh_init` で GPU PDC に `PDC_GPU_SEQ_START_ADDR=0`、`PDC_GPU_ENABLE_PDC=0x80000001` を書く（§9.10）。この電話では PDC を読むだけでリセットするので書けない。PDC が有効になっていなければ、TCS の投票が RPMh に届かず、GX のレールが上がらないおそれがある。今の電話の PDC の状態はわからない。OOB の ACK が来ない、または GMU のログにエラーが出るなら、これが第一の候補。

**2. GX が入ったかどうかの見分け方** [P]
- `a6xx_gmu_gx_is_on`（95 行）：`GMU_SPTPRAC_PWR_CLK_STATUS`（0x1F8D0）の bit6（GX_HM_GDSC_POWER_OFF）と bit7（GX_HM_CLK_OFF）がどちらも 0 なら ON。今の安全策（`0x1F8D0 & 0xC0 == 0xC0` を OFF とする）と同じビット。
- あわせて GPU_CC の GX GDSCR（0x100C）の bit31（PWR_ON）も見る。v40〜v42 では 0x00222001（OFF）。

**3. OOB の GPU_SET（非 legacy）**（364〜492 行）[P]
- 要求：`HOST2GMU_INTR_SET`（0x1F994）に bit30 を書く。
- 応答：`GMU2HOST_INTR_INFO`（0x1F992）の bit31 を待つ（100 µs 間隔、最大 10 ms）。そのあと `GMU2HOST_INTR_CLR`（0x1F991）に bit31 を書く。時間切れでも CLR は書く。
- 解除：`HOST2GMU_INTR_SET` に bit31 を書く。解除には応答を待たない。
- Linux はこの間（`hw_init`）に GX 側のレジスタ（RBBM、CP など）を設定する。**最初の試験では GX 側のレジスタには触らず、入ったことを確かめるだけにする**のが安全 [I]。
- 7c3 のアイドル状態は ACTIVE（§9.12 の 3）なので、OOB を解除しても GMU は自分から GX を落とさない [I]。落とすのは PREPARE_SLUMBER。

**4. 止め方（`a6xx_gmu_shutdown`、1427 行。OOB を済ませた場合）** [P]
1. `wait_for_idle`：`RPMH_POWER_STATE`（0x1F8EC）が 0（ACTIVE）になるまで待つ。
2. `bus_halt`（a6xx_gpu.c 2083 行、`gx_off = false`）：
   - `GBIF_HALT`（GPU の dword 0x3C45）= 1（クライアントの停止）→ `GBIF_HALT_ACK`（0x3C46）の bit0 を待つ。
   - `GBIF_HALT` = 2（AXI の停止）→ `GBIF_HALT_ACK` の bit1 を待つ。
   - `GBIF_HALT` = 0（明示的に戻す必要がある）。
   - Linux の `spin_until` は時間の上限が長い。こちらでは上限を決めて待つ [I]。
   - `RBBM_GBIF_HALT`（0x0016／ACK 0x0017）は GX 側のレジスタで、GPU が固まったとき（`gx_off = true`）にしか使わない。
3. `notify_slumber`（577 行）：
   - `POWER_COUNTER_ENABLE`（0x1F840）= 0。
   - SPTP の無効化は legacy だけ（`a6xx_sptprac_disable` は非 legacy ではすぐ戻る、525 行）。
   - `PREPARE_SLUMBER`（id 33）：3 dword `{header, bw = 0, freq = 0}`。今の順番なら seq 6、header は `(6 << 20) | (3 << 8) | 33` = 0x00600321。
   - `AO_AHB_FENCE_CTRL`（0x23B10）= 0。
   - 7c3 は `CX_FALNEXT_INTF`（0x1F8F1）= 1（GEMNoC の対策）。
4. `AO_GPU_CX_BUSY_STATUS`（0x23B0C）の bit23 が 0 になるのを待つ（100 µs 間隔、最大 10 ms）。失敗してもエラーを出すだけで先に進む。
5. HFI を止める（キューが空かを確かめるだけ）。割り込みのマスク（0x23B06、0x1F993）を全部立てる。
6. `CM3_SYSRESET` = 1。
7. `rpmh_stop`（648 行）：`RSCC_CONTROL_REQ`（0x23B07）= 1 → RSCC の `RSC_STATUS0_DRV0`（dword 0x004）の bit16 を待つ（100 µs 間隔、最大 10 ms）→ REQ = 0。RSCC だけで、PDC には触らない。今の後片付けにはまだない。

**5. うまくいかないとき（`a6xx_gmu_force_off`、1139 行）** [P]
- 順番：`PWR_COL_KEEPALIVE`（0x1F8C3）= 0 → HFI を止める → 割り込みを止める → SPTP（非 legacy では何もしない）→ `CX_FALNEXT_INTF` = 1 → `rpmh_off`（TCS0〜3 の STATUS の bit0 を待つ）→ `AHB_FENCE_STATUS_CLR`（0x23B14）= 7、`AO_AHB_FENCE_CTRL` = 0 → `CM3_SYSRESET` = 1 → `bus_halt(gx_off = true)` → GPU のソフトリセット（`RBBM_SW_RESET_CMD` 0x0043 に 1、100 µs 以上待つ）→ `rpmh_stop`。
- `bus_halt(true)` とソフトリセットは GX 側のレジスタに触る。GX の状態がわからないときに触ると、バスが固まるおそれがある [U]。最初の試験では、GX が OFF に戻らなかったら force_off をせず、今の KeepCx（CX とすべてを残す）で止めるのが安全 [I]。電話を再起動すれば元に戻る。

**6. 試験の組み立て（案）**
- `0xE80` に新しいビット（例：bit12 = 0x1000、合計 `0x1E80`）を足す。VOTE の ACK のあとに次の順で行う。
  1. 記録：GX GDSCR、0x1F8D0、`RPMH_POWER_STATE`、TCS0〜3、RSCC `SEQ_BUSY`、GPU_CC の PLL0 の MODE（0x000、読むだけ）。
  2. OOB の GPU_SET を送り、ACK を待つ。待ちの回数を記録する。
  3. 記録（同じ項目）。GX が ON になったか、TCS が動いたか、PLL0 が動いたか。
  4. OOB を解除する。
  5. 4 の止め方：idle 待ち → GBIF の halt → PREPARE_SLUMBER → BUSY 待ち → HFI を止める → CM3 の reset → `rpmh_stop`。
  6. 記録。そのあと今のアイドル確認（GX OFF、RSCC idle）。条件が合わなければ KeepCx。
- OOB の ACK が来なかった場合も、解除を書き、PREPARE_SLUMBER を試してから CM3 の reset に進む。GX が OFF のままなら、今の後片付けで終われる。
- GMU のログ（0x1000 バイト）は記録が増えるので、足りなくなるかもしれない。2 ページ目まで読むとよい [I]。
- GMU を扱うコードはユーザーが書く。

### 9.13.1 OOB GPU_SET・slumber・rpmh_stop 試験の実装と結果

bit12 `EPI_GMU_PROBE_GX_ON=0x1000` を追加した。`EpiGpuProbe=0x1E80`（bit7/9/10/11/12）のみ、HFI 5通の成功後に OOB GPU_SET と解除、PREPARE_SLUMBER を実行し、CM3 reset 後に rpmh_stop を行う。bit12に前提の4ビットが伴わない指定はハードウェアアクセス前に `GpuProbeStatus=6` で拒否する。bit12もワンショット解除とflush確認の対象。既存モードはOOB/slumber/rpmh_stopを実行しない。

- OOB要求前に古いbit31のACKを消す。HOST2GMU_INTR_SETへbit30を書いたらAttempted=1とし、INFO bit31を100 µs×100まで待つ。ACKの有無にかかわらずCLR bit31と解除（HOST2GMU_INTR_SET bit31）を必ず書く。
- OOBを送った場合は、結果にかかわらずslumber手順を試す。ACTIVE待ち→GBIF HALT=1 / ACK bit0待ち→HALT=2 / ACK bit1待ち→HALT=0→POWER_COUNTER_ENABLE=0→PREPARE_SLUMBER→AO_AHB_FENCE_CTRL=0 / CX_FALNEXT_INTF=1→CX_BUSY bit23の解除待ち→コマンド/応答キューの空確認。HALT各段は10 µs×1000、ACTIVE/BUSYは100 µs×100まで。HALTとBUSYの時間切れでも後続へ進む。HALT=0は必ず書く。
- PREPARE_SLUMBER は既存GmuHfiExchangeで id=33、seq=6、3 DWORD `{header,0,0}`、header=0x00600321。最初のHFI 5通が失敗した場合はOOBもslumberも実行しない。slumberの失敗時も残りの停止処理とログ採取を続ける。
- CM3 resetの成功確認後にREQ=1、RSCC STATUS0 bit16を100 µs×100まで待ち、成否にかかわらずREQ=0へ戻す。rpmh_stopの失敗は記録し、最終判断は既存のidle確認（CM3 reset、GX OFF、RSCC SEQ_BUSY/REQ/ACK=0）に任せる。この条件は変更しない。
- POWER_COUNTER_ENABLEの元値を保存し、0以外ならGMU setupを復元する箇所で書き戻して比較する。元値0なら追加の復元書き込みはしない。復元失敗はstatus 8 / KeepCx。ログ領域は既存の0x4000バイトで足りるため確保量は変えず、全モードの `GpuGmuLog` 採取量を0x2000へ拡張した。0x1E80ではslumber後・reset前にログを取る。
- OOB要求後のACKクリア・解除、HALT=0、REQ=0はブレッドクラム保存が失敗しても実行する。最初の保存失敗をBreadcrumbStatusに記録する。OOB/slumber中の保存失敗はstatus 13の対象に含める。reset後のrpmh_stopの成否は記録のみとし、idle確認の扱いを維持する。

`GpuGmuGx`（REG_BINARY、little endian、340バイト）：先頭25 DWORDと、続く `Snap[5][12]`（60 DWORD、240バイト）。0x1E80でcleanupとkeepの両経路から保存する。OOB未到達でも要求モードの記録を残し、他モードでは空データ。

先頭25 DWORDの順序は次のとおり：

`{Attempted, OobStatus, OobPolls, OobInfo, HaltStatus, HaltPolls[2], HaltAck[2], SlumberStatus, BusyStatus, BusyPolls, PowerCounterOriginal, QueueEmpty[2], RpmhStopStatus, RpmhStopPolls, IdleStatus, IdlePolls, IdleLast, BusyLast, RpmhStopLast, BreadcrumbStatus, PowerCounterSaved, PowerCounterRestoreStatus}`。

StatusはNTSTATUSで、未試行はSTATUS_NOT_FOUND（BreadcrumbStatusは保存失敗なしなら0）。未読値・未試行のPolls・QueueEmptyは0xFFFFFFFF。Attempted/PowerCounterSavedは0/1、QueueEmptyは取得済みなら0/1。IdleStatus/Polls/Lastはslumber前のACTIVE待ち、BusyLastとRpmhStopLastは各待ちの最後の値。Pollsは待ち回数で、OOB/ACTIVE/BUSY/rpmh_stopは100 µs単位、HALTだけ10 µs単位。PowerCounterRestoreStatusは元値0または未保存なら未試行のまま。

Snapの各行は `{Stage, GxGdscr, GxHm, RpmhPower, Tcs0, Tcs1, Tcs2, Tcs3, SeqBusy, Status0, CxBusy, Pll0Mode}`。Stage 0=OOB前、1=ACK待ち終了・解除前、2=解除/slumber後・CM3 reset前、3=reset/rpmh_stop後、4=最終idle確認後（KeepCxへ進む場合も採取）。段階ごとに既存のブレッドクラムを1回使い、読み取り項目ごとの更新はしない。

HFIの現在の診断は全モードで Messages が6件×148＝888バイト、Queues が9段×28＝252バイト。Messages[5]がSLUMBER、Queue stage 7がslumber/BUSY/キュー確認後、stage 8がreset後。未指定メッセージのStatusはSTATUS_NOT_FOUND、未到達キューは0xFFFFFFFF。Power/Rscc/Extraの形は維持し、0x1E80のstage 1はOOB/slumberを済ませた後になる。v41/v42の740バイト・224バイト（reset stage 7）は過去の形式。

| ブレッドクラム | 処理 |
|---|---|
| 0x7C0 | 古いACK bit31をクリア、Snap 0 |
| 0x7C1 / 0x7C2 | OOB bit30要求 / ACK待ち（100 µs×100） |
| 0x7C3 / 0x7C4 | ACK bit31をクリア、Snap 1 / bit31でOOB解除 |
| 0x7D0 | ACTIVE待ち（100 µs×100）、記録のみ |
| 0x7D1 / 0x7D2 | HALT=1 / ACK bit0待ち（10 µs×1000） |
| 0x7D3 / 0x7D4 | HALT=2とACK bit1待ち / HALT=0（必須） |
| 0x7D5 | POWER_COUNTER_ENABLEの保存と0設定 |
| 0x7E0〜0x7E3 | PREPARE_SLUMBERのキュー公開、doorbell、応答待ち、割り込みクリア |
| 0x7F0 / 0x7F1 | FENCE_CTRL=0・FALNEXT=1 / CX_BUSY bit23待ち（100 µs×100） |
| 0x7F2 | キュー空確認、Queue stage 7、Snap 2 |
| 0x7F4 / 0x7F5 / 0x7F6 | reset後にREQ=1 / STATUS0 bit16待ち（100 µs×100） / REQ=0（必須）、Snap 3 |
| 0x7F3 | 既存のidle確認後にSnap 4 |
| 0x7F8 | 元のPOWER_COUNTER_ENABLEが0以外の場合のみ復元 |

待ちループ内ではブレッドクラムを書かない。設定・待ち開始・後続処理への移行時にだけ更新する。

結果コード13（EPI_GMU_E_GX）はOOB/slumberに失敗したがGX OFFに戻って通常の後片付けを完了した場合。成功は0。HALT・ACTIVE・BUSYの時間切れとrpmh_stopの失敗だけでは13にせず、個別診断で確認する。どの経路でもGXがOFFに戻らない、idle確認に失敗、または後片付けに失敗した場合は8 / KeepCx=1で全資源を保持する。force_off、RBBM/CP、RBBM_GBIF_HALT、GPU PDCには触らない。

shimにはOOB ACK、GBIF HALT ACK、RSCC sleep完了の応答と、OOB/slumberでのGX ON/OFFの模擬状態を追加した。今回テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat、Epinephrine v43 / 20.0.2.43、`EpiGpuProbe=0x1E80`）：GX の電源が初めて入り、slumber で切れた。後片付けは RSCC の SEQ_BUSY のため KeepCx** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | 8 / 1 / 16 |
| `GpuGmuHfiStatus` / `GpuGmuHfiIdleStatus` | 0 / 0xC00000B5（時間切れ） |
| OOB | Status 0、Polls 0、INFO 0x80000000（ACK あり） |
| GBIF HALT | ACK 0x3 / 0x3（どちらも 0 回目で成立） |
| PREPARE_SLUMBER（送信 0x00600321） | ACK {0x00510321, 0x00600321, 0} |
| CX_BUSY bit23 待ち | 0 回目で成立（0x30000000） |
| キューの空確認 | コマンド・応答とも空。コマンド r/w = 0xEC、応答 r/w = 0x18 |
| rpmh_stop | Status 0、Polls 0、STATUS0 = 0x01010006 |
| POWER_COUNTER_ENABLE の元値 | 0（復元なし） |
| SMMU `FSR` | 0x400（fault なし） |
| BreadcrumbStatus | 0 |

Snap（`GxGdscr`、`GxHm`=0x1F8D0、`RpmhPower`、TCS0〜3、`SeqBusy`、`Status0`、`CxBusy`、`Pll0Mode`）：

| Stage | GxGdscr | GxHm | RpmhPower | TCS0〜3 | SeqBusy | Status0 | CxBusy | Pll0Mode |
|---|---|---|---|---|---|---|---|---|
| 0 OOB 前 | 0x00222001 | 0xC0 | 0 | 1/1/1/1 | 0 | 0x01000000 | 0x30000000 | 0xD8000005 |
| 1 ACK 後 | **0xF8222800** | **0** | 0 | 1/1/1/1 | 0 | 0x01000000 | 0x30000000 | 0xD8000005 |
| 2 slumber 後 | 0x00222801 | 0xC0 | **0xF** | 1/1/1/1 | 0 | 0x01000000 | 0x30000000 | 0x08000004 |
| 3 reset・rpmh_stop 後 | 0x00222801 | 0xC0 | 0xF | 1/1/1/1 | **1** | **0x01010006** | 0x71000000 | 0x08000004 |
| 4 idle 確認後 | 0x00222801 | 0xC0 | 0xF | 1/1/1/1 | 1 | 0x01010006 | 0x71000000 | 0x08000004 |

- **GX が ON になった**：OOB GPU_SET の ACK のあと、GX GDSCR の bit31（PWR_ON）が立ち、SW_COLLAPSE（bit0）が 0、0x1F8D0 の bit6/7 が 0。`GX_BW_PERF_VOTE` だけ（Stage 0）では GX は OFF のままで、OOB で入った。
- **slumber で GX が OFF に戻った**：GDSCR 0x00222801、0x1F8D0 = 0xC0、`RPMH_POWER_STATE` = 0xF（SLUMBER）。GPU_CC PLL0 の MODE も 0xD8000005 から 0x08000004 に変わった。PLL0 は OOB 前から動いていた。VOTE のときにファームウェアが起こしたと見られる [U]。
- TCS0〜3 は全段で idle（1）。投票の途中を捉えていないだけかもしれない [U]。GPU PDC に触れずに GX が入ったので、gfx レールの投票は AOP 側の PDC などで届いていると見られる [I]。
- **KeepCx の原因**：rpmh_stop のあと、RSCC の `SEQ_BUSY` が 1、STATUS0 が 0x01010006 のまま。既存の idle 確認は SEQ_BUSY=0 を求めるため、10 ms で時間切れになった。本家 Linux の `a6xx_rpmh_stop`（a6xx_gmu.c 650 行）は STATUS0 bit16 だけを待ち、REQ=0 に戻して `PDC_SLEEP` の印を付ける。SEQ_BUSY が 0 になるのを待つのは、次に起こすときの `a6xx_rpmh_start`（620 行：REQ=bit1 → ACK bit1 → SEQ_BUSY=0 → REQ=0）。したがって、rpmh_stop のあとに SEQ_BUSY=1 なのは RSC の sleep 状態と見られる [I]。
- CX_BUSY は Stage 3 で 0x71000000 に変わった（bit24 と bit30 が増えた）。意味はまだ調べていない [U]。
- ファームウェアのログ（0x2000 バイト）：VOTE の受信（0x0080B788）の直後に種類 0x05、0x24、0x25、0x11、0x26 の記録がある。OOB は約 6.7 ms 後（0x0082B637〜）、PREPARE_SLUMBER は 0x008884C6 に受信し、種類 0x07 の記録のあと 0x00510321 で ACK した。
- 後片付けは KeepCx で止まったので、SMMU の disable、クロック・GBIF・RSCC・setup の復元は実行していない（各 RestoreStatus は 0xC0000225）。CX は ON のまま、CM3 は reset のまま、RSCC は sleep 状態。
- 次：rpmh_stop のあとの idle 確認を Linux に合わせて変える案（STATUS0 bit16 が立ち、REQ=0、GX OFF、CM3 reset なら SEQ_BUSY=1 を許す）。次の起動の前に rpmh_start の手順を入れるかどうかも決める。試験の前に電話を再起動し、RSCC と CX の状態を消しておく。

### 9.13.2 rpmh_stop 後の sleep 許容と次回起動の扱い（実機未実行）

v43の停止結果に合わせ、今回の実行で `RSCC_CONTROL_REQ=1` を実際に書いた場合だけ、idle確認でRSCCのsleep状態を許容するよう変更した。REQ書き込み前にブレッドクラム保存が失敗した場合や、OOB/rpmh_stopを実行していないモードには適用しない。rpmh_stopの待ちが時間切れでも、その後のidle待ち中に下記条件が成立すれば受け入れる。

- 共通条件：CM3_SYSRESET=1、REQ=0、GX GDSCRのPWR_ON=0・SW_COLLAPSE=1、GX HMのbit6/7がともに1。
- 通常経路：従来どおりSEQ_BUSY=0かつACK=0。
- sleep経路：今回rpmh_stopの要求を発行済み、SEQ_BUSY=1、STATUS0 bit16=1。共通条件は緩めず、sleep経路ではACK=0を追加条件にしない。SEQ_BUSYの他の値や、sleep完了bit16がない状態は許容しない。

この条件でidle確認が通れば、SMMU Disable、クロック・GMU setup・GBIF・RSCCの復元へ進む。復元の順番・比較・失敗時のKeepCxは変更していない。v43で止まっていたidle判定を修正するものであり、未実行だった復元処理が実機で成功するかは別途確認する必要がある。

次回起動に `rpmh_start` は追加しない。現在のプローブは `EpiGpuCxTest` が開始時にCX collapseを要求する単発試験で、CXを保持したまま再開する運用は扱わない。一方、CX collapseでRSCCのsleep状態も消えることは仮定せず、RSCC初期化の前に読み取りで確認する。

追加診断 `GpuGmuRsccEntry`（REG_BINARY、little endian、16バイト）は `{Status0, SeqBusy, ControlReq, ControlAck}`。ブレッドクラム0x680で採取し、STATUS0 bit16=0・SEQ_BUSY=0・REQ=0・ACK=0がそろわなければ、RSCCへ書く前に `EPI_GMU_E_PRECOND=1` / STATUS_INVALID_DEVICE_STATEで停止する。未到達なら空データ。これにより、再起動やCXの再投入後にもsleepが残った場合、未実装のwake手順で上書きせずに状態を記録できる。warm resumeが必要になった場合に、REQ bit1→ACK bit1→SEQ_BUSY=0→REQ=0を別の変更として実装する。

既存のGpuGmuGx（340バイト）、Messages（888バイト）、Queues（252バイト）の形式は維持する。Gx Snap 3/4とHfiRscc stage 2で、sleepの状態とidle判定後の状態を比較できる。shimもrpmh_stop後にSEQ_BUSY=1を保持するよう変更した。今回、電話の再起動・テスト・ビルド・署名・配置・実機実行は行っていない。

### 9.13.3 RSCC STATUS0 の復元比較マスク（実機未実行）

RSCC復元（0x6B0以降）のSTATUS0（dword 0x004）だけ、比較マスクを0x01000000に変更した。今回変更したbit24が元の値に戻ったことを確認し、sleep後のbit16/bit2/bit1などの状態ビットの差は復元失敗としない。他の9項目は従来どおり全ビットで比較する。初期化時の比較は変更しない。

`GpuGmuRsccSetup` のRestoredにはマスク前の生値を保存する。10レコード×20バイト＝200バイトの形式、元値を書き戻す処理、失敗時のKeepCxの扱いは維持する。例えば元値0に対する読み戻し0x00010006は比較成功、bit24が残った0x01010006は比較失敗となる。眠っているRSCCへの復元書き込みの実際の挙動は、まだ検証していない。今回、電話の再起動・テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat を再起動した直後、Epinephrine v44 / 20.0.2.44、`EpiGpuProbe=0x1E80`）：idle の判定は通った。SMMU の Disable で失敗して KeepCx** [P]。

- `GpuGmuRsccEntry` = {0, 0, 0, 0}。電話を再起動すると、RSCC は起きた状態に戻っていた。
- GX の ON→OFF、HFI の 6 通の ACK、Snap 0〜4 は v43 とまったく同じ値で、再現した。
- rpmh_stop のあとは SEQ_BUSY=1、STATUS0=0x01010006。sleep の経路で idle と判定された（`GpuGmuHfiIdleStatus=0`）。
- そのあと `EpiSmmuDisable` が 0xC0000185（STATUS_IO_DEVICE_ERROR）で失敗した。`GpuGmuStatus=8`、`KeepCx=1`。WindowReady の失敗（0xC0000184）でも Sync の時間切れ（0xC00000B5）でもないので、Disable の中の Write の読み戻しが合わなかったと見られる。v37〜v42 では同じ Disable が通っていた。
- `GpuSmmuBadWrite` は Enable が失敗したときにしか保存されないので、どのレジスタかはわからない。ブレッドクラムも 0xFFFF で上書きされている。
- そのため、クロック、GBIF、RSCC、setup の復元は実行されていない。SMMU `FSR` は 0x400（fault なし）。
- 次：Disable が失敗したときも `EpiSmmuQueryLastError` の内容を `GpuSmmuBadWrite` に保存する。実行の前には電話を再起動する。

**結果（v45 / 20.0.2.45、再起動直後、`EpiGpuProbe=0x1E80`、Disable が失敗したときの `GpuSmmuBadWrite` を追加した版）：Disable は CBAR0 の書き戻しで失敗** [P]。

- `GpuSmmuBadWrite` = {Step 0x531, Offset 0x1000, Value 0, Readback 0x0001F300}。
- 0x1000 は SMMU の GR1 の `CBAR0`（`ConfigOffsets[1]`）。Enable で書いた 0x1F300 を元の値 0 に戻そうとしたが、書き込みが効かず 0x1F300 のまま読めた。
- その前の SMR の無効化（0x520）、SCTLR=0（0x521）、TLB の Sync（0x522）、`CBA2R0`（0x1800）の書き戻し（0x530）は通っている。
- `CBAR0` の書き戻しは v37〜v42 では通っていた。GX の ON/OFF、slumber、rpmh_stop のどれかのあとで、`CBAR0` だけが書けなくなったと見られる [I]。ハイパーバイザが `CBAR` を管理していて、条件によって書き込みを止めている可能性がある [U]。
- RSCC の状態、GX の ON→OFF、HFI の結果は v44 と同じ。`GpuGmuRsccEntry` = 0。SMMU `FSR` は 0x400。

### 9.13.4 CBAR0の段階別読み取りとDisable復元全項目の診断（実機未実行）

CBAR0をいつ書けなくなるか切り分けるため、既存のGX Snapと同じ5段階（0=OOB前、1=GX ONのACK待ち終了後、2=slumber後、3=reset/rpmh_stop後、4=idle確認後）で読む。`EpiSmmuQueryCbar()` はSMMUの生きた写像とCX/ブランチを確認してからCBAR0（SMMUバイトoffset 0x1000）を読むだけで、状態を変えない。段階途中にCBAR0を書き直す試験は入れていない。

Disableは、WindowReady、SMR0の無効化（0x520）、CB0停止（0x521）、TLB同期（0x522）がすべて成功した後に限り、ConfigOffsetsの11項目を最後まで復元して結果を記録する。CBAR0で読み戻しが合わなくても残り9項目へ進む。変更するoffsetと値は従来と同じで、TCR2の比較マスクも維持する。書き込み前のブレッドクラム保存・flushが失敗した場合は、書き込みを続けず停止する。

読み戻し不一致は最初の失敗statusを返し、1項目でも失敗したら最後のSCTLR/SMR復元（0x53B/0x53C）やReleaseHardwareは実行しない。CB/streamは停止したまま、ctx・ページテーブル・データ・CXを保持してstatus 8 / KeepCxへ進む。停止境界や同期の失敗でも従来どおり即停止する。これは翻訳が止まった状態で既存の設定レジスタへの復元だけを続ける変更で、全不一致を成功扱いにする変更ではない。

追加診断（EpiDiag、REG_BINARY、little endian）：

| 値 | 内容 |
|---|---|
| `GpuSmmuCbar` | 5レコード×12バイト＝60バイト。各 `{Stage, QueryStatus, Cbar0}`。QueryStatusはNTSTATUS、未到達はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。0x1E80のcleanup/keepの両経路で保存する。GPU/GX診断の既存構造体サイズは変更しない |
| `GpuSmmuDisableWrites` | 最後に到達したConfigOffsets復元の試行を、成功も含めて最大11レコード×28バイト＝308バイトで保存する。各 `{Step, Offset, Value, Readback, CmpMask, Status, Written}`。OffsetはSMMU 0x03DA0000からのバイト、StatusはNTSTATUS。Written=1が実際に書いた項目、0は書き込み前に停止した項目でReadback=0xFFFFFFFF。復元ループ未到達なら空 |

レコード順はCBA2R0（0x1800）、CBAR0（0x1000）、TCR2（0x10010）、TCR（0x10030）、TTBR0 LO/HI（0x10020/24）、TTBR1 LO/HI（0x10028/2C）、MAIR0/1（0x10038/3C）、S2CR0（0xC00）。失敗行はStatusで抽出でき、マスクで許容された生値の差もReadback/CmpMaskで確認できる。初期停止や最後のSCTLR/SMRで止まった場合の最初の不一致は、従来の `GpuSmmuBadWrite` に残す。

`EpiSmmuQueryDisableWrites()` は保持した記録をソフトウェアだけでコピーするAPIで、成功したDisable後もctx破棄までは取得できる。Enable内のrollbackの記録も、Enable失敗時に保存する。`GpuSmmuBadWrite` は引き続きctx寿命中の最初の不一致だけを保持する。Enable成功後の通常Disableでは、今回の復元記録から各項目を確認する。

rpmh_stopを省く試験、OOBだけでslumberを省く試験、GX/RBBM/CPやPDCへの新しいアクセスは追加していない。テスト用GMU stubには新しいquery APIの受け口を追加した。今回テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat を再起動した直後、Epinephrine v46 / 20.0.2.46、`EpiGpuProbe=0x1E80`）：書き戻せないのは `CBAR0` だけ。値はどの時点でも変わらない** [P]。

- `GpuSmmuCbar`：5 つの時点（Stage 0〜4）とも、Query は成功、`CBAR0` = 0x0001F300（Enable で書いた値）。GX の ON/OFF、slumber、rpmh_stop のどれでも変わらない。
- `GpuSmmuDisableWrites`：11 項目のうち、合わなかったのは 0x531（`CBAR0`、元の値 0 → 読み戻し 0x1F300）だけ。ほかの 10 項目は元の値どおりに読み戻せた。
- 元の値は、`CBA2R0` が 0、`CBAR0` が 0。CB0 のほかのレジスタは 0x40030077、0x18B3028B、0xD7BBEEFB などで、規則性のない値だった。リセットのあとに初期化されていない値と見られる [I]。
- **見直し**：失敗した v44〜v46 は、どれも電話を再起動した直後の実行で、`CBAR0` の元の値は 0 だった。`CBAR0` の書き戻しが通った v32〜v42 は、同じ起動の中で続けて実行したもので、そのときの元の値は記録していない。
- `CBAR` = 0 は TYPE（bit17:16）= 0、つまり stage 2 の context を意味する。この SMMU は stage 1 だけ（IDR0 = 0x4C017E06）なので、0 という値を受け付けず、書き込みが無視されていると考えられる。この場合、原因は GX ではない [I]。
- 確かめ方：再起動した直後に、GX を使わない `0xE80` を 1 回実行する。Disable がまた 0x531 で失敗すれば、GX とは無関係だとわかる。

**確認（v46、再起動直後、`EpiGpuProbe=0xE80`、GX なし）：同じく 0x531 の `CBAR0` で失敗。GX とは関係ない** [P]。

- `GpuSmmuBadWrite` = {0x531, 0x1000, 0, 0x1F300}。ほかの 10 項目は通った。HFI と idle の判定は 0、`GpuGmuStatus=8`、`KeepCx=1`。
- CB0 の元の値は、一部のビットが v46 の `0x1E80` のときと違っていた（TCR 0x18B3028B → 0x18BF0A83 など）。初期化されていない値であることと合う [I]。
- 結論：電話を再起動した直後の 1 回目は、`CBAR0` の元の値が 0（TYPE=0）なので、書き戻しが必ず失敗する。v32〜v42 で通ったのは、同じ起動の中で続けて実行していたためと見られる [I]。GX の ON/OFF、slumber、rpmh_stop の手順は、後片付けの失敗の原因ではない。

### 9.13.5 CBAR0の復元元がTYPE=0の場合の扱い（実機未実行）

案1を採用し、DisableのConfigOffsets復元で、CBAR0（SMMUバイトoffset 0x1000）の保存値がTYPE=0の場合だけ比較マスクを0にする。TYPEはbit17:16（マスク0x00030000、照合元は [Linux arm-smmu.h](https://kernel.googlesource.com/pub/scm/linux/kernel/git/lpieralisi/linux/+/bc139119a1708ae3db1ebb379630f286e28d06e8/drivers/iommu/arm-smmu.h) の定義）。このデバイスは確認済みのS1専用レイアウトを前提にしており、TYPE=0への読み戻し不一致を失敗にしない。

元の値を書いて読み戻す処理は維持する。`GpuSmmuDisableWrites` の該当行に Value=保存値、Readback=生値、CmpMask=0、Written=1、Status=0を残すため、値が変わらなかったことも確認できる。Enable時や元のTYPEが0以外の場合、他の10項目、SMR無効化・SCTLR停止・TLB同期の条件は変更しない。書き込み前のブレッドクラム保存・flush失敗は引き続き失敗として止め、未書き込み行はWritten=0で記録する。SCTLR/SMRの最後の復元や資源解放は、許容対象以外の比較と処理がすべて成功した場合だけ行う。

`GpuSmmuBadWrite` は許容された差分をエラーにせず、その他の最初の不一致を従来どおり保持する。既存診断の形式・サイズは変更しない。今回テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-08、v34-akat を再起動した直後、Epinephrine v47 / 20.0.2.47、`EpiGpuProbe=0x1E80`）：GX の ON→OFF から後片付けまで、すべて成功** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | **0 / 0 / 0** |
| `GpuGmuHfiStatus` / `GpuGmuHfiIdleStatus` | 0 / 0（sleep の経路） |
| `GpuGmuDisableStatus` | 0 |
| RSCC / GBIF / クロックの復元 | すべて 0 |
| `GpuGmuRsccEntry` | {0, 0, 0, 0} |

- GX：Snap 1 で GDSCR 0xF8222800、0x1F8D0 = 0（ON）。Snap 2 で 0x00222801、0xC0、`RPMH_POWER_STATE` = 0xF（OFF）。v43〜v46 と同じ値。
- `GpuSmmuDisableWrites`：0x531 の `CBAR0` は、比較マスク 0（TYPE=0 の元の値）で通った。読み戻しは 0x1F300 のまま。ほかの 10 項目は全ビットが一致した。
- RSCC の STATUS0 の復元：読み戻しは 0x00010006。比較するのは bit24 だけなので通った。
- 電話を再起動した直後の 1 回目で、GMU の起動 → HFI 6 通 → GX ON → slumber で GX OFF → rpmh_stop → SMMU Disable → 全復元 → CX OFF まで、初めて最後まで通った。
- 次：GX が ON の間に、GX 側のレジスタ（chip id など）を読むだけの試験。GPU 本体のレジスタに触れるのは初めてになる。

### 9.14 GX 側のレジスタを初めて読む試験の計画（本家 Linux の事実、2026-11-12）

出典は手元の参照コピーの `a6xx_gpu.c`、`a6xx.xml`、`a6xx_gmu.c`、`kodiak.dtsi`。行番号は手元の参照コピーのもの。コードは持ち込まない。実装はユーザーが書く。

**1. chip id は読めない** [P]
- A6xx の GPU には、chip id を返すレジスタがない。Linux の `chip_id`（0x06030500）は、カタログと DT の compatible（`qcom,adreno-635.0`）から決まる（`adreno_gpu.c` 1207 行）。
- Linux はこの値の並びを変えて、GMU のファームウェアを起動するときに `GMU_HFI_SFR_ADDR`（0x1F982）へ書く（`a6xx_gmu.c` 1012〜1038 行）。Epinephrine もすでに 0x06035000 を書いている（`epi_gmu.cxx` の `V_GMU_CHIPID`）。
- そこで最初に読む相手は、chip id の代わりに **`RBBM_STATUS`** にする。

**2. GX 側のレジスタの位置** [P]
- GPU のレジスタの窓は `kgsl_3d0_reg_memory` = 0x03D00000、大きさ 0x40000（`kodiak.dtsi` 3218 行）。RBBM はその先頭にある。今の実装が持っている窓は GBIF のページ（+0xF000、CX 側）だけなので、**先頭のページ（0x03D00000〜0x03D00FFF）を新しく map する必要がある。**

| レジスタ | dword | 物理アドレス | 中身 |
|---|---|---|---|
| `RBBM_STATUS` | 0x210 | 0x03D00840 | 各ブロックの BUSY。bit0 = CP_AHB_BUSY_CX_MASTER、bit2 = CP_BUSY、bit23 = GPU_BUSY_IGN_AHB |
| `RBBM_STATUS3` | 0x213 | 0x03D0084C | bit24 = SMMU_STALLED_ON_FAULT |
| `RBBM_INT_0_STATUS` | 0x201 | 0x03D00804 | 割り込みの状態 |
| `RBBM_CLOCK_CNTL` | 0xAE | 0x03D002B8 | ハードウェアのクロックゲーティング |
| `RBBM_GBIF_HALT` / `_ACK` | 0x16 / 0x17 | 0x03D00058 / 0x5C | GX 側の GBIF の halt |

**3. Linux が GX 側に触るタイミング** [P]
- `hw_init`（`a6xx_gpu.c` 1179 行）の最初に OOB GPU_SET を立て、「GMU が GPU を ON のまま保つ」ようにする（1190 行）。ACK が返ったら、待たずにすぐ GX 側に触る。最初に書くのは `GBIF_HALT=0` と `RBBM_GBIF_HALT=0`（1203〜1207 行）で、どちらも書いたあと読み戻している。
- `hw_init` の最後で OOB を解除する（1533 行、「GPU に触り終わったので、電源の管理を始めてよい」）。**GX 側に触ってよいのは、OOB の ACK から解除までの間だけ。**
- `RBBM_STATUS` は、idle の判定（128 行：`RBBM_STATUS & ~bit0` が 0 なら idle）とダンプ（1565 行）で読むだけのレジスタ。読んでも状態は変わらない。

**4. 試験の形（案）**
- 新しい試験用のビット 0x2000 を足す。0x1E80 の 5 ビットをすべて前提にして、全体は `EpiGpuProbe=0x3E80`。
- 読むのは **`RBBM_STATUS` を 1 回だけ**。書き込みはしない。
- 読む場所は、今の `GmuGxOob` の **0x7C3（ACK のクリア、Snap 1）と 0x7C4（OOB の解除）の間**。解除のあとでは読まない。
- 読む直前にもう一度確かめ、どれかが外れていたら読まずに飛ばして記録する：
  - OOB の ACK が成功した
  - GX GDSCR（GPU_CC +0x100C）の bit31 が 1
  - 0x1F8D0 の bit6／bit7 が 0
- 読む直前にブレッドクラム（例：0x7C8）を保存して flush する。読んだ瞬間に電話が固まっても、再起動のあとで `EpiGpuProbeStep` に 0x7C8 が残り、この読み取りが原因だとわかる。
- 記録（例）：`GpuGmuGxReg` = {Attempted、Skipped の理由、そのときの GDSCR、0x1F8D0、`RBBM_STATUS`}。
- 期待する値 [U]：CP も SQE も動かしていないので、0、または bit0（CX 側からの AHB アクセス）だけが立つ値。Linux の idle の判定（bit0 を除いて 0）に合うかを見る。

**5. 危険** [I]
- GX 側のレジスタに CPU から触るのは初めて。GX の GDSC が ON でも、クロックが来ていないとバスが応答せず、電話が固まるか再起動するかもしれない（GPU PDC を読んだときと同じ）。
- 今回わかっていること：OOB のあと GX GDSC は ON で、GPU_CC の PLL0 も動いていた（0xD8000005）。上の 3 つの確認は、残る危険を減らすためのもの。
- `RBBM_CLOCK_CNTL` などほかのレジスタは、`RBBM_STATUS` が無事に読めてから、次の試験で 1 つずつ足す。

### 9.14.1 RBBM_STATUSの単発読み取りの実装（実機未実行）

この節はv48の単発読み取りと20バイト診断の説明。現行の6レジスタ・44バイト形式は§9.14.2を参照。

bit13 `EPI_GMU_PROBE_GX_REG=0x2000` を追加し、`EpiGpuProbe=0x3E80` のときだけ RBBM_STATUS を1回読む。bit13にはbit7/9/10/11/12（0x1E80）をすべて要求し、不足する指定はワンショット解除・flush後、ハードウェアアクセス前に `GpuProbeStatus=6` で拒否する。既存モードのOOB・slumber・後始末は維持する。

読み取りは `GmuGxOob()` のACKクリア・Snap 1（0x7C3）とOOB解除（0x7C4）の間だけで行う。OOB ACK成功、GX GDSCR bit31=1、GX HM（0x1F8D0）のbit6/7=0を確認し、GPU先頭ページ（物理0x03D00000、0x1000バイト）をPAGE_READONLY / non-cacheableで一時mapする。ブレッドクラム0x7C8の保存とflushを確認した後、同じ3条件を再確認してから dword 0x210（物理0x03D00840）を一度だけ読む。OOBのACK判定は保存済みの待ち結果を使う（INFO bit31は既にクリア済み）。ページは解除前にunmapする。

条件不成立・map失敗・ブレッドクラム保存/flush失敗の場合は読まず、理由を記録して必須のOOB解除とslumberへ進む。追加読み取りのスキップ自体は既存のGX試験の結果コードを変えない。0または1との一致を成功条件にする処理も入れず、RBBM_STATUSは生値を記録する。GX側への書き込み、他のRBBM/CPレジスタの読み取り、PDCアクセスは追加しない。

追加診断 `GpuGmuGxReg`（REG_BINARY、little endian、20バイト）は5 DWORD：

`{Attempted, Reason, Gdscr, GxHm, RbbmStatus}`。

Attemptedは実際のREAD_REGISTER_ULONG直前に1とし、読み取りを飛ばした場合は0。Gdscr/GxHmは条件確認時の値で、実際に読む場合はmap・flush後の最終確認値。RbbmStatusと未取得の確認値は0xFFFFFFFF。cleanup/keep両経路で保存し、bit13を指定しない場合は空データ。

| Reason | 内容 |
|---|---|
| 0 | RBBM_STATUSを読み取った（値の内容は判定しない） |
| 1 | 読み取り処理未到達（OOB要求前の失敗、HFI失敗など） |
| 2 | OOB ACKが成功していない |
| 3 | GX GDSCRのbit31が0 |
| 4 | GX HMのbit6またはbit7が1（完全ONではない） |
| 5 | 読み取り専用ページのmap失敗 |
| 6 | 0x7C8の保存またはflush失敗 |

最初のGXアクセスなので、条件を満たしてもクロック等の状態次第ではバスが応答しない可能性が残る [U]。READで停止した場合はOOB解除まで戻らず、再起動後のブレッドクラム0x7C8で位置を特定する。既存の `GpuGmuGx`（340バイト）、Messages（888バイト）、Queues（252バイト）の形式は変更していない。shimにはGPU先頭ページの受け口を追加した。今回、電話の再起動・テスト・ビルド・署名・配置・実機実行は行っていない。

**結果（2026-11-12、v34-akat を起動した直後、Epinephrine v48 / 20.0.2.48、`EpiGpuProbe=0x3E80`）：GX 側のレジスタを初めて読めた** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuGxReg` | {Attempted 1, Reason 0（読んだ）, GDSCR 0xF8222800, 0x1F8D0 = 0, **`RBBM_STATUS` = 0x00000001**} |
| `GpuGmuStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | 0 / 0 / 0 |
| HFI / idle の判定 / SMMU の Disable / 各復元 | すべて 0 |

- 電話は固まらず、バスも応答した。読んだ時点で GX は ON（GDSCR の bit31 = 1、0x1F8D0 = 0）。
- `RBBM_STATUS` = 0x1：立っているのは bit0（CP_AHB_BUSY_CX_MASTER）だけ。CP も描画のブロックも BUSY ではない。Linux の idle の判定（bit0 を除いて 0）に合う。値は、CPU が CX 側から AHB でこのレジスタを読んでいること自体を示していると見られる [I]。
- そのあとの OOB の解除、slumber、rpmh_stop、後片付けは v47 と同じく、すべて通った。
- 次：同じ ON の間に、読むだけのレジスタを 1 つずつ足す（`RBBM_STATUS3`、`RBBM_INT_0_STATUS`、`RBBM_CLOCK_CNTL` など）。そのあと、Linux の `hw_init` の最初の書き込み（`GBIF_HALT=0`、`RBBM_GBIF_HALT=0`）を調べる。

### 9.14.2 同じOOB中の6レジスタ読み取りへの拡張（実機未実行）

案Aを採用し、bit13の `EpiGpuProbe=0x3E80` の読み取りを6レジスタに広げた。前提の0x1E80、新しい試験ビットを追加しない点、OOB ACK後・解除前だけで読む点、1ページを読み取り専用でmap/unmapする点はv48と同じ。GX側への書き込みは追加しない。

| Valueの添字 | レジスタ | GPU dword | ページ内バイト | ブレッドクラム |
|---|---|---|---|---|
| 0 | RBBM_STATUS | 0x210 | 0x840 | 0x7C8 |
| 1 | RBBM_STATUS3 | 0x213 | 0x84C | 0x7C9 |
| 2 | RBBM_INT_0_STATUS | 0x201 | 0x804 | 0x7CA |
| 3 | RBBM_CLOCK_CNTL | 0x0AE | 0x2B8 | 0x7CB |
| 4 | RBBM_GBIF_HALT | 0x016 | 0x058 | 0x7CC |
| 5 | RBBM_GBIF_HALT_ACK | 0x017 | 0x05C | 0x7CD |

map前にv48の条件（OOB ACK成功、GDSCR bit31=1、GX HM bit6/7=0）を確認する。その後、表の順に各レジスタのブレッドクラムを保存・flushし、条件を再確認して1回だけ読む。読み取りが戻るたびにCountを増やす。確認または保存・flushに失敗した時点で後続を読まず、ページをunmapして必須のOOB解除・slumberへ進む。GBIF側の別ページのHALTをこの読み取り一覧には加えず、INT_CLEAR_CMDにも書かない。

`GpuGmuGxReg`（REG_BINARY、little endian）は11 DWORD＝44バイト：

`{Attempted, Reason, Gdscr, GxHm, Count, Value[6]}`。

Attemptedは最初のREAD直前に1となる。Countは完了した読み取りの数（0〜6）、Valueは上表の順で、未読は0xFFFFFFFF。Reasonはv48と同じ0〜6で、0は全6項目の読み取り完了、途中の失敗はその理由（条件不成立・map失敗・保存失敗）を保持する。途中まで読めた場合はAttempted=1と非0のReasonが共存し、Count件のValueが有効。Gdscr/GxHmは最後に条件確認した値。値が0xFFFFFFFFだったこと自体はエラー条件にせず、生値を保存する。

CPUのMMIO読み取りにはNTSTATUSの戻り値がない。バスが応答しなければ処理自体が戻らず、unmapやOOB解除もできない可能性がある。その場合は0x7C8〜0x7CDのどのブレッドクラムで止まったかを使って対象を特定する。Countの診断は処理が戻ってcleanup/keepに到達した場合に保存される。

机上試験用のshimのGPU先頭ページには、上表の位置に `{1, 0x213, 0x201, 0xAE, 0x16, 0x17}` を設定するfixtureを追加した。ホストテストには、Valueの順序・Count=6・Reason=0・MMIO書き込みなしの確認と、0x7CAの記録保存失敗を注入してCount=2・Reason=6・残り未読で止まる確認を追加した。テストは未実行。版番号の変更・ビルド・署名・配置・実機実行も行っていない。

**机上の試験（2026-11-12）**：`smmu_test` と `gmu_test` はどちらも通った。ただし `gmu_test` は、今回より前からある C4459 の警告（`epi_gmu.cxx` の局所変数 `writes` が `nt_shim.hxx` のグローバル変数と同じ名前）で `build.cmd` のままでは止まる。`/wd4459` を付けて一時的にビルドして確かめた。そのあと shim 側のグローバル変数を `mmioWrites` に変えて直し、`build.cmd` のままで両方の試験が通ることを確かめた（ドライバのコードは変えていない）。

**最初の実行（v48 の実行のあと、再起動しないで、v49 / 20.0.2.49）**：入口で `EPI_GMU_E_PRECOND`（`GpuGmuStatus` 1、NtStatus 0xC0000184）となり、何も触らずに止まった。`GpuGmuRsccEntry` = {0x00010006, 1, 0, 1} で、v48 の rpmh_stop で眠った RSCC が残っていた [P]。KeepCx=0 で終わっていても、GX を ON にする実行（0x1E80／0x3E80）の前には毎回再起動がいる。

**結果（2026-11-12、v34-akat を起動した直後、Epinephrine v49 / 20.0.2.49、`EpiGpuProbe=0x3E80`）：6 個とも読めた** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuGxReg` | {Attempted 1, Reason 0, GDSCR 0xF8222800, 0x1F8D0 = 0, Count 6} |
| `RBBM_STATUS`（0x210） | 0x00000001 |
| `RBBM_STATUS3`（0x213） | 0x00000000 |
| `RBBM_INT_0_STATUS`（0x201） | 0x00000000 |
| `RBBM_CLOCK_CNTL`（0x0AE） | 0x00000000 |
| `RBBM_GBIF_HALT`（0x016） | 0x00000000 |
| `RBBM_GBIF_HALT_ACK`（0x017） | 0x00000000 |
| `GpuGmuStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | 0 / 0 / 0 |
| `GpuGmuRsccEntry` | {0, 0, 0, 0}（再起動で眠りが解けている） |
| HFI / idle の判定 / GBIF / クロック / SMMU の Disable / 各復元 | すべて 0 |

- `RBBM_STATUS` は v48 と同じ 0x1。`STATUS3` も 0 で、どのブロックも BUSY ではない。
- `INT_0_STATUS` = 0：GX を入れた直後で、残っている割り込みはない。
- `CLOCK_CNTL` = 0：hwcg（ハードウェアのクロックゲーティング）がまだ設定されていない、リセット直後の値と見られる。Linux では `hw_init` の中で hwcg の表を書いてから、この値を設定する [I]。
- `GBIF_HALT` = 0、`HALT_ACK` = 0：RBBM 側の GBIF は止められていない。`hw_init` が最初に書く `RBBM_GBIF_HALT=0` は、今の値と同じになる [I]。
- 6 回の読み取りのあとも、OOB の解除、slumber、rpmh_stop、後片付けはすべて通った。
- 次：Linux の `hw_init` の最初の書き込み（`GBIF_HALT=0`、`RBBM_GBIF_HALT=0` と、そのあとの読み返し）を調べる。

### 9.15 GX 側への最初の書き込みの計画（本家 Linux の事実、2026-11-12）

**Linux の `hw_init` の冒頭**（`a6xx_gpu.c` 1179〜1210）：
1. `a6xx_gmu_set_oob(GMU_OOB_GPU_SET)`（GX を ON に保たせる）。
2. A635 は `a6xx_has_gbif()` が真（偽になるのは A630 だけ）なので、次の 4 つを順に行う。読み返しは値を確かめるためではなく、書き込みを届かせるためのもの。
   - `GBIF_HALT` ← 0、`GBIF_HALT` を読む
   - `RBBM_GBIF_HALT` ← 0、`RBBM_GBIF_HALT` を読む
3. そのあとに `RBBM_SECVID_TSB_CNTL`（0xF803）← 0、TSB の TRUSTED_BASE/SIZE ← 0、各ブロックの `*_ADDR_MODE_CNTL` ← 1、`a6xx_set_hwcg(true)` …と続く。

コメントは「GX domain が collapse されなかった場合に備えて GBIF の halt を外す」。つまり、どちらも 0 になっているのが普通の状態で、2 の書き込みは前回に止めたままの halt を外すためのもの。

**レジスタの場所**（`a6xx.xml`）：

| 名前 | dword | 物理アドレス | 置き場所 |
|---|---|---|---|
| `GBIF_HALT` | 0x3C45 | 0x03D0F114 | GBIF のページ。bit10 の QSB 設定ですでに読み書き可能で割り当てている（`Maps[6]` = 0x03D0F000、dword 0x3C00 から）。GX が OFF でも書けた |
| `GBIF_HALT_ACK` | 0x3C46 | 0x03D0F118 | 同上 |
| `RBBM_GBIF_HALT` | 0x016 | 0x03D00058 | GPU の先頭ページ（GX 側）。v49 の 0 はここ |
| `RBBM_GBIF_HALT_ACK` | 0x017 | 0x03D0005C | 同上。v49 で 0 |

ビットは `GBIF_CLIENT_HALT_MASK` = bit0、`GBIF_ARB_HALT_MASK` = bit1。

**Linux が halt を立てる場所**（`a6xx_bus_clear_pending_transactions`、2085〜2120）：
- 普通の停止（`a6xx_gmu_shutdown` 1455、slumber を通知する前）では `gx_off` = `hung`（普通は偽）。`GBIF_HALT` ← bit0 を書いて ACK の bit0 を待ち、← bit1 を書いて ACK の bit1 を待ち、最後に `GBIF_HALT` ← 0 で外す。`RBBM_GBIF_HALT` には触らない。
- 固まったときと `force_off`（1178）だけ `RBBM_GBIF_HALT` ← 1 を書いて ACK を待ち、0 には戻さない。ここで残った halt を、次の `hw_init` が外す。
- 現行の `GmuGxSlumber()` には、既に0x7D1〜0x7D4でGBIF_HALT=1→ACK bit0待ち→HALT=2→ACK bit1待ち→HALT=0が実装されている。v50でもこの停止手順は維持する。RBBM_GBIF_HALTを立てるforce_offの経路は追加しない。

**提案（v50）**：GX 側に初めて書く。v49 までの読み取りより危険が大きいので、新しいビット（例：bit14、合わせて `0x7E80`、bit13 が前提）に分ける。
- OOB の ACK のあと、v49 の 6 個の読み取りが終わってから、解除の前に行う。
- 先に元の値（`GBIF_HALT`、`GBIF_HALT_ACK`、`RBBM_GBIF_HALT`、`RBBM_GBIF_HALT_ACK`）を読んで記録する。
- Linux と同じ順に、`GBIF_HALT` ← 0 → 読み返し、`RBBM_GBIF_HALT` ← 0 → 読み返し。どちらも書く前に breadcrumb を残し、v48 と同じ GX の確かめをやり直す。
- 元に戻す処理はいらない。0 は普通の状態で、Linux も戻さない。元の値が 0 でなかったときは、その値を診断に残す（halt を外したことになる）。
- 先頭ページは今は読み取り専用で割り当てているので、この段階だけ読み書き可能で割り当てる必要がある。
- 読み返しが 0 でないときは、それ以上書かずに OOB の解除と slumber に進み、診断に残す。その場合に後片付けをどうするかは決めておく必要がある。`RBBM_GBIF_HALT` は GX 側なので、GX が切れれば消えると見られる [I]。`GBIF_HALT` は CX 側（GX が OFF でも書けた）なので、CX を切るまで残る可能性がある [U]。
- 机上の試験：shim の GBIF ページと先頭ページで、書いた 0 が読み返せること、元の値が 0 でないときの記録、途中で breadcrumb が失敗したら書かないこと。

その次の候補は 2 つある。
- slumber の前にバスの halt（`GBIF_HALT` の bit0 → bit1 → 0）を入れて、Linux の普通の停止に合わせる。
- `hw_init` の 3（SECVID と ADDR_MODE）に進む。

### 9.15.1 OOB中のGBIF/RBBM halt解除の実装（実機未実行）

bit14 `EPI_GMU_PROBE_GX_HALT=0x4000` を追加した。前提はbit13とその全前提（0x3E80）で、`EpiGpuProbe=0x7E80` のときだけ実行する。ビット不足はワンショット解除・flush後、ハードウェアアクセス前にstatus 6で拒否する。既存の0x3E80は読み取り専用のまま維持し、0x7E80だけGPU先頭ページをRWでmapする。ページ数や既存の診断ABIは増やさない。

- OOB ACK後、6レジスタの読み取りがCount=6 / Reason=0で終わった場合にだけ進む。先に、CX GBIF_HALT/ACKとRBBM_GBIF_HALT/ACKの4元値を同じOOB中に読む。
- 書く順番はCX GBIF_HALTへ0→同じレジスタを直後に読み返す、次にRBBM_GBIF_HALTへ0→同じレジスタを直後に読み返す。書き込みと読み返しの間にブレッドクラムや状態確認は挟まない。各ACKも記録するが、この時点のACK非0だけでは失敗にしない。
- 元値保存と両書き込みの前に、ブレッドクラム保存・flushとGX確認（OOB ACK成功、GDSCR bit31=1、GX HM bit6/7=0）を行う。途中で確認・保存に失敗したら後続を書かない。HALT本体の読み返しが非0なら後続の追加書き込みを止め、試験失敗として記録する。0を書いた元値を復元する処理は入れない。
- 書き込みや読み返しに異常があっても、既存のOOB解除とslumberを試みる。GX OFFと既存のidle確認に通った後、CX側のGBIF_HALTが0、ACK bit0/1がともに0になることを100 µs間隔・最大10 msで確認する。これは読み取りだけで、追加のリトライ書き込みはしない。解消していればSMMU Disableと通常の復元へ進み、追加試験の失敗は最終status 13に残す。CX側のHALT/ACKが残る、または確認用の記録保存に失敗した場合はstatus 8 / KeepCxとして、クロック・SMMU・資源を保持する。
- RBBM側はOOB解除後に読み直さない。GX OFFの既存安全条件が成立した場合だけ後始末を許し、読み返し異常そのものはstatus 13と生値で残す。GX電源断でHALTが消えることを測定済みとして扱う処理は入れない。GX OFFに戻らない場合は、従来どおりstatus 8 / KeepCx。

追加診断 `GpuGmuGxHalt`（REG_BINARY、little endian、72バイト＝18 DWORD）：

`{Attempted, Reason, Gdscr, GxHm, Count, Status, CxIdleStatus, Before[4], After[4], FinalCx[2], CxPolls}`。

Before/Afterの順序は `{CX HALT, CX ACK, RBBM HALT, RBBM ACK}`。Beforeは4元値、Afterは各HALT書き込み直後のHALT/ACK。Countは完了した書き込み・読み返し組数（0〜2、不一致の組も含む）、Attemptedは最初の書き込み直前に1。Gdscr/GxHmは最後の確認値。未読値と未試行のCxPollsは0xFFFFFFFF。Statusは追加試験、CxIdleStatusは後片付け中のCX HALT/ACK確認のNTSTATUSで、未試行はSTATUS_NOT_FOUND。FinalCxはその最終HALT/ACK、CxPollsは100 µs待ちの回数（0〜100）。cleanup/keepの両経路で保存し、他モードでは空データ。

Reasonは0=2組完了、1=未到達、2=OOB ACK失敗、3=GDSCR OFF、4=GX HMが完全ONでない、5=map失敗、6=ブレッドクラム失敗、7=HALT読み返し非0、8=前提の6読み取り未完了。前提の読み取り段階で止まった場合はそのReasonを引き継ぎ、StatusをSTATUS_INVALID_DEVICE_STATEとする。成功済みでも後片付けでKeepCxになることがあるため、CxIdleStatusと既存のGpuGmuStatus/KeepCxを併読する。

| ブレッドクラム | 内容 |
|---|---|
| 0x7C8〜0x7CD | 既存の6レジスタ読み取り |
| 0x7CE | 4元値保存前のflush・GX確認 |
| 0x7D6 | CX GBIF_HALT=0の前のflush・GX確認と即時読み返し |
| 0x7D7 | RBBM_GBIF_HALT=0の前のflush・GX確認と即時読み返し |
| 0x7D8 | reset/rpmh_stop・既存idle確認後のCX HALT/ACK待ち |

実行順は0x7CD→0x7CE→0x7D6→0x7D7→既存の0x7C4（OOB解除）で、後片付けの0x7D8は既存のidle確認後。0x7D0〜0x7D5のslumber処理と番号は変えていない。追加のGX書き込みはRBBM_GBIF_HALT=0だけで、SECVID・ADDR_MODE・hwcg、force_off、PDCには進まない。

shimにはRW/RO写像の確認、両haltのclear応答、片側のclearが効かない状態の注入を追加した。ホスト試験には2組成功、CX側不一致でRBBMへ書かず停止・最終CX確認も失敗、RBBM側だけ不一致でもCX側は解消しているケースを追加した。試験・版番号変更・ビルド・署名・配置・実機実行は行っていない。

**最初の実行（2026-11-12、再起動の直後、v50 / 20.0.2.50、`EpiGpuProbe=0x7E80`）**：halt の解除そのものは成功した（Reason 0、Count 2、Status 0）。ただし最後の確かめで止まり、`GpuGmuStatus` 8、`KeepCx` 1 になった [P]。
- Before = After = {CX HALT 0, **CX ACK 3**, RBBM HALT 0, RBBM ACK 0}。どの HALT も元から 0 で、書き込みで何も変わっていない。
- `GBIF_HALT_ACK` は、何も書く前から 3 だった。halt が立っていなくても bit0/1 が立っているのが普通の状態である [P]。Linux は halt を立てたあと ACK のビットが立つのを待つだけで、0 に戻るのは待たないので、Linux の手順とも矛盾しない [I]。
- 最後の確かめが「ACK の bit0/1 = 0」を求めていたため、100 回（10 ms）見ても FinalCx = {0, 3} のままで `STATUS_IO_TIMEOUT`（0xC00000B5）になった。halt が残っていたわけではない。
- 修正（v51）：最後の確かめは `GBIF_HALT == 0` だけを見る。ACK は記録するだけにする。HALT が残ったときに KeepCx にする動きは残す。机上の試験に、HALT=0 で ACK=3 のまま成功するケースを足した。

**結果（2026-11-12、再起動の直後、v51 / 20.0.2.51、`EpiGpuProbe=0x7E80`）：GX 側への初めての書き込みが成功し、後片付けまで通った** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuGxReg` | {1, 0, 0xF8222800, 0, Count 6}、値は v49 と同じ {1, 0, 0, 0, 0, 0} |
| `GpuGmuGxHalt` | Attempted 1、Reason 0、GDSCR 0xF8222800、GX HM 0、Count 2、Status 0、CxIdleStatus 0 |
| Before / After | {0, 3, 0, 0} / {0, 3, 0, 0} |
| FinalCx / CxPolls | {0, 3} / 0（最初の 1 回で確認できた） |
| `GpuGmuStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | 0 / 0 / 0 |
| HFI / idle の判定 / GBIF / クロック / SMMU の Disable / RSCC と各復元 | すべて 0 |

- `GBIF_HALT=0`（CX 側）と `RBBM_GBIF_HALT=0`（GX 側）を書いて、電話も固まらず、読み返しも 0 だった。GX 側のレジスタに書けることを初めて確かめた。
- どちらの HALT も元から 0 だったので、ハードウェアの状態は変わっていない。`hw_init` の最初の手順を、Linux と同じ順に実行できたということになる。
- 次：`hw_init` の 3（`RBBM_SECVID_TSB_CNTL`、TSB の TRUSTED_BASE/SIZE、`*_ADDR_MODE_CNTL`）に進む。slumber の前のバスの halt は、v43 からもう入っている（§9.15 の訂正を参照）。

### 9.16 hw_init の次の書き込み（SECVID と ADDR_MODE）の計画（本家 Linux の事実、2026-11-12）

**Linux の順番**（`a6xx_gpu.c` 1210〜1237、GBIF の halt を外した直後）：
1. `RBBM_SECVID_TSB_CNTL` ← 0
2. （A619 holi だけ sptprac。A635 には関係ない）
3. `RBBM_SECVID_TSB_TRUSTED_BASE`（64 ビット、`gpu_write64` なので下位 → 上位の順）← 0、`RBBM_SECVID_TSB_TRUSTED_SIZE` ← 0。コメントは「安全なメモリの範囲を無効にする。使わないし、仮想アドレス空間の一部をふさぎたくない」
4. A7xx でなければ（A635 は A6xx なので当てはまる）、12 個の `*_ADDR_MODE_CNTL` ← 1（全ブロックで 64 ビットのアドレスを使う）
5. そのあとに `a6xx_set_hwcg(true)` が続く（今回の範囲外）

`RBBM_SECVID_TRUST_CNTL`（0xF400）は、もっとあとで zap シェーダーがないときだけ書かれるもので、TZ に守られている可能性がある。今回の範囲には入れない。

**レジスタの場所**（`a6xx.xml`。物理アドレス = 0x03D00000 + dword × 4）：

| 段 | 名前 | dword | 窓の中のバイト | ページ | Linux が書く値 |
|---|---|---|---|---|---|
| A | `RBBM_SECVID_TSB_CNTL` | 0xF803 | 0x3E00C | 0x3E000 | 0 |
| A | `RBBM_SECVID_TSB_TRUSTED_BASE` 下位 / 上位 | 0xF800 / 0xF801 | 0x3E000 / 0x3E004 | 0x3E000 | 0 / 0 |
| A | `RBBM_SECVID_TSB_TRUSTED_SIZE` | 0xF802 | 0x3E008 | 0x3E000 | 0 |
| B | `CP_ADDR_MODE_CNTL` | 0x0842 | 0x02108 | 0x02000 | 1 |
| B | `VSC_ADDR_MODE_CNTL` | 0x0C01 | 0x03004 | 0x03000 | 1 |
| B | `GRAS_ADDR_MODE_CNTL` | 0x8601 | 0x21804 | 0x21000 | 1 |
| B | `RB_ADDR_MODE_CNTL` | 0x8E05 | 0x23814 | 0x23000 | 1 |
| B | `PC_ADDR_MODE_CNTL` | 0x9E01 | 0x27804 | 0x27000 | 1 |
| B | `HLSQ_ADDR_MODE_CNTL` | 0xBE05 | 0x2F814 | 0x2F000 | 1 |
| B | `VFD_ADDR_MODE_CNTL` | 0xA601 | 0x29804 | 0x29000 | 1 |
| B | `VPC_ADDR_MODE_CNTL` | 0x9601 | 0x25804 | 0x25000 | 1 |
| B | `UCHE_ADDR_MODE_CNTL` | 0x0E00 | 0x03800 | 0x03000 | 1 |
| B | `SP_ADDR_MODE_CNTL` | 0xAE01 | 0x2B804 | 0x2B000 | 1 |
| B | `TPL1_ADDR_MODE_CNTL` | 0xB601 | 0x2D804 | 0x2D000 | 1 |
| B | `RBBM_SECVID_TSB_ADDR_MODE_CNTL` | 0xF810 | 0x3E040 | 0x3E000 | 1 |

B の行は Linux が書く順に並べてある。`ADDR_MODE_CNTL` は `a5xx_address_mode` 型で、bit0 が 64 ビットの指定。

**ページの割り当て**：
- 使うページは 11 枚（0x02000、0x03000、0x21000、0x23000、0x25000、0x27000、0x29000、0x2B000、0x2D000、0x2F000、0x3E000）で、すべて GPU のレジスタ窓（0x03D00000、0x40000 バイト）の中にある。
- 次のどちらかを選べる。
  1. 窓全体（0x40000 バイト）を一度に、読み書き可能で割り当てる。Linux と同じ形で、手順が簡単になる。割り当てるだけではハードウェアに触らない。
  2. 使うページだけを 1 枚ずつ割り当てる。触れる範囲が一番狭くなる。
- 窓の中には GPU PDC（0x0B290000）は含まれない。

**値の記録と読み返し**：
- 書く前に、その段のレジスタの元の値をすべて読んで記録する。
- 1 つずつ、breadcrumb と GX の確かめ（v48 と同じ）→ 書く → すぐ読み返す。
- 読み返しが書いた値と違っても、その段のそれ以降は書かないで止め、OOB の解除と slumber に進む。
- B の `GRAS`〜`TPL1` などは、描画の設定（context）のレジスタになる。CPU から読み返したときに書いた値が見えるかどうかは確かめていない [U]。最初は、読み返しが違うことだけを理由にした失敗にはせず、値を記録するだけにする案もある。
- **元に戻す処理はいらない**と見ている。どれも GX 側の設定で、GX を切ると消える [I]。Linux も戻していない。次の実行の最初に読む元の値で確かめられる（毎回再起動するので、リセット直後の値になる）。

**段に分けて試す**：
- **段 A（SECVID の 4 つ、1 ページ）**：新しいビット（例：bit15、合わせて `0xFE80`、bit14 が前提）。halt を外したあと、OOB を解除する前に行う。
- **段 B（ADDR_MODE の 12 個、11 ページ）**：さらに新しいビット（例：bit16、合わせて `0x1FE80`、bit15 が前提）。段 A のあとに続けて行う。
- 段 A だけで 1 回実機で試し、通ってから段 B を足す。

**診断**（案）：
- `GpuGmuGxInit`：`{Attempted, Reason, Gdscr, GxHm, Count, Status, Before[16], After[16]}`。並び順は上の表と同じ（A の 4 つ → B の 12 個）。
- まだ読んでいない値は 0xFFFFFFFF。Reason は v48 の 0〜6 と、7 = 読み返しが違う、8 = 前の段が終わっていない。

**机上の試験**：
- shim の 0x03D00000 からの 0x40000 バイト（または 11 ページ）に受け口を作る。
- 試すこと：
  - 書いた値が上の順で読み返せること
  - 途中で breadcrumb が失敗したら、それ以降を書かないこと
  - 段 A だけのときは、段 B のページに何も書かないこと
  - 読み返しが違うときに止まること

### 9.16.1 段Aの実装（実機未実行）

`EPI_GMU_PROBE_SECVID=0x8000` を追加。bit14と全前提が必要で、`0xFE80` で段Aだけを実行する。bit16は今回は予約扱いとし、指定時はワンショット解除・flush後にstatus 6で拒否する。段Bは段Aの実機確認後に追加する。

既存の6読み取りと2組のhalt解除が成功したあと、同じOOBの解除前に、物理0x03D3E000の1ページだけをRW・非キャッシュでmapする。元値を4個すべて記録してから、TSB_CNTL、TRUSTED_BASE下位、上位、TRUSTED_SIZEの順に0を書き、各書き込み直後に読み返す。元値の各読み取り前（0x800〜0x803）と各書き込み前（0x810〜0x813）にbreadcrumbを保存・flushし、OOB ACK成功・GDSCR bit31・GX HM bit6/7を再確認する。

確認・breadcrumb・mapの失敗、または読み戻し不一致で後続の書き込みを止め、mapを解除してOOB解除と既存のslumberへ進む。元値の復元は行わない。安全な後片付けが成功すれば試験失敗はstatus 13、既存のGX OFF・idle・CX HALT確認や後片付けが失敗すればstatus 8 / KeepCx。CX ACKは記録だけで、0を要求しない。

`GpuGmuGxInit` はREG_BINARY・152バイト（38 DWORD）：`{Attempted, Reason, Gdscr, GxHm, Count, Status, Before[16], After[16]}`。cleanup/keepの両経路で保存する。Before/Afterの先頭4個は段Aの上記順序、残り12個は予約で0xFFFFFFFF。未読値は0xFFFFFFFF、未試行StatusはSTATUS_NOT_FOUND。Countは完了した書き込み・読み返し数（不一致も含む0〜4）、Attemptedは最初の書き込み直前に1。Reasonは0=完了、1=未到達、2=OOB ACK失敗、3=GDSCR OFF、4=GX HM不成立、5=map失敗、6=breadcrumb失敗、7=読み戻し不一致、8=前提のhalt解除未完了。Statusは段AのNTSTATUS。

shimとホスト試験に、書き込み順と全元値の保存、予約スロット未読、元値読み取り中・書き込み途中のbreadcrumb失敗、読み戻し不一致での停止を追加。試験・ビルド・配置・実機実行は未実施。TRUST_CNTL、ADDR_MODE、hwcgには触れない。

**結果（2026-11-12、v34-akat を起動した直後、Epinephrine v52 / 20.0.2.52、`EpiGpuProbe=0xFE80`）：段 A が成功し、後片付けまで通った** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuGxInit` | Attempted 1、Reason 0、GDSCR 0xF8222800、GX HM 0、Count 4、Status 0 |
| Before[0..3]（TSB_CNTL、TRUSTED_BASE 下位、上位、TRUSTED_SIZE） | 0、0、0、0 |
| After[0..3] | 0、0、0、0 |
| Before/After[4..15] | 0xFFFFFFFF（段 B は予約のまま） |
| `GpuGmuGxReg` / `GpuGmuGxHalt` | v51 と同じ（Count 6 / Count 2、Before = After = {0, 3, 0, 0}） |
| `GpuGmuStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | 0 / 0 / 0 |
| HFI / idle の判定 / GBIF / クロック / SMMU の Disable / RSCC と各復元 | すべて 0 |

- SECVID の 4 つのレジスタに書いても、TZ に止められることはなく、電話も固まらなかった。
- 4 つとも元から 0 だったので、halt のときと同じく、ハードウェアの状態は変わっていない。書き込みが本当に効いているかどうかは、この結果からは区別できない [I]。
- 段 B（`*_ADDR_MODE_CNTL` ← 1）では、リセット直後の値（0 と見られる）とは違う値を書くことになる。書いた値が読み返せるかどうかで、GX 側への書き込みが効くことを初めて確かめられる。

### 9.16.2 段Bの実装（実機未実行）

v52での段A成功を受け、bit16 `EPI_GMU_PROBE_ADDR_MODE=0x10000` の拒否を解除した。bit15とその全前提が必要で、`EpiGpuProbe=0x1FE80` のとき、段Aが成功したあと、同じOOBを解除する前に段Bを実行する。前提不足はワンショット解除・flush後にstatus 6で拒否する。`0xFE80` までの処理は変更していない。

段Bでは§9.16の11ページだけをRW・非キャッシュでmapする。VSCとUCHEのページは共有する。12個すべての元値を記録してから、CP、VSC、GRAS、RB、PC、HLSQ、VFD、VPC、UCHE、SP、TPL1、SECVID TSBの順に1を書き、各書き込み直後に同じレジスタを読み返す。元値の各読み取り前に0x820〜0x82B、各書き込み前に0x830〜0x83Bのbreadcrumbを保存・flushし、OOB ACK成功、GDSCR bit31=1、GX HM bit6/7=0を再確認する。

最初の試験では読み返しも厳密に1と比較する方を採用した。不一致・breadcrumb・GX確認の失敗でそれ以降の段B書き込みを止める。map失敗なら段Bは書かない。取得済みのmapはすべて解除し、OOB解除・slumberと既存の後片付けへ進む。元値の復元は行わない。安全な後片付けが成功すればstatus 13、既存の安全確認や復元が失敗すればstatus 8 / KeepCxという扱いは段Aと同じ。TRUST_CNTL、hwcg、CP実行、GPU PDCには進まない。

`GpuGmuGxInit` の形式は152バイトのまま。Before/After[0..3]は段A、[4..15]は段Bの上記順序。Countは両段を通した完了済み書き込み・読み返し数（不一致を含む最大16）で、段Bの元値記録やmapで失敗した場合は4のまま。未読値は0xFFFFFFFF。Status/Reasonは最後に実行した段の結果で、段Bの不一致もReason 7となる。段A失敗時は段Bを呼ばず、段Aの結果を保持する。

shimに段Bのページと書き込みの記録、map失敗・書き込み無視の注入を追加。ホスト試験には、12個の順序と全元値の保存、段Aだけなら段Bを書かないこと、段A未完了時の拒否、各書き込み前のbreadcrumb失敗、元値記録中・mapの失敗、各項目の読み戻し不一致による停止を追加した。試験・ビルド・配置・実機実行は未実施。次にGXをONにする実機試験の前には、ユーザー側で再起動する。

**結果（2026-11-12、v34-akat を起動した直後、Epinephrine v53 / 20.0.2.53、`EpiGpuProbe=0x1FE80`）：段 B が成功した。GX 側に書いた値が読み返せることを初めて確かめた** [P]。

| 項目 | 値 |
|---|---|
| `GpuGmuGxInit` | Attempted 1、Reason 0、GDSCR 0xF8222800、GX HM 0、Count 16、Status 0 |
| Before[0..3] → After[0..3]（段 A） | 0 → 0（v52 と同じ） |
| Before[4..15] → After[4..15]（段 B：CP、VSC、GRAS、RB、PC、HLSQ、VFD、VPC、UCHE、SP、TPL1、SECVID_TSB） | **すべて 0 → 1** |
| `GpuGmuGxReg` / `GpuGmuGxHalt` | v51 と同じ |
| `GpuGmuStatus` / `GpuGmuKeepCx` / `GpuCxStatus` | 0 / 0 / 0 |
| HFI / idle の判定 / GBIF / クロック / SMMU の Disable / RSCC と各復元 | すべて 0 |

- 12 個とも、リセット直後の値は 0 だった（32 ビットのアドレス）。1 を書くと、すぐに 1 が読み返せた。
- 描画の設定（context）のレジスタ（GRAS、RB、PC、HLSQ、VFD、VPC、SP、TPL1）も、CPU から書いた値がそのまま読み返せた。§9.16 で [U] としていた点は、ADDR_MODE については解決した。
- 元に戻していないが、後片付け（slumber、rpmh_stop、CX OFF）はすべて通った。次の実行の最初に読む元の値が 0 に戻っていれば、GX を切ると設定が消えることも確かめられる（毎回再起動するので、その確認は再起動のあとになる）。
- これで `hw_init` の冒頭（OOB → GBIF の halt の解除 → SECVID → ADDR_MODE）を、Linux と同じ順に実行できた。次は `a6xx_set_hwcg(gpu, true)`（クロックゲーティングの表）になる。

### 9.17 hwcg（ハードウェアのクロックゲーティング）の計画（本家 Linux の事実、2026-11-12）

`hw_init` では ADDR_MODE の直後に `a6xx_set_hwcg(gpu, true)` を呼ぶ（`a6xx_gpu.c` 1240）。中身は `a6xx_gpu.c` 641〜721。

**A635 で使う値**（`a6xx_catalog.c` 1124〜1137、chip_ids 0x06030500）：
- `.hwcg = a660_hwcg`（コメントにも「a635 と a660 用」とある）、`.gmu_cgc_mode = 0x00020202`
- A630・A610・A612・A702・A615 の系列・A7xx のどれでもないので、`clock_cntl_on = 0x8AA8AA82`、`cgc_delay = 0x10111`、`cgc_hyst = 0x5555`
- A610 の系列でも A7xx でもないので、SP のクロックを止めてから表を書く手順が入る

**Linux の順番**（`state = true` のとき）：
1. GMU 側の 3 つ（`gmu_write`）：
   - `GPU_GMU_AO_GMU_CGC_MODE_CNTL`（dword 0x23B09）← 0x00020202
   - `GPU_GMU_AO_GMU_CGC_DELAY_CNTL`（0x23B0A）← 0x00010111
   - `GPU_GMU_AO_GMU_CGC_HYST_CNTL`（0x23B0B）← 0x00005555
2. `RBBM_CLOCK_CNTL`（0x0AE）を読む。もう 0x8AA8AA82 なら、ここで終わる（v49 で読んだ値は 0 なので、先へ進む）。
3. `GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL`（dword 0x1A880）の bit0 を 0 にする（`gmu_rmw(…, 1, 0)`、SP のクロックを止める）。
4. 下の表の 49 個を、表の順に書く。
5. 同じレジスタの bit0 を 1 に戻す（`gmu_rmw(…, 0, 1)`、SP のクロックを戻す）。
6. `RBBM_CLOCK_CNTL` ← 0x8AA8AA82。

**レジスタの場所**：
- dword 0x23B09〜0x23B0B は GMU のページ B（0x03D8E000、dword 0x23800〜0x23BFF）にある。このページはもう割り当ててある。
- dword 0x1A880 は物理アドレス 0x03D6A200（ページ 0x03D6A000）。**今は割り当てていない新しいページ**。隣の 0x1A881 は `GMU_GX_SPTPRAC_POWER_CONTROL`。
- 表の 48 個と `RBBM_CLOCK_CNTL` は GPU の先頭ページ（0x03D00000）。v48 から割り当てているページと同じ。
- `RBBM_ISDB_CNT`（0x533）だけはページ 0x03D01000（ページ内 0x4CC）。

**表 `a660_hwcg`**（順番どおり。dword ← 値）：

| # | 名前 | dword | 値 |
|---|---|---|---|
| 0 | CLOCK_CNTL_SP0 | 0x0B0 | 0x02222222 |
| 1 | CLOCK_CNTL2_SP0 | 0x0B4 | 0x02222220 |
| 2 | CLOCK_DELAY_SP0 | 0x0B8 | 0x00000080 |
| 3 | CLOCK_HYST_SP0 | 0x0BC | 0x0000F3CF |
| 4 | CLOCK_CNTL_TP0 | 0x0C0 | 0x22222222 |
| 5 | CLOCK_CNTL2_TP0 | 0x0C4 | 0x22222222 |
| 6 | CLOCK_CNTL3_TP0 | 0x0C8 | 0x22222222 |
| 7 | CLOCK_CNTL4_TP0 | 0x0CC | 0x00022222 |
| 8 | CLOCK_DELAY_TP0 | 0x0D0 | 0x11111111 |
| 9 | CLOCK_DELAY2_TP0 | 0x0D4 | 0x11111111 |
| 10 | CLOCK_DELAY3_TP0 | 0x0D8 | 0x11111111 |
| 11 | CLOCK_DELAY4_TP0 | 0x0DC | 0x00011111 |
| 12 | CLOCK_HYST_TP0 | 0x0E0 | 0x77777777 |
| 13 | CLOCK_HYST2_TP0 | 0x0E4 | 0x77777777 |
| 14 | CLOCK_HYST3_TP0 | 0x0E8 | 0x77777777 |
| 15 | CLOCK_HYST4_TP0 | 0x0EC | 0x00077777 |
| 16 | CLOCK_CNTL_RB0 | 0x0F0 | 0x22222222 |
| 17 | CLOCK_CNTL2_RB0 | 0x0F4 | 0x01002222 |
| 18 | CLOCK_CNTL_CCU0 | 0x0F8 | 0x00002220 |
| 19 | CLOCK_HYST_RB_CCU0 | 0x100 | 0x00040F00 |
| 20 | CLOCK_CNTL_RAC | 0x104 | 0x25222022 |
| 21 | CLOCK_CNTL2_RAC | 0x105 | 0x00005555 |
| 22 | CLOCK_DELAY_RAC | 0x106 | 0x00000011 |
| 23 | CLOCK_HYST_RAC | 0x107 | 0x00445044 |
| 24 | CLOCK_CNTL_TSE_RAS_RBBM | 0x108 | 0x04222222 |
| 25 | CLOCK_MODE_VFD | 0x111 | 0x00002222 |
| 26 | CLOCK_MODE_GPC | 0x114 | 0x00222222 |
| 27 | CLOCK_DELAY_HLSQ_2 | 0x117 | 0x00000002 |
| 28 | CLOCK_MODE_HLSQ | 0x11B | 0x00002222 |
| 29 | CLOCK_DELAY_TSE_RAS_RBBM | 0x109 | 0x00004000 |
| 30 | CLOCK_DELAY_VFD | 0x112 | 0x00002222 |
| 31 | CLOCK_DELAY_GPC | 0x115 | 0x00000200 |
| 32 | CLOCK_DELAY_HLSQ | 0x11C | 0x00000000 |
| 33 | CLOCK_HYST_TSE_RAS_RBBM | 0x10A | 0x00000000 |
| 34 | CLOCK_HYST_VFD | 0x113 | 0x00000000 |
| 35 | CLOCK_HYST_GPC | 0x116 | 0x04104004 |
| 36 | CLOCK_HYST_HLSQ | 0x11D | 0x00000000 |
| 37 | CLOCK_CNTL_TEX_FCHE | 0x120 | 0x00000222 |
| 38 | CLOCK_DELAY_TEX_FCHE | 0x121 | 0x00000111 |
| 39 | CLOCK_HYST_TEX_FCHE | 0x122 | 0x00000000 |
| 40 | CLOCK_CNTL_UCHE | 0x10B | 0x22222222 |
| 41 | CLOCK_HYST_UCHE | 0x110 | 0x00000004 |
| 42 | CLOCK_DELAY_UCHE | 0x10F | 0x00000002 |
| 43 | ISDB_CNT | 0x533 | 0x00000182 |
| 44 | RAC_THRESHOLD_CNT | 0x044 | 0x00000000 |
| 45 | SP_HYST_CNT | 0x042 | 0x00000000 |
| 46 | CLOCK_CNTL_GMU_GX | 0x118 | 0x00000222 |
| 47 | CLOCK_DELAY_GMU_GX | 0x119 | 0x00000111 |
| 48 | CLOCK_HYST_GMU_GX | 0x11A | 0x00000555 |

名前の頭の `RBBM_` は省いた。dword は `a6xx.xml` で A6XX の変種に当たるもの。

**提案（段 C、例：bit17、合わせて `0x3FE80`、bit16 が前提）**：段 B のあと、OOB を解除する前に行う。
- 書く前に、次の元の値を全部読んで記録する：GMU の CGC の 3 つ、`RBBM_CLOCK_CNTL`、`SPTPRAC_CLOCK_CONTROL`、表の 49 個。
- `RBBM_CLOCK_CNTL` がもう 0x8AA8AA82 なら、Linux と同じく何も書かない（記録だけ）。
- 書く順番は Linux と同じ 1 → 3 → 4 → 5 → 6。どの書き込みの前にも breadcrumb と GX の確かめをし、書いたらすぐ読み返す。
- **3 で SP のクロックを止めたあとに途中で止まった場合も、必ず 5（bit0 を 1 に戻す）を行ってから** OOB の解除と slumber に進む。止める理由は、breadcrumb の失敗、GX の確かめの失敗、読み返しの違いのどれでも同じ。
- 読み返しが違ったら、表のそれ以降と 6 は書かない。表の中には予約ビットがあって、書いた値どおりに読めないものがあるかもしれない [U]。最初の実行では、違ったところで止めて、その値を見る。
- **元に戻す処理**：
  - GPU 側（表と `RBBM_CLOCK_CNTL`）と `SPTPRAC_CLOCK_CONTROL` は GX 側なので、GX を切ると消える [I]。Linux も戻していない。
  - GMU の CGC の 3 つは `GPU_GMU_AO`（常時 ON の領域）にあり、GX を切っても残る可能性が高い [I]。後片付けで元の値に戻すのがよい（bit10 の GBIF QSB と同じ扱い）。戻せなかったら `KeepCx` にする。
- **ページ**：0x03D6A000 を新しく読み書き可能で割り当てる。GPU のページは、先頭（0x03D00000）と 0x03D01000 の 2 枚。

**診断**（案）：`GpuGmuGxHwcg` = `{Attempted, Reason, Gdscr, GxHm, Count, Status, Skipped, CgcBefore[3], CgcAfter[3], ClockCntlBefore, ClockCntlAfter, SptpracBefore, SptpracAfterClear, SptpracAfterSet, Before[49], After[49], CgcRestored[3], CgcRestoreStatus}`。
- Count は書いて読み返した数（CGC の 3 個 + 表の 49 個 + `RBBM_CLOCK_CNTL` の最大 53。SPTPRAC の 2 回は別に記録する）。
- Reason は v48 の 0〜6 に、7 = 読み返しが違う、8 = 前の段が終わっていない、9 = SP のクロックを戻せなかった、を足す。
- 9 のときは、GX が切れるまで SP のクロックが止まったままになるだけなので、後片付けは続ける [I]。

**机上の試験**：
- shim に、GMU のページ 0x03D6A000、GPU の 0x03D01000 の受け口と、SPTPRAC の bit0 の書き込みの記録を足す。
- 試すこと：
  - 53 個の書き込みの順番と値
  - `RBBM_CLOCK_CNTL` = 0x8AA8AA82 のとき何も書かないこと
  - 表の途中で止まっても SPTPRAC の bit0 が 1 に戻ること
  - CGC の 3 つが後片付けで元の値に戻ること

### 9.17.1 段C（hwcg）の実装（実機未実行）

bit17 `EPI_GMU_PROBE_HWCG=0x20000` を追加した。bit16とその全前提が必要で、`0x3FE80` で段B成功後、同じOOBの解除前に実行する。前提不足はワンショット解除・flush後にstatus 6で拒否する。既存の`0x1FE80`までの設定手順は変更していない。

§9.17の提案に合わせ、まずCGC 3個、CLOCK_CNTL、SPTPRAC、表49個の全元値を保存する。CLOCK_CNTLが既に0x8AA8AA82ならSkipped=1としてCGCを含む書き込みを省略する。それ以外はCGC 3個→SPTPRAC bit0クリア→表49個→SPTPRAC bit0セット→CLOCK_CNTLの順とし、各書き込み直後に読み返し、全ビット一致を求める。元値保存中と通常書き込み前には、breadcrumb保存・flushとGX確認を行う。mapはGPU先頭、GPU+0x1000、GPU+0x6A000の3ページだけで、全経路で解除する。

SPTPRACのクリアを試みたあとは、どのエラーでも必須のbit0セット処理を通ってからOOB解除・slumberへ進む。復帰用breadcrumb/確認が失敗した場合も、この復帰書き込みは省略しない。SPTPRACはその時点の値を読み、bit0だけ変更して書く。復帰読み戻しが一致しなければReason 9、それ以外の途中エラーでは元のReasonを保持する。異常があった場合は残りの表と最後のCLOCK_CNTLを書かない。GX側の元値復元はしない。

CGCは実際に書いた個数（Countの先頭最大3個）を、既存のreset/idle確認とSMMU Disableが成功したあと、クロック復元の前に元へ戻す。各項目のbreadcrumbが失敗しても復元書き込み自体は行い、3個すべての復元を試みる。読み戻しまたは記録保存に失敗した場合はstatus 8 / KeepCxとする。既存の安全確認やSMMU Disableで先にKeepCxとなった場合は、CGCもその時点の状態を保持する。hwcg設定だけの失敗で安全な後片付けが成功すればstatus 13。

`GpuGmuGxHwcg` はREG_BINARY・480バイト（120 DWORD）で、§9.17の提案どおり `{Attempted, Reason, Gdscr, GxHm, Count, Status, Skipped, CgcBefore[3], CgcAfter[3], ClockCntlBefore, ClockCntlAfter, SptpracBefore, SptpracAfterClear, SptpracAfterSet, Before[49], After[49], CgcRestored[3], CgcRestoreStatus}`。CGCの順序はMODE、DELAY、HYST。CountはCGC・表・CLOCK_CNTLの読み戻し完了数（不一致も含む最大53）で、SPTPRACの2回は含めない。Attemptedは最初のCGC書き込み直前に1、Skippedは既設定時だけ1。Status/CgcRestoreStatusはNTSTATUS、未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。Reasonは0〜8が従来と同じ、9はSPクロック復帰失敗。cleanup/keepの両経路とCGC復元直後に保存する。

| breadcrumb | 内容 |
|---|---|
| 0x840〜0x875 | 全54個の元値保存前のflush・GX確認（0x840はmap前にも使用） |
| 0x880〜0x882 | CGC 3個の設定 |
| 0x883 | SPTPRAC bit0クリア |
| 0x890〜0x8C0 | 表49個の設定 |
| 0x8C1 | 必須のSPTPRAC bit0セット |
| 0x8C2 | CLOCK_CNTL設定 |
| 0x8D0〜0x8D2 | CGC元値復元 |

shimに2ページの受け口、SPTPRACとhwcgの書き込み記録、書き込み無視の注入を追加。ホスト試験に全55書き込み（SPTPRACを含む）の順序と値、既設定時の省略、各表項目のbreadcrumb失敗後のSP復帰、読み戻し不一致、復帰breadcrumb失敗、SP復帰失敗、元値保存中の停止、CGC復元と復元失敗を追加した。表49組のレジスタと値は§9.17との機械的照合で一致を確認した。試験・ビルド・署名・配置・実機実行は未実施。

#### 9.17.2 結果（v54）

**結果（2026-11-12、v34-akat、Epinephrine v54 / 20.0.2.54、`EpiGpuProbe=0x3FE80`、23:25 の再起動後）：途中で停止したが、安全に後片付けまで通った** [P]。

- 全体：
  - `GpuGmuStatus=13`（E_GX）、NtStatus 0xC0000185。
  - KeepCx 0。teardown、slumber、CX の後片付けはすべて 0。
  - CGC の復元は 0／0／0 に戻り、`CgcRestoreStatus=0`。
  - 機器が止まることはなく、WinRM も生きていた。
- `GpuGmuGxHwcg`（480 B）：
  - Attempted 1、Reason 9、Gdscr 0xF8222800、GxHm 0、Count 23、Skipped 0。
  - CGC：元値は 0／0／0、書き込み後は 0x00020202／0x00010111／0x00005555（一致）。
  - `RBBM_CLOCK_CNTL` の元値は 0。最後の書き込みには進んでいない（After は 0xFFFFFFFF）。
  - **SPTPRAC_CLOCK_CONTROL（0x03D6A200）は、元値、bit0 クリア後、bit0 セット後のどれも 0**。
  - 表の元値 49 個はすべて 0。
  - 表 #0〜#18 は書いた値と一致した。
  - **表 #19 `CLOCK_HYST_RB_CCU0`（dword 0x100）は 0x00040F00 を書いて 0x00000F00 が返った**。bit18 だけが落ちた。#20 以降は書いていない。
- Count 23 の内訳は、CGC 3 個と表 #0〜#19 の 20 個。
- 停止の順番：
  1. 表 #19 の不一致で Reason 7 になる。
  2. 必須の SP 復帰に進む。
  3. SPTPRAC は bit0 をセットしても 0 のままなので、Reason が 9 に上書きされた。
  - 最初の原因は #19 で、Reason 9 は 2 番目の異常 [P]。
- 解釈：
  - SPTPRAC：アドレスは Linux の GMU の基点（0x03D6A000）+ 0x80×4 なので正しい。Linux は a660 系でも `gmu_rmw` で書くが、読み戻しはしない。元値も 0 なので、この GPU ではこのレジスタは読むと 0 で、書いても値が残らない（RAZ/WI）可能性が高い [I]。
  - #19：Linux も読み戻しはしない。a660_hwcg は A660 用の表を流用しているので、A635 では bit18 のフィールドが実装されていない予約ビットの可能性がある [U]。
  - 「書いた値と全ビットが一致する」という判定は、hwcg の表には厳しすぎる [I]。

### 9.17.2 v55向け読み戻し判定の緩和（実機未実行）

SPTPRACのbit0クリア・セットは書き込みと直後の読み戻し記録だけにし、値の不一致を失敗にしない。表49個も読み戻しをAfter[]へ記録するが比較せず、不一致だけでは止めずに最後まで書く。breadcrumb・GX確認の失敗時は従来どおり通常の書き込みを中止し、SP停止を試みていれば必須のSP再開書き込みを行ってからOOB解除・slumberへ進む。

SP再開のbreadcrumb/GX確認で後から失敗しても、既に失敗していれば最初のStatus/Reasonを保持する。最初の失敗がSP再開時のbreadcrumb/GX確認なら、その失敗を返す。SPTPRACの読み戻しだけによるReason 9は今回から発生しない（番号は従来の記録解読用に予約）。CGC 3個、最後のCLOCK_CNTL、後片付けのCGC復元の比較は厳密なまま。

`GpuGmuGxHwcg`の480バイトの形式、Count、プローブ指定`0x3FE80`は変更しない。読み戻し不一致だけでほかの失敗がなければReason/Statusは0、Countは53となり、不一致は各Afterの生値で確認する。

ホスト試験を、各表項目の不一致でも全項目とCLOCK_CNTLまで進むこと、SPTPRACのクリア・セット不一致を記録して続けること、先行するbreadcrumb失敗を後続のSP読み戻しで上書きしないことに更新した。CGCとCLOCK_CNTLの不一致は引き続き失敗するケースも追加した。試験・版番号変更・ビルド・署名・配置・実機実行は未実施。

#### 9.17.3 結果（v55）

**結果（2026-11-12、v34-akat、Epinephrine v55 / 20.0.2.55、`EpiGpuProbe=0x3FE80`、23:46 の再起動後）：成功** [P]。**hwcg を ON にしたまま、GX の OFF と後片付けまで通った。**

- 全体：
  - `GpuGmuStatus=0`、NtStatus 0、KeepCx 0、`GpuCxStatus=0`。
  - CGC の復元は 0／0／0 に戻り、`CgcRestoreStatus=0`。
- `GpuGmuGxHwcg`：
  - Attempted 1、Reason 0、Status 0、Count 53、Skipped 0、Gdscr 0xF8222800、GxHm 0。
  - CGC：元値は 0／0／0、書き込み後は 0x00020202／0x00010111／0x00005555（一致）。
  - **`RBBM_CLOCK_CNTL` は 0 から 0x8AA8AA82 になり、読み戻しも一致した。**
  - SPTPRAC の 3 回の読み戻しは、v54 と同じですべて 0。
  - 表の元値 49 個はすべて 0。
- 表の 49 個のうち、47 個は書いた値と一致した。一致しなかったのは 2 個：

  | # | レジスタ（dword） | 書いた値 | 読み戻し | 落ちたビット |
  |---|---|---|---|---|
  | 19 | `CLOCK_HYST_RB_CCU0`（0x100） | 0x00040F00 | 0x00000F00 | bit18 |
  | 43 | `RBBM_ISDB_CNT`（0x533） | 0x00000182 | 0 | 全部 |

  - #19 は v54 と同じ結果で、再現した。
  - #43 は、書き込み専用か、デバッグ用（ISDB）で読み出せないレジスタの可能性がある [U]。
- これで `a6xx_set_hwcg(true)` に相当する処理を終えた。

### 9.18 次の段階：hwcg の後の単純な設定（stage D の計画）

出典は Linux master の `a6xx_gpu.c` の `hw_init`（hwcg の直後から `adreno_hw_init` の前まで）、`a6xx_catalog.c` の A635 の項目、`registers/adreno/a6xx.xml`、`a6xx_gmu.xml`、`a6xx_perfcntrs.xml`。事実の確認だけに使った。

#### 9.18.1 A635 での分岐

A635（chip id 0x06030500）の family は `ADRENO_6XX_GEN4` [L]。このため、`hw_init` の分岐は次のとおりになる [L]。

| 判定 | A635 |
|---|---|
| `adreno_is_a650_family`（GEN3 か GEN4） | 真 |
| `adreno_is_a660_family`（GEN4） | 真 |
| `adreno_is_a640_family`、`a610_family`、`a7xx` | 偽 |
| `adreno_is_a660`（revn 660 だけ） | 偽 |
| `hw_apriv`（quirk `ADRENO_QUIRK_HAS_HW_APRIV`） | 真 |
| `prim_fifo_threshold` | 0x00200200 |
| `gmem` | 512 KB（ただし a650 系は GMEM 範囲の設定を飛ばす） |
| `uche_trap_base` | 0x1FFFFFFFFF000 |

#### 9.18.2 書き込みの一覧（Linux の順）

アドレスはすべて dword 単位で、GPU の窓（0x03D00000）からの位置。64 ビットのレジスタは lo、hi の順に 2 dword を書く。

| # | レジスタ | dword | 値 | 備考 |
|---|---|---|---|---|
| 1 | `RBBM_GBIF_CLIENT_QOS_CNTL` | 0x011 | 0x3 | a650 系。#21 で 0 に書き直す |
| 2 | `RBBM_PERFCTR_GPU_BUSY_MASKED` | 0x50B | 0xFFFFFFFF | |
| 3 | `UCHE_WRITE_RANGE_MAX` lo／hi | 0xE05／0xE06 | 0xFFFFFFC0／0x0001FFFF | trap_base + 0xFC0 |
| 4 | `UCHE_TRAP_BASE` lo／hi | 0xE09／0xE0A | 0xFFFFF000／0x0001FFFF | |
| 5 | `UCHE_WRITE_THRU_BASE` lo／hi | 0xE07／0xE08 | 0xFFFFF000／0x0001FFFF | |
| — | `UCHE_GMEM_RANGE_MIN／MAX` | — | 書かない | a650 系は飛ばす [L] |
| 6 | `UCHE_FILTER_CNTL` | 0xE18 | 0x804 | |
| 7 | `UCHE_CACHE_WAYS` | 0xE17 | 0x4 | |
| 8 | `CP_ROQ_THRESHOLDS_2` | 0x8C2 | 0x02000140 | a650 系 |
| 9 | `CP_ROQ_THRESHOLDS_1` | 0x8C1 | 0x8040362C | a650 系 |
| 10 | `CP_LPAC_PROG_FIFO_SIZE` | 0xB34 | 0x20 | a660 系。xml では A7XX の名前だが、Linux は a660 系にも書く |
| 11 | `CP_MEM_POOL_SIZE` | 0x8C3 | 128（0x80） | |
| 12 | `PC_DBG_ECO_CNTL` | 0x9E00 | 0x00200200 | prim_fifo_threshold |
| 13 | `CP_AHB_CNTL` | 0x98D | 0x1 | AHB の既定の応答を ERROR にする。**§9.18.4 の注意を参照** |
| 14 | `RBBM_PERFCTR_CNTL` | 0x500 | 0x1 | 性能カウンタを ON |
| 15 | `CP_PERFCTR_CP_SEL(0)` | 0x8D0 | 0 | `PERF_CP_ALWAYS_COUNT`=0 |
| — | `a6xx_set_ubwc_config` | — | 今回は書かない | 値が UBWC の設定データに依存する。別の段階にする |
| 16 | `RBBM_INTERFACE_HANG_INT_CNTL` | 0x01F | 0x401FFFFF | `(1<<30)|0x1FFFFF`（A635 は既定の分岐） |
| 17 | `UCHE_CLIENT_PF` | 0xE19 | 0x81 | `BIT(7)|1` |
| 18 | `TPL1_BICUBIC_WEIGHTS_TABLE(0..4)` | 0xB608〜0xB60C | 0、0x3FE05FF4、0x3FA0EBEE、0x3F5193ED、0x3F0243F0 | a650 系 |
| 19 | `GPU_GMU_AO_GPU_CX_BUSY_MASK` | 0x23B0E | 0xFF000000 | GMU 側（CX、AO） |
| 20 | `GMU_CX_GMU_POWER_COUNTER_SELECT_0` | 0x1F841 | 下位 8 ビットを 0x20（`BIT(5)`）に rmw | GMU 側（CX） |
| 20b | `GMU_CX_GMU_POWER_COUNTER_ENABLE` | 0x1F840 | 1 | GMU 側（CX）。slumber の前に既存の処理が 0 に戻す |
| — | `a6xx_set_cp_protect` | — | 今回は書かない | 表が大きい（a660_protect）。別の段階にする |
| 21 | `CP_CHICKEN_DBG` | 0x841 | 0x1 | a660 系（a690 以外） |
| 21b | `RBBM_GBIF_CLIENT_QOS_CNTL` | 0x011 | 0x0 | a660 系。#1 の 3 を上書き |
| — | `UCHE_CMDQ_CONFIG` | — | 書かない | a660／a663 だけ。A635 は対象外 [L] |
| 22 | `CP_APRIV_CNTL` | 0x844 | 0x6E | `BIT(6)|BIT(5)|BIT(3)|BIT(2)|BIT(1)`（hw_apriv、a6xx） |
| — | `RBBM_INT_0_MASK` | 0x038 | 今回は書かない | 割り込みの処理がまだないので 0 のまま |

この後の `adreno_hw_init`、SQE の命令の基点、リングバッファ、`CP_SQE_CNTL=1`、`cp_init`、zap は、メモリ（IOVA）と SQE ファームウェアが要るので、stage D の範囲外。

#### 9.18.3 マップするページ

| ページ（物理） | dword の範囲 | 中のレジスタ | 状態 |
|---|---|---|---|
| 0x03D00000 | 0x000〜0x3FF | 0x011、0x01F | stage C で使用中 |
| 0x03D01000 | 0x400〜0x7FF | 0x500、0x50B | stage C で使用中 |
| 0x03D02000 | 0x800〜0xBFF | 0x841、0x844、0x8C1〜0x8C3、0x8D0、0x98D、0xB34 | stage B（0x842）で使用 |
| 0x03D03000 | 0xC00〜0xFFF | 0xE05〜0xE0A、0xE17〜0xE19 | stage B（0xE00）で使用 |
| 0x03D27000 | 0x9C00〜0x9FFF | 0x9E00 | stage B（0x9E01）で使用 |
| 0x03D2D000 | 0xB400〜0xB7FF | 0xB608〜0xB60C | stage B（0xB601）で使用 |
| 0x03D7E000 | 0x1F800〜0x1FBFF | 0x1F840、0x1F841 | GMU。既存の処理で使用中 |
| 0x03D8E000 | 0x23800〜0x23BFF | 0x23B0E | GMU のページ B。既存 |

新しいページは要らない [I]。

#### 9.18.4 注意点

- **`CP_AHB_CNTL=1`（#13）：** Linux のコメントは「AHB の既定のスレーブ応答を ERROR にする」[L]。CPU から存在しないレジスタを読んだときにバスエラー（SError）になるかどうかは分からない [U]。今の診断は存在するレジスタしか読まないが、安全のため次のどちらかにすることを提案する。
  - 案 A：最初の実行では #13 だけ飛ばし、別のビットで後から足す。
  - 案 B：書くが、書いた後の GX 側の読み出しを一覧の値の読み戻しだけに限る。
- **読み戻し：** v54/v55 で、書いた値が読み戻せないレジスタがあると分かった。stage D の読み戻しは全部を診断用の記録だけにし、止めるのは breadcrumb、GX の確認、map の失敗だけにすることを提案する。0x011 は 2 回書くので、記録は 2 回分とる。
- **GMU 側（#19〜#20b）：** CX 側なので、GX を切っても消えない [I]。元の値を記録し、teardown で戻す。ENABLE（0x1F840）は既存の slumber の処理が 0 にするので、二重に戻さなくてよい。
- **GX 側の元の値：** hwcg と同じく、GX を切ると消えるので戻さない [I]。Linux も戻さない。
- **順番の条件：** stage C（bit17）が成功してから行う。bit18 は bit17 を前提にする。

#### 9.18.5 提案する実装の形

- プローブのビット：bit18（0x40000）。`EpiGpuProbe=0x7FE80`。bit17 の前提が足りなければ、既存のとおり status 6 で拒む。
- breadcrumb：元の値の読み出しは 0x900+i、書き込みは 0x940+i、GMU 側の復元は 0x9A0+i（範囲は実装の都合で決めてよい）。
- 記録（`GpuGmuGxCfg` など）：
  - Attempted、Reason、Status、Count、Gdscr、GxHm。
  - 書いた各項目の Before と After。64 ビットのものは lo と hi を別の項目にする。
  - GMU 側の元の値 3 個、復元後の値、復元の Status。
- 期待：Before はすべて 0（v55 の時点で GX は起動直後の状態）。After は書いた値になるが、一部は実装されていないビットや書き込み専用のため違う値になりうる [U]。

#### 9.18.6 stage D の実装（実機未実行）

bit18 `EPI_GMU_PROBE_GX_CFG=0x40000` を追加。bit17とその全前提を必要とし、`0x7FE80` でstage C成功後、同じOOBの解除前に実行する。前提不足はワンショット解除・flush後、ハードウェアに触る前にstatus 6で拒否する。CP_AHB_CNTLは案Aを採用し、元値の読み取りも書き込みも含めない。追加の試験ビットは今回は作らない。

§9.18.2の順序を保ち、64ビット値をlo・hiに分けた30項目を実装した。先頭、+0x1000、+0x2000、+0x3000、+0x27000、+0x2D000の既知の6ページを一時的にRW・非キャッシュでmapし、GMUの2ページは既存mapを使う。全元値を保存してから書き始める。各元値読み取り前と各書き込み前にbreadcrumb保存・flushとGX確認を行う。書いた直後の読み戻しは全項目とも診断専用で、不一致だけでは止めない。map、breadcrumb、GX確認の失敗でそれ以降の書き込みを止め、取得済みmapをすべて解除してOOB解除・slumberへ進む。stage Cが失敗した場合はstage Dを呼ばずReason 8を記録する。

SELECT_0は保存した元値の下位8ビットだけを0x20に置き換える。QOSは3と0の2項目として記録し、それぞれのBeforeは書き始める前の同じ元値、Afterはそれぞれの書き込み直後の値になる。GX側の元値は復元しない。

CX側のBUSY_MASK、SELECT_0、ENABLEは、実際に書いたものだけをreset/idle確認とSMMU Disable成功後、CGC・クロック復元の前に元へ戻す。各復元は読み戻しを厳密に比較し、失敗しても残るCX項目を試みる。いずれかの復元・breadcrumbに失敗すればstatus 8 / KeepCx。ENABLEの元値はstage D開始前の値を既存のPowerCounterOriginalにも引き継ぎ、slumberで一時値1を保存し直さない。slumberは従来どおりENABLEに0を書く。復元時にすでに元値なら値の確認だけにし、既存の後段のENABLE復元は重ねて行わない。初期値が非0なら、その初期値へ戻す。GX/idle確認やSMMU Disableで先にKeepCxとなった場合は、従来どおりCX設定も保持する。stage D自体の失敗でも安全な後片付けが成功すればstatus 13。

診断 `GpuGmuGxCfg` はREG_BINARY、536バイト（134 DWORD）：`{Attempted, Reason, Status, Count, Gdscr, GxHm, Writes[30], CxBefore[3], CxRestored[3], CxRestoreStatus, CxWritten}`。Writesの各項目は `{Reg, Value, Before, After}` の16バイト。RegはGPU窓からのdword、Valueは実際の要求値（SELECT_0もRMW後の値）、順序は§9.18.2からCP_AHB_CNTLを除いて展開した順。Countは読み戻しまで完了した項目数（最大30）、Attemptedは最初の書き込み直前に1。Reasonの0〜6は従来と同じ、8は前段未完了。Status/CxRestoreStatusはNTSTATUSで未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。CxBefore/CxRestoredの順序とCxWrittenのbit0〜2はBUSY_MASK、SELECT_0、ENABLE。CxWrittenは各書き込み直前に立てる。cleanup/keepの両経路とCX復元直後に保存する。

| breadcrumb | 内容 |
|---|---|
| 0x900〜0x91D | 30項目の元値保存（0x900はmap前にも使用） |
| 0x940〜0x95D | 同じ順序の書き込みと即時読み戻し |
| 0x9A0〜0x9A2 | CX 3項目の復元・確認 |

shim/ホスト試験に、30項目の順序、lo/hi、QOSの2記録、SELECTの上位ビット保持、不一致を無視した続行、各書き込み前のbreadcrumb失敗、元値記録中・mapの失敗、CXの復元と復元失敗、slumber後のENABLE二重書き込み回避を追加した。30組の定数と順序は仕様表との機械的照合で一致した。試験・版番号変更・ビルド・署名・配置・実機実行は未実施。UBWC、cp_protect、割り込みマスク、SQE、リングバッファ、CP起動は含めない。

#### 9.18.7 結果（v56）

**結果（2026-11-13、v34-akat、Epinephrine v56 / 20.0.2.56、`EpiGpuProbe=0x7FE80`、00:31 の再起動後）：成功** [P]。

- 全体：
  - `GpuGmuStatus=0`、NtStatus 0、KeepCx 0、`GpuCxStatus=0`。
  - hwcg（stage C）は v55 と同じく Reason 0、Count 53、`RBBM_CLOCK_CNTL=0x8AA8AA82`。
- `GpuGmuGxCfg`（536 B）：
  - Attempted 1、Reason 0、Status 0、Count 30、Gdscr 0xF8222800、GxHm 0。
  - CX 側 3 項目の元値は 0／0／0、復元後も 0／0／0、`CxRestoreStatus=0`、CxWritten 7。
- 30 項目のうち 28 項目は、書いた値が読み戻せた。違ったのは 2 項目：

  | # | レジスタ（dword） | 書いた値 | 読み戻し | 解釈 |
  |---|---|---|---|---|
  | 1 | `RBBM_PERFCTR_GPU_BUSY_MASKED`（0x50B） | 0xFFFFFFFF | 0x002F7FFF | 実装されているビットだけが残る [I] |
  | 10 | `CP_ROQ_THRESHOLDS_2`（0x8C2） | 0x02000140 | 0 | 読むと 0。書き込み専用か、この GPU にない [U] |

- 元の値が 0 でなかった項目（リセット直後の既定値と見られる [I]）：

  | # | レジスタ（dword） | 元の値 |
  |---|---|---|
  | 2、3 | `UCHE_WRITE_RANGE_MAX` lo／hi（0xE05／0xE06） | 0xFFFFFFC0／0x0001FFFF（Linux の値と同じ） |
  | 9 | `UCHE_CACHE_WAYS`（0xE17） | 0x00040000 |
  | 14 | `PC_DBG_ECO_CNTL`（0x9E00） | 0x00180000 |
  | 17 | `RBBM_INTERFACE_HANG_INT_CNTL`（0x01F） | 0x0FFFFFFF |

- #28（QOS を 0 に戻す 2 回目）の Before は 0。元の値は、どの書き込みよりも先に 30 項目まとめて読むので、#0 で書いた 3 は映っていない。
- これで、hwcg の後から `adreno_hw_init` の前までの単純な設定を、`CP_AHB_CNTL`、UBWC、cp_protect、割り込みマスクを除いて終えた。

### 9.19 次の段階：UBWC、cp_protect、CP_AHB_CNTL（stage E／F の計画）

出典は Linux master の `a6xx_gpu.c`（`a6xx_set_ubwc_config`、`a6xx_set_cp_protect`、`hw_init`）、`a6xx_gpu.h`（`A6XX_PROTECT_*`）、`a6xx_catalog.c`（`a660_protect`）、`drivers/soc/qcom/ubwc_config.c`、`include/linux/soc/qcom/ubwc.h`、`registers/adreno/a6xx.xml`。事実の確認だけに使った。

#### 9.19.1 UBWC の設定値

- `ubwc_config.c` で `qcom,sm7325` は `ubwc_3_1_hbb14`。`ubwc_enc_version` は UBWC_3_1（0x30010000）、`highest_bank_bit` は 14、flags は 0 [L]。sc7280 と qcm6490 も同じデータ [L]。
- `a6xx_set_ubwc_config` の中間値（A635 で計算した）[L]：

  | 名前 | 計算 | 値 |
  |---|---|---|
  | hbb | 14 − 13 | 1 |
  | hbb_hi／hbb_lo | hbb >> 2／hbb & 3 | 0／1 |
  | rgb565_predicator | 版 ≥ 4.0 | 0 |
  | level2_swizzling_dis | 3.1 なので LVL2 は有効 | 0 |
  | ubwc_mode | 版 == 1.0 | 0 |
  | amsbc | 版 ≥ 3.0 | 1 |
  | min_acc_len_64b | 版 == 1.0 | 0 |
  | uavflagprd_inv | a650 系 | 2 |
  | macrotile_mode | 版 ≥ 3.1 | 1 |

- 書き込み（Linux の順。A7XX だけの部分は飛ばす）：

  | # | レジスタ | dword | 式 | 値 |
  |---|---|---|---|---|
  | U1 | `RB_NC_MODE_CNTL` | 0x8E08 | amsbc<<4 \| hbb_lo<<1 | 0x12 |
  | U2 | `TPL1_NC_MODE_CNTL` | 0xB604 | hbb_lo<<1 | 0x2 |
  | U3 | `SP_NC_MODE_CNTL` | 0xAE02 | uavflagprd_inv<<4 \| hbb_lo<<1 | 0x22 |
  | U4 | `UCHE_MODE_CNTL` | 0xE01 | min_acc_len_64b<<23 \| hbb_lo<<21 | 0x00200000 |
  | U5 | `RBBM_NC_MODE_CNTL` | 0x534 | macrotile_mode | 0x1 |

- Linux では、`CP_PERFCTR_CP_SEL(0)` の直後（stage D の #16 と #17 の間）に呼ばれる [L]。

#### 9.19.2 cp_protect の値

- `CP_PROTECT_CNTL`（dword 0x84F）= ACCESS_PROT_EN（bit0）| ACCESS_FAULT_ON_VIOL_EN（bit1）| LAST_SPAN_INF_RANGE（bit3）= **0xB** [L]。
- 各項目の形 [L]：
  - `A6XX_PROTECT_NORDWR(reg, len)` = `(1<<31) | ((len & 0x3FFF) << 18) | (reg & 0x3FFFF)`。読み書きの両方を禁止する。
  - `A6XX_PROTECT_RDONLY(reg, len)` は bit31 を立てない。読むことは許す。
- `a660_protect` は 41 項目で、count_max は 48 [L]。書き方は次のとおり [L]：
  - 項目 0〜39 を `CP_PROTECT(0..39)`（dword 0x850〜0x877）に書く。値が 0 の項目は飛ばすが、a660 の表には 0 の項目はない。
  - 最後の項目 40 は `CP_PROTECT(47)`（dword 0x87F）に書く。CNTL の bit3 と組み合わせて「その番地から末尾まで」の意味になる。
  - `CP_PROTECT(40..46)`（0x878〜0x87E）は書かない。
  - xml の配列の長さは 32 だが、Linux は 48 個目まで使う。
- 書く値（Linux のマクロから機械的に計算した）：

  | 項目 | dword | 値 | 項目 | dword | 値 |
  |---|---|---|---|---|---|
  | 0 | 0x850 | 0x13FC0000 | 21 | 0x865 | 0x876C9624 |
  | 1 | 0x851 | 0x00140501 | 22 | 0x866 | 0x80449E60 |
  | 2 | 0x852 | 0x0BD0050B | 23 | 0x867 | 0x861C9E78 |
  | 3 | 0x853 | 0x8000050E | 24 | 0x868 | 0x873CA630 |
  | 4 | 0x854 | 0x80000510 | 25 | 0x869 | 0x8000AE02 |
  | 5 | 0x855 | 0x80000534 | 26 | 0x86A | 0x84BCAE50 |
  | 6 | 0x856 | 0x82080800 | 27 | 0x86B | 0x8000B604 |
  | 7 | 0x857 | 0x802008A0 | 28 | 0x86C | 0x8018B608 |
  | 8 | 0x858 | 0x809008AB | 29 | 0x86D | 0x8004BE02 |
  | 9 | 0x859 | 0x02B808DE | 30 | 0x86E | 0x857CBE20 |
  | 10 | 0x85A | 0x81340900 | 31 | 0x86F | 0x97FCD000 |
  | 11 | 0x85B | 0x89C8098D | 32 | 0x870 | 0xAFFCF000 |
  | 12 | 0x85C | 0x80040E00 | 33 | 0x871 | 0x7FFCFC00 |
  | 13 | 0x85D | 0x80300E03 | 34 | 0x872 | 0xFFFD8400 |
  | 14 | 0x85E | 0x830C3C00 | 35 | 0x873 | 0xFFFDA400 |
  | 15 | 0x85F | 0x7FFC3CC4 | 36 | 0x874 | 0x910DF400 |
  | 16 | 0x860 | 0x873C8630 | 37 | 0x875 | 0x01EDF844 |
  | 17 | 0x861 | 0x80008E00 | 38 | 0x876 | 0x8001F860 |
  | 18 | 0x862 | 0x80008E08 | 39 | 0x877 | 0x806DF887 |
  | 19 | 0x863 | 0x807C8E50 | 40 → (47) | 0x87F | 0x8001F8C0 |
  | 20 | 0x864 | 0x89FC8E80 | | | |

- 元の a660 の表の (reg, len) の組：項目 0 = RDONLY(0x00000, 0x4FF)、項目 11 = NORDWR(0x0098D, 0x272)、項目 31 = NORDWR(0x0D000, 0x5FF)、項目 40 = NORDWR(0x1F8C0, 0) など。全項目は Linux の `a660_protect_regs` のとおり。
- Linux では、GMU の電力カウンタの直後（stage D の #20b と #21 の間）に呼ばれる [L]。
- この保護は CP（SQE）が出すレジスタアクセスにだけ効く。CPU から AHB 経由で触る分には効かない [I]。CP はまだ動かしていないので、今書いても動作は変わらない [I]。

#### 9.19.3 CP_AHB_CNTL

- dword 0x98D に 0x1 を書く。Linux のコメントは「AHB の既定のスレーブ応答を ERROR にする」[L]。
- 意味が CPU 側のアクセスにも及ぶかどうかは分からない [U]。今の診断が読むのは、存在が確かめられたレジスタだけ。
- 別のビットにして、ほかの段階が全部通った後に、単独で 1 回だけ試すことを提案する。

#### 9.19.4 マップするページ

| ページ（物理） | dword の範囲 | 使う番地 | 状態 |
|---|---|---|---|
| 0x03D01000 | 0x400〜0x7FF | 0x534 | 使用中 |
| 0x03D02000 | 0x800〜0xBFF | 0x84F、0x850〜0x877、0x87F、0x98D | 使用中 |
| 0x03D03000 | 0xC00〜0xFFF | 0xE01 | 使用中 |
| 0x03D23000 | 0x8C00〜0x8FFF | 0x8E08 | stage B（0x8E05）で使用 |
| 0x03D2B000 | 0xAC00〜0xAFFF | 0xAE02 | stage B（0xAE01）で使用 |
| 0x03D2D000 | 0xB400〜0xB7FF | 0xB604 | 使用中 |

新しいページは要らない [I]。

#### 9.19.5 提案する実装の形

- **stage E（bit19 = 0x80000、`EpiGpuProbe=0xFFE80`）：** UBWC 5 個、`CP_PROTECT_CNTL`、protect 41 個の計 47 書き込み。
  - Linux の位置どおりに stage D の途中へ差し込むと、stage D の記録の形が変わる。stage D の直後にまとめて行うことを提案する。どちらも CP が動く前の静的な設定なので、順番による差はないと見ている [I]。
  - 読み戻しは記録だけにする。止めるのは breadcrumb、GX の確認、map の失敗だけ。
  - GX 側だけなので、元の値は戻さない（GX を切ると消える）[I]。
- **stage F（bit20、`EpiGpuProbe=0x1FFE80`）：** `CP_AHB_CNTL=1` だけを書く。
  - 書いた後に、stage D で書いたレジスタのいくつか（たとえば 0x011、0x500、0xE17）を読み、値が変わらず読めて SError が出ないことを確かめる。
- **書かないもの：** `RBBM_INT_0_MASK`（割り込みの処理がまだない）。
- stage F が通れば、`adreno_hw_init` の前の設定はすべて終わる。その次はメモリが要る段階に入る（SQE ファームウェアの読み込み、`CP_SQE_INSTR_BASE`、リングバッファ、`CP_SQE_CNTL=1`、`cp_init`）。

#### 9.19.6 stage E の実装（実機未実行）

bit19 `EPI_GMU_PROBE_STAGE_E=0x80000` を追加。bit18とその全前提が必要で、`EpiGpuProbe=0xFFE80` のとき、stage D成功後、同じOOBの解除前に実行する。前提不足はワンショット解除・flush後、ハードウェアアクセス前にstatus 6で拒否する。stage F（bit20）は未実装として同じくstatus 6で拒否し、CP_AHB_CNTLを読んだり書いたりしない。既存の`0x7FE80`までの設定手順と後片付けは維持する。

書く順序はUBWC 5個、CP_PROTECT_CNTL、保護表41個の計47個。保護表の項目0〜39は0x850〜0x877、最後の項目40は0x87Fへ書く。0x878〜0x87Eは元値読み取りにも書き込みにも含めない。定数と順序は§9.19.1/9.19.2の表との機械的照合で47組すべて一致した。

既知の6ページ（GPU窓の+0x1000、+0x2000、+0x3000、+0x23000、+0x2B000、+0x2D000）だけを一時的にRW・非キャッシュでmapする。全47個の元値を保存してから書き始める。各元値読み取り・各書き込みの前にはbreadcrumb保存・flushとGX確認を行い、各書き込み直後に同じレジスタを読み返す。読み戻しは診断だけで、不一致によって止めない。map、breadcrumb、GX確認の失敗時には後続のstage E書き込みを止め、取得済みmapをすべて解除して既存のOOB解除・slumber・resetへ進む。元値の復元はしない。stage D未完了ならstage Eには触らずReason 8を記録する。stage Eだけの失敗でも安全な後片付けが成功すればstatus 13、従来の安全確認や後片付けに失敗すればstatus 8 / KeepCx。stage DのCX復元、CGC復元、GX OFF確認の扱いは変えない。

追加診断 `GpuGmuGxStageE` はREG_BINARY、776バイト（194 DWORD）：`{Attempted, Reason, Status, Count, Gdscr, GxHm, Writes[47]}`。Writesの各項目は16バイトの `{Reg, Value, Before, After}`。RegはGPU窓からのdword、Valueは要求値で、順序はUBWC[5]、CP_PROTECT_CNTL、protect[41]。Attemptedは最初の書き込み直前に1、Countは読み戻し完了までの項目数（最大47）。Reasonは0=完了、1=未到達、2=OOB ACK失敗、3=GDSCR OFF、4=GX HM不成立、5=map失敗、6=breadcrumb失敗、8=前段未完了。StatusはNTSTATUS、未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。cleanup/keepの両経路で保存する。

| breadcrumb | 内容 |
|---|---|
| 0xA00〜0xA2E | 元値保存前のflush・GX確認（0xA00はmap前にも使用） |
| 0xA40〜0xA6E | 同じ順序での書き込みと即時読み戻し |

shim/ホスト試験に、47個の順序・値と全元値の保存、最後の保護項目の47番配置、40〜46番に触れないこと、各項目の読み戻し不一致でも最後まで進むこと、各書き込み前のbreadcrumb失敗、元値記録中・map・GX確認の失敗で書き込まないことを追加した。試験・版番号変更・ビルド・署名・配置・実機実行は未実施。割り込みマスク、stage F、SQE、リングバッファ、CP起動は含めない。

#### 9.19.7 結果（v57）

**結果（2026-11-13、v34-akat、Epinephrine v57 / 20.0.2.57、`EpiGpuProbe=0xFFE80`、00:49 の再起動後）：成功** [P]。

- 全体：
  - `GpuGmuStatus=0`、NtStatus 0、KeepCx 0、`GpuCxStatus=0`。
  - 前提の段階も v56 と同じ結果だった：
    - hwcg：Reason 0、Count 53、`RBBM_CLOCK_CNTL=0x8AA8AA82`。
    - stage D：Reason 0、Count 30、CX の復元 0。
- `GpuGmuGxStageE`（776 B）：
  - Attempted 1、Reason 0、Status 0、Count 47、Gdscr 0xF8222800、GxHm 0。
  - **47 項目すべてで、書いた値がそのまま読み戻せた。**
  - `CP_PROTECT_CNTL`（0x84F）、`CP_PROTECT(0..39)`、`CP_PROTECT(47)` は、元の値が 0 だった。
- UBWC の 5 個のうち 4 個は、元の値が 0 ではなかった（リセット直後の既定値と見られる [I]）：

  | レジスタ（dword） | 元の値 | 書いた値 |
  |---|---|---|
  | `RB_NC_MODE_CNTL`（0x8E08） | 0x2 | 0x12（amsbc を追加） |
  | `TPL1_NC_MODE_CNTL`（0xB604） | 0x2 | 0x2（同じ） |
  | `SP_NC_MODE_CNTL`（0xAE02） | 0x2 | 0x22（uavflagprd_inv を追加） |
  | `UCHE_MODE_CNTL`（0xE01） | 0x00200000 | 0x00200000（同じ） |
  | `RBBM_NC_MODE_CNTL`（0x534） | 0x1 | 0x1（同じ） |

  - 既定値の hbb_lo はすでに 1（highest bank bit 14）で、macrotile も有効になっていた。sm7325 向けの既定値と見られる [I]。
- これで、`adreno_hw_init` の前の設定で残っているのは `CP_AHB_CNTL`（stage F）と、割り込みのマスクだけになった。

#### 9.19.8 stage F の仕様（CP_AHB_CNTL）

- **ビット：** bit20（0x100000）、`EpiGpuProbe=0x1FFE80`。stage E（bit19）が成功していることを前提にする。前提を満たさなければ、既存のとおり拒否する。
- **場所：** stage E の直後で、OOB を解除する前。
- **手順：**
  1. breadcrumb を残し、GX の確認をする（ほかの段階と同じ）。
  2. `CP_AHB_CNTL`（dword 0x98D、ページ 0x03D02000）の元の値を読む。
  3. breadcrumb と GX の確認をしてから、0x1 を書き、すぐに読み戻す。
  4. 書いた後の確認として、次の 4 個を 1 個ずつ読む。読む前には毎回 breadcrumb を残す。どれも v56／v57 で値が分かっているレジスタ：

     | レジスタ | dword | 期待値（v56／v57） |
     |---|---|---|
     | `RBBM_GBIF_CLIENT_QOS_CNTL` | 0x011 | 0 |
     | `RBBM_PERFCTR_CNTL` | 0x500 | 1 |
     | `UCHE_CACHE_WAYS` | 0xE17 | 4 |
     | `CP_PROTECT_CNTL` | 0x84F | 0xB |

  5. 書いた後の読み出しで止まった場合は、breadcrumb で、どの読み出しで止まったかが分かるようにする。
- **判定：** 読み戻しと 4 個の確認は、記録するだけにする。値が違っていても止めない。止めるのは、breadcrumb、GX の確認、map が失敗したときだけ。
- **元の値：** GX 側なので戻さない（GX を切ると消える）[I]。
- **記録（例 `GpuGmuGxStageF`）：** Attempted、Reason、Status、Count、Gdscr、GxHm、Before、After、Check[4]。
- **breadcrumb の例：** 0xB00（開始）、0xB01（元の値を読む）、0xB02（書き込み）、0xB10〜0xB13（4 個の確認）。
- **期待：** After=1、Check は表の値のとおり。SError やハングは起きない [U]。

#### 9.19.9 stage F の実装（実機未実行）

bit20 `EPI_GMU_PROBE_STAGE_F=0x100000` の拒否を解除し、bit19とその全前提を必要とする`0x1FFE80`を追加した。stage E成功直後、同じOOBの解除前にstage Fを実行する。前提不足はワンショット解除・flush後、ハードウェアアクセス前にstatus 6で拒否する。stage E未完了ならstage Fを呼ばずReason 8を残す。`0xFFE80`までの手順は変更していない。

GPU窓の先頭4ページを一時的にRW・非キャッシュでmapする。0xB00を保存・flushしてGXを確認してからmapし、0xB01の保存・flushとGX再確認後にCP_AHB_CNTL（0x98D）の元値を読む。0xB02で同じ確認を行ってから1を書き、直後に同じレジスタを読み返す。続いて0xB10〜0xB13で1個ずつ保存・flushとGX確認を行い、QOS（0x011）、PERFCTR_CNTL（0x500）、CACHE_WAYS（0xE17）、CP_PROTECT_CNTL（0x84F）の順に読む。

AfterとCheckは診断専用で、期待値との差によって止めない。map、breadcrumb、GX確認に失敗した場合だけ残りのstage Fアクセスを止める。取得したmapはすべて解除し、従来どおりOOB解除・slumber・resetと後片付けへ進む。CP_AHB_CNTLの元値復元はしない。安全な後片付けが成功すればstage Fの失敗はstatus 13、従来の安全確認や復元に失敗すればstatus 8 / KeepCx。stage DのCX設定やCGCの復元は維持する。割り込みマスク、SQE、リングバッファ、CPの起動は追加しない。

`GpuGmuGxStageF` はREG_BINARY、48バイト（12 DWORD）：`{Attempted, Reason, Status, Count, Gdscr, GxHm, Before, After, Check[4]}`。Before/AfterはCP_AHB_CNTL、Checkは上記4個の順。Attemptedは1を書き込む直前に1、Countは書き込み後に読み終えたCheckの個数（0〜4、CP_AHB_CNTLの読み戻しは含めない）。Reasonは0=完了、1=未到達、2=OOB ACK失敗、3=GDSCR OFF、4=GX HM不成立、5=map失敗、6=breadcrumb失敗、8=前段未完了。StatusはNTSTATUS、未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。cleanup/keepの両経路で保存する。SErrorなどで戻らない場合の位置は、各アクセスの直前にflushしたbreadcrumbで確認する。

shim/ホスト試験に、stage FでだけCP_AHB_CNTL書き込みを許す仕組み、1回の書き込みと4個の確認、すべての読み戻し不一致を無視した続行、開始・元値・書き込み・各確認前のbreadcrumb失敗、map失敗、確認途中のGX状態不成立で残りを読まないことを追加した。試験・版番号変更・ビルド・署名・配置・実機実行は未実施。

#### 9.19.10 結果（v58）

**結果（2026-11-13、v34-akat、Epinephrine v58 / 20.0.2.58、`EpiGpuProbe=0x1FFE80`、09:58 の起動後）：成功** [P]。

- 全体：`GpuGmuStatus=0`、NtStatus 0、KeepCx 0、`GpuCxStatus=0`。stage E は v57 と同じく Reason 0、Count 47。
- `GpuGmuGxStageF`（48 B）：
  - Attempted 1、Reason 0、Status 0、Count 4、Gdscr 0xF8222800、GxHm 0。
  - **`CP_AHB_CNTL` は、元の値がすでに 1 だった。** 1 を書いた後の読み戻しも 1。値は変わっていない。
  - 書いた後に読んだ 4 個は、期待どおりだった：QOS 0、PERFCTR_CNTL 1、CACHE_WAYS 4、CP_PROTECT_CNTL 0xB。
  - SError やハングは起きなかった。
- 解釈：この GPU では、リセット直後から AHB の既定の応答が ERROR になっていると見られる [I]。これまでの段階も、この設定のもとで問題なく動いていた。
- これで、`adreno_hw_init` の前の設定は、割り込みのマスクを除いてすべて終えた。

### 9.20 次の大きな段階：SQE、リングバッファ、CP の起動（調査）

出典は Linux master の次のファイル。事実の確認だけに使った。

- `a6xx_gpu.c`：`hw_init` の後半、`a6xx_ucode_load`、`a6xx_ucode_check_version`、`a6xx_cp_init`、`a6xx_flush`、`fenced_write`、`a6xx_idle`、`a6xx_get_rptr`、`a6xx_zap_shader_init`
- `adreno_gpu.c`：`adreno_fw_create_bo`、`adreno_hw_init`、`adreno_idle`
- `adreno_gpu.h`：`PKT7`、`PM4_PARITY`
- `msm_gpu.h`、`msm_ringbuffer.h`
- `registers/adreno/{a6xx.xml, adreno_common.xml, adreno_pm4.xml}`
- `kodiak.dtsi`、`sm7325-nothing-spacewar.dts`

#### 9.20.1 Linux の順番（割り込みマスクの後）

1. `RBBM_INT_0_MASK` を書く（今は飛ばす）。
2. `adreno_hw_init`：
   - SCM で `qcom_scm_set_gpu_smmu_aperture(0)` を呼ぶ（使えるときだけ）[L]。CP が自分でページテーブルを切り替えるための設定で、今は要らないと見ている [I]。Windows には SCM の API がないので、どのみち呼べない。
   - リングとメモリの中身を初期化する（ソフトウェア側だけ）。
3. `CP_SQE_INSTR_BASE`（dword 0x830／0x831、64 ビット）に SQE の IOVA を書く。
4. `CP_RB_BASE`（0x800／0x801）にリングバッファの IOVA を書く。
5. `CP_RB_CNTL`（0x802）に `MSM_GPU_RB_CNTL_DEFAULT` を書く。hw_apriv なので `NO_UPDATE`（bit27）は立てない [L]。
6. `CP_RB_RPTR_ADDR`（0x804／0x805）に、RPTR の影の IOVA を書く（hw_apriv のとき）[L]。
7. `CP_SQE_CNTL`（0x808）= 1：SQE を動かす。
8. `a6xx_cp_init`：リングに `CP_ME_INIT` を置き、WPTR を進め、空になるまで待つ。
9. zap shader を読み込み、`CP_SET_SECURE_MODE 0` を送る。zap がなければ `RBBM_SECVID_TRUST_CNTL`（0xF400）= 0 を書く（§9.20.5）。
10. OOB GPU_SET を解除する。

#### 9.20.2 メモリに置くもの

| 名前 | 大きさ | 中身 | GPU から見た属性 |
|---|---|---|---|
| SQE | ファイルの大きさ − 4 | `a660_sqe.fw` の **先頭 4 バイトを飛ばした残り**（`adreno_fw_create_bo` は `&fw->data[4]` からコピーする）[L] | WC、GPU からは読み出し専用 |
| リングバッファ | 32 KB（`MSM_GPU_RINGBUFFER_SZ`） | PM4 のパケット | WC |
| RPTR の影 | 4 バイト × リング数（1） | CP が RPTR を書き込む | WC、**特権**（`MSM_BO_MAP_PRIV`）。APRIV（stage D で 0x6E）によって CP の特権アクセスになる [L] |
| memptrs | `struct msm_rbmemptrs` | rptr、fence など | 今回の最初の試験では使わない |

- 電話の純正イメージの `stock-rom/vx/firmware/a660_sqe.fw` は 40496 バイト [P]。
  - 先頭の 8 dword は 0x00000000、0x016DC094、0x0100278B、0x016DC071、0x88121F18、0x88131F98、0x88020001、0xA8020080。
  - 4 バイト飛ばした後の buf[0] は 0x016DC094。
  - `a6xx_ucode_check_version` は、a660_sqe.fw の版を調べない（最初から安全とみなす）[L]。
- `MSM_GPU_RB_CNTL_DEFAULT` = BUFSZ(ilog2(32768/8) = 12) | BLKSZ(ilog2(32/8) = 2) << 8 = **0x0000020C** [L]。BUFSZ は bit0〜5、BLKSZ は bit8〜13（`adreno_common.xml`）。

#### 9.20.3 SMMU（GPU 側のストリーム）

- DT の GPU のノードは `iommus = <&adreno_smmu 0 0x400>, <&adreno_smmu 1 0x400>` [L]。**CP（GPU 本体）のストリーム ID は 0 と 1**（mask 0x400）。GMU は 5 [L]。
- 今の `epi_smmu` は、SMR0 = ID 5／mask 0x400 だけを CB0 に向けている。sCR0 の USFCFG=1 なので、ID 0／1 のアクセスは一致する SMR がないとフォルトになる（G1c-1）[P]。
- 必要な追加（文章だけ。実装はユーザー）：
  - ID 0 と 1 を受ける SMR を足す。たとえば ID 0／mask 0x401 で 1 本、または ID 0 と ID 1 で 1 本ずつ。SMR は全部で 6 本ある [P]。
  - 向け先の CB は、GMU と同じ CB0 にしてページテーブルを共有するのがいちばん簡単 [I]。IOVA は GMU の領域（0x4000〜、0x60000000〜）と重ならない場所にする。
  - Linux は GPU 用に別のドメイン（CB）を使う。共有にするか分けるかは、ユーザーが決める。
  - RPTR の影は特権（PRIVILEGED）属性で張り、SQE は読み出し専用で張る。
- 番地の幅：ADDR_MODE=1（stage B）なので、CP は 64 ビットの番地を使う。今の CB0 の TCR は 32 ビットの IOVA 向けなので、IOVA は 4 GB 未満に置く [I]。

#### 9.20.4 CP_ME_INIT と WPTR

- パケットの形 [L]：
  - `PKT7(op, cnt)` = 0x70000000 | cnt | PM4_PARITY(cnt) << 15 | (op & 0x7F) << 16 | PM4_PARITY(op) << 23。
  - PM4_PARITY は、32 ビットを 4 ビットずつ XOR した値を n として (0x9669 >> n) & 1。
- `CP_ME_INIT` は opcode 0x48、本体は 8 dword [L]。ヘッダを計算すると **0x70C80008**。
- リングの最初の 9 dword：

  | 位置 | 値 | 意味（Linux のコメント） |
  |---|---|---|
  | 0 | 0x70C80008 | ヘッダ |
  | 1 | 0x0000002F | 有効にする項目のマスク |
  | 2 | 0x00000003 | 複数のハードウェアコンテキスト |
  | 3 | 0x20000000 | エラー検出 |
  | 4、5 | 0、0 | ヘッダダンプなし |
  | 6 | 0 | ワークアラウンドなし |
  | 7、8 | 0、0 | 埋め草 |

- WPTR を進める：
  - `CP_RB_WPTR`（0x807）に 9（dword 単位）を書く。
  - Linux は `fenced_write` を使う。書いた後に `GMU_AHB_FENCE_STATUS` を見て、フェンスに落とされていれば書き直す [L]。
  - OOB GPU_SET を取っている間は、フェンスは開いていると見ている [I]。
- 空になるまで待つ（`a6xx_idle`）[L]：
  1. RPTR の影（メモリ）が WPTR（9）と等しくなる。hw_apriv なので、レジスタではなく影を見る（`a6xx_get_rptr`）。
  2. `RBBM_STATUS`（0x210）が、bit0（CP_AHB_BUSY_CX_MASTER）以外すべて 0。
  3. `RBBM_INT_0_STATUS`（0x201）の bit23（RBBM_HANG_DETECT）が 0。
  4. GMU も idle（`a6xx_gmu_isidle`）。
- 失敗したときのために、`CP_RB_RPTR`（0x806）、`CP_HW_FAULT`（0x821）、`RBBM_INT_0_STATUS`、SMMU の FSR／FAR も記録することを提案する。

#### 9.20.5 zap shader と SECVID_TRUST_CNTL（大きな障害）

- Linux は、`cp_init` の後で zap shader（`a660_zap.mbn`）を SCM の PAS で TZ に読み込ませる [L]。
  - spacewar の DT では `qcom/sm7325/nothing/spacewar/a660_zap.mbn`。
  - 成功したら、リングで `CP_SET_SECURE_MODE 0`（ヘッダ 0x70E60001、本体 0）を送り、CP を安全モードから出す [L]。
- zap がない場合は、`RBBM_SECVID_TRUST_CNTL`（0xF400）= 0 を直接書く [L]。
  - ただし Linux のコメントは、推測が外れていればこのレジスタへのアクセスは塞がれ、すぐに権限違反が起きる、と書いている [L]。
- 量産の電話では TZ が zap を前提に設定しているはずなので、0xF400 は塞がれている可能性が高い [I]。GPU PDC を読んだときのように、電話がリセットされるおそれもある [U]。
- 純正イメージには `a660_zap.mdt`、`.b00`〜`.b02`、`.elf` がある [P]。しかし Windows から TZ の PAS を呼ぶ手段がない（qcscm は軽々しく戻さない方針）。
- **結論：最初の CP の試験は zap の手前（`cp_init` の idle 確認）で止める。0xF400 は読みも書きもしない。** zap は別の課題として切り分ける。
  - CP が安全モードのままでも、`CP_ME_INIT` の処理と RPTR の書き戻しはできるはず [U]。

#### 9.20.6 後片付け

- Linux の通常の停止（`a6xx_pm_suspend`）は、SQE を止めずに、バスを halt してから GX を落とす [L]。エラー回復のときだけ、`CP_SQE_CNTL=3` で SQE を止める [L]。
- 今の後片付けの順番は、バスの halt → slumber（GX OFF）→ SMMU の Disable → メモリの解放。こうすると、CP がメモリを触らなくなってから SMMU を外すことになるので、この順番を守る [I]。
- メモリ（SQE、リング、影）の解放は、SMMU を Disable した後に行う。Disable が失敗したら解放しない（既存の規則と同じ）。

#### 9.20.7 段階の提案

| 段階 | ビット | 中身 | CP は動くか |
|---|---|---|---|
| G-1 | bit21 | メモリ 3 つの確保と SMMU への map（ID 0／1 の SMR を追加）、SQE の中身のコピー、`CP_SQE_INSTR_BASE`、`CP_RB_BASE`、`CP_RB_CNTL`、`CP_RB_RPTR_ADDR` を書いて読み戻す | 動かさない |
| G-2 | bit22 | G-1 の後に `CP_SQE_CNTL=1`、ME_INIT を置いて WPTR=9、idle を待つ（タイムアウト付き） | 動かす |
| Z | 未定 | zap shader／SECVID_TRUST_CNTL | 別途調査 |

- G-1 は、SMMU の変更（ユーザーの担当）が中心になる。
- G-2 が成功すれば、CP の SQE が動き、GPU が初めて命令を実行したことになる。
- G-2 の前に、stage F（`CP_AHB_CNTL`）を済ませる。

#### 9.20.8 G-1の実装（実機未実行）

bit21 `EPI_GMU_PROBE_G1=0x200000` を追加した。stage Fと全前提を必要とし、`EpiGpuProbe=0x3FFE80` で実行する。前提不足はワンショット解除・flush後にstatus 6で拒否する。G-2（bit22）は未実装として拒否する。今回の範囲には、ユーザー確認によりSMMU・メモリだけでなくCPの7レジスタ設定も含めた。CP_SQE_CNTL、CP_RB_WPTR、ME_INIT、zap、SECVID_TRUST_CNTLは追加していない。

**SMMU：** `EpiSmmuConfigureGpuStreams()` は無効なコンテキストへのソフトウェア上の指定だけを行う。Enable時にGPU用SMR1（byte offset 0x804、ID 0／mask 0x401、値0x84010000）を追加し、S2CR1（0xC04、値0）でGMUと同じCB0・ページテーブルを共有する。ID 0/1とbit10付きの0x400/0x401を受け、GMUのID 5/0x405とは重ならない。GMUのSMR0は変更しない。既存の全SMR無効・全CB停止の前提確認も維持する。

EnableはSMR1/S2CR1の元値も保存し、S2CR1設定→TLB同期→CB0/GMU SMR有効化→GPU SMR有効化の順に進める。DisableはGPU SMR無効化→GMU SMR無効化→CB停止→TLB同期→GPU S2CRと既存設定の復元→CB/SMR保存値の復元まで成功してから所有権を解放する。追加の比較にも最初の不一致記録を使う。追加の無効化・復元失敗でもctx・ページテーブル・データ・CXを保持し、後でDisableを再試行できる。既存のGpuSmmuDisableWritesは11項目のままで、S2CR1などの追加項目は後述の別診断に残す。

**ファイルとメモリ：** `C:\Epinephrine\fw\a660_sqe.fw` を、GMUファイルと同じフォルダからCX窓に入る前に読む。ファイル長40,496、先頭DWORD 0、次のDWORD 0x016DC094を確認し、完全な読み取りに成功したときだけ進む。`GpuSqeFwStatus`はNTSTATUS、`GpuSqeFwLen`は完全に読み終えたファイル長。失敗時はGpuProbeStatus 8としてGPU_CC/CXの操作前に戻る。ファイルのソース用poolは別管理で、SQEを専用バッファへコピーした後はKeepCx時でも解放できる。

| 用途 | IOVA | 確保・mapサイズ | SMMU属性 | 初期化 |
|---|---|---|---|---|
| SQE | 0x70000000 | 0xA000（40KB） | READのみ、GPU読み出し専用 | file[4..40495]の40,492バイトをコピーし、末尾468バイトを0 |
| リング | 0x70010000 | 0x8000（32KB） | READ/WRITE | 全0 |
| RPTR影 | 0x70020000 | 0x1000（1ページ） | READ/WRITE/PRIVILEGED | 全0、先頭4バイトを影として使う |

今回は既存のEpiSmmuMapと整合させ、CPUとSMMUの両方をNormal non-cacheableとしている（LinuxのWCを新規導入しない）。物理メモリは既存と同じく連続・非ページ・36ビット未満。IOVAは4GB未満で、GmuPlanの全領域と3領域の追加前に重複検査する。既存領域に包含される場合も使い回さず拒否する。3領域はGMUと同じ所有リストに入り、部分的な確保/map失敗は通常のcleanupで取り消す。SMMU Disable失敗やGX OFF確認失敗では解放せず、成功時だけSMMUから外して解放する。

**レジスタ：** メモリとGPUストリームは既存のSMMU Enableより前に準備する。stage F成功後、OOB解除前にSQE_BASE lo/hi、RB_BASE lo/hi、RB_CNTL=0x20C、RB_RPTR_ADDR lo/hiの順に7個を設定する。全元値を先に読み、各アクセス前にbreadcrumb保存・flushとGX確認を行う。要求値の上位DWORDはすべて0。読み戻しは診断専用で、不一致だけでは止めない。前段未完了なら書かずReason 8を残す。CPは起動しない。元値の復元は行わず、GX OFF後にメモリを片付ける。

**診断：** `GpuGmuG1`はREG_BINARY・196バイト（49 DWORD）：`{Requested, MemoryStatus, RegisterStatus, Reason, Count, Gdscr, GxHm, SqeBytes, SqeFirst, SqeLast, Regions[3][6], Before[7], Value[7], After[7]}`。Regionsは上記SQE・リング・RPTRの順に `{Iova, PaddedSize, SmmuFlags, PhysLow, PhysHigh, Mapped}`。Mappedは解放前のスナップショットであり、成功終了後も割り当てが残る意味ではない。Countは完了したレジスタ書き込み数（最大7）、Reasonは0〜6が従来と同じ、8は前段未完了。NTSTATUSの未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。SQEのコピー後にバイト数と最初/最後のDWORDも保存する。メモリとIOVAの準備不成立に結果コード14（EPI_GMU_E_GPU_MEMORY）を追加した。確保失敗3、map失敗4、SMMU Enable失敗5、レジスタ設定だけの失敗13、後片付け失敗8は既存の扱いに従う。

`GpuSmmuGpuStreams`はREG_BINARY・36バイト：`{Configured, SmrOriginal, S2crOriginal, SmrReadback, S2crReadback, SmrDisabled, SmrRestored, S2crRestored, RestoreStatus}`。SMR1/S2CR1の元値、設定・無効化・復元の生値を記録する。RestoreStatusはGPUストリームを含むDisable全体のNTSTATUSで、未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。G1診断はmap完了時、cleanup/keep、Disable直後に保存する。

| breadcrumb | 内容 |
|---|---|
| 0x420 | SQEファイル読み込み前 |
| 0xC00 | G-1ファイル/IOVA計画 |
| 0xC01 | SQEコピー前 |
| 0xC10〜0xC16 | CP 7レジスタ元値読み取り前 |
| 0xC20〜0xC26 | CP 7レジスタ書き込み・即時読み戻し |
| 0x550 / 0x551 | GPU S2CR1設定 / SMR1有効化 |
| 0x552 / 0x553 / 0x554 | GPU SMR1無効化 / S2CR1復元 / SMR1元値復元 |

ホスト試験に、GPU用AP属性、SMRのID/maskと元値復元、GPU無効化・復元失敗時の保持、GPU有効化失敗時のrollback、IOVA衝突とファイル長/先頭の拒否、SQEの4バイト除外と末尾ゼロ、リング/影のゼロ、7書き込みの順序と不一致無視、breadcrumb停止、各確保とGPU map失敗時の解放、Disable失敗時に3バッファを保持するケースを追加した。試験・版番号変更・ビルド・署名・配置・実機実行は未実施。

#### 9.20.9 結果（v59）

**結果（2026-11-13、v34-akat、Epinephrine v59 / 20.0.2.59、`EpiGpuProbe=0x3FFE80`、10:18 の再起動後）：SMMU の Enable で停止。安全に後片付けまで通った** [P]。

- 全体：
  - `GpuGmuStatus=5`（EpiSmmuEnable の失敗）、NtStatus 0xC0000185（読み戻しの不一致）。
  - KeepCx 0。`GpuGmuDisableStatus=0`。`GpuCxStatus=16`（GMU の試験が成功しなかったときの値）。
  - GMU の CM3 は動かしていない。GX も ON にしていないので、stage F と G-1 のレジスタの段階には届いていない（stage F は Reason 1、G-1 の RegisterStatus は 0xC0000225、Before／After は未読の 0xFFFFFFFF）。
- メモリの準備は成功した：
  - MemoryStatus 0。SqeBytes 40492、SqeFirst 0x016DC094（ファイルの 2 dword 目）、SqeLast 0x0000008E。
  - 3 つの領域は、どれも確保して SMMU に張れた：

    | IOVA | 大きさ | flags | 物理アドレス |
    |---|---|---|---|
    | 0x70000000 | 0xA000 | 1（R） | 0x27FF9A000 |
    | 0x70010000 | 0x8000 | 3（RW） | 0x27FF92000 |
    | 0x70020000 | 0x1000 | 7（RW、特権） | 0x27FFE7000 |

- **止まった場所：** `GpuSmmuBadWrite` = step 0x551、オフセット 0x804（SMR1）、書いた値 0x84010000、**読み戻し 0**。
  - SMR1 への書き込みが受け付けられなかった。
- `GpuSmmuGpuStreams`：
  - SMR1 の元の値は 0。S2CR1 の元の値は 0x3F228BFF。
  - S2CR1 への 0 の書き込みは受け付けられた（読み戻し 0）。
  - Disable で SMR1 は 0、S2CR1 は 0x3F228BFF に戻った。RestoreStatus 0。
- 解釈：
  - SMR0 の 0x84000005（ID 5、mask 0x400）は、v30 以降ずっと書けている [P]。この値は DT の GMU の `iommus = <&adreno_smmu 5 0x400>` と同じ。
  - 今回の SMR1 は ID 0、mask 0x401 で、DT の GPU の 2 項目（`0 0x400` と `1 0x400`）を 1 本にまとめた値。DT にそのままの組はない。
  - SMMU の書き込みはハイパーバイザが検査しており、許可された（ID, mask）の組だけを通している可能性がある [U]。
- 次の候補（文章だけ。実装はユーザー）：
  - DT のとおりに、ID ごとに 1 本ずつ SMR を使う。SMR1 = 0x84000000（ID 0、mask 0x400）、SMR2 = 0x84000001（ID 1、mask 0x400）。S2CR1 と S2CR2 は CB0 に向ける。
  - これも通らない場合は、SMR の番号（index）が制限されている可能性もある。そのときは、SMR に書いて読み戻すだけの独立した試験（GMU も GX も動かさない、§9.5 の 0x20 と同じ形）で、ID と mask の組と index を 1 つずつ確かめる。

#### 9.20.10 GPUストリームをDTの2本へ分割（実機未実行）

v59のSMR1読み戻し不一致を受け、GPU用SMRを結合しない形に変更した。SMR1（byte offset 0x804）はID 0／mask 0x400、値0x84000000、SMR2（0x808）はID 1／mask 0x400、値0x84000001とする。S2CR1（0xC04）とS2CR2（0xC08）はどちらも0でCB0へ向ける。GMUのSMR0=0x84000005、GPUメモリのIOVA・属性・コピー、G-1の7レジスタ設定、プローブ指定0x3FFE80は変更していない。ハイパーバイザによる組やindexの制約が原因かどうかは、引き続き未確認。

Enable時には追加2組の元値をすべて保存し、S2CR1→S2CR2→既存のTLB同期・CB0/GMU有効化→SMR1→SMR2の順に進む。途中で失敗した場合は従来どおりDisableでrollbackする。DisableではGPU用SMR1/2の無効化を両方試み、1本でも確認に失敗したらCB0には進まず、ctx・テーブル・データ・CXを保持する。両方の無効化成功後はGMU SMR無効化→CB0停止→TLB同期→S2CR1/2と既存設定の復元へ進む。S2CRの読み戻し不一致があってももう一方の復元と既存の設定復元を試みる。すべて成功した場合だけ保存したSMR1/2へ戻して所有権を解放する。最初のWrite不一致の記録と、失敗時のKeepCx規則は維持する。

`GpuSmmuGpuStreams`（REG_BINARY）は36バイトから**64バイト（16 DWORD）**へ変更した。形式は `{Configured, SmrOriginal[2], S2crOriginal[2], SmrReadback[2], S2crReadback[2], SmrDisabled[2], SmrRestored[2], S2crRestored[2], RestoreStatus}`。各配列の[0]はindex 1、[1]はindex 2。Configured=1はこの2本を有効にする指定。未読値0xFFFFFFFF、未試行RestoreStatus=STATUS_NOT_FOUND、RestoreStatusはDisable全体の結果という扱いは従来どおり。GpuGmuG1の196バイトとGpuSmmuDisableWritesの11項目は変更しない。

| 操作 | index 1 のbreadcrumb | index 2 のbreadcrumb |
|---|---|---|
| S2CR設定（CB0へ） | 0x550 | 0x555 |
| SMR有効化 | 0x551 | 0x556 |
| SMR無効化 | 0x552 | 0x557 |
| S2CR元値復元 | 0x553 | 0x558 |
| SMR元値復元 | 0x554 | 0x559 |

ホスト試験を2本のID/maskとCB0経路の確認、相互・GMUとの非重複、両組の元値復元、各GPUストリームの無効化失敗時の保持、片方のS2CR復元不一致でも他方を試みること、S2CR1/2とSMR1/2の設定失敗時のrollbackに更新した。独立したSMR試験はまだ追加していない。試験・版番号変更・ビルド・署名・配置・実機実行は未実施。

#### 9.20.11 v60 実機結果（2026-11-13、0x3FFE80、GPU SMR 2本）

- 版 20.0.2.60（oem118）。端末は 10:30 に再起動してから実行した。机上試験では `smmu_test.cxx:142` の C4389 だけを直した（定数に `UL` を付けただけ）。
- **G-1 は最後まで成功した。**
  - EpiGpuProbeStep 0xFFFF、GpuGmuStatus 0、GpuGmuNtStatus 0、GpuSmmuBadWrite は 0 バイト。
  - GpuGmuKeepCx 0、GpuGmuDisableStatus 0、GpuCxStatus 0。デバイスは Started。
- `GpuSmmuGpuStreams`（64 B）：
  | 項目 | index 1 | index 2 |
  |---|---|---|
  | SMR 元の値 | 0 | 0 |
  | S2CR 元の値 | 0x3FAAABFF | 0x2AAE59FF |
  | SMR 読み戻し | **0x84000000** | **0x84000001** |
  | S2CR 読み戻し | 0 | 0 |
  | SMR 無効化後 | 0 | 0 |
  | SMR 復元 | 0 | 0 |
  | S2CR 復元 | 0x3FAAABFF | 0x2AAE59FF |
  - Configured 1、RestoreStatus 0。
  - SMR が無効な間の S2CR の元の値は起動ごとに変わる（v59 の S2CR1 は 0x3F228BFF）。意味のない残り値と見てよい [P]。
- 解釈：DT のとおりの（ID 0, mask 0x400）と（ID 1, mask 0x400）は受け付けられた。v59 で読み戻しが 0 になったのは、2 本をまとめた mask 0x401 の値だった。ハイパーバイザが DT の組だけを許しているという仮説と合う [P]。index 1 と 2 は使えた。
- `GpuGmuG1`（196 B）：
  - Requested 1、MemoryStatus 0、RegisterStatus 0、Reason 0、Count 7。
  - Gdscr 0xF8222800、GxHm 0。
  - SqeBytes 0x9E2C（40492）、SqeFirst 0x016DC094、SqeLast 0x8E。
  - 3 つの領域はすべて Mapped=1：
    | 領域 | IOVA | サイズ | 属性 | PA |
    |---|---|---|---|---|
    | SQE | 0x70000000 | 0xA000 | 1 | 0x2_7FFA1000 |
    | RB | 0x70010000 | 0x8000 | 3 | 0x2_7FF99000 |
    | RPTR | 0x70020000 | 0x1000 | 7 | 0x2_7FF98000 |
  - 7 個の CP レジスタ（SQE_INSTR_BASE lo/hi、RB_BASE lo/hi、RB_CNTL、RPTR_ADDR lo/hi）：
    - 書く前はすべて 0。
    - 書いた値は {0x70000000, 0, 0x70010000, 0, 0x20C, 0x70020000, 0}。
    - 読み戻しはすべて一致した。
- 次は G-2（§9.20.7）：CP_SQE_CNTL=1、ME_INIT 9 dword、WPTR=9、idle 待ち。仕様は文章だけで示し、実装はユーザーが行う。

#### 9.20.12 G-2の実装（実機未実行）

bit22 `EPI_GMU_PROBE_G2=0x400000` の拒否を解除し、G-1とその全前提を必要とする`EpiGpuProbe=0x7FFE80`を追加した。G-1のメモリ・7レジスタ設定成功後、同じOOB GPU_SETの解除前に実行する。前提不足はワンショット解除・flush後、ハードウェアアクセス前にstatus 6で拒否する。既存の`0x3FFE80`ではCPを動かさない。

CP起動前に、G-1の7個のAfterがValueと一致していること、3バッファが正しい属性でmap済みであること、RPTRの影とリング先頭9 DWORDとCP_RB_WPTRが0であることを確認する。G-1自体の読み戻しは従来どおり診断専用だが、G-2を指定した場合だけ実行前のベース設定確認に使う。不成立ならCPを起動せずReason 12を残す。GX用には既知の先頭ページと+0x2000の2枚だけを一時mapする。

CP_SQE_CNTL=1を書き、直後の読み戻しを保存する。続いてリング先頭に `{0x70C80008, 0x2F, 3, 0x20000000, 0, 0, 0, 0, 0}` をコピーし、メモリバリア後にCP_RB_WPTR=9を書く。SQEの読み戻しは診断専用。WPTRはメモリバリア後、GMU_AHB_FENCE_STATUS（GPU dword 0x23B13）のbit0を確認する。bit0が立つ間だけ、10 µs待ってGXを再確認してから同じWPTR=9を再送する。上限は200回の待ち（約2 ms、初回を含む書き込み最大201回）。bit0が消えなければReason 10 / STATUS_IO_TIMEOUTで止める。fence確認のハードウェア条件は[Linuxのfenced_writeとa6xx_flush](https://codebrowser.dev/linux/linux/drivers/gpu/drm/msm/adreno/a6xx_gpu.c.html#50)を事実として確認し、実装は既存プローブ用に新規記述した。

CPの完了待ちは100 µs間隔、最大10,000回（待ちの合計1秒）。各回でOOB/GXの前提を再確認し、RPTR影=9、RBBM_STATUSのbit0以外が0、RBBM_INT_0_STATUSのbit23=0、CX_BUSY_STATUSのbit23=0が同時に成立することを求める。最後の条件は[Linuxのa6xx_gmu_isidle](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_gmu.c)の条件と同じ。ハング検出bit23が立てばReason 11 / STATUS_IO_DEVICE_ERRORで打ち切り、ほかのbusyが残る場合はReason 9 / STATUS_IO_TIMEOUTとする。fence/idleのループ内ではレジストリへの書き込みをしない。

完了・失敗時には、まだ同じOOB中かつGX確認が通る場合に限りCP_RB_RPTR、WPTR、CP_HW_FAULT、RBBM_STATUS、INT_0_STATUS、CX_BUSYとRPTR影を保存する。SMMUのFSR/FAR/FSYNRも読み、値は生の診断として残す。CP_HW_FAULTやFSRの値だけによる追加判定はしない。診断用breadcrumb/GX確認で後から失敗しても、先に失敗していれば元のStatus/Reasonを保持する。

G-2の結果にかかわらず、既存のOOB解除→GBIF halt→PREPARE_SLUMBER→CM3 reset→rpmh_stop→GX OFF/idle確認→SMMU Disableの順に進む。SQEを止める値3やRBBMソフトリセットは追加しない。GX OFFやSMMU Disableで止まった場合はSQE・リング・影を含むメモリとCXを保持し、CPが使い得るメモリを解放しない。G-2だけが失敗し、それ以外の後片付けが成功した場合は新しいstatus 15（EPI_GMU_E_CP）。従来の段階やslumberの失敗はstatus 13、teardown失敗は引き続きstatus 8 / KeepCxが優先する。zap、SECVID_TRUST_CNTL、割り込みマスク、CP_SET_SECURE_MODEは追加しない。

`GpuGmuG2`はREG_BINARY・140バイト（35 DWORD）：`{Requested, Attempted, Reason, Status, SqeWritten, WptrWrites, FenceStatus, Polls, ShadowBefore, ShadowAfter, SqeBefore, SqeAfter, WptrBefore, WptrAfter, Rptr, RbbmStatus, IntStatus, CpHwFault, CxBusy, Gdscr, GxHm, FaultStatus, Fsr, FarLow, FarHigh, Fsynr, Packet[9]}`。Attempted/SqeWrittenはSQE=1の書き込み直前に1。WptrWritesは実際に試みたWPTR書き込み回数。Pollsは100 µsのidle待ち回数で、未到達なら0xFFFFFFFF。Packetはコピー後にリングから保存した9 DWORD。Status/FaultStatusはNTSTATUS、未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。Reason 0〜6は従来と同じ、8=G-1未完了、9=idle時間切れ、10=fence時間切れ、11=ハング検出、12=起動前のメモリ・ベース・初期ポインタ条件不成立。cleanup/keepの両経路で保存する。GpuGmuG1、GPUストリーム、既存HFI診断の形式は変更しない。

| breadcrumb | 内容 |
|---|---|
| 0xD00 | 起動前提確認・map前 |
| 0xD01 | 初期SQE/WPTR/影と空リングの確認 |
| 0xD02 | SQE=1の書き込み |
| 0xD03 | ME_INIT 9 DWORDコピー |
| 0xD04 | 最初のWPTR=9 |
| 0xD05 | fence確認・必要時のみWPTR再送 |
| 0xD06 | CP/GMU idle待ち |
| 0xD07 | 最終のGX診断読み取り |

shim/ホスト試験に、G-2だけでSQE/WPTR書き込みを許す仕組み、パケット・順序・正常完了、fenceによる再送と上限、RPTR/RBBM/GMUの各未完了条件、ハング検出、各breadcrumb失敗、元のエラーの保持、GX喪失後の診断中止、mapと起動前条件の不成立を追加した。全体の経路には、正常解放、CP時間切れ後の後片付け、G-1ベース不一致時に起動しないこと、GX OFF失敗・Disable失敗時のバッファ保持のケースを追加。ME_INITヘッダは§9.20.4のパリティ計算式との独立した計算で一致した。ホスト試験の実行・版番号変更・ビルド・署名・配置・実機実行は未実施。

#### 9.20.13 v61 実機結果（2026-11-13、0x7FFE80、G-2）

- 版 20.0.2.61（oem119）。端末は 11:01 に再起動してから実行した。
  - 机上試験の段階で直したのは、`GmuG2Start` の変数名 `far` → `faultAddr` だけ（`far` は Windows ヘッダーのマクロで、ビルドエラーになった）。処理は変えていない。
- **CP が起動し、ME_INIT を処理して idle になった。**
  - EpiGpuProbeStep 0xFFFF、GpuGmuStatus 0、GpuGmuNtStatus 0。
  - KeepCx 0、DisableStatus 0、CxStatus 0、FaultStatus 0。デバイスは Started。
- `GpuGmuG2`（140 B）：
  | 項目 | 値 |
  |---|---|
  | Requested / Attempted / Reason / Status | 1 / 1 / 0 / 0 |
  | SqeWritten / WptrWrites | 1 / 1（fence の再送なし） |
  | FenceStatus | 0 |
  | Polls | **0**（最初の 1 回目で idle） |
  | Shadow 前 → 後 | 0 → **9** |
  | SQE_CNTL 前 → 後 | 0 → 1 |
  | WPTR 前 → 後 | 0 → 9 |
  | RPTR（0x806） | 9 |
  | RBBM_STATUS | 0x1（bit0 CP_AHB_BUSY_CX_MASTER のみ） |
  | INT0_STATUS | 0（hang 検出なし） |
  | CP_HW_FAULT | 0 |
  | CX_BUSY（0x23B0C） | 0x71000000（bit23 GPUBUSYIGNAHB は 0） |
  | GX GDSCR / GxHm | 0xF8222800 / 0 |
  | Packet | {0x70C80008, 0x2F, 3, 0x20000000, 0, 0, 0, 0, 0}（書いたとおり） |
  - FSR は 0x400。これは FORMAT フィールドだけで、フォルトのビットは立っていない。FAR 0x1F634_67E73FD7 と FSYNR 0xF001B は以前の残り値 [P]（§9.10 以降と同じ見方）。
- G-1 部分は v60 と同じ値だった。SMR1/2 は再び受け付けられ、後片付けで元に戻った。
  - S2CR の残り値は今回も起動ごとに違っていた（0x3FAA8BFF、0x2ACE59FF）。
- 意味：
  - SQE のマイクロコードは、SMMU 経由で IOVA 0x70000000 から読み込まれて動いた。
  - CP は ring（0x70010000）から ME_INIT を取り出した。
  - CP は RPTR の shadow（0x70020000、特権バッファ）へ書き戻した。
  - Linux の `a6xx_cp_init` → `a6xx_idle` に当たるところまで、Windows 上で届いた。
- まだ残っていること：
  - zap shader と SECVID_TRUST_CNTL（§9.20.5）。0xF400 には触れない方針は変えていない。
  - zap なしでは GPU は secure mode から抜けられない [P]。そのため、次の段階で何ができるかは別に調べる必要がある。

### 9.21 cp_init の後（zap なしで何ができるか）

#### 9.21.1 Linux の流れ（cp_init の後）[L]

1. `a6xx_zap_shader_init` → `adreno_zap_shader_load(gpu, GPU_PAS_ID)`。TZ の PAS に zap を読み込ませる。`static bool loaded` で、読み込みは最初の 1 回だけ。
2. 成功したら、リングで `CP_SET_SECURE_MODE`（0x70E60001、本体 0）を送り、`a6xx_idle` を待つ。
3. zap が DT にない（-ENODEV）ときだけ、`RBBM_SECVID_TRUST_CNTL`（0xF400）= 0 を直接書く。
4. その後は通常の投入（`a6xx_submit`）になる。fence は次の 2 つで返す：
   - `PKT4 CP_SCRATCH(2)` に書く。
   - `CP_EVENT_WRITE(CACHE_FLUSH_TS | IRQ)` でメモリに書く。

#### 9.21.2 いまわかっていること

- G-2（v61）は secure mode のまま動いた。
  - 読み出し：SQE の命令と ring は SMMU 経由で読めた。
  - 書き込み：RPTR shadow（特権バッファ）には書けた。
- secure mode が何を制限するのかは、Linux のソースには書かれていない [U]。
  - KGSL の考え方では、secure mode 中の GPU は secure でない（non-secure）メモリへ書けない（保護されたコンテンツを漏らさないため）とされている [U]。
  - しかし RPTR shadow には書けた。shadow の書き込みは特別扱いの可能性もある [U]。
- したがって、**ふつうの CP の書き込みが secure mode で通るかを、まず実機で確かめる**のがいちばん安くて安全な次の一歩、と考える [I]。

#### 9.21.3 zap を入れる手段（整理）

| 手段 | 状態 |
|---|---|
| Windows から SCM の PAS を呼ぶ | qcscm は軽々しく戻さない方針。使わない |
| `RBBM_SECVID_TRUST_CNTL`（0xF400）を直接書く | 禁止（§9.20.5）。読みもしない |
| UEFI（Mu）の段階で zap を読み込む | 未調査 [U]。Qualcomm の UEFI に PIL／SCM のプロトコルがあるか、Mu-spacewar に入っているかを別に調べる |
| secure mode のまま進む | G-3 で、何が動くかを確かめる |

#### 9.21.4 G-3 の提案（bit23、0xFFFE80）

- G-2 が成功した後、同じ OOB の中で続けて行う。ME_INIT（dword 0〜8）の後ろに、パケットを足す。
- 使うバッファ：
  - 新しいメモリは確保しない。SMMU の変更も不要。
  - ring（IOVA 0x70010000、32 KB、RW、非特権）の **オフセット 0x7000（IOVA 0x70017000）** を、データ領域として借りる。
  - ring が使うのは先頭の 20 dword だけなので、重ならない。
- 準備：
  - データ領域の先頭 4 dword に、目印として 0xFFFFFFFF を書いておく（CPU から書く。メモリバリアを入れる）。
  - `CP_SCRATCH[2]`（0x885）の元の値を読んで記録する（期待値は 0）。
- ring の dword 9〜19：
  | dword | 値 | 意味 |
  |---|---|---|
  | 9 | 0x48088501 | `PKT4`：レジスタ 0x885（CP_SCRATCH[2]）に 1 dword |
  | 10 | 0xCAFE0001 | 書く値 |
  | 11 | 0x703D8003 | `CP_MEM_WRITE`（0x3D）、本体 3 dword |
  | 12、13 | 0x70017000、0 | 書き込み先 IOVA（下位、上位） |
  | 14 | 0xCAFE0002 | 書く値 |
  | 15 | 0x703E8003 | `CP_REG_TO_MEM`（0x3E）、本体 3 dword |
  | 16 | 0x00040885 | REG=0x885、CNT=1（bit18〜29）、64 ビットでない、加算しない |
  | 17、18 | 0x70017008、0 | 書き込み先 IOVA |
  | 19 | 0x70268000 | `CP_WAIT_FOR_IDLE`（0x26）、本体なし |
  - ヘッダは §9.20.4 の式で計算した。同じ式で ME_INIT（0x70C80008）と SET_SECURE_MODE（0x70E60001）が既知の値と一致することを確かめてある。
  - PKT4 = 0x40000000 | cnt | parity(cnt)<<7 | reg<<8 | parity(reg)<<27 [L]。
- WPTR を 9 → **20** にする（fenced write。G-2 と同じ再送の規則）。
- 待つ条件：G-2 と同じ 3 つ。ただし shadow の期待値は **20**。上限は 1 秒。
- 成功の判定（どれも読み戻しで確かめる）：
  - MMIO の `CP_SCRATCH[2]`（0x885）= 0xCAFE0001。PKT4 がレジスタに届いたことを示す。
  - データ[0]（0x70017000）= 0xCAFE0002。CP_MEM_WRITE で非特権メモリに書けたことを示す。
  - データ[2]（0x70017008）= 0xCAFE0001。レジスタからメモリへの複写が動いたことを示す。
  - データ[1] と データ[3] は 0xFFFFFFFF のまま。余計な書き込みがないことを示す。
- 失敗したときに記録するもの（読み出しは OOB の中で、GX の確認をしてから）：
  - shadow、`CP_RB_RPTR`（0x806）、WPTR
  - `CP_INTERRUPT_STATUS`（0x823）、`CP_PROTECT_STATUS`（0x824）、`CP_STATUS_1`（0x825）、`CP_HW_FAULT`（0x821）
  - `RBBM_STATUS`（0x210）、`RBBM_INT_0_STATUS`（0x201）。とくに bit1 CP_AHB_ERROR、bit9 CP_HW_ERROR、bit23 HANG_DETECT、bit24 UCHE_OOB_ACCESS
  - SMMU の FSR／FAR／FSYNR
  - データ領域の 4 dword
- 0x885 が CP_PROTECT の範囲に入っていないかについて：Linux の a660 の表では `0x800` から 0x82 個（0x800〜0x882）が読み書き禁止で、0x883 からの scratch はそのすぐ外にある [L]。stage E の表でも確かめた：CP_PROTECT[6]（0x856）= 0x82080800 は、base 0x800、個数 0x82（bit18〜30）、bit31 は読み出しも禁止。次の CP_PROTECT[7]（0x857）の base は 0x8A0。したがって 0x885 は保護されていない [P]。
- 結果の読み方：
  - 3 つとも成功した場合：secure mode でも、CP のふつうの書き込みは通る。
  - CP_MEM_WRITE だけ失敗し、FSR にフォルトがない場合：secure mode が非 secure メモリへの書き込みを止めている、という仮説に合う。
- 後片付けは G-2 と同じ。SQE は止めない（§9.20.6）。

#### 9.21.5 その後の候補（G-3 が通ってから）

- **G-3b：** `CP_EVENT_WRITE`（0x70460004、本体 {0x00000004（CACHE_FLUSH_TS、IRQ なし）, 0x70017010, 0, 0xCAFE0003}）。
  - CP だけでなく、パイプラインのイベントとキャッシュの flush を通る。Linux の fence と同じ経路。
  - G-3 よりも触れる範囲が広いので、別のビットに分けることを提案する。
- `CP_SET_SECURE_MODE 0` は、zap なしで送ると何が起きるかわからない [U]。当面は送らない。
- 描画（シェーダを動かす）は、zap の問題が解決するか、secure mode のままで何が動くかがはっきりしてから考える。

#### 9.21.6 G-3の実装（実機未実行）

bit23 `EPI_GMU_PROBE_G3=0x800000` を追加し、G-2とその全前提を必要とする`EpiGpuProbe=0xFFFE80`で実行する。G-2成功後、同じOOBを解除する前にG-3を続ける。前提不足はワンショット解除・flush後、ハードウェアアクセス前にstatus 6で拒否する。G-3b用の次ビット（bit24）は今回は予約扱いとして拒否する。G-2の140バイトの診断形式と、`0x7FFE80`までの実行内容は維持する。

リングとRPTR影はG-1のものを再利用し、追加のデータ用確保やSMMU mapはしない。既知のGPU先頭ページと+0x2000だけを一時mapする。開始時にリング/影のIOVA・属性・map済み状態、影とWPTRが9、リング先頭9 DWORDがME_INITのままであることを確認する。CP_SCRATCH[2]の元値を記録し、リング+0x7000の4 DWORDにCPUで0xFFFFFFFFを置く。§9.21.4の11 DWORDをring[9..19]にコピーしてメモリバリアを入れ、WPTR=20をfenced writeする。SQEは再起動しない。

fence再送はG-2と同じ200×10 µs、idle待ちは10000×100 µsを上限とする。RPTR影=20、RBBM_STATUSのbit0以外が0、RBBM_INT_0_STATUSのハングbit23が0、CX_BUSYのbit23が0を待つ。G-3ではRBBM_INTのCP_AHB_ERROR（bit1）、CP_HW_ERROR（bit9）、UCHE_OOB_ACCESS（bit24）もエラーとして打ち切り、Reason 14を残す。fence/idleのループ内ではレジストリを書かず、GXの確認は継続する。共通のfence/idle待ち関数は目標WPTRを引数にし、G-2は9、G-3は20を渡す。

結果はscratch=0xCAFE0001、data[0]=0xCAFE0002、data[2]=0xCAFE0001、data[1]/[3]=0xFFFFFFFFを個別に判定する。idleが成立してもいずれかが違えばReason 13 / STATUS_IO_DEVICE_ERROR。すでに時間切れやGX/breadcrumbエラーがあれば、その最初のStatus/Reasonを保持し、値の不一致で上書きしない。部分的に成功した項目はChecksと生値で区別できる。secure modeが原因かどうかは自動では判定しない。

終了時のGX診断は、scratch、CP_RB_RPTR、WPTR、CP_HW_FAULT、CP_INTERRUPT_STATUS、CP_PROTECT_STATUS、CP_STATUS_1、RBBM_STATUS、RBBM_INT_0_STATUSを1つずつ、各breadcrumbの保存・flushとGX確認の後に読む。途中で確認に失敗したら残りのGXレジスタは読まない。RAMの4 DWORDとRPTR影、CX_BUSY、SMMUのFSR/FAR/FSYNRは別に記録する。FSR等は生値の診断であり、その値だけでsecure modeやフォルトを断定しない。

診断 `GpuGmuG3` はREG_BINARY・244バイト（61 DWORD）：`{Cp, ScratchBefore, ScratchAfter, DataBefore[4], DataAfter[4], Packet[11], CpInterrupt, CpProtect, CpStatus1, Checks, Prepared}`。先頭Cpは既存のGpuGmuG2と同じ140バイトの形式だが、G-3単独の進行を表す。Cp.Requested=1、Cp.AttemptedはWPTR=20の書き込み直前に1、Cp.WptrWritesは20の書き込み回数。SQEを起動し直さないのでCp.SqeWritten=0、SqeBefore/AfterとCp.Packet[9]は未読0xFFFFFFFFのまま。追加パケットは外側のPacket[11]へコピー後のリングから保存する。

PreparedはCPUの目印とパケットのコピー・バリアが完了すると1。DataBeforeはコマンド実行前の4個の目印（0xFFFFFFFF）、DataAfterは読み出した生値。Checksはbit0=scratch一致、bit1=data[0]一致、bit2=data[2]一致、bit3=data[1]保持、bit4=data[3]保持で、成功時は0x1F。Prepared=0ではChecksは0のまま。Cp.Reasonの0〜12はG-2と同じ、13=結果不一致、14=上記CP/UCHEエラー割り込み。Status類の未試行はSTATUS_NOT_FOUND、未読値は0xFFFFFFFF。G-2のGpuGmuG2は別途そのまま保存し、G-3診断もcleanup/keepの両経路で保存する。

| breadcrumb | 内容 |
|---|---|
| 0xE00 | 前提確認・map前 |
| 0xE01 | scratch元値・影/WPTR・ME_INITの確認 |
| 0xE02 | 4個の目印と11 DWORDのコピー |
| 0xE03 | WPTR=20 |
| 0xE04 | fence確認・再送 |
| 0xE05 | idle待ち |
| 0xE10〜0xE18 | 上記9個のGX診断を順に読み取る前 |

後片付けはG-2と同じで、結果にかかわらずOOB解除・バスhalt・slumber・GX OFF確認へ進む。GX OFFやSMMU Disableで失敗したらメモリとCXを保持する。G-3だけの失敗で安全に後片付けできれば既存のCPエラーstatus 15を使い、teardownの失敗はstatus 8 / KeepCxが優先する。0xF400は読み書きとも行わず、SCM/PAS、CP_SET_SECURE_MODE、CP_EVENT_WRITE、SQEの停止、割り込みマスクは追加しない。

shim/ホスト試験に、11 DWORDの実行モデル、5個の結果確認と各項目の失敗、fence再送・時間切れ、RPTR/RBBM/GMUの未完了、エラー割り込み、各breadcrumbとGX喪失時の停止、最初のReasonの保持、追加SMMU mapなし、全体の正常終了とG-2失敗時のG-3不実行、後片付け失敗時のバッファ保持を追加した。11 DWORDの一致と、PKT4/PKT7のヘッダ（既知のME_INIT/SET_SECURE_MODEを含む）は独立したパリティ計算で確認した。ホスト試験の実行・版番号変更・ビルド・署名・配置・実機実行は未実施。

#### 9.21.7 v62 実機結果（2026-11-13、0xFFFE80、G-3）

- 実行条件：版 20.0.2.62（oem120）。端末を 11:52 に再起動してから実行した。
- **3 つの書き込みはすべて成功した。Checks は 0x1F。**
  - EpiGpuProbeStep は 0xFFFF、GpuGmuStatus は 0、GpuGmuNtStatus は 0 だった。
  - KeepCx、DisableStatus、CxStatus、FaultStatus もすべて 0 だった。デバイスは Started のまま。
- `GpuGmuG3`（244 B）の中身：
  | 項目 | 値 |
  |---|---|
  | Cp：Attempted／Reason／Status | 1／0／0 |
  | WptrWrites／FenceStatus／Polls | 1／0／**0**（idle 待ちの 1 回目で idle になった） |
  | shadow の前→後 | 9 → **20** |
  | WPTR の前→後、RPTR（0x806） | 9 → 20、20 |
  | RBBM_STATUS／INT0／CP_HW_FAULT | 0x1／0／0 |
  | CX_BUSY | 0x71000000（bit23 は 0） |
  | CP_INTERRUPT_STATUS／CP_PROTECT_STATUS／CP_STATUS_1 | 0／0／0 |
  | CP_SCRATCH[2] の前→後 | 0 → **0xCAFE0001** |
  | データの前 | {FFFFFFFF ×4} |
  | データの後 | {**CAFE0002**, FFFFFFFF, **CAFE0001**, FFFFFFFF} |
  | Packet（ring[9..19]） | §9.21.4 の値と一致 |
  - FSR は 0x400 だった。これは FORMAT の値で、フォルトは起きていない。FAR と FSYNR は前の値が残っているだけと見ている [P]。
- G-2 の部分（`GpuGmuG2`）は v61 と同じ値だった。SMR1 と SMR2 もまた受け付けられ、後片付けで元の値に戻った。
- 意味：
  - zap を入れず secure mode のままでも、CP の書き込みは 3 種類とも通った。
    - PKT4 によるレジスタへの書き込み。
    - `CP_MEM_WRITE` による非特権の非 secure メモリへの書き込み。
    - `CP_REG_TO_MEM` によるレジスタからメモリへの複写。
  - 余計な場所への書き込みもなかった（隣の 2 dword は目印の値のまま）。
  - §9.21.2 の「secure mode 中は非 secure メモリへ書けない」という仮説は、**少なくとも CP の直接の書き込みには当てはまらない** [P]。
- 次の候補（§9.21.5）：G-3b（`CP_EVENT_WRITE` CACHE_FLUSH_TS で fence を書く）。パイプラインのイベントとキャッシュの flush を通る経路で確かめる。

#### 9.21.8 G-3bの実装（実機未実行）

bit24 `EPI_GMU_PROBE_G3B=0x1000000` の拒否を解除した。G-3とその全前提を必要とし、`EpiGpuProbe=0x1FFFE80`でG-3成功直後、同じOOBを解除する前に実行する。前提不足はワンショット解除・flush後、ハードウェアアクセス前にstatus 6で拒否する。G-3未完了ならReason 8でG-3bを飛ばす。`0xFFFE80`までの実行内容と既存の診断形式は維持する。

リングとRPTR影は従来の割り当てを使い、追加のバッファ確保とSMMU mapはしない。既知のGPU先頭ページと+0x2000だけを一時mapする。影とWPTRが20、リング先頭20 DWORDがME_INITとG-3のまま、データ[0..3]がG-3成功時の値であることを確認する。CP_SCRATCH[2]も元値を記録する。データ[4..5]にだけCPUで0xFFFFFFFFを置き、ring[20..24]へ `{0x70460004, 4, 0x70017010, 0, 0xCAFE0003}` をコピーしてメモリバリアを入れる。データ[0..3]と先頭20 DWORDは書き直さない。IRQのbit31は立てない。

WPTR=25をfenced writeし、G-2/G-3と共通の再送処理（200×10 µs）を使う。idle待ちも共通で、RPTR影=25、RBBM_STATUSのbit0以外が0、GMU CX_BUSYのbit23=0を、最大10000×100 µs待つ。RBBM_INT_0_STATUSのbit1/9/24はReason 14、ハングbit23はReason 11で打ち切る。待ちのループ内でレジストリは書かず、GX確認は継続する。

結果は、データ[4]=0xCAFE0003、データ[5]=0xFFFFFFFF、データ[0..3]が投入前から変わっていないことをすべて求める。不一致ならReason 13 / STATUS_IO_DEVICE_ERROR。時間切れやGX/breadcrumbの失敗が先にあれば、その最初のStatus/Reasonを保持する。診断はG-3と同じ9個のGXレジスタを1つずつbreadcrumb保存・flushとGX確認の後に読み、確認に失敗したら残りのGX読み取りを止める。RPTR影、CX_BUSY、SMMUのFSR/FAR/FSYNR、データ6 DWORDも保存する。SQEは起動し直さない。

追加診断 `GpuGmuG3b` はREG_BINARY・236バイト（59 DWORD）：`{Cp, ScratchBefore, ScratchAfter, DataBefore[6], DataAfter[6], Packet[5], CpInterrupt, CpProtect, CpStatus1, Checks, Prepared}`。先頭CpはGpuGmuG2と同じ140バイトで、G-3bだけの進行を表す。Cp.AttemptedはWPTR=25の直前に1、Cp.WptrWritesは25の書き込み回数、Cp.Pollsは100 µsの待ち回数。SQEを起動しないためCp.SqeWritten=0、SqeBefore/AfterとCp.Packet[9]は未読0xFFFFFFFF。Packet[5]はコピー後のリングから保存する。DataBefore[0..3]はG-3の結果、[4..5]はCPUが準備した目印。Preparedは目印とパケットのコピー・バリア完了後に1。

Checksのbit0〜3はそれぞれデータ[0..3]の保持、bit4はデータ[4]=0xCAFE0003、bit5はデータ[5]=0xFFFFFFFFを表し、成功値は0x3F。Prepared=0ならChecksは0のまま。ReasonとNTSTATUSの意味はG-3と同じで、未読値0xFFFFFFFF、未試行StatusはSTATUS_NOT_FOUND。cleanup/keepの両経路で保存する。

| breadcrumb | 内容 |
|---|---|
| 0xF00 | 前提確認・map前 |
| 0xF01 | scratch元値・影/WPTR・G-3のデータとパケット確認 |
| 0xF02 | データ[4..5]の目印と5 DWORDのコピー |
| 0xF03 | WPTR=25 |
| 0xF04 | fence確認・再送 |
| 0xF05 | idle待ち |
| 0xF10〜0xF18 | scratch、RPTR、WPTR、HW_FAULT、CP_INT、PROTECT、STATUS_1、RBBM、INT0の個別読み取り前 |

後片付けは従来と同じOOB解除・バスhalt・slumber・GX OFF/idle確認・SMMU Disableの順。G-3bだけが失敗して安全に後片付けできれば既存のstatus 15を使い、teardown失敗はstatus 8 / KeepCxで全バッファを保持する。CP_SET_SECURE_MODE、0xF400の読み書き、SCM/PAS、SQE停止、割り込みマスクの変更は追加しない。

shim/ホスト試験に、IRQなしイベントパケットとWPTR=25、先頭20 DWORDとG-3データの保持、イベント結果と目印の確認、結果6項目の個別失敗、再送と時間切れ、エラー割り込み、各breadcrumb失敗、GX喪失後の読み取り停止、最初のReasonの保持、mapと開始条件の失敗、G-3失敗時の不実行、正常解放とteardown失敗時の保持を追加した。5 DWORDの一致、独立したパリティ計算、IRQ bit31=0、リングとデータ領域の非重複・サイズ境界を静的に確認した。ホスト試験の実行・版番号変更・ビルド・署名・配置・実機実行は未実施。

#### 9.21.9 v63 実機結果（2026-11-13、0x1FFFE80、G-3b）

- 版 20.0.2.63（oem121）。端末は 12:11 に再起動してから実行した。
- **CACHE_FLUSH_TS で fence が書けた。Checks は 0x3F。**
  - EpiGpuProbeStep は 0xFFFF。GpuGmuStatus、GpuGmuNtStatus、KeepCx、DisableStatus、CxStatus、FaultStatus はどれも 0。デバイスは Started。
- `GpuGmuG3b`（236 B）の中身：
  | 項目 | 値 |
  |---|---|
  | Cp：Attempted／Reason／Status | 1／0／0 |
  | WptrWrites／FenceStatus／Polls | 1／0／**0** |
  | shadow の前→後 | 20 → **25** |
  | WPTR の前→後、RPTR | 20 → 25、25 |
  | RBBM_STATUS／INT0／CP_HW_FAULT | 0x1／0／0 |
  | CP_INTERRUPT／CP_PROTECT／CP_STATUS_1 | 0／0／0 |
  | scratch[2] の前／後 | 0xCAFE0001／0xCAFE0001 |
  | データの前 | {CAFE0002, FFFFFFFF, CAFE0001, FFFFFFFF, FFFFFFFF, FFFFFFFF} |
  | データの後 | {CAFE0002, FFFFFFFF, CAFE0001, FFFFFFFF, **CAFE0003**, FFFFFFFF} |
  | Packet（ring[20..24]） | {0x70460004, 4, 0x70017010, 0, 0xCAFE0003}（仕様と一致） |
  - FSR は 0x400 で、フォルトはない。
  - INT0 が 0 なのは、IRQ ビットを立てていないので CP_CACHE_FLUSH_TS（bit20）の割り込みが出ないため。予想どおり。
- G-3 の部分も前回と同じで、Checks は 0x1F だった。
- 意味：
  - Linux の fence と同じ経路が、zap も secure mode からの切り替えもなしで通った。CP のイベントがパイプラインを通り、キャッシュを flush してから、タイムスタンプを書く経路である。
  - IRQ ビットは立てていないので、割り込みの経路はまだ確かめていない。
- 次の候補 [I]：
  - CACHE_FLUSH_TS に IRQ を付ける。RBBM_INT_0_MASK の bit20 を立て、INT0_STATUS を polling で見る。ISR はまだつながない。
  - あるいは、IB（`CP_INDIRECT_BUFFER`）を 1 段使う。
  - 描画に進むのは、そのあと。

#### 9.21.10 G-3c の提案：IRQ 付き fence（bit25、0x3FFFE80）

**Linux の流れ [L]**
- `hw_init` の中で、cp_init より**前**に `RBBM_INT_0_MASK`（0x38）= `A6XX_INT_MASK` を書く。
  - `A6XX_INT_MASK` には次の 11 ビットが入っている：CP_AHB_ERROR（1）、ATB_ASYNCFIFO_OVERFLOW（6）、CP_HW_ERROR（9）、CP_IB2（13）、CP_IB1（14）、CP_RB（15）、CP_CACHE_FLUSH_TS（20）、ATB_BUS_OVERFLOW（22）、HANG_DETECT（23）、UCHE_OOB_ACCESS（24）、UCHE_TRAP_INTR（25）。
- `a6xx_irq` の処理：
  1. keepalive の投票をする。
  2. `RBBM_INT_0_STATUS`（0x201）を読む。
  3. 読んだ値をそのまま `RBBM_INT_CLEAR_CMD`（0x37）に書いて、割り込みを消す。
  4. bit20（CP_CACHE_FLUSH_TS）が立っていれば、`msm_gpu_retire` を呼ぶ。
- fence は `CP_EVENT_WRITE` に `CACHE_FLUSH_TS | IRQ`（dword 0 の bit31）を付けて送る。

**今回は割り込みのマスクを外さない。理由は次のとおり。**
- Epinephrine の割り込み資源は 3 本あり、どれもレベル割り込み（`Resources` では f0）。
- `DxgkDdiInterruptRoutine`（`bdd.cxx`）は INTF1 の割り込みしか自分のものとして受け取らず、それ以外では FALSE を返す。
- どの vector が GPU の割り込みなのかは、まだ確かめていない [U]。
- この状態で bit20 のマスクを外すと、GPU の割り込み線が立ったままになる。ISR は FALSE を返し続けるので、割り込みが止まらなくなるおそれがある [I]。
- そこで今回は、割り込み線を立てずに、割り込みの「元」が立つかどうかだけを確かめる。

**提案（bit25、G-3b の後に同じ OOB の中で行う）**
1. 準備：
   - データ[6]〜[7]（IOVA 0x70017018 と 0x7001701C）に、目印の 0xFFFFFFFF を書く。
   - 次の 3 つの元の値を読んで記録する：
     - `RBBM_INT_0_MASK`（0x38）
     - `RBBM_INT_0_STATUS`（0x201）
     - `GMU_RBBM_INT_UNMASKED_STATUS`（GMU の 0x23B15）
   - **MASK が 0 でなければ、ここで止める。** 新しい Reason を割り当てる。MASK には書き込まない。
2. ring の dword 25〜29：
   | dword | 値 | 意味 |
   |---|---|---|
   | 25 | 0x70460004 | `CP_EVENT_WRITE`、本体 4 dword |
   | 26 | **0x80000004** | CACHE_FLUSH_TS に IRQ（bit31）を付けたもの |
   | 27、28 | 0x70017018、0 | 書き込み先はデータ[6] |
   | 29 | 0xCAFE0004 | 書く値 |
3. WPTR を 25 から **30** に進める。fenced write と待ち方は G-3b と同じで、shadow が 30 になるのを待つ。
4. 割り込みの元を見る。idle になった後で、最大 1 ms（10 µs × 100 回）の間、次の 2 つを polling する：
   - `GMU_RBBM_INT_UNMASKED_STATUS`
   - `RBBM_INT_0_STATUS`
   - ループの中ではレジストリに書かない。
5. 後片付け：
   - `RBBM_INT_CLEAR_CMD`（0x37）に 1<<20 を書く。
   - その後に `RBBM_INT_0_STATUS` と UNMASKED_STATUS をもう一度読む。
   - MASK はもう一度読むだけで、書かない。
6. 成功の判定：
   - データ[6] = 0xCAFE0004。
   - データ[7] とデータ[0]〜[5] は変わっていない。
   - 次の 3 つは判定には使わず、記録だけにする：
     - UNMASKED_STATUS の bit20 が立ったかどうか
     - INT_0_STATUS の bit20 が立ったかどうか
     - CLEAR を書いた後に消えたかどうか
   - 理由：`GMU_RBBM_INT_UNMASKED_STATUS` の意味は、Linux のソースには定義しかなく、使い方が書かれていない [U]。名前から、マスクをかける前の RBBM 割り込みの状態と推測しているだけである [I]。
7. 記録するもの：
   - G-3b と同じ項目
   - MASK の前と後
   - STATUS と UNMASKED の前、polling で最初に bit20 が見えた回数、最後の値、CLEAR 後の値

**結果の読み方**
- UNMASKED の bit20 が立ち、CLEAR の後に消えた場合：割り込みの元は正しく動いている。次は ISR の準備に進める。
  - 準備 1：GPU の割り込みが 3 本のうちどの vector かを確かめる。
  - 準備 2：GX が ON のときだけ GPU のレジスタを読む ISR の設計を考える。
- どちらにも bit20 が出ない場合：EVENT_WRITE の IRQ ビットだけでは不足しているか、MASK が立っていないと STATUS に出ない作りになっている [U]。そのときは、ISR の準備をしてから MASK を外す試験を考える。

#### 9.21.11 G-3c の実装（実機未実行）

- bit25（`EPI_GMU_PROBE_G3C=0x2000000`、合計 `0x3FFFE80`）を追加した。G-3b とその前提が必要で、指定は既存と同じくワンショットで解除する。G-3b が成功した直後、同じ OOB の中でだけ実行する。以前のモードの動作と診断 ABI は変えていない。
- MASK / STATUS / UNMASKED を先に記録する。MASK が非0なら Reason=15、`STATUS_INVALID_DEVICE_STATE` で停止し、CPU の目印の準備、WPTR、CLEAR のいずれも書かない。投入直前と fenced write の再送直前にも MASK=0 を確認する。MASK は全経路で読むだけで、ISR も変更していない。
- ring[25..29] に `{0x70460004,0x80000004,0x70017018,0,0xCAFE0004}` を追加する。データ[6..7] の目印を準備し、WPTR=30、従来の fence 待ちと idle 待ちを行う。旧コマンドとデータ[0..5] は書き換えない。
- idle のあと、10 µs 間隔で最大100回待つ（初回を含む読み取りは最大101回）。STATUS と UNMASKED の bit20 の初出と最後の値を記録する。両方の初出を記録したら早期終了する。ループ内ではレジストリに書かない。bit20 が見えなかったことは失敗にしない。
- WPTR を一度でも書いた場合は、失敗経路でも GX が安全に触れる状態なら CLEAR に bit20 **だけ**を書く。CLEAR の breadcrumb 保存に失敗しても、この解除は試みる。GX の前提が崩れた場合は書かない。STATUS / UNMASKED / MASK を再記録する。bit20 が消えたかは判定に使わない。
- 成功にはデータ[0..5] 不変、[6]=`0xCAFE0004`、[7]=`0xFFFFFFFF`（Checks=`0xFF`）を求める。エラー割り込み、hang、GX の前提、breadcrumb、map、fence / idle の時間切れは従来と同様に失敗とし、最初の Reason を保持する。安全に片付いた CP 試験の失敗は GMU status=15、片付けの失敗は従来どおり status=8 / KeepCx となる。OOB 解除、slumber、reset と復元の順番は変えていない。

診断 `GpuGmuG3c` は REG_BINARY、304バイト（ULONG 76個）。未記録は `0xFFFFFFFF`。要求したが前段が失敗した場合は Cp.Reason=8、Cp.Status=`STATUS_INVALID_DEVICE_STATE`。

```text
{ Cp (既存 GpuGmuG2 と同じ140バイト),
  ScratchBefore, ScratchAfter, DataBefore[8], DataAfter[8], Packet[5],
  CpInterrupt, CpProtect, CpStatus1, Checks, Prepared,
  MaskBefore, MaskAtSubmit, MaskAfter, StatusBefore, UnmaskedBefore, IrqPolls,
  StatusFirst, UnmaskedFirst, StatusLast, UnmaskedLast,
  ClearAttempted, StatusAfterClear, UnmaskedAfterClear }
```

- DataBefore[0..5] は G-3b の出力、[6..7] は CPU が準備した目印。Checks の bit0..5 は旧データ不変、bit6 は新しい値、bit7 は目印不変を表す。
- MaskAtSubmit は最後の投入／再送前の読み取り値。StatusFirst / UnmaskedFirst は10 µsの待ち回数（0..100）、一度も見えなければ `0xFFFFFFFF`。IrqPolls も待ち回数。
- Cp の SqeWritten=0。SQE は再起動しないため SqeBefore / SqeAfter と Cp.Packet[9] は未使用。追加の Packet[5] が今回のイベント。ClearAttempted / Prepared の初期値は0。

| breadcrumb | 内容 |
|---|---|
| 0x1000 | 前提確認、既存の2ページを map |
| 0x1001〜0x1003 | 元の MASK、STATUS、UNMASKED |
| 0x1004 | G-3b のデータ、WPTR / shadow=25、旧コマンドの確認 |
| 0x1005 | 目印と追加パケットの準備 |
| 0x1006 | MASK 再確認、WPTR=30 |
| 0x1007 / 0x1008 | fence 待ち / idle 待ち |
| 0x1009 | IRQ の元を観察 |
| 0x100A | CLEAR bit20（GX が安全なときだけ） |
| 0x100B〜0x100D | CLEAR 後の STATUS、UNMASKED、MASK |
| 0x1010〜0x1018 | scratch、RPTR、WPTR、HW_FAULT、CP_INT、PROTECT、STATUS_1、RBBM、INT0 |

shim とホスト試験ケースには、正常完了、MASK 非0／投入・再送前の変更、IRQ の元の片方／両方／不出現／遅延、CLEAR 後の残留、旧データ破壊、fence / idle の時間切れ、エラー割り込み、breadcrumb / map / GX の失敗、最初の Reason の保持、前段の失敗、KeepCx 時のメモリ保持を追加した。shim は MASK への書き込みを禁止し、CLEAR は bit20 だけを受け付ける。テスト、ビルド、配置、実機実行はまだ行っていない。

#### 9.21.12 v64 実機結果（2026-11-13、0x3FFFE80、G-3c）

- 実行条件：版 20.0.2.64（oem122）。端末を 12:45 に再起動してから実行した。
- **結果：IRQ 付きの fence も書けた（Checks 0xFF）。割り込みの元（bit20）は、マスクを外す前の状態のところにだけ立った。**
  - EpiGpuProbeStep は 0xFFFF だった。GpuGmuStatus、NtStatus、KeepCx、DisableStatus、CxStatus、FaultStatus はすべて 0。デバイスは Started のまま。
  - 前段の確認も通っている。G-3 は Checks 0x1F、G-3b は 0x3F。
- `GpuGmuG3c`（304 B）：
  | 項目 | 値 |
  |---|---|
  | Cp：Attempted／Reason／Status | 1／0／0 |
  | shadow／WPTR／RPTR | 25 → **30**／30／30 |
  | RBBM_STATUS／INT0／CP_HW_FAULT／CP_INT／PROTECT／STATUS_1 | 0x1／0／0／0／0／0 |
  | データ[6]／[7] | **0xCAFE0004**／0xFFFFFFFF |
  | データ[0..5] | G-3b の後と同じ |
  | Packet | {0x70460004, 0x80000004, 0x70017018, 0, 0xCAFE0004}（仕様と一致） |
  | MASK：前／投入時／後 | 0／0／0（一度も書いていない） |
  | INT_0_STATUS：前／観察中の最後／CLEAR 後 | 0／0／0（StatusFirst は「見えず」） |
  | UNMASKED_STATUS：前 | **0x00000001**（bit0 RBBM_GPU_IDLE だけ） |
  | UNMASKED_STATUS：観察中 | 1 回目（idle になった直後）から **0x00100001**（bit20 が立った） |
  | UNMASKED_STATUS：CLEAR 後 | **0x00000001**（bit20 だけ消えた） |
  | IrqPolls | 100（2 つとも見えるまで回す作りなので、STATUS 側が出ずに上限まで回った） |
  - FSR は 0x400 で、フォルトはない。
- わかったこと：
  - `GMU_RBBM_INT_UNMASKED_STATUS`（0x23B15）は、マスクをかける前の RBBM 割り込みの状態である [P]。
    - bit20 は、IRQ ビットを付けた CACHE_FLUSH_TS で立った。
    - G-3b（IRQ ビットなし）の後でも、前の値は 0x1 だった。つまり IRQ ビットがないときは立たない。
    - `RBBM_INT_CLEAR_CMD`（0x37）に 1<<20 を書くと消えた。
  - `RBBM_INT_0_STATUS`（0x201）は、マスクをかけた後の状態である [P]。MASK が 0 のままなので、0 のままだった。マスクが 0 なので、割り込みの線も立っていない [I]。
  - Linux の `a6xx_irq` の処理（STATUS を読んで、CLEAR_CMD に書く）が前提にしている仕組みを、割り込みの線を立てずに確かめられた。
- 次に割り込みを本当に使うには、次の準備が必要である [I]：
  1. GPU の割り込みが、3 本のうちどの vector かを確かめる（DSDT の `GPU0._CRS` の順番と、`Resources` の vector の対応）。
  2. ISR の設計。GX が OFF のときに GPU のレジスタを読まないこと。どの割り込みかは MessageNumber か、別の手段で見分けること。

#### 9.21.13 G-4 の提案：IB 1 段（bit26、0x7FFFE80）

**Linux の投入の形 [L]（`a6xx_submit`）**
- `a6xx_set_pagetable` → 統計用の `CP_REG_TO_MEM` → `CP_EVENT_WRITE`（PC_CCU_INVALIDATE_DEPTH／COLOR）の順に出す。
- コマンドの数だけ `CP_INDIRECT_BUFFER`（opcode 0x3F、本体 3 dword）を出す。
  - 本体は {IB の IOVA の下位, 上位, `IB_SIZE`（dword 数、bit0〜19）}。
- 最後に `PKT4 CP_SCRATCH(2)` = seqno と、`CP_EVENT_WRITE`（CACHE_FLUSH_TS | IRQ）→ fence を出す。
- 今回は、ページテーブルの切り替え、統計、CCU の invalidate は入れない。IB を 1 つ呼ぶだけにする。

**メモリ（新しい確保と SMMU の変更はしない）**
- IB は ring の **オフセット 0x6000（IOVA 0x70016000）** に置く。
- データは今までと同じ ring+0x7000 の [8]〜[11]（IOVA 0x70017020〜0x7001702C）を使う。

**IB の中身（0x70016000 から 6 dword、CPU が書く）**
| dword | 値 | 意味 |
|---|---|---|
| 0 | 0x48088601 | `PKT4`：0x886（CP_SCRATCH[3]）に 1 dword |
| 1 | 0xCAFE0005 | 書く値 |
| 2 | 0x703D8003 | `CP_MEM_WRITE`、本体 3 dword |
| 3、4 | 0x70017020、0 | 書き込み先はデータ[8] |
| 5 | 0xCAFE0006 | 書く値 |

**ring の dword 30〜38**
| dword | 値 | 意味 |
|---|---|---|
| 30 | **0x70BF8003** | `CP_INDIRECT_BUFFER`（0x3F）、本体 3 dword |
| 31、32 | 0x70016000、0 | IB の IOVA |
| 33 | 6 | IB_SIZE（dword） |
| 34 | 0x70460004 | `CP_EVENT_WRITE`、本体 4 dword |
| 35 | 0x00000004 | CACHE_FLUSH_TS。IRQ は付けない（MASK は 0 のまま） |
| 36、37 | 0x70017024、0 | 書き込み先はデータ[9] |
| 38 | 0xCAFE0007 | 書く値 |

- ヘッダは §9.20.4 の式で計算した。PKT4 0x885 の値（0x48088501）が、G-3 で実機で動いた値と一致することも確かめた。
- WPTR を 30 から **39** に進める。fenced write と、idle を待つ条件は G-3c と同じ（shadow が 39、上限 1 秒、エラー割り込みで止める）。
- 準備：
  - データ[8]〜[11] に目印として 0xFFFFFFFF を書く。
  - IB を書いた後にメモリバリアを入れる。
  - `CP_SCRATCH[3]`（0x886）の元の値を読む。
  - 前の段階が終わった状態かを確かめる：shadow と WPTR が 30、ring[0..29] とデータ[0..7] が期待どおり。

**成功の判定**
| ビット | 条件 | わかること |
|---|---|---|
| 0 | `CP_SCRATCH[3]` = 0xCAFE0005 | IB の中の PKT4 が動いた |
| 1 | データ[8] = 0xCAFE0006 | IB の中の MEM_WRITE が動いた |
| 2 | データ[9] = 0xCAFE0007 | IB から ring に戻り、続きを処理した |
| 3 | データ[10]、[11] = 0xFFFFFFFF | 余計な書き込みがない |
| 4 | データ[0..7] と `CP_SCRATCH[2]`（0xCAFE0001）が変わらない | 前の結果を壊していない |

- 全部そろえば 0x1F。

**記録するもの**
- G-3b と同じ項目。
- 新しく `CP_IB1_BASE`（0x928/0x929）と `CP_IB1_REM_SIZE`（0x92A）。止まったときに、IB のどこで止まったかがわかる [L]。
  - 0x928〜0x92A は CP_PROTECT[9]（0x8DE から 0xAE 個）の範囲に入る。ただしこれは書き込みの禁止（bit31 が 0）で、CPU からの読み出しは関係しない [P]。

**結果の読み方**
- 0x1F：Linux と同じ「IB で呼ぶ」形が動いた。次は描画の準備か、割り込みの準備に進める。
- ビット 2 だけ落ちた場合：IB から戻れていない。`CP_IB1_REM_SIZE` で確かめる。
- ビット 0 と 1 が落ちた場合：IB を読めていない。`CP_IB1_BASE`、FSR、FAR で確かめる。

#### 9.21.14 G-4 の実装（実機未実行）

- bit26（`EPI_GMU_PROBE_G4=0x4000000`、合計 `0x7FFFE80`）を追加した。G-3c と全前提を要求し、ワンショット解除にも含める。G-3c が成功した直後、同じ OOB の中で実行する。
- 前提確認では WPTR / shadow=30、ring[0..29]、データ[0..7]、scratch[2]=`0xCAFE0001`、MASK=0 を確認する。MASK は投入直前と fence の再送直前にも読む。MASK への書き込み、ページテーブルの切り替え、統計、CCU invalidate は追加しない。
- IB は既存 ring+0x6000 に仕様どおり6 dwordを配置する。ring[30..38] は IB 呼び出しと IRQ なしの CACHE_FLUSH_TS。データ[8..11] を `0xFFFFFFFF` で準備し、メモリバリアのあと WPTR=39 を書く。追加の確保や SMMU の変更はない。
- 待ちは既存の fenced write（10 µs × 200、同じ WPTR の再送）と idle 待ち（100 µs × 10000）を使う。エラー割り込みと hang は停止する。成功には §9.21.13 の Checks=`0x1F` を要求する。
- 各 GX アクセスの段階では breadcrumb を保存・flush し、OOB ACK / GDSCR / HM の前提を確認する。失敗時も安全に読める限り CP と IB の状態を記録し、最初の Reason を保持する。OOB 解除、slumber、reset と既存の後片付けへ進む。安全に片付いた CP 試験の失敗は GMU status=15、片付けの失敗は status=8 / KeepCx。KeepCx 時は IB とデータを含む既存メモリを保持する。

`GpuGmuG4` は REG_BINARY、356バイト（ULONG 89個）。既存の診断 ABI は変更しない。

```text
{ Cp (既存 GpuGmuG2 と同じ140バイト),
  ScratchBefore, ScratchAfter, Scratch2Before, Scratch2After,
  DataBefore[12], DataAfter[12], Packet[9], Ib[6],
  CpInterrupt, CpProtect, CpStatus1, IbBaseLow, IbBaseHigh, IbRemaining,
  Checks, Prepared, MaskBefore, MaskAtSubmit, MaskAfter }
```

- ScratchBefore / After は scratch[3]、Scratch2Before / After は scratch[2]。IbBaseLow / High は CP_IB1_BASE（0x928 / 0x929）、IbRemaining は CP_IB1_REM_SIZE（0x92A）。Packet は ring[30..38]、Ib は ring+0x6000 の6 dword。
- DataBefore[0..7] は G-3c の出力、[8..11] は CPU が準備した目印。Checks は §9.21.13 の5ビット。未記録は `0xFFFFFFFF`、Prepared / Checks の初期値は0。Cp.SqeWritten=0 で SQE は再起動しない（Cp の SQE 前後と Packet[9] は未使用）。
- Reason / NtStatus は G-3c と同じ。Reason=8 は前段失敗で未実行、12 は旧状態／メモリの不整合、13 は結果不一致、15 は MASK 非0。MaskAtSubmit は最後の投入／再送前の読み取り値。

| breadcrumb | 内容 |
|---|---|
| 0x1100 | 前提確認、既存の2ページを map |
| 0x1101 | MASK、旧コマンド・データ、scratch[2..3]、WPTR / shadow |
| 0x1102 | 目印、IB、追加 ring パケットの準備 |
| 0x1103 | MASK 再確認、WPTR=39 |
| 0x1104 / 0x1105 | fence 待ち / idle 待ち |
| 0x1110〜0x1119 | scratch[3]、scratch[2]、RPTR、WPTR、HW_FAULT、CP_INT、PROTECT、STATUS_1、RBBM、INT0 |
| 0x111A〜0x111D | IB1_BASE 下位、上位、IB1_REM_SIZE、MASK |

shim とホスト試験ケースを追加した。IB の内容と ring の呼び出しを検査し、scratch / メモリ / 復帰後のイベント、両方の目印、既存データ8個と scratch[2] の破壊を個別に模擬する。fence 再送と時間切れ、idle の時間切れ、エラー割り込み、breadcrumb / map / GX の失敗、MASK の変化、前段失敗、最初の Reason と KeepCx 時のメモリ保持も対象にした。テスト、ビルド、配置、実機実行はまだ行っていない。

#### 9.21.15 v65 実機結果（2026-11-13、0x7FFFE80、G-4）

- 実行条件：版 20.0.2.65（oem123）。端末を 13:07 に再起動してから実行した。
- **結果：IB 1 段が動いた。Checks は 0x1F。**
  - EpiGpuProbeStep は 0xFFFF。
  - GpuGmuStatus、NtStatus、KeepCx、DisableStatus、CxStatus、FaultStatus はすべて 0。デバイスは Started。
- `GpuGmuG4`（356 B）：
  | 項目 | 値 |
  |---|---|
  | Cp：Attempted／Reason／Status | 1／0／0 |
  | shadow／WPTR／RPTR | 30 → **39**／39／39 |
  | Polls | 0 |
  | RBBM_STATUS／INT0／CP_HW_FAULT／CP_INT／PROTECT／STATUS_1 | 0x1／0／0／0／0／0 |
  | CP_SCRATCH[3]：前 → 後 | 0 → **0xCAFE0005** |
  | CP_SCRATCH[2]：前 → 後 | 0xCAFE0001 → 0xCAFE0001 |
  | データ[8]／[9]／[10]／[11] | **0xCAFE0006**／**0xCAFE0007**／FFFFFFFF／FFFFFFFF |
  | データ[0..7] | G-3c の後と同じ |
  | `CP_IB1_BASE`／`CP_IB1_REM_SIZE` | **0x70016000**／0（IB を最後まで処理した） |
  | MASK：前／投入時／後 | 0／0／0 |
  | Packet と IB | 仕様と一致 |
  - FSR は 0x400 で、フォルトはない。
  - G-4 が始まる前の確認には、データ[6] = 0xCAFE0004 が含まれている。この確認を通ったので、G-3c も成功していたとわかる。
- わかったこと：
  - Linux の `a6xx_submit` と同じ「ring から IB を呼ぶ」形が、zap なしの secure mode で動いた。
  - CP は IB を IOVA 0x70016000 から SMMU 経由で読んだ。
  - IB の中の PKT4 と MEM_WRITE を処理し、ring に戻って fence のイベントまで進んだ。
- ここまでの CP の段階（G-1〜G-4）で、コマンドの投入、メモリへの書き込み、fence の仕組みが一通りそろった。
- 次の候補 [I]：
  1. 割り込みを使えるようにする準備（GPU の vector を確かめる、ISR の設計）。
  2. 描画やコンピュートの準備。必要なものが多いので、Linux の調査から始める。
     - 例：シェーダの投入、`CP_SET_MARKER`、CCU の invalidate、ビンの設定。

#### 9.21.16 割り込みの準備：GPU の vector と ISR の設計（文章仕様、未実装）

**GPU の割り込み線**
- v32 系 DSDT の GPU0 `_CRS` には割り込みが 3 本あり、すべて Level／ActiveHigh／Exclusive。
- RESI の名前の並びから、Resources の順番との対応は次のとおり。

  | 順番 | GSIV | 名前 | 出どころ |
  |---|---|---|---|
  | 1 本目 | 0x73（SPI 83） | VSYNC_INTERRUPT | MDSS。今の HW VSync（EpiHwVsync=1）が使っている |
  | 2 本目 | **0x14C（SPI 300）** | **GFX_INTERRUPT** | **Adreno（RBBM）** |
  | 3 本目 | 0xCE | VIDC_INTERRUPT | ビデオ。使わない |

- SPI 300 は Linux の `kodiak.dtsi` にある gpu ノードの `interrupts = <GIC_SPI 300 IRQ_TYPE_LEVEL_HIGH>` と一致する。
- 今の `InterruptRoutine`（bdd.cxx:891）は、MessageNumber を見ずに `EpiVsyncIsr` の結果だけを返す。
- 今は RBBM_INT_0_MASK が 0 のままなので、SPI 300 がアサートされることはない。G-3c／G-4 で IRQ 付きのイベントを出したときも、立ったのは raw status（`GMU_RBBM_INT_UNMASKED_STATUS` 0x23B15）だけだった。

**わかっていないこと**
- [U] dxgkrnl が DOD の 2 本目以降の line 割り込みにも ISR をつなぐかどうか。
- [U] line 割り込みのとき MessageNumber が何になるか（0 になると思われる）。
- このため ISR は、MessageNumber で区別せずに、VSync と GPU の両方を毎回調べる作りにする。
- [U] `DxgkCbSynchronizeExecution` が 2 本目の割り込みとも同期をとるかどうか。とらない場合は、arm／disarm を GPU レジスタの操作の順番で安全にする必要がある（下の「disarm」を参照）。I-1 の試験でこれも確かめる。

**危険**
- line は level 割り込みである。ソースを unmask したのに誰も CLEAR しないと、割り込みが鳴りっぱなしになり、端末が固まる。
- GX が切れている状態で GX のレジスタ（0x201 など）を読むと、バスが止まる恐れがある。

**ISR の設計（文章仕様。実装はユーザー）**
- 状態（nonpaged、デバイス拡張に置く）：
  - `Armed`（BOOLEAN）：GX が入っていて、GPU の MMIO がマップ済みで、MASK を書いた後だけ TRUE。
  - `Mask`：今 unmask しているビット。
  - `Count`：claim した回数。
  - `Pending`：まだ DPC が処理していない status の OR。
  - `LastStatus`。
  - `StormTrip`：鳴りっぱなし対策が働いたかどうか。
- ISR の流れ：
  1. まず VSync 側を調べる（今の `EpiVsyncIsr`）。結果は覚えておくが、ここで return はしない。
  2. `Armed` が FALSE なら GPU のレジスタには触らず、VSync の結果を返す。
  3. `Armed` が TRUE なら RBBM_INT_0_STATUS（0x201）を読む。0x201 は masked status なので、unmask したビットしか出てこない。
  4. 値が 0 なら GPU の割り込みではない。VSync の結果を返す。
  5. 0 でなければ RBBM_INT_CLEAR_CMD（0x37）に同じ値を書く。そのあと `Pending |= status`、`LastStatus = status`、`Count++` として、DPC をキューする。DPC は VSync と共通の `DxgkCbQueueDpc` を使い、DPC の中で両方を処理する。
  6. 鳴りっぱなし対策：`Count` が上限（試験では 64）を超えたら、ISR の中で MASK（0x38）に 0 を書く。そして `Armed` を FALSE、`StormTrip` を TRUE にする。
  7. TRUE を返す。
  - Linux の `a6xx_irq` も同じ順番で、status を読んで CLEAR_CMD に同じ値を書き、そのあとビットごとに処理する。
  - Linux は status を読む前に GMU の keepalive に投票している。これは IFPC で GX が勝手に落ちる場合のためのものである。今の Epinephrine は試験の間ずっと GX を入れたままで、IFPC もないので、keepalive は要らない [I]。IFPC を入れるときに追加する。
- arm の手順（PASSIVE で、GX が入っている間）：
  1. CLEAR_CMD に、unmask する予定のビットを書く。これで、前の段階で残った raw status（G-3c／G-4 の bit20）を消す。
  2. `GMU_RBBM_INT_UNMASKED_STATUS` を読み、そのビットが 0 になったことを確かめる。0 にならなければ arm しない。
  3. SynchronizeExecution の中で `Armed=TRUE`、`Mask=…` とし、MASK（0x38）に書く。
- disarm の手順（GX を切る前、CP を止める前、MMIO のマップを外す前に必ず行う。失敗の経路でも行う）：
  1. MASK（0x38）に 0 を書く。これで新しいアサートが起きなくなる。
  2. CLEAR_CMD に 0xFFFFFFFF を書く。
  3. SynchronizeExecution の中で `Armed=FALSE` とする。ここを抜けた後は、ISR が GPU のレジスタを読むことはない。
  4. 2 本目の割り込みとは同期がとれない場合に備えて、最初に MASK を 0 にしておく。そうすれば、ISR が走っていても 0x201 が 0 を読むだけになる。マップを外すのは、さらに `KeFlushQueuedDpcs` を呼んだ後にする。
- 対象ビット：最初は bit20（CP_CACHE_FLUSH_TS）だけにする。
  - Linux は 1, 6, 9, 13, 14, 15, 20, 22, 23, 24, 25 を unmask している。
  - エラーのビット（CP_HW_ERROR、CP_AHB_ERROR、UCHE_OOB など）は、DPC がそれを処理できるようになってから足す。

**I-1 の試験の提案（bit28、0x1FFFFE80。G-5 の後に置く。G-5 より先にする場合は bit27）**
- 前提：
  - G-4 まで（G-5 を先にする場合は G-5 まで）成功していること。
  - データ[12] = 0xFFFFFFFF であること。CPU が先に書いておく。
- 手順：
  1. 上の arm の手順で、bit20（0x00100000）だけを unmask する。
  2. ring に `EVENT_WRITE`（CACHE_FLUSH_TS | IRQ）を 5 dword 置く。
     - 内容：0x70460004、0x80000004、0x70017030（データ[12]）、0、0xCAFE000A。
     - G-5 の後なら ring の dword 48..52 で、WPTR は 53 になる。
  3. WPTR を進める。
  4. `Count` が 1 以上になるまで待つ。PASSIVE で 1 ms ずつ、最大 100 ms。
  5. disarm の手順で disarm する。
- 記録する項目：
  - `Count`、`LastStatus`、`StormTrip`。
  - データ[12]。
  - 試験の前と後の 0x201 と 0x23B15。
  - RPTR／WPTR。
  - MASK の前／arm 後／disarm 後。
- 成功の条件：
  - `Count` が 1 以上 4 以下。
  - `LastStatus` に bit20 がある。
  - `StormTrip` が FALSE。
  - データ[12] が 0xCAFE000A。
  - disarm の後、0x201 が 0 で、0x23B15 の bit20 も 0。
- `Count` が 0 のままで、0x23B15 の bit20 が 1 のとき：dxgkrnl が 2 本目の線に ISR をつないでいない可能性がある。MASK を 0 に戻せば線は下がるので、危険はない。その場合は、次に `DXGK_INTERRUPT_TYPE` 側か、自分で `IoConnectInterruptEx` する方法を調べる [U]。

#### 9.21.17 描画・コンピュートの調査と G-5 の提案：2D の単色塗りつぶし（文章仕様、未実装）

**比べた候補**
| 候補 | 必要なもの | 判断 |
|---|---|---|
| コンピュート（`CP_EXEC_CS`） | ir3 のシェーダバイナリ、SP／HLSQ の状態、定数、UAV の設定。turnip の `tu6_init_hw` の SP／HLSQ 部分（tu_cmd_buffer.c の 716〜823 行）も要る | 大きい。後回し |
| 3D の draw | 上に加えて VS／FS、VFD、VPC、GRAS、RB の状態、ビンの設定 | もっと大きい |
| **2D ブリット（`CP_BLIT`）の単色塗りつぶし** | シェーダは要らない。RB_2D／GRAS_2D の十数個のレジスタと CCU の設定だけ | **最初の描画にちょうどよい** |

- turnip は `vkCmdClearColorImage` の一部を、この 2D エンジン（`r2d_*`）で行っている。
- 出典：Mesa の `src/freedreno/vulkan/tu_clear_blit.c`（`r2d_setup_common`、`r2d_dst_buffer`、`r2d_clear_value`、`r2d_coords`、`r2d_run`）、`tu_cmd_buffer.c`（`tu_emit_cache_flush_ccu`、`tu6_init_hw`）、`registers/adreno/a6xx.xml` と `adreno_pm4.xml`。取得元は Chromium のミラー（古い main）。

**turnip の手順（sysmem に直接描く場合）**
1. CCU を sysmem（bypass）モードにする（`tu_emit_cache_flush_ccu`）。
   - CCU の状態が変わるときは、COLOR／DEPTH を flush し（PC_CCU_FLUSH_COLOR_TS 29、PC_CCU_FLUSH_DEPTH_TS 28）、invalidate し（PC_CCU_INVALIDATE_COLOR 25、PC_CCU_INVALIDATE_DEPTH 24）、WFI してから RB_CCU_CNTL（0x8E07）を書く。
   - RB_CCU_CNTL の OFFSET はビット 23..31 で、バイト数を 12 ビット右にずらした値を入れる。GMEM（bit22）は 0 にする。
2. `r2d_setup_common`：RB_2D_UNKNOWN_8C01（0x8C01）＝0。RB_2D_BLIT_CNTL（0x8C00）と GRAS_2D_BLIT_CNTL（0x8400）に同じ値。SP_2D_DST_FORMAT（0xACC0）。
3. `r2d_dst_buffer`：RB_2D_DST_INFO（0x8C17）、RB_2D_DST（0x8C18／0x8C19、64 ビット）、RB_2D_DST_PITCH（0x8C1A）を 4 dword の PKT4 でまとめて書く。
4. `r2d_clear_value`：RB_2D_SRC_SOLID_C0..C3（0x8C2C..0x8C2F）。UNORM8 のときは、各チャンネルを 0〜255 のバイト値で入れる。
5. `r2d_coords`：GRAS_2D_DST_TL（0x8405）と GRAS_2D_DST_BR（0x8406）。X はビット 0..13、Y はビット 16..29。BR は端を含む。
6. `r2d_run`：`CP_BLIT`（opcode 0x2C）、1 dword ＝ BLIT_OP_SCALE（3）。
7. 後始末：CP_BLIT は CCU に書くので、CPU が読む前に PC_CCU_FLUSH_COLOR_TS が必要。

**CCU の bypass オフセット（a635）**
- Mesa の古い表の値：a618 は 0x10000、a630／a640 は 0x20000、a650 は 0x30000。これは「CCU の数 × 64 KB」になっている。
- この古いミラーには a635／a660 の項目がない（`freedreno_devices.py` は見つからなかった）。
- [U] a635（7c+ Gen 3 と同じ系統）は CCU が 2 つだと記憶している。そうなら 0x20000 で、RB_CCU_CNTL ＝ 0x20 << 23 ＝ **0x10000000** になる。
- 新しい Mesa の gen4（a660 系）では、RB_CCU_CNTL にほかのビットもある [U]。まず書く前の値を CPU から読んで記録し（0x8E07 は CP_PROTECT の対象外）、その値と比べる。

**レジスタの保護**
- G-1 の CP_PROTECT 表を見ると、0x8C00..0x8C2F、0x8400..0x8406、0xACC0、0x8E07 はどれも対象外。ring や IB の PKT4 から書いてよい。
- 保護されているのは 0x8E00、0x8E08、0x8E50..、0xA630..0xA7FF など。

**G-5 の提案（bit27、0xFFFFE80）**
- 描く先：16×16 画素の RGBA8（FMT6_8_8_8_8_UNORM ＝ 0x30）、リニア、pitch 64 バイト、合計 1 KB。
  - 置き場所は IOVA 0x70017400..0x700177FF（ring の RW 領域の中、64 バイト境界）。
  - その後ろの 0x70017800..0x7001783F（16 dword）を番兵にする。
  - CPU が先に、画素と番兵の 272 dword すべてに 0xFFFFFFFF を書いておく。
- 色：R=0x12、G=0x34、B=0x56、A=0x78。メモリ上の 1 画素は 0x78563412 になるはず [I]。
- IB2：IOVA 0x70016100（G-4 の IB の後ろ）、36 dword。

  | # | 中身 | dword |
  |---|---|---|
  | 1 | `EVENT_WRITE` PC_CCU_INVALIDATE_COLOR：0x70460001、25 | 2 |
  | 2 | `EVENT_WRITE` PC_CCU_INVALIDATE_DEPTH：0x70460001、24 | 2 |
  | 3 | `WAIT_FOR_IDLE`：0x70268000 | 1 |
  | 4 | RB_CCU_CNTL：0x408E0701、0x10000000 [U] | 2 |
  | 5 | RB_2D_UNKNOWN_8C01：0x488C0101、0 | 2 |
  | 6 | RB_2D_BLIT_CNTL：0x408C0001、**0x10F03080** | 2 |
  | 7 | GRAS_2D_BLIT_CNTL：0x48840001、0x10F03080 | 2 |
  | 8 | SP_2D_DST_FORMAT：0x48ACC001、**0x0000F180** | 2 |
  | 9 | RB_2D_DST_INFO..PITCH：0x408C1704、0x30、0x70017400、0、1 | 5 |
  | 10 | RB_2D_SRC_SOLID_C0..C3：0x488C2C04、0x12、0x34、0x56、0x78 | 5 |
  | 11 | GRAS_2D_DST_TL／BR：0x48840502、0、0x000F000F | 3 |
  | 12 | `CP_BLIT`：0x702C0001、3 | 2 |
  | 13 | `EVENT_WRITE` PC_CCU_FLUSH_COLOR_TS：0x70460004、29、0x70017028（データ[10]）、0、0xCAFE0008 | 5 |
  | 14 | `WAIT_FOR_IDLE`：0x70268000 | 1 |

  - 値の中身：
    - BLIT_CNTL ＝ SOLID_COLOR（bit7）｜COLOR_FORMAT 0x30（<<8）｜MASK 0xF（<<20）｜IFMT R2D_UNORM8 0x10（<<24）。SCISSOR と ROTATE は 0。
    - SP_2D_DST_FORMAT ＝ COLOR_FORMAT 0x30（<<3）｜MASK 0xF（<<12）。turnip は UNORM のとき NORM ビットを立てていない。
    - DST_INFO ＝ COLOR_FORMAT 0x30。TILE_MODE は 0（リニア）、COLOR_SWAP は 0（WZYX）、FLAGS は 0。
    - PITCH ＝ 64 >> 6 ＝ 1。
  - ヘッダは、ME_INIT（0x70C80008）、IB（0x70BF8003）、PKT4 0x885（0x48088501）と同じ計算で出した。既知の 3 つとも一致した。
- ring（G-4 の後、dword 39..47、9 dword）：
  - `INDIRECT_BUFFER`：0x70BF8003、0x70016100、0、36。
  - `EVENT_WRITE` CACHE_FLUSH_TS：0x70460004、4、0x7001702C（データ[11]）、0、0xCAFE0009。UCHE も flush する。
  - WPTR は 48 になる。
- 待ち方：G-4 と同じく `GmuCpIdleWait` を使う。
- 記録する項目：
  - RB_CCU_CNTL の前と後（CPU から読む）。
  - shadow／WPTR／RPTR、Polls。
  - RBBM_STATUS、INT0、CP_HW_FAULT、CP_INTERRUPT_STATUS、CP_PROTECT_STATUS、CP_STATUS_1。
  - CP_IB1_BASE、CP_IB1_REM_SIZE。
  - データ[10]／[11]。
  - 画素の一致数（0..256）、最初に一致しなかった番号と値、画素[0]・[15]・[16]・[255] の値。
  - 番兵が全部 0xFFFFFFFF のままか。
  - FSR。
- 成功の条件（Checks のビット）：
  - bit0：RPTR＝WPTR＝48 で、エラーのビットがない。
  - bit1：データ[10] ＝ 0xCAFE0008。
  - bit2：データ[11] ＝ 0xCAFE0009。
  - bit3：256 画素すべてが 0x78563412。
  - bit4：番兵が変わっていない。
- 失敗したときの読み方 [I]：
  - 画素がすべて 0x12345678 になったとき：チャンネルの並びが逆なだけである。描画そのものは成功している。
  - 画素が 0xFFFFFFFF のままで、データ[10] だけが書かれたとき：CCU の flush が sysmem まで届いていない。または RB_CCU_CNTL が違う。
  - CP が idle にならないとき（ハング）：turnip の `tu6_init_hw` の一部を、ring の G-5 の前に足して試す。足すのは CACHE_INVALIDATE（49）、RB_UNKNOWN_8E04 ＝ 0x00100000、RB_UNKNOWN_8E01 ＝ 0、UCHE_UNKNOWN_0E12 ＝ 0x3200000、UCHE_CLIENT_PF ＝ 4。その前に、0x8E01／0x8E04 が CP_PROTECT に入っていないかを確かめる。
  - 番兵が変わったとき：BR が端を含むことの扱いか、pitch の解釈が違う。
- G-5 の後の候補 [I]：
  1. 描く先を scanout のフレームバッファ（MDSS が今読んでいるもの）の一部にする。色の付いた矩形が画面に出れば、目で確かめられる。SMMU に、そのフレームバッファの物理アドレスをマップする必要がある。
  2. `BLIT_OP_COPY`（SP_PS_2D_SRC_* を使うコピー）。
  3. その後でコンピュート（ir3 のシェーダを手で組む）に進む。

#### 9.21.18 G-5 の実装（実機未実行）

- bit27（`EPI_GMU_PROBE_G5=0x8000000`、合計 `0xFFFFE80`）を追加した。G-4 と全前提を要求し、ワンショット解除にも含める。G-4 成功後、同じ OOB の中で実行する。以前のモードと診断 ABI は変更しない。
- 前提として WPTR / shadow=39、ring[0..38]、G-4 の IB、データ[0..11] が前段の結果と一致することを確認する。MASK=0 を求め、投入直前と fence 再送前にも再確認する。MASK には書かない。
- RB_CCU_CNTL（GPU dword 0x8E07、物理 0x03D2381C）を **CPU から先に読む**。既存のレジスタページ 0x03D23000 を一時的に map し、元の値を CcuBefore に記録する。推測値 `0x10000000` は仕様の IB にだけ含め、CPU から直接は書かない。終了時に CcuAfter も読む。両者の一致は成功条件にしない。
- IB は ring+0x6100 に36 dword、ring[39..47] は IB の呼び出しと IRQ なしの CACHE_FLUSH_TS。内容は §9.21.17 の表どおり。CPU は画素256個（ring+0x7400）と番兵16個（ring+0x7800）をすべて `0xFFFFFFFF` にする。準備後にメモリバリアを入れ、WPTR=48 を書く。追加の GPU メモリ確保や SMMU の変更はない。
- 待ちは既存の fence 待ち（最大2 ms、同じ WPTR を再送）と idle 待ち（最大1秒）を使う。エラー割り込みと hang は停止する。各段階で breadcrumb を保存・flush し、OOB ACK / GX GDSCR / HM を確認する。最初の Reason を保持し、OOB 解除、slumber、reset と既存の後片付けへ進む。安全に片付いた CP 試験の失敗は GMU status=15、片付け失敗は status=8 / KeepCx。KeepCx 時は画素・番兵・IB を含む既存メモリを保持する。

`GpuGmuG5` は REG_BINARY、564バイト（ULONG 141個）。

```text
{ Cp (既存 GpuGmuG2 と同じ140バイト), CcuBefore, CcuAfter,
  DataBefore[12], DataAfter[12], Packet[9], Ib[36],
  CpInterrupt, CpProtect, CpStatus1, IbBaseLow, IbBaseHigh, IbRemaining,
  Checks, Prepared, MaskBefore, MaskAtSubmit, MaskAfter,
  PixelMatches, PixelFirstBad, PixelBadValue, Pixels[4], Sentinel[16], SentinelMatches }
```

- PixelMatches は0..256、PixelFirstBad / PixelBadValue は最初の不一致の番号／値。Pixels は画素[0]、[15]、[16]、[255]。Sentinel は番兵16個の実測値、SentinelMatches は `0xFFFFFFFF` の個数（0..16）。未記録、および不一致がなかった番号／値は `0xFFFFFFFF`。Prepared / Checks の初期値は0、一致数は準備済みのメモリを記録した時点で数える。
- Checks は §9.21.17 の5ビット（全成功=`0x1F`）。bit0 は hardware RPTR / WPTR / shadow がすべて48、INT0 の bit1 / 9 / 23 / 24 が0、RBBM / CX が idle。bit1 / 2 はデータ[10] / [11]、bit3 は全画素一致、bit4 は全番兵不変。画素の並びが予想と違う場合も、値を変換せず記録する。
- Reason / NtStatus は G-4 と同じ。前段失敗で未実行は Reason=8、旧状態／メモリの不整合は12、結果不一致は13、MASK 非0は15。Cp.SqeWritten=0、SQE は再起動しない。Cp の SQE 前後と Packet[9] は未使用。追加の Packet[9] / Ib[36] が今回のコマンド。DataBefore は前段の結果を保存する。

| breadcrumb | 内容 |
|---|---|
| 0x1200 | 前提確認、レジスタの3ページを map |
| 0x1201 | MASK、旧コマンド・IB・データ、WPTR / shadow |
| 0x1202 | 元の RB_CCU_CNTL を CPU から読む |
| 0x1203 | 画素、番兵、IB、追加 ring パケットの準備 |
| 0x1204 | MASK 再確認、WPTR=48 |
| 0x1205 / 0x1206 | fence 待ち / idle 待ち |
| 0x1210〜0x1218 | CCU、RPTR、WPTR、HW_FAULT、CP_INT、PROTECT、STATUS_1、RBBM、INT0 |
| 0x1219〜0x121C | IB1_BASE 下位、上位、IB1_REM_SIZE、MASK |

shim とホスト試験ケースを追加した。仕様の IB / ring パケット、正常な塗りつぶし、5つの Checks の個別不成立、チャンネル順の違い、最初の不一致と指定画素、16個すべての番兵、fence 再送と時間切れ、idle の時間切れ、エラー割り込み、breadcrumb / map / GX の失敗、MASK の変化、前段失敗、最初の Reason と KeepCx 時のメモリ保持を対象とする。shim は CPU による RB_CCU_CNTL への書き込みを禁止する。テスト、ビルド、配置、実機実行はまだ行っていない。

#### 9.21.19 v66 実機結果（2026-11-13、0xFFFFE80、G-5）

- 実行条件：版 20.0.2.66（oem124）。端末を 13:33 に再起動してから実行した。
- **結果：CP はすべてのパケットを処理したが、画素は 1 つも書かれなかった。Checks は 0x17（bit3 だけが不成立）。**
  - EpiGpuProbeStep は 0xFFFF。
  - GpuGmuStatus は 15（EPI_GMU_E_CP）、NtStatus は 0xC0000185（Reason 13＝結果の不一致）。
  - KeepCx、DisableStatus、FaultStatus は 0。後片付けは正常に終わり、デバイスは Started のまま。
- `GpuGmuG5`（564 B）：
  | 項目 | 値 |
  |---|---|
  | shadow／WPTR／RPTR | 39 → **48**／48／48 |
  | Polls | 0 |
  | RBBM_STATUS／INT0／CP_HW_FAULT／CP_INT／PROTECT／STATUS_1 | 0x1／0／0／0／0／0 |
  | CX_BUSY | 0x71000000（bit23 は 0） |
  | RB_CCU_CNTL：前 → 後 | **0 → 0x10000000**（IB の PKT4 は届いた） |
  | データ[10]／[11] | **0xCAFE0008**／**0xCAFE0009** |
  | `CP_IB1_BASE`／`CP_IB1_REM_SIZE` | 0x70016100／0（IB を最後まで処理した） |
  | MASK：前／投入時／後 | 0／0／0 |
  | 画素の一致数 | **0**（最初の不一致は画素[0]、値は 0xFFFFFFFF） |
  | 画素[0]／[15]／[16]／[255] | すべて 0xFFFFFFFF（CPU が書いた値のまま） |
  | 番兵 | 16 個とも 0xFFFFFFFF（変化なし） |
  | Packet と IB | 仕様と一致 |
  - FSR は 0x400 で、私たちの context bank にフォルトはない。
- わかったこと：
  - CP 自身の書き込み（PKT4 による RB_CCU_CNTL、CCU_FLUSH_COLOR_TS と CACHE_FLUSH_TS のタイムスタンプ）はすべて届いた。
  - `CP_BLIT` も CP の処理としては終わり、GPU は idle になった。ハングもエラー割り込みもない。
  - しかし 2D エンジン（RB）の書き込みは、見ている 1 KB にも番兵にも現れなかった。
  - RB_CCU_CNTL の値は、新しい Mesa（AOSP のミラー、§9.21.20）で確かめた値と一致した。CCU のオフセットは原因ではない。

#### 9.21.20 画素が書かれない原因の候補と G-5b の提案（文章仕様、未実装）

**新しい Mesa で確かめたこと**（AOSP `platform/external/mesa3d` main：`freedreno_devices.py`、`tu_device.cc`、`tu_cmd_buffer.cc`、`tu_clear_blit.cc`、`a6xx.xml`）
- a635（chip_id 0x06030500、「Adreno 7c+ Gen 3」系）は `[a6xx_base, a6xx_gen4]`、`num_ccu = 2`。
- `ccu_offset_bypass` ＝ depth の CCU キャッシュ（2 × 64 KB）の後ろ ＝ **0x20000**。`has_gmem_fast_clear` は真、`concurrent_resolve` は偽。したがって RB_CCU_CNTL ＝ **0x10000000** で正しい（§9.21.17 の [U] は解消）。
- a635 の magic_regs：
  | レジスタ | 値 |
  |---|---|
  | RB_DBG_ECO_CNTL（0x8E04） | 0x04100000（blit 用も同じ値） |
  | RB_UNKNOWN_8E01 | 0 |
  | UCHE_UNKNOWN_0E12 | **0x00000001** |
  | UCHE_CLIENT_PF（0xE19） | 0x00000084（今の G-1 表では 0x81） |
  | PC_MODE_CNTL | 0x1F |
  | PC_POWER_CNTL | 1 |
  | SP_CHICKEN_BITS | 0x00001400 |
  | SP_DBG_ECO_CNTL | 0x00000006 |
  | TPL1_DBG_ECO_CNTL | 0x05008000 |
  | VPC_DBG_ECO_CNTL | 0x02000000 |
  | GRAS_DBG_ECO_CNTL、HLSQ_DBG_ECO_CNTL | 0 |
- turnip はコマンドバッファの始めに必ず `tu6_init_static_regs` でこれらを書く。`vkCmdClearColorImage` の r2d は、その状態を前提にしている。
- 新しい `r2d_run` は、`RB_DBG_ECO_CNTL_blit` が通常の値と違う GPU でだけ、CP_BLIT の前後で RB_DBG_ECO_CNTL を書き換える。a635 は同じ値なので、追加の手順はない。
- `r2d_setup_common` の中身は、古い版と同じだった（A7XX 用の TPL1_2D_SRC_CNTL を除く）。

**候補**
1. **secure mode** [I]。いちばん疑わしい。
   - CP はリセット直後の secure mode のままである。zap も `CP_SET_SECURE_MODE 0` も送っていない（0xF400 にも触れていない）。
   - Adreno の content protection では、secure mode の間、描画系（RB／CCU など）のメモリアクセスは secure の印を付けて出ると考えられる [I]。その場合、書き込みは TZ が持つ secure の context bank に行くか、捨てられる。私たちの CB（FSR 0x400）にフォルトが出ないこととも合う。
   - これまでの G-3〜G-4 で動いたのは、すべて CP 自身のアクセスだった。これとも矛盾しない。
2. **初期化の不足** [I]：上の magic_regs、とくに RB_DBG_ECO_CNTL と UCHE_UNKNOWN_0E12。
3. **別の場所に書いた**：番兵が変わっていないので、近くではない。可能性は低いが、念のため広く調べる。

**G-5b の提案（bit28、0x1FFFFE80）**
- 目的：候補 2 と 3 を一度に確かめる。どちらでもなければ、候補 1（secure mode）が残る。
- 前提：G-5 まで（データ[10]／[11] と、G-5 の ring と IB を含む）。G-5 の Checks は 0x17 でもよい。画素がまだ全部 0xFFFFFFFF であることを確かめてから進む。
- CPU が先に読んで記録する（読むだけ）：
  - 0x8E04、0x8E01、0xE12、0xE19。
  - SMMU の GR0 の sGFSR（0x48）、sGFSYNR0（0x50）、sGFSYNR1（0x54）、sGFSYNR2（0x58）。上の候補 1 で secure 以外の未知の SID が出ていれば、ここに出る可能性がある。書き込みはしない。
- IB3：IOVA 0x70016200、14 dword。
  | # | 中身 | dword |
  |---|---|---|
  | 1 | `EVENT_WRITE` CACHE_INVALIDATE：0x70460001、49 | 2 |
  | 2 | `WAIT_FOR_IDLE`：0x70268000 | 1 |
  | 3 | RB_DBG_ECO_CNTL：0x408E0401、0x04100000 | 2 |
  | 4 | RB_UNKNOWN_8E01：0x408E0101、0 | 2 |
  | 5 | UCHE_UNKNOWN_0E12：0x400E1201、0x00000001 | 2 |
  | 6 | `WAIT_FOR_IDLE`：0x70268000 | 1 |
  | 7 | `INDIRECT_BUFFER`（G-5 の IB2 をもう一度呼ぶ）：0x70BF8003、0x70016100、0、36 | 4 |
  - IB の中から IB を呼ぶ（IB2 になる）のを避けたい場合は、#7 を ring に置いてもよい。下の ring の案はそうしている。その場合、IB3 は #1〜#6 の 10 dword になる。
  - 0x8E04、0x8E01 は CP_PROTECT の対象外（0x861 は 0x8E00 だけ）。0xE12 も対象外（0x85D は 0xE03..0xE0F）。
- ring（dword 48..）：
  - `INDIRECT_BUFFER`：0x70BF8003、0x70016200、0、10（IB3 の #1〜#6）。
  - `INDIRECT_BUFFER`：0x70BF8003、0x70016100、0、36（G-5 の IB2 そのまま）。
  - `EVENT_WRITE` CACHE_FLUSH_TS：0x70460004、4、0x70017030（データ[12]）、0、0xCAFE000A。
  - 合計 13 dword で、WPTR は 61。データ[10] は IB2 の CCU flush でもう一度 0xCAFE0008 になる（変化しない）。
- 終わった後に記録する：
  - G-5 と同じ項目（画素、番兵、データ[10..12]、レジスタ）。
  - 0x8E04、0x8E01、0xE12 の後の値（届いたかどうか）。
  - SMMU の sGFSR／sGFSYNR0..2 の後の値。
  - **広い走査**：ring の領域 32 KB のうち、ring の dword 0..60、IB（0x6000..0x623F）、データ（0x7000..0x703F）、画素と番兵（0x7400..0x783F）を除いた範囲で、0x78563412、0x12345678、0x00000012 のどれかに一致する dword の数と、最初の位置。
- 成功の条件（Checks）：bit0＝RPTR／WPTR／shadow 61 でエラーなし、bit1＝データ[12] ＝ 0xCAFE000A、bit2＝画素 256 個がすべて 0x78563412、bit3＝番兵が不変。0x1F ではなく 0xF が全成功。
- 読み方 [I]：
  - 画素が書かれた：候補 2（初期化の不足）だった。どのレジスタが効いたかは、後で 1 つずつ外して調べる。
  - 広い走査で見つかった：候補 3。アドレスや pitch の解釈を直す。
  - どちらもない：候補 1（secure mode）がいちばん有力になる。そこから先は zap shader の読み込み（SCM／PAS 経由）が必要になる。これは qcscm などの扱いを含む大きな判断なので、ユーザーと相談して決める。

#### 9.21.21 G-5b の実装（実機未実行）

- bit28（`EPI_GMU_PROBE_G5B=0x10000000`、合計 `0x1FFFFE80`）を追加した。G-5 と全前提を要求し、ワンショット解除にも含める。G-5 の Checks=`0x17` / Reason=13 / `STATUS_IO_DEVICE_ERROR` は続行可能とするが、時間切れ、アクセス・breadcrumb・map の失敗、エラー割り込みは許容しない。
- 実際の入口では WPTR / shadow=48、旧 ring と IB、データ[0..11]、全256画素と番兵16個が前段の期待値であることを再確認する。画素は全個 `0xFFFFFFFF` が必要。G-5 がすでに描画に成功した場合は、過去の画素を G-5b の結果と取り違えないよう、ここで停止する。
- 採用したのは IB を入れ子にしない案。ring[48..60] から ring+0x6200 の10 dword IB（CACHE_INVALIDATE、WFI、3レジスタ、WFI）、既存の ring+0x6100 の G-5 IB、最後の CACHE_FLUSH_TS の順に実行する。WPTR=61、データ[12] に `0xCAFE000A`。画素と番兵は入口の値を保ち、CPU から再初期化しない。追加の GPU メモリ確保や SMMU マッピング変更はない。
- MASK=0 を入口、投入・fence 再送前に確認し、MASK には書かない。magic レジスタの書き込みは CP の IB だけで行う。0xE19 は前後を読むだけ。レジスタページ 0 / 0x2000 / 0x23000 / 0x3000 を一時的に map し、OOB 解除より前に unmap する。
- `EpiSmmuQueryGlobalFault` を追加した。既存の SMMU context と CX 窓を確認し、GR0 の 0x48 / 0x50 / 0x54 / 0x58 を読むだけ。clear / acknowledge は行わない。前後の値と各呼び出しの NTSTATUS を保存する。global 診断の読み取り失敗は記録のみで、描画の成功条件には追加しない。
- 最後に既存の32 KB ring を CPU から走査する。除外はバイトの半開区間 `[0,0xF4)`、`[0x6000,0x6240)`、`[0x7000,0x7040)`、`[0x7400,0x7840)`。対象は7699 dword。`0x78563412` / `0x12345678` / `0x00000012` それぞれの一致数と最初の **ring 内バイト位置**を記録する。見つかったこと自体は描画の成功条件にしない。
- Checks は仕様の4ビットで、全成功=`0xF`。bit0 は hardware RPTR / WPTR / shadow=61、エラーなし・idle、bit1 はデータ[12]、bit2 は全画素、bit3 は全番兵。既存の `GpuGmuG5` は変更せず失敗の記録も保持する。許容した G-5 の画素不一致だけで全体を失敗にせず、G-5b の結果を使う。後片付けの失敗は引き続き status=8 / KeepCx を優先する。

`GpuGmuG5b` は REG_BINARY、764バイト（ULONG 191個）。既存 ABI は変更しない。

```text
{ Draw (既存 GpuGmuG5 と同じ564バイト), Data12Before, Data12After,
  Packet[13], Ib[10], MagicBefore[4], MagicAfter[4],
  GlobalStatus[2], GlobalBefore[4], GlobalAfter[4],
  ScanCount[3], ScanFirst[3], ScanDwords }
```

- Magic の順は 0x8E04 / 0x8E01 / 0xE12 / 0xE19。Global の順は sGFSR / sGFSYNR0 / 1 / 2。Scan の順は上記3色。未記録と最初の一致なしは `0xFFFFFFFF`、GlobalStatus の初期値は `STATUS_NOT_FOUND`。Draw.Prepared / Checks は0から始める。走査を実行した場合だけ一致数は0以上になる。
- Draw.DataBefore / After はデータ[0..11]、Data12Before / After は別に保存する。Draw.Packet[9] は未使用、今回の ring は外側の Packet[13] に保存する。Draw.Ib[36] は再利用した G-5 の IB、外側の Ib[10] が初期化 IB。Draw の画素・番兵・CCU・CP・FSR の項目は G-5 と同じ。

| breadcrumb | 内容 |
|---|---|
| 0x1300 / 0x1301 | map / 前段の状態と MASK の確認 |
| 0x1302〜0x1305 | magic レジスタ4個の元の値 |
| 0x1306 / 0x1307 | 元の CCU / global fault |
| 0x1308 / 0x1309 | IB と追加 ring の準備 / WPTR=61 |
| 0x130A / 0x130B | fence / idle 待ち |
| 0x1310〜0x131C | G-5 と同じ順の GX 診断 |
| 0x1320〜0x1323 / 0x1324 | magic レジスタ前後比較用の値 / global fault の後 |

shim と試験ケースに、G-5 の Checks=0x17 からの正常続行、4条件の個別不成立、global 診断失敗の記録、走査の境界と3種類の値、前提・breadcrumb・fence・idle・エラー割り込み、全体結果の選択、KeepCx 時の保持を追加した。SMMU の global 読み取りがレジスタを書き換えない試験も追加した。テスト、ビルド、配置、実機実行は行っていない。

#### 9.21.22 v67 実機結果（2026-11-13、0x1FFFFE80、G-5b）

- 実行条件：版 20.0.2.67（oem125）。端末を 16:56 に起動し、配置のあとに実行した（配置のときは EpiGpuProbe＝0 で、GX には触れていない）。
- **結果：magic レジスタは届いたが、画素はやはり 1 つも書かれなかった。G-5b の Checks は 0xB（bit2 だけが不成立）。広い走査にも何も出なかった。**
  - EpiGpuProbeStep は 0xFFFF。GpuGmuStatus は 15、NtStatus は 0xC0000185（Reason 13）。
  - KeepCx、DisableStatus、FaultStatus は 0。デバイスは Started。
- `GpuGmuG5`（G-5 の部分）は v66 とほぼ同じで、Checks 0x17、画素の一致は 0。
- `GpuGmuG5b`（764 B）：
  | 項目 | 値 |
  |---|---|
  | shadow／WPTR／RPTR | 48 → **61**／61／61 |
  | RBBM_STATUS／INT0／CP_HW_FAULT／CP_INT／PROTECT／STATUS_1 | 0x1／0／0／0／0／0 |
  | RB_CCU_CNTL：前 → 後 | 0x10000000 → 0x10000000 |
  | データ[12]：前 → 後 | 0 → CPU が 0xFFFFFFFF にした後 → **0xCAFE000A** |
  | 0x8E04（RB_DBG_ECO_CNTL）：前 → 後 | 0x00100000 → **0x04100000** |
  | 0x8E01：前 → 後 | 0xDEAFBEAD → 0xDEAFBEAD（CPU からは読めないレジスタ） |
  | 0xE12（UCHE_UNKNOWN_0E12）：前 → 後 | 0 → **1** |
  | 0xE19（UCHE_CLIENT_PF）：前 → 後 | 0x81 → 0x81 |
  | SMMU sGFSR／sGFSYNR0..2：前／後 | すべて 0／すべて 0 |
  | 画素の一致数／画素[0]・[15]・[16]・[255] | **0**／すべて 0xFFFFFFFF |
  | 番兵 | 16 個とも 0xFFFFFFFF |
  | 走査（7699 dword）：0x78563412／0x12345678／0x00000012 | **0／0／0** |
  | IB1_BASE／REM_SIZE | 0x70016100／0（G-5 の IB を最後まで処理した） |
  | FSR | 0x400 |
- わかったこと：
  - 候補 2（初期化の不足）：RB_DBG_ECO_CNTL と UCHE_UNKNOWN_0E12 を Mesa の値にしても変わらなかった。少なくともこの 3 つは原因ではない。
  - 候補 3（別の場所に書いた）：ring の領域 32 KB のどこにも、塗った色の値は現れなかった。私たちの CB にも、SMMU の global にもフォルトはない。
  - したがって、RB の書き込みは私たちの context bank を通っていない。**候補 1（secure mode のため、描画系の書き込みが secure の側に行くか捨てられる）が、いちばん有力になった** [I]。
  - 残る小さな候補 [I]：ほかの magic レジスタ（PC_POWER_CNTL＝1、SP_CHICKEN_BITS、PC_MODE_CNTL など）。2D エンジンは SP_2D_DST_FORMAT を使うので、SP 側の設定が要る可能性はゼロではない。
- 次に決めること（ユーザーの判断）：
  1. zap shader を読み込んで `CP_SET_SECURE_MODE 0` を送る道（§9.20.5）。TZ の PAS（SCM）を呼ぶ手段が要る。qcscm を戻すか、自前の SCM 呼び出しを作るかを決める必要がある。
  2. その前に、残りの magic レジスタを全部書く G-5c で、候補 2 を完全に消しておく道。安くて安全だが、効く見込みは低い [I]。

#### 9.21.23 zap shader の調査と Z-1 の提案（文章仕様、未実装）

ユーザーの判断で 1（zap）に進む。この節は調査と仕様だけで、コードは書いていない。

**Linux の手順**（master：`a6xx_gpu.c` の `hw_init`、`adreno_gpu.c` の `zap_shader_load_mdt`、`soc/qcom/mdt_loader.c`、`firmware/qcom/qcom_scm.c`、`qcom_scm-smc.c`、`qcom_scm.h`）
1. `a6xx_cp_init`（ME_INIT）が終わって idle になった後、GX が入ったまま `a6xx_zap_shader_init` を呼ぶ。1 回の起動で 1 回だけ（`static bool loaded`）。
2. `qcom_mdt_load`：
   1. メタデータ（ELF ヘッダ＋ハッシュのセグメント）を、物理的に連続で 4 KB 境界、キャッシュなしのバッファに置く。
   2. SCM `PAS_INIT_IMAGE(13, メタデータの物理アドレス)` を呼ぶ。
   3. 再配置できる（relocatable）イメージなら、`PAS_MEM_SETUP(13, 読み込み先, サイズ)` を呼ぶ。
   4. PT_LOAD のセグメントを、読み込み先にコピーする。
3. `qcom_pas_auth_and_reset(13)`：TZ がイメージを検証し、GPU の secure 側を設定する [I]。
4. 成功したら、ring で `CP_SET_SECURE_MODE`（0x70E60001、本体 0）を送って idle を待つ。zap がない端末だけ、代わりに 0xF400 を 0 にする（Epinephrine では禁止のまま）。

**SCM の呼び出し形式**（SMCCC、SiP、64 ビット、std call）
- x0 ＝ 0x42000000 ｜ (svc << 8) ｜ cmd、x1 ＝ arginfo、x2..x5 ＝ 引数、x6 ＝ 0、x7 ＝ 0。
- 戻り値：x0 ＝ 0 なら成功（負なら SCM のエラー）、x1..x3 ＝ 結果。PAS の呼び出しは、x0 が 0 で x1 も 0 のときだけ成功とみなす。
- arginfo ＝ 引数の数 ｜ (型 << (4 + 2×i))。型は VAL＝0、RO＝1、RW＝2。

  | 呼び出し | x0 | x1（arginfo） | 引数 |
  |---|---|---|---|
  | PAS_IS_SUPPORTED | 0x42000207 | 0x01 | 13 |
  | PAS_INIT_IMAGE | 0x42000201 | **0x82**（VAL, RW） | 13、メタデータの物理アドレス |
  | PAS_MEM_SETUP | 0x42000202 | 0x03 | 13、読み込み先の物理アドレス、サイズ |
  | PAS_AUTH_AND_RESET | 0x42000205 | 0x01 | 13 |

- やり直しの規則（`qcom_scm-smc.c`）：
  - x0 ＝ 1（INTERRUPTED）：x0 ＝ 1、x6 ＝ 返ってきた x6（セッション ID）、ほかの引数は同じで、もう一度 SMC する。
  - x0 ＝ −12（V2_EBUSY）：30 ms 待って、最初から同じ呼び出しをやり直す。最大 20 回。
  - x0 ＝ 2（WAITQ_SLEEP）：新しい TZ だけの仕組み。この端末で出たら、失敗として止める。
- Linux は呼び出しの前後に、暗号エンジンの帯域（interconnect）を投票している。Windows では、これを同じようには行えない [U]。TZ が自分で投票していれば問題ない。

**ファームウェア**（`stock-rom/vx/firmware/`。Nothing の署名なので、この端末の TZ で検証が通るはず [I]）
- `a660_zap.elf`（14264 B、SHA-256 ADB53DED…87965D6E0）を 1 つだけ使う。`.mdt`（6708 B）＝`.b00`＋`.b01`、`.b01` と `.b02` は `.elf` の 0x1000 と 0x3000 にある中身と同じことを確かめた。
- プログラムヘッダは 3 つ：
  | # | type | ファイル上の位置 | paddr | filesz／memsz | flags | 意味 |
  |---|---|---|---|---|---|---|
  | 0 | NULL | 0 | 0 | 0x94／0 | 0x07000000 | ELF ヘッダ |
  | 1 | NULL | 0x1000 | 0x6000 | 0x19A0／0x2000 | 0x02200000 | ハッシュのセグメント（flags & 0x07000000 ＝ 0x02000000） |
  | 2 | LOAD | 0x3000 | 0x5000 | 0x7B8／0x7B8 | **0x08000007** | 本体。bit27 ＝ **relocatable** |
- メタデータ ＝ `.elf` の [0, 0x94) ＋ [0x1000, 0x29A0)、合計 0x1A34（6708）B。
- relocatable なので、`MEM_SETUP` のサイズは ALIGN(0x5000＋0x7B8, 4K) − 0x5000 ＝ **0x1000**。セグメントは読み込み先の先頭（オフセット 0）に 0x7B8 B をコピーする。

**読み込み先：Linux の 0x8B71A000 は使わない**
- Linux の DT（`kodiak.dtsi`）は `gpu_zap_mem: zap@8b71a000`（0x2000）を使う。
- しかしこの端末の Windows では、PIL ドライバ（qcPILC、`ACPI\QCOM06E0`）の `SubsystemLoad\DHMS` が 0x8B200000..0x8B7FFFFF を使っている。0x8B71A000 はその中にある。書けば DHMS を壊す恐れがある。
- 代わりに、同じ設定にある **`SubsystemLoad\GFXSUC`（0x8B110000、0x5000、境界 0x1000）** を使う。これは Windows で GPU の secure microcode（zap）のために取ってある場所で、qcdx を入れていない今は使われていない [I]。イメージは relocatable なので、場所は `MEM_SETUP` で TZ に伝わる。
- [U] `PilConfig\HypProtectionEnabled` ＝ 1。この場所が HLOS（Windows）から書ける状態かどうかはわからない。書けなければ、CPU のコピーが例外になり、端末が bugcheck する恐れがある（再起動で戻る）。

**Windows の状況**
- `HypervisorPresent` は False。Windows は EL1 で動いている。
- qcPILC、qcPILFC、qcsubsys、QcTrEE に加えて、**qcscm、qcsecapp、QCRPEN も Running になっている**。メモリには「この 3 つは入れない」という 2026-11-07 の記録があるので、いつ戻ったのかをユーザーに確かめたい。
- qcscm が SCM の呼び出しを直列化しているはずなので、Epinephrine が自分で SMC すると、別の CPU からの呼び出しと重なることがある。TZ はそのとき EBUSY を返すので、上のやり直しの規則で扱う [I]。

**SMC の出し方**
- MSVC には `smc` の intrinsic がない。armasm64 の .asm に、x0..x7 を構造体から読んで `smc #0` を実行し、x0..x3 と x6 を書き戻す関数を 1 つ置く。vcxproj に MARMASM のビルド規則を足す必要がある。
- PASSIVE_LEVEL で呼ぶ。std call は時間がかかることがある（ハッシュの検証）。

**Z-1 の提案（bit29、0x3FFFFE80。G-5b の後に置く）**
- 前提：G-5b まで（G-5b の Checks ＝ 0xB と、画素が全部 0xFFFFFFFF のままであることを含む）。
- 置く場所：G-5b の後、同じ OOB の中。TZ の GPU の reset が GPU のレジスタを触るので、GX が入っている間に行う [I]。
- 手順：
  1. `C:\Epinephrine\fw\a660_zap.elf` を読む（SQE と同じ仕組み）。サイズ 14264 と、上のプログラムヘッダの値がすべて一致することを確かめる。
  2. `PAS_IS_SUPPORTED(13)`。x0 ＝ 0 かつ x1 ＝ 1 でなければ止める。
  3. メタデータ用に、連続で 4 KB 境界、キャッシュなしのバッファ（2 ページ）を取り、6708 B をコピーする。
  4. `PAS_INIT_IMAGE(13, そのバッファの物理アドレス)`。
  5. `PAS_MEM_SETUP(13, 0x8B110000, 0x1000)`。
  6. 0x8B110000 の 1 ページを、キャッシュなしでマップする。`.elf` の 0x3000 から 0x7B8 B をコピーし、残りは 0 で埋める。バリアを入れてからマップを外す。
  7. `PAS_AUTH_AND_RESET(13)`。
  8. メタデータのバッファを解放する。
  9. 2〜7 のどれかが失敗したら、`CP_SET_SECURE_MODE` は送らない。そのまま後片付けへ進む。一度でも INIT_IMAGE が成功していたら、TZ に状態が残るので、同じ起動の中で Z-1 をやり直さない。
  10. すべて成功したら、ring（dword 61..）に次を置く。
      - `CP_SET_SECURE_MODE`：0x70E60001、0。
      - `WAIT_FOR_IDLE`：0x70268000。
      - `INDIRECT_BUFFER`：0x70BF8003、0x70016100、0、36（G-5 の IB2 をもう一度）。
      - `EVENT_WRITE` CACHE_FLUSH_TS：0x70460004、4、0x70017034（データ[13]）、0、0xCAFE000B。
      - 合計 12 dword で、WPTR は 73。
  11. G-5 と同じく、fence 待ちと idle 待ちをする。
- 記録する項目：
  - SCM の呼び出しごとに、x0、x1、x2、x3、やり直しの回数（INTERRUPTED と EBUSY を別に）。
  - ファイルのサイズと、確かめたヘッダの値。メタデータの物理アドレス。
  - 0x8B110000 にコピーした後の先頭 4 dword を読み返した値。
  - G-5 と同じ項目（画素、番兵、データ[12..13]、レジスタ、FSR、global fault）。
- 成功の条件（Checks）：
  - bit0：4 つの SCM 呼び出しがすべて成功。
  - bit1：RPTR／WPTR／shadow が 73 で、エラーがない。
  - bit2：データ[13] ＝ 0xCAFE000B。
  - bit3：256 画素がすべて 0x78563412。
  - bit4：番兵が不変。
  - 全成功は 0x1F。
- 読み方 [I]：
  - bit0 だけが立たない：TZ が拒否した。どの呼び出しで、どのエラーかを見る。INIT_IMAGE で失敗なら署名かメタデータ、MEM_SETUP なら場所、AUTH なら帯域かイメージを疑う。
  - SCM が成功して、bit3 も立った：secure mode が原因だったことが確定する。
  - SCM が成功したのに、画素が出ない：secure mode 以外の原因がまだある（SP 側の magic レジスタなど）。
- 危険：
  - 0x8B110000 への書き込みが hyp に止められれば、bugcheck になる（再起動で戻る）。
  - TZ の中で何かが壊れても、再起動で元に戻ると考えられる。ただし RPMB などの secure storage には触れない呼び出しだけを使う。
  - 0xF400 は、この段階でも読み書きしない。

**Z-1 の実装の注意（補足）**

- 準備の段階：GX を入れる前に済ませる（PASSIVE）
  - ファイルを読み、上の表の値をすべて確かめ、メタデータのバッファを作るところまでを、GX を入れる前に行う。GX を入れている時間を短くし、ファイルの問題なら GX に触れる前に止めるためである。
  - どれかが違ったら、Z-1 は実行しない。Reason を記録して、G-5b までで終える。
- メモリの書き方
  - メタデータのバッファ：
    - 2 ページ（0x2000）を、物理的に連続、4 KB 境界、キャッシュなしで取る。4 GB より下にあると安全 [I]。例：`MmAllocateContiguousNodeMemory(0x2000, 0, 0xFFFFFFFF, 0, PAGE_READWRITE|PAGE_NOCACHE, MM_ANY_NODE_OK)`。
    - [0, 0x94) と [0x1000, 0x29A0) を続けて置き、残りは 0 で埋める。
  - 0x8B110000 は `PAGE_READWRITE|PAGE_WRITECOMBINE` でマップする（Linux も `MEMREMAP_WC`）。`PAGE_NOCACHE` だと Device 属性になり、そろっていないアクセスが例外になる。
  - キャッシュなしのメモリや WC のメモリには、`RtlCopyMemory`／`RtlZeroMemory` を使わない。4 バイト単位の単純なループで書く。これらの関数は、そろっていないアクセスや `DC ZVA` を使うことがあるからである。
  - 書き終えたら `KeMemoryBarrier` を入れる。0x8B110000 は、先頭の 4 dword を読み返して記録してから、マップを外す。
- SMC を出す関数（armasm64）
  - 引数と結果は、`struct EPI_SMC_REGS { ULONG64 X[8]; }` のような構造体 1 つで受け渡す。入るときに X[0..7] を x0..x7 に読み込み、`smc #0` の後で x0..x3 と x6 を書き戻す。
  - 構造体へのポインタは、callee-saved の x19 などに置く。関数の入口でスタックに退避し、出口で戻す。
  - x18 は Windows のプラットフォームレジスタ（TEB）なので、読み書きしない。SMCCC では、x18〜x30 は SMC の前後で保たれる。
  - SMC の前に `dsb sy` を入れておくと安心である。
  - vcxproj に MARMASM（armasm64）のビルド規則を足す必要がある。
- やり直しは C の側で扱う：
  | 返ってきた x0 | 意味 | 扱い |
  |---|---|---|
  | 0 | 呼び出しは成功 | x1 で PAS の結果を見る（IS_SUPPORTED は 1、ほかは 0 で成功） |
  | 1（INTERRUPTED） | TZ の中で割り込まれた | x0 ＝ 1、x6 ＝ 返ってきた x6 にして、ほかはそのままもう一度 SMC する。回数を数え、上限（例：1000 回）で失敗にする |
  | −12（0xFFFFFFFFFFFFFFF4、EBUSY） | ほかの呼び出し（qcscm など）と重なった | 30 ms 待って（PASSIVE で `KeDelayExecutionThread`）、最初からやり直す。最大 20 回 |
  | 2（WAITQ_SLEEP） | 新しい TZ だけの仕組み | 失敗として止める |
  | そのほかの負の値 | SCM のエラー | 生の値を記録して止める |
- 診断（GpuGmuZ1 として。細かい並びは実装に任せる）
  - G-5 と同じ描画の記録（画素、番兵、レジスタ、FSR、global fault）。データ[13] の前後。
  - ファイルのサイズ、ヘッダを確かめた結果（どの項目で違ったか）、メタデータの物理アドレス。
  - SCM の呼び出しごとに、x0、x1、x2、x3、INTERRUPTED の回数、EBUSY の回数、呼んだかどうか。
  - 0x8B110000 に書いた後の、先頭 4 dword の読み返し。
  - ring の 12 dword（Packet）。
- breadcrumb
  - SCM の呼び出しの直前と直後、0x8B110000 をマップする直前と書く直前に、それぞれ breadcrumb を書いて、レジストリを flush する。端末がリセットされても、どこで止まったかを読めるようにするためである。
  - 例：0x1400 前提、0x1401〜0x1408 は C-1〜C-4 の前後、0x1409 マップ、0x140A コピー、0x140B SET_SECURE_MODE の投入、0x140C／0x140D は fence／idle。
- 配置：
  - 私が `stock-rom/vx/firmware/a660_zap.elf` を、端末の `C:\Epinephrine\fw\` にコピーする。
  - SHA-256 が ADB53DED…87965D6E0 であることを確かめてから実行する。

## 参照

- 本家 Linux（master）：`drivers/gpu/drm/msm/adreno/{a6xx_gmu.c, a6xx_gmu.xml, a6xx_gpu.c, a6xx_hfi.c, a6xx_hfi.h, a6xx_catalog.c, adreno_gpu.c}`、`drivers/iommu/arm/arm-smmu/{arm-smmu.h, arm-smmu.c}`、`drivers/iommu/io-pgtable-arm.c`、`drivers/clk/qcom/{gpucc-sc7280.c, gcc-sc7280.c}`、`arch/arm64/boot/dts/qcom/{kodiak.dtsi, sm7325-nothing-spacewar.dts}`
- DSDT v33-akat：`\_SB.PEP0.G0MD / G4MD / G5MD`、`\_SB.GPU0._CRS / RESI`
- silicium/opus-session/report_qcdx_gx*.md（特に GX6、GX7、GX8、GX22、GX23）、session-logs/kd_gx_measure_2026-10-31.txt
- goals/.../GPU_STATUS_AND_OPTIONS.md、ANALYSIS_RESULTS.md（Kodiak パックの WDDM スタック）
- Arm System MMU Architecture Specification（SMMUv2）：`CBAR`、`SCTLR`、`TCR2` などの意味
- 事実の確認にだけ使い、コードは取り込んでいない。リポジトリ全体の出典とライセンスは [CREDITS.md](../../CREDITS.md)
