# 出自

このディレクトリは WDK サンプル KMDOD（Kernel Mode Display Only Driver、"Basic Display Driver"）を土台にしている。

- 取得元：https://github.com/microsoft/Windows-driver-samples の `video/KMDOD`
- 取得したコミット：`d49bb7d72f3651952b2b6e4ad28d14fe2247eb69`（2026-11-05）
- ライセンス：**Microsoft Public License（MS-PL）**。全文は `LICENSE-MSPL.txt`。
  - このディレクトリのうち、上記サンプルに由来するファイル（およびその改変版）は MS-PL に従う。ソースで配布するときは MS-PL のままにする。
  - Epinephrine のために新しく書いたファイルは、リポジトリ直下の `LICENSE`（MIT）に従う。新規ファイルは先頭にその旨を書く。
- 取り込みの最初のコミットは無改変のコピー。以降の改変は git の履歴で追える。

## render-only-sample（W-1）

`epi_wddm.cxx` の DDI の形（DDI の表、segment と caps、allocation、context、BuildPagingBuffer、DMA の完了通知）は、Microsoft の render-only-sample（ROS）の `roskmd` から取り込んで改変した。

- 取得元：https://github.com/microsoft/graphics-driver-samples の `render-only-sample/roskmd`
- 取得したコミット：`de4a216`
- ライセンス：**MIT**（Copyright (c) Microsoft Corporation）。全文は `LICENSE-ROS-MIT.txt`。`epi_wddm.cxx` / `epi_wddm.hxx` の先頭にその旨を書いた。
- 取り除いたもの：VC4 のハードウェアの部分、WPP の trace、ACPI と電源の component。表示の部分（Vc4Display）は使わず、KMDOD 由来の VidPN の実装を共有する。
