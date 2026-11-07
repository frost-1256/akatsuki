# Akatsuki M1 — 未決事項1「バインド先 ACPI ノード」机上検討（ドラフト）

2026-11-07 作成（Muse 机上作業）。スコープ文書 `AKATSUKI_M1_SCOPE.md` の未決事項1に対する下調べ。決定ではなく、はるの判断材料。DSDT は `~/workspace/silicium/Silicium-ACPI-fork/Platforms/Nothing/spacewar/Decompiled/DSDT.dsl`（v24 系譜）を直接読んだ。

## 確認できた現状（DSDT の実測値）

- **GPU0 は表示・グラフィックス・ビデオを1ノードに束ねている**（`Device (GPU0)`、DSDT.dsl:14038〜）
  - `_HID "QCOM0A36"`、`_STA` は `0x0F`（常時 present）、`_DOD` あり、子に `MON0`（`_ADR 0`）
  - `_DEP` は現在 8 件：`MMU0, MMU1, IMM0, IMM1, PEP0, PMIC, RPEN, SCM0`。**PILC／TREE は入っていない**（v13b で除去されたまま。復帰条件は 2026-11-06 に満たされたが復帰は未実施）
  - `_CRS` は 14 リソース。`RESI` メソッドの名前付き一覧と対応する（順序の対応は未検証のため、名前ベースで読むこと）：
    - 表示系：`MDP_REGS`（0x0AE00000, 2MB — DSI レジスタはこの窓の +0x94000 付近に含まれる）、`DP_PHY_REGS`、`VSYNC_INTERRUPT`（IRQ 0x73）、`DSI_PANEL_RESET`（GPIO 44）
    - グラフィックス系：`GFX_REGS`（0x03D00000）、`GFX_REG_CONT`、`GFX_INTERRUPT`（IRQ 0x14C）、`GPU_PDC_SEQ_MEM`、`GPU_PDC_REGS`、`GPU_CC`、`GPU_RSCC`、`GPU_RPMH_CPRF`
    - ビデオ系：`VIDEO_REGS`（0x0AA00000）、`VIDC_INTERRUPT`（IRQ 0xCE）
  - `_ROM`（PCFG パネル XML、EDID 合成の元）が GPU0 に付いている。EDID が壊れると dxgkrnl がアダプタを外す、という本線の知見がある
- **公式ドライバのバインドは HID マッチ**：`qcdx7325.inf`（UTF-16LE のため素の grep では見えない。デコードして確認）は `ACPI\VEN_QCOM&DEV_0A36`（＋ `_HRV` 由来の REV 別バリアント）にマッチする。つまり GPU0 の `_HID` が QCOM0A36 である限り、qcdxkm が第一候補になる
- **表示専用ノードは存在しない**：DSDT 内に MDP／DSI 単独のデバイスはない。作るなら新規
- **PILC／TREE は両方 `_STA 0x0F`**（PILC: QCOM06E0、TREE: QCOM04DE）。`MMU0/MM1` は QCOM0A09 で `_DEP` は PEP0 のみ。`IMM0/IMM1` は QCOM068F

## 選択肢

### A. GPU0 を流用する（推し案・M1a 向け）

- やること：GPU0 の `_HID` を自作ドライバ用の別 ID に差し替え（例：`QCOM0A36` → 新規 ID）、Epinephrine 側の INF をその ID にマッチさせる。qcdxkm はもう GPU0 にバインドしない。`_CRS`・`_DEP`・`_ROM`（PCFG）・`MON0`・`_DOD` はそのまま使える
- 利点：差分が最小。EDID 合成の土台（PCFG）がそのまま残る。`_DEP` の 8 件も、M1a の「真っ黒で安定」には過不足なく付いてくる
- 注意点：
  - M1a のドライバは GFX／VIDEO 系のリソース（レジスタ窓・割り込み）も受け取ることになる。使わないものは開かない／要求しない設計にする（触らなければ害はない、が、不用意な初期化はしない）
  - `_DEP` に PILC／TREE が無い現状は、M1a〜M1b（テストパターンは MDP 内蔵生成で GPU シェーダ不要）にはむしろ都合がいい。M1c 以降でサブシステム起動が必要になるかは、その段で再検討
  - 公式ドライバとの排他は `_HID` 差し替え＋ DriverStore からの qcdxkm 除去（または ConfigFlags による無効化）の両方で担保する。片方だけだと取り違えの余地が残る
  - DSDT 変更なので DSL と AML の両方をコミットする（v22 の出荷フローと同じ轍を踏まない）

### B. 表示専用の新ノードを切る（例：`MDP0`）

- やること：`_HID` を新規 ID にした新デバイスを作り、`_CRS` は表示系 4 件（MDP_REGS、DP_PHY_REGS、VSYNC_INTERRUPT、DSI_PANEL_RESET）だけ、`_DEP` は表示に要るものだけ（PEP0、PMIC、MMU0/1 あたりを候補に）、子に MON0、`_ROM` に PCFG のコピーを付ける。GPU0 は `_STA 0` に落とすか無効化する
- 利点：隔離がきれい。Epinephrine が見るリソースが表示系だけになり、GFX 系の誤作動の心配がない
- 注意点：
  - **同じ MMIO 窓を GPU0 と新ノードの両方が主張すると競合する**。GPU0 側を確実に無効化しないと成立しない（`_STA 0` への書き換え＋ ConfigFlags での無効化の二重化が要る）
  - `_ROM`（PCFG）のコピーは、コピー元との乖離の温床になる。EDID 合成が壊れるとアダプタが外れる、という本線の罠がそのまま適用される
  - `_DEP` の絞り込みは「表示に何が要るか」の実証が要る。絞りすぎると列挙ゲートで弾かれる（`_DEP` は列挙ゲートなので、依存先が開始失敗するとノード自体が出てこない）
  - 工数は A より明らかに大きい

## 提案

- **M1a は A で入る**。差分が最小で、PCFG／MON0／_DEP を流用できる。B は、A で GFX 系の依存が実際に邪魔をしたと分かったときの逃げ道として取っておく
- 決める前に未検証の 2 点（机上では決められない）：
  1. `_CRS` と `RESI` の対応順序（名前ベースで読む分には問題ないが、ドライバが順序依存でリソースを取るなら要確認）
  2. 表示系だけで本当に足りる `_DEP` の最小集合（M1a は A なら現状 8 件のままでよいので、実は M1a の時点では決めなくてよい。B を選ぶときだけ必須）

## 参考：本線の再インストール状況との関係

- 2026-11-06 深夜、はるが TrEE 系ドライバを抜いた再インストールで Getting Ready まで到達したところで会話が止まっている。M1 の DSDT 変更（A 案の `_HID` 差し替え）は、**再インストールが安定してから**別ブランチで試すもの。いまの不安定な WIN に重ねないこと
- PILC／TREE の GPU0 `_DEP` 復帰は、本線側の未実施事項として残っている。A 案では M1a 時点では復帰しなくてよい（上記の通り）が、復帰する場合は M1 の `_DEP` も一緒に見直す
