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
