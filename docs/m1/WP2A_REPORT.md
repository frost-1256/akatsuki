# WP2a 報告：KMDOD の取り込みと ARM64 ビルド（2026-11-07）

## 要約
- KMDOD サンプルを無改変で取り込み（`791e631`、MS-PL）、Epinephrine に改名して `ACPI\AKAT0001` だけにマッチする INF を書いた。
- EWDK（`EWDK_br_release_svc_im_28000_260714-1640.iso`、WDK 10.0.28000.0、VS 18 BuildTools）で ARM64 Release のビルドが通る。警告 0、カタログ生成も成功。
- テスト署名（spacewar テスト証明書 `270E8946…F890`）で sys と cat に署名した。
- ドライバの動作は **まだサンプルのまま**（POST フレームバッファを引き継ぐ Basic Display 相当）。表示ブロック固有の処理は WP2b で入れる。

## 改変点（サンプルからの差分）
| ファイル | 変更 |
|---|---|
| `Sample/sampledisplay.inf` → `Sample/Epinephrine.inf` | 書き直し。マッチは `ACPI\AKAT0001` のみ（NTarm64.10.0...16299）。サービス名 `Epinephrine`、DIRID 13（DriverStore から直接実行）、StartType demand、ErrorControl ignore |
| `Sample/SampleDisplay.vcxproj` | TargetName を `Epinephrine` に |
| `sampledisplay.rc` | バージョン情報の説明とファイル名 |

**注意：サンプルの INF は PCI／ACPI のクラスコード（`ACPI\CLS_0003&SUBCLS_0000` など）にもマッチしていた。** GPU0 は `_CLS 03,00,00` を持つので、それを残すと `_HID` と無関係に GPU0（公式 DSDT でも）にバインドしてしまう。Epinephrine の INF からは削除済み。

## 成果物（`build/Release/SampleDisplay/`、リポジトリには含めない）
| ファイル | SHA256 |
|---|---|
| Epinephrine.sys（署名済み、ARM64 0xAA64、Subsystem 1） | `3D357E972010F701AAB94213CE612BE7E76B32B372FFD7ED62BB9AE2F40EC907` |
| epinephrine.cat（署名済み） | `1C1823559BE01FC237793950279DBA0473FEF211C932C03DA19B1B22D9F11BC7` |
| Epinephrine.inf（stampinf 済み） | `7F37345D624E1DBC0F62C0FB6D65B5D32A1DA9C660AAC366D7891AC8B8C7E1D0` |

- 署名の検証（`signtool verify /pa`）は PC 上では「信頼されていないルート」で失敗する。テスト証明書なので想定どおり。

## 手順
1. EWDK の ISO をマウントする（`Mount-DiskImage`）。
2. `powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_epinephrine.ps1`
3. `powershell -NoProfile -ExecutionPolicy Bypass -File tools\sign_epinephrine.ps1`
- ログは `build/build.log`、`build/sign.log`。

## レビュー
- 今回は Codex の残量が少ないため、別主体レビューは未実施（Claude の自己確認のみ）。実機投入の前にレビューを通すこと。

## 次（WP2b）
- `_CRS` を RESI の名前で対応付け、表示系 4 件だけをマップする（GFX／VIDEO 系は触らない）。
- モード 1080×2400@60、PCFG（`_ROM`）からの EDID 合成、VSYNC（IRQ 0x73）のカウント、診断用 Ioctl。
- 実機の M1a 試験（WP6）の前に、サンプルのままの版を一度だけ `v33-akat` で動かして「Started になる／POST フレームバッファで表示される／凍結しない」を見る案もある（POST 表示の引き継ぎだけで、ハードウェアには触らない）。
