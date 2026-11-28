# Sekiro test results

- The archive test failed on duplicate data from concurrent readers and successful short reads before the fix. All four cases passed on Linux and Wine after the fix.
- The temporary LAN archive fixture failed to connect from Series S. Hardware archive transfer remains unverified.
- Xbox packages 0.2.5.31 and 0.2.5.32 built, signed, installed and opened their dashboards through Device Portal. The focused D3D11 test matched all expected pixels on both builds.
- The account ownership fixture verifies that installed-only entries are absent from the entitlements passed to the guest runtime. The Series S reported three installed-only Steam library rows with `account_owned=false`.
- All 12 automated Xbox validation checks passed offline. The live preflight reported `steam_signed_out`.
