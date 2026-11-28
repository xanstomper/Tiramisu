# Sekiro blockers

1. The standard Xbox package and the older 0.2.4.12 package contain Sekiro folders with Spacewar AppID 480 and a replacement Steam DLL. A USB rescan showed zero Tiramisu-readable volumes. The 0.2.5.34 preflight inspected the exact local folder and reported `replacement_marker_present`. Steam is also signed out. Device Portal did not list a separate Sekiro package in Dev Mode.
2. The last hardware rendering trace with the registered title reported a fatal error inside the system DXBC conversion path. WARP got further and produced incomplete menu pixels. The source of the missing artwork and text remains undetermined.
3. The game agreement screen requires a readable frame and user action if it appears during a genuine run.
