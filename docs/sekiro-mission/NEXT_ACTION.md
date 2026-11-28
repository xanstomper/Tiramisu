# Resume the Sekiro mission

1. Confirm the Series S is idle and record the active Xan.Tiramisu package.
2. Sign into Steam through Tiramisu on the console. Confirm a current account library entry for Sekiro, AppID 814380, and bind a fresh authenticated install or a separately verified original PC install to the exact game entry.
3. Launch that entry through normal Play. Record the first failed checkpoint and collect only approved redacted diagnostics. Start on the default hardware renderer and use WARP for a diagnostic comparison if the converter fails again.
4. Verify readable title and dialog text, then proceed through New Game, the opening scene, and visible controller movement. Record audio, combat, saves, and sustained play only when observed.

Candidate 0.2.5.34 is on the Series S. Its signed package SHA-256 is `51f56aa97696703ecb841e584beca232411085ded97752112f558302910dec79`. The candidate uses the previous private hardware diagnostic snapshot plus the archive, Steam ownership, AppID parsing and DXGI device lookup fixes.

Run [validate_sekiro_xbox.py](../../tools/validate_sekiro_xbox.py) from the host that can reach Xbox Dev Mode. Provide `--host`, the exact `--package`, the verified `--game` folder, and a fresh `--report` path. Use `--verified-original-install` after verifying the PC files. Add `--launch` to queue Play and sample checkpoints. Observe actual output on the Xbox display. The harness records Present rates for diagnosis.
