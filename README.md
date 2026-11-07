# Akatsuki Project

**Akatsuki Project** は、Qualcomm Adreno GPU 向けのフリーな Windows ドライバを、クリーンルーム方式（ブラックボックス観測と仕様化による再実装）で作るプロジェクトです。ドライバ本体の名前は **Epinephrine**。

土台にあるのは、Nothing Phone (1)「spacewar」(SM7325) へ Windows 11 を移植してきた調査の蓄積と、freedreno / Turnip の公開知見です。

## 現在の状態

- 2026-11-07 始動。最初のマイルストーン **M1 = 表示専用ドライバ**（レンダリングなしで内蔵パネルに Windows の画面を出す）の計画が完了し、M1a（骨格）の実装に入るところ
- M1 の範囲・決定・計画は [docs/m1/](docs/m1/) にある
  - [AKATSUKI_M1_SCOPE.md](docs/m1/AKATSUKI_M1_SCOPE.md) — 範囲と設計規則
  - [AKATSUKI_M1_DECISIONS.md](docs/m1/AKATSUKI_M1_DECISIONS.md) — 未決 5 点の決定
  - [AKATSUKI_M1_PLAN.md](docs/m1/AKATSUKI_M1_PLAN.md) — 作業パッケージへの分解
- 構想全体は [docs/CONCEPT.md](docs/CONCEPT.md)

## 設計の要点（M1）

- バインド先は GPU0 を流用し、識別子（ACPI HID）を自作用 `AKAT0001` に差し替える
- 表示専用モデルは Kernel Mode Display-Only Driver (KMDOD) 系
- フレームバッファは SMMU 変換経由の通常メモリ（PTM=1 設計）
- 圧縮 (DSC) はオンを正とし、パラメータは実測で確定したセットを使う
- 電源は Platform Extension Plug-in のデバイス電源モデルへ PoFx で正規登録する。モデルの外からレールを直接叩かない

## プラットフォーム設定

このドライバが依存するプラットフォーム側の設定（DSDT 差分、PEP エントリ等）は [platform/MANIFEST.md](platform/MANIFEST.md) で一覧管理する。プロプライエタリなバイナリ、証明書、パスワード類はこのリポジトリに含めない。

## ライセンス

MIT License（[LICENSE](LICENSE)）。土台に取り込む予定の WDK サンプル (KMDOD) は Microsoft の MIT ライセンスに従う。
