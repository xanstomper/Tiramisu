# Sekiro test results

- The archive test failed on duplicate data from concurrent readers and successful short reads before the fix. All four cases passed on Linux and Wine after the fix.
- The temporary LAN archive fixture failed to connect from Series S. Hardware archive transfer remains unverified.
- Xbox packages 0.2.5.31 and 0.2.5.32 built, signed, installed and opened their dashboards through Device Portal. The focused D3D11 test matched all expected pixels on both builds.
- The account ownership fixture verifies that installed-only entries are absent from the entitlements passed to the guest runtime. The Series S reported three installed-only Steam library rows with `account_owned=false`.
- All 44 Python tooling tests passed, including the 12 automated Xbox validation checks. The live 0.2.5.34 preflight reported `replacement_marker_present`. The follow-up preflight test suite passed all 13 cases.
- Five new DXGI assertions failed before the fix. All 83 focused D3D/DXGI checks passed after it. On Series S 0.2.5.32 returned a different device and the same fixture passed on 0.2.5.33.
- The WebXfer suite passed all 102 checks. Four AppID regressions failed before the parser fix. On 0.2.5.34 the dashboard omitted the legacy AppID and preserved the console install.
