# Akatsuki Project

~~プロプラドライバに殺意が湧いて~~ オープンソースのWindows用Adrenoドライバーを作るプロジェクト  
Akatsuki Ephinephrineって言います、Akatsukiは特に由来無くて、EphinephrineはAdrenoの語源がAdrenalineで国際一般名がEphinephrineだから。かっこいいしね。  

大元はもれがNothing Phone (1)「spacewar」にWin11を移植しようとしたときに苦しみながらGPUドライバを動作させようとしたときのいろいろな情報から。  
現状としては普通にまず動作させることを第一目標としているからまぁバイブコーディングだけど今後としては普通に勉強とかしてちゃんと書きたい。  

なんで寺院に機械があんだよ

教えはどうなってんだ教えは

お前ら禁じられた機械を平気で使ってんじゃねえか

分かってんのか！？

「シン」が生まれたのは人間が機械に甘えたせいだろうが

金取んのかよ！？

くそったれ！

## 現在の状態（2026-11-08）

- **M1 = 表示専用ドライバ**：Epinephrine が `ACPI\AKAT0001` に入り、内蔵パネル（1080×2400）に Windows の画面を出している。これまでにできたこと：
  - PCFG から EDID を作る
  - 実際のリフレッシュレートを使う
  - VSync（ソフトウェア／INTF1 の割り込み）
  - 速い CPU 転送
  - DSI でパネルの明るさを変える
- **G0/G1 = GPU コアの立ち上げ**（試験用の `EpiGpuProbe` で 1 段ずつ確かめている）：
  - GPU_CC を読む
  - CX の電源を入れる
  - adreno SMMU を設定する
  - GMU のファームウェアを起動する（FW_INIT_RESULT=0x100）
  - HFI でメッセージを送り、6 通すべてに ACK が返る
  - GMU のクロックを 200 MHz にする
  - **GX の電源を入れて、slumber で切り、後片付けまで通す**（v47、2026-11-08）
- 次は、GX が ON の間に GPU 本体のレジスタ（chip id など）を読むこと。まだ描画はできない
- 記録：
  - 調査と実機の結果：[docs/g0/G0_GPU_SURVEY.md](docs/g0/G0_GPU_SURVEY.md)
  - SMMU／GMU の API：[docs/g0/G1C_SMMU_GMU_API.md](docs/g0/G1C_SMMU_GMU_API.md)
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

Epinephrine のために新しく書いたコードは MIT License（[LICENSE](LICENSE)）。

土台の WDK サンプル KMDOD（`src/epinephrine/` のうちサンプル由来のファイルとその改変版）は **Microsoft Public License（MS-PL）** に従う（[src/epinephrine/LICENSE-MSPL.txt](src/epinephrine/LICENSE-MSPL.txt)、出自は [src/epinephrine/UPSTREAM.md](src/epinephrine/UPSTREAM.md)）。リポジトリは MIT と MS-PL の混在になる。

Linux カーネル（GPL-2.0）などは、ハードウェアの事実の確認だけに使い、コードは取り込んでいない。使った資料とデータの一覧は [CREDITS.md](CREDITS.md)。
