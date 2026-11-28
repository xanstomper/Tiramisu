# Sekiro API coverage

| Interface | Evidence |
| --- | --- |
| Win64 PE and D3D11/DXGI | Prior traces reached guest entry, archive opens and D3D11 Presents. |
| Windows font outlines | The prior trace recorded zero calls during the affected title prompt and dialog. The independent DirectWrite implementation passed focused GDI tests. |
| D3DX11 | The historical executable imported D3DX11SaveTextureToFileW. Startup use is unverified. |
| Steamworks | Store account ownership and local install readiness have separate fields. Original Steam integration requires an authenticated run. |
| FMOD, Bink, compression and input | Bundled DLL names appear in the historical install. Real playback, decoding and gameplay input remain unverified. |
