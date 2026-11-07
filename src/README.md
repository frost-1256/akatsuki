# src

Epinephrine のソースコード置き場。

- `epinephrine/`：WDK サンプル KMDOD（microsoft/Windows-driver-samples の video/KMDOD、**MS-PL**）を土台にしたドライバ本体。出自は `epinephrine/UPSTREAM.md`
- ビルド：EWDK の ISO をマウントして `tools/build_epinephrine.ps1`（ARM64 Release、未署名）→ `tools/sign_epinephrine.ps1`（テスト署名）。出力は `build/Release/SampleDisplay/`（Epinephrine.sys、Epinephrine.inf、epinephrine.cat）
- 構成の計画は [docs/m1/AKATSUKI_M1_PLAN.md](../docs/m1/AKATSUKI_M1_PLAN.md) の WP2 を参照
