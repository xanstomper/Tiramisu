# Regression report

| Check | Result |
| --- | --- |
| Archive duplicate and short-read regressions | Passed on Linux and Wine. |
| End-of-file clipping and seek range | Passed on Linux and Wine. |
| D3D11 single-channel font atlas | Passed with 0.2.5.30, 0.2.5.31, 0.2.5.32 and 0.2.5.33 on Series S. |
| Steam library metadata | Three local items stayed installed and reported no account ownership on 0.2.5.32. |
| DXGI swap-chain device lookup | On Series S 0.2.5.32 returned the wrong device. 0.2.5.33 returned the creation device and cleared the output for an unsupported IID. |
| AppID parsing | 102 WebXfer assertions passed. The legacy sidecar is still present and 0.2.5.34 no longer advertises its placeholder. |
| Shader Model 5 texture and pixel reference | R8 and A8 channel values matched the Wine/DXVK baseline on Series S. |
| Full corpus and title gameplay | Pending for the candidate. |
