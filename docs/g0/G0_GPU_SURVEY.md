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

## 参照

- 本家 Linux（master）：`drivers/gpu/drm/msm/adreno/{a6xx_gmu.c, a6xx_gpu.c, a6xx_hfi.c, a6xx_catalog.c, adreno_gpu.c}`、`drivers/clk/qcom/{gpucc-sc7280.c, gcc-sc7280.c}`、`arch/arm64/boot/dts/qcom/{kodiak.dtsi, sm7325-nothing-spacewar.dts}`
- DSDT v33-akat：`\_SB.PEP0.G0MD / G4MD / G5MD`、`\_SB.GPU0._CRS / RESI`
- silicium/opus-session/report_qcdx_gx*.md（特に GX6、GX7、GX8、GX22、GX23）、session-logs/kd_gx_measure_2026-10-31.txt
- goals/.../GPU_STATUS_AND_OPTIONS.md、ANALYSIS_RESULTS.md（Kodiak パックの WDDM スタック）
