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

`EpiGpuProbe=0x80` を拡張した。FW_INIT_RESULT の完了確認後に HFI を開始し、PERF_TABLE（id 4、seq 1、59 DWORD）と BW_TABLE（id 3、seq 2、160 DWORD）を1件ずつ送って ACK を待つ。CORE_FW_START、START、GX_BW_PERF_VOTE、PREPARE_SLUMBER、GBIF QSB 設定は追加していない。キュー ABI の照合元は [本家 a6xx_hfi.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_hfi.h) と [a6xx_hfi.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_hfi.c)（事実のみ、コードの取り込みなし）。

- PERF_TABLE は GX が off / 315000 / 450000 kHz の3段、CX が off / 200000 kHz の2段。投票値は上記の実測 cmd-db に基づく固定値。BW_TABLE は上記7c3固定値を使う。両メッセージとも未使用の投票・アドレス・データは0、GX の ACD は0xFFFFFFFF。
- HFI_CTRL_INIT=1 の後、HFI_CTRL_STATUS bit0 を100 µs間隔で最大10 ms待つ。共有 RAM は volatile とメモリバリアで読み書きし、4 DWORDへのパディングを含む空きを確認してからコマンドを公開する。キュー位置・サイズ・index・メッセージ長を検証し、破損時は読み進めない。
- 各応答の待ち時間は100 µs間隔で最大1 s。MSGQ 割り込みをクリアし、応答を読む。id 100 以外で3 DWORD以上、packet[1] の seqnum が一致すれば応答とみなし、ret_header 全体が送信 header と一致し、error=0なら成功。id 126 の条件は設けない。読み飛ばしたパケットは件数・最後の長さ・先頭8 DWORDを記録する。FWエラー通知（id 100）は別途件数と最後の4 DWORDも記録して読み続ける。異常や時間切れでも既存のレジスタ・SMMU fault・ログの採取を行ってから CM3 を reset に戻す。
- BW_TABLE の write_index 公開後は、doorbell や応答待ちが失敗した場合も投票済みの可能性があると扱う。CM3 reset 後に最大10 ms待ち、CM3 reset の維持、RSCC SEQ_BUSY=0、RSCC_CONTROL_REQ/ACK=0、GX GDSC collapse / PWR_ON=0、GX HM OFF を確認する。rpmh_power_state は診断として記録し、0xFとの一致を条件にしない。上記条件を確認できなければ status 8 / KeepCx=1 とし、SMMU・RSCC・GMU設定・マップ・バッファ・CXを保持する。RSCC が停止していても CNOC投票が取り消された証明にはならない [U]。PREPARE_SLUMBER/OOB での停止は今回の対象外。

EpiDiag の追加診断（REG_BINARY は little endian）：

| 値 | 内容 |
|---|---|
| `GpuGmuHfiStatus`（REG_DWORD） | HFI 開始と2件の交換の NTSTATUS。0は両 ACK 成功、STATUS_NOT_FOUNDは未到達。`GpuGmuStatus=10` は HFI 試験失敗。teardown 失敗で status 8 になってもこの値は保持する |
| `GpuGmuHfiCtrlStatus`（REG_DWORD） | 開始待ちで最後に読んだ HFI_CTRL_STATUS。未読は0xFFFFFFFF |
| `GpuGmuHfiBwPublished`（REG_DWORD） | BW_TABLE の write_index を公開したら1 |
| `GpuGmuHfiIdleStatus`（REG_DWORD） | BW公開後、reset後の停止状態確認の NTSTATUS。0は上記条件を観測、STATUS_IO_TIMEOUTは未確認、STATUS_NOT_FOUNDは未試行 |
| `GpuGmuHfiMessages`（REG_BINARY） | PERF、BWの順で各148バイト、合計296バイト。各37 DWORD：`{SentHeader, NtStatus, AckDwords, Ack[19], Skipped, FwErrors, LastFwError[4], LastSkippedDwords, LastSkipped[8]}`。応答は `{header, ret_header, error, payload[最大16]}`。Skipped はFWエラー通知も含む読み飛ばし件数。LastSkippedDwords は最後に読み飛ばしたパケットの全長（DWORD数）、LastSkipped は先頭最大8 DWORD（短い場合の残りは0）。未送信の NtStatus は STATUS_NOT_FOUND。AckDwords=0は未取得。サイズ不正時は申告された長さと先頭headerを残す場合があるため、NtStatus・AckDwordsを併読する |
| `GpuGmuHfiQueues`（REG_BINARY） | 5レコード×28バイト＝140バイト。各 `{Stage, CmdRead, CmdWrite, RspRead, RspWrite, DebugRead, DebugWrite}`。Stage 0=開始前、1=HFI ready、2=PERF交換終了、3=BW交換終了、4=CM3 reset直後。デバッグキューは消費しない |
| `GpuGmuHfiPower`（REG_BINARY） | 4レコード×24バイト＝96バイト。各 `{Stage, RpmhPowerState, GxGdscr, GxHm, HfiCtrlStatus, Gmu2HostIntrInfo}`。Stage 0=開始前、1=HFI試験終了、2=CM3 reset後（BW公開時は停止待ちの後）、3=GMU/RSCC設定復元後 |
| `GpuGmuHfiRscc`（REG_BINARY） | 4レコード×20バイト＝80バイト。各 `{Stage, Status0, SeqBusy, ControlReq, ControlAck}`。Stage は Power と同じ。Status0 は RSCC dword 0x004、SeqBusy は0x101、ControlReq/ControlAck は GMU 側0x23B07/0x23B08。未取得フィールドは0xFFFFFFFF |

キュー・電源の未取得フィールドは0xFFFFFFFF。FW起動に失敗して HFI 未到達なら HFI バイナリ診断は空。`GpuGmuStatus=0` は FW起動・2件の ACK・後始末の成功を意味する。`GpuGmuHfiStatus=0` だけでは後始末の成功を意味しないため、`GpuGmuStatus`、`GpuGmuKeepCx`、`GpuCxStatus` を併読する。`GpuCxText` には `gmu_hfi_status` も追記する。

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

## 参照

- 本家 Linux（master）：`drivers/gpu/drm/msm/adreno/{a6xx_gmu.c, a6xx_gpu.c, a6xx_hfi.c, a6xx_catalog.c, adreno_gpu.c}`、`drivers/clk/qcom/{gpucc-sc7280.c, gcc-sc7280.c}`、`arch/arm64/boot/dts/qcom/{kodiak.dtsi, sm7325-nothing-spacewar.dts}`
- DSDT v33-akat：`\_SB.PEP0.G0MD / G4MD / G5MD`、`\_SB.GPU0._CRS / RESI`
- silicium/opus-session/report_qcdx_gx*.md（特に GX6、GX7、GX8、GX22、GX23）、session-logs/kd_gx_measure_2026-10-31.txt
- goals/.../GPU_STATUS_AND_OPTIONS.md、ANALYSIS_RESULTS.md（Kodiak パックの WDDM スタック）
