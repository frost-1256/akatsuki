# DSDT 差分（WP1：GPU0 の HID 差し替え）

| 項目 | 値 |
|---|---|
| 変更 | GPU0 `_HID` `"QCOM0A36"` → `"AKAT0001"`（この 1 点のみ） |
| ベース | v32（Silicium-ACPI `511ef62`。v27 に WPSS `_DEP` = SSDD を足したもの。再インストール後の環境はこれがないと起動時に 0xA） |
| Silicium-ACPI | `frost-1256/Silicium-ACPI` ブランチ `test/v33-akat`、コミット `c76edb32ce337a8bf9cf8843d498d79a95032e6b` |
| Mu-Silicium | `frost-1256/Mu-Silicium` ブランチ `test/v33-akat`（サブモジュールを上記に更新）、CI run 42364476078 |
| コンパイラ | iasl 20260408（v32 と同じ。v32 の AML は DSL からバイト単位で再現できることを確認済み） |
| DSDT.aml | 81,507 バイト（v32 と同じ長さ）、SHA256 `7d47c7a58f74963aad41da8ed731d1f731d59e19c6abc6ee2ecdd110d3cbe938` |
| UEFI イメージ | `Mu-spacewar-v33-akat.img`、SHA256 `2c4bcfad809cf9461f6699d5c9c008b929ed7f6c45cac3d5b104cc1980963cb4` |
| 検証 | v32 との逆コンパイル差分は、ヘッダのチェックサム（0x62 → 0x8A）と GPU0 `_HID` の 1 行だけ。AML のバイト差分も 8 バイト（チェックサム 1、HID 7） |

- `0001-gpu0-hid-akat0001.patch` は DSL の差分（`Platforms/Nothing/spacewar/Decompiled/DSDT.dsl`）。AML 本体は Silicium-ACPI 側のコミットにある。
- ロールバックは `Mu-spacewar-v32.img` に戻すだけでよい（qcdxkm が再び GPU0 にバインドする）。
- 計画書（`docs/m1/AKATSUKI_M1_PLAN.md` WP1）では、ベースを v24、コンパイラを iasl 20260930 としていた。上の理由で、v32 と iasl 20260408 に変更した。

## v34-akat（G1c-1：adreno SMMU を Epinephrine が持つ）

| 項目 | 値 |
|---|---|
| 変更 | `\_SB.MMU1`（adreno SMMU、qcsmmu）と `\_SB.IMM1`（qciommu）の `_STA` を 0x0F から 0 に変えた。GPU0 の `_DEP` を 8 件から 6 件にした（MMU1 と IMM1 を外した） |
| 理由 | MMU1 と IMM1 を使うのは GPU0 だけ。GMU と GPU の SMMU の設定は Epinephrine が自分で行う（G0 §9.1、案 S1） |
| ベース | v33-akat |
| Silicium-ACPI | `test/v34-akat`、コミット `5fe92aac34dafcddd95d382b8541ea05320a9c59` |
| Mu-Silicium | `test/v34-akat`（サブモジュールの更新だけ）、CI run 42382200618 |
| コンパイラ | iasl 20260408。v33 の AML をバイト単位で再現できることを確かめた |
| DSDT.aml | 81,497 バイト、SHA256 `7226f5f111806bc56abfbc8c361d34a2feb9bde3bc125fcbfcd1f663bbcf1bf2` |
| 検証 | v33 との逆コンパイル差分は、ヘッダ（長さ、チェックサム 0x8A → 0xFC）と上の 3 か所だけ |
| ロールバック | v33-akat のイメージで起動し直す |
