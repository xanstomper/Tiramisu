# Mouse support on Xbox (Tiramisu host)

Two separate gates decide whether a pointer reaches a UWP app on Xbox. Only the
first one is in our control.

## 1. Input plumbing inside the app (ours, already in place)

`src/host/main.cpp` and `src/trt/input.cpp` already consume every pointer source
the shell is willing to deliver:

- `CoreWindow::PointerPressed` / `PointerMoved` / `PointerReleased` with
  `PointerUpdateKind` mapped to guest button bits.
- `GameInputKindMouse` via `native_gameinput.h` (relative deltas + buttons),
  which takes priority over synthetic pad-as-mouse movement.
- `webxfer.cpp` pad-as-mouse as the fallback for controller-only sessions.

So the guest side (`trt_msg` ring, `GetCursorPos`, `WM_MOUSEMOVE`) is wired. If
the shell delivers pointer events, Tiramisu already forwards them to the game.

Observed on Series S: `input: GameInput mouse callback failed hr=0x80004001`
(`E_NOTIMPL`). `webxfer.cpp` logs this and falls back to controller input. That
is the shell refusing the gamepad-adjacent mouse subscription, not a Tiramisu
bug.

## 2. The per-title allowlist (Microsoft's, not ours)

Microsoft's position, unchanged since 2020:

> Xbox supports the use of mouse and keyboard in some games and apps, but it
> doesn't work for all content. **The game or app publisher must enable this
> feature for their content.**
> — Xbox Support, "Navigate with a mouse and keyboard"

Enabling is a per-title publisher opt-in. For a sideloaded dev-mode package
there is no supported switch. Everything below is an unsupported experiment.

## What "spoofing the app name" can and cannot do

The relevant identifier is the **Package Family Name**:

```
PFN = <Identity Name> + "_" + <PublisherId>
```

- `Identity Name` is free text you choose (`Xan.Tiramisu` today).
- `PublisherId` is a 13-character hash **derived from the `Publisher` string**,
  e.g. `CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond,
  S=Washington, C=US` → `8wekyb3d8bbwe`.
- The manifest `Publisher` must match exactly the subject of the signing
  certificate, field order included, or SignTool fails with `0x8007000B`.

Consequences:

1. You *can* set `Identity Name="Microsoft.MicrosoftEdge"` in a copy of the
   manifest — nothing stops that.
2. You can also make a **self-signed** certificate whose subject is exactly
   `CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond,
   S=Washington, C=US`, and then `PublisherId` computes to `8wekyb3d8bbwe`.
   Anyone can mint a cert with any subject; dev-mode sideload only needs the
   cert trusted on the target.
3. Therefore the *string* `Microsoft.MicrosoftEdge_8wekyb3d8bbwe` is
   reproducible. What is **not** reproducible is Microsoft's signature — if the
   gate ever validates the signer chain rather than the PFN string, the spoof
   fails at install or at launch.
4. If a package with that identity is already registered on the console,
   deployment refuses with `0x80073CF3` ("a different package with the same name
   is already installed"). In Dev Mode retail apps are not registered, so a
   collision is unlikely but possible.

**Unverified premise:** there is no public documentation that the mouse
allowlist is keyed on PFN at all. The documented mechanism is a publisher
opt-in tied to the title, which is server/store side. It is equally plausible
that the console checks a signed title configuration and ignores local identity
entirely. Treat the spoof as a hypothesis to test, not a known bypass.

The "channel/release" part has no documented per-app knob either. The switches
that exist are the console OS Insider ring (Settings → Insider) and Store app
flight rings; neither is settable for a sideloaded package.

## Experiment (unsupported, own console, dev mode)

The spoofer ships as the **KBM edition** of every release (the normal
`Tiramisu.msix` keeps the public `Xan.Tiramisu` identity). The one-command entry
point builds the experimental flavor into its own output dir:

- `tools/edge_spoof.py` — patches the manifest Identity/Publisher to
  `Microsoft.MicrosoftEdge` + the Microsoft publisher string (PFN
  `Microsoft.MicrosoftEdge_8wekyb3d8bbwe`) and exposes the matching cert subject.
- `tools/pack.sh` — `EDGE_SPOOF=1` (or `--edge-spoof`) stages the patched
  manifest and signs with a *separate* self-signed cert (`edge-dev.cer`) whose
  subject matches the Microsoft Publisher string. The normal `dev.cer` is never
  reused or overwritten.
- `tools/edge-spoof.sh` — one-command entry point, builds into a separate
  `out-edgespoof/` so the normal build is untouched.
- `tools/release.sh` — builds the host once and packs **both** flavors into
  `out/release/` (`Tiramisu.msix` + `dev.cer`, `Tiramisu-EdgeSpoof.msix` +
  `edge-dev.cer`). Keep both in the GitHub Release: public builds cannot unlock
  hardware mouse, so the KBM artifact is the only path for keyboard/mouse users.

```sh
tools/release.sh                         # both flavors from one host build
tools/edge-spoof.sh                      # KBM only → out-edgespoof/
# or, reusing an existing host build:
SKIP_BUILD=1 tools/edge-spoof.sh out_mousefix
# prints the deploy command with the Edge PFN package + edge-dev.cer
```

Tiramisu can pick up a mouse attached after launch. Physical pointer input
uses the same guest input bridge as a mouse present at startup.

On a previously tested Series S, the Edge-identity package received physical
mouse input (console log: `keyboard=1 mouse=1`, `PointerDevices count=1`,
`physical relative mouse motion received`).

When Tiramisu receives pointer input, its white arrow with a dark outline
replaces the shell cursor on the dashboard and quick menu. It stays visible
when the mouse is idle. Games with a live Tiramisu presentation overlay can show
the same style of arrow, while honoring the guest's cursor visibility and
clipping. Other rendering paths use the shell cursor. GameInput, MouseDevice
and CoreWindow can all deliver physical motion. Overlapping button reports
generate one guest press or release.

**Games disappear after the identity change** because the PFN determines the
LocalState folder. Migrate the old state (games, profiles, covers, drive_c,
incoming, settings.ini) with
`tools/migrate-localstate.py --from-pfn <old-full-name> --to-pfn <new-full-name>`;
it resumes on rerun. Use the **full** package name including version/arch.

The pack step verifies signature, round-trip unpack, entry point and the Edge
identity before it reports success. Note the cert subject is built with its RDNs
in **reverse** order (`/C=US/ST=Washington/.../CN=Microsoft Corporation`):
OpenSSL drops `S=`, and the MSIX tool reads the name back in reverse, yielding
the exact `CN=..., O=..., L=..., S=..., C=US` string the manifest requires.

Deploy to your own Dev Mode console and launch, then watch the host log for
either `input: physical mouse button received` (allowlist was satisfied) or
`input: GameInput mouse callback failed hr=0x80004001` (still refused). If the
install refuses with `0x80073CF3`, a package with that identity is already
registered; `0x8007000B` means the cert subject did not match the Publisher.

Do not submit this identity to the Microsoft Store, and do not use it outside
your own Dev Mode console. It cannot be published, it can collide with a real
Edge package, and a future console build may reject the signature outright. It
is shipped only as the clearly-labelled KBM experiment artifact.

## What actually helps without spoofing

There is no API that turns hardware mouse on. A Microsoft employee answer on
Q&A states it plainly: "For UWPs, developers are not able to enable hardware
mouse support themselves ... it is not broadly available for UWPs"
(`learn.microsoft.com/en-us/answers/questions/505382`), and Chuck Walbourn
(DirectX team) gives the same answer on Stack Overflow: "Mouse support is only
'enabled' for specific applications. It's not available to general UWP on Xbox
applications." The WebView2 discussion asking for exactly this
(`MicrosoftEdge/WebView2Feedback#4117`) is still unanswered for the same reason.
The allowlist is a publisher opt-in held outside the package.

What the host does instead, and what was fixed for it:

- **Every pointer source the shell is willing to deliver is now consumed.**
  `PointerPressed` / `PointerMoved` / `PointerReleased` no longer bail out
  because GameInput *once* reported a mouse: they defer only while GameInput has
  produced data in the last 300 ms (`mouse_active_recent()`), so a GameInput
  device that goes quiet can no longer silence CoreWindow pointer input for the
  rest of the session.
- **The Win32 fallback runs in-game.** `GetCursorPos` + `GetAsyncKeyState`
  (L/R/M) used to be confined to the launcher; it now feeds the guest during
  play too, and yields for 500 ms after any real CoreWindow pointer event so the
  two absolute sources never fight over a frame.
- **Auto exposes connected devices.** Choose Controller under Default input
  mode to use a keyboard-backed gamepad when there is no physical controller.
  Choose Pad as mouse, press F1, or click the right stick to drive the pointer
  with a controller. F8 enables the keyboard-driven cursor.
The spoof above remains the only path to actual hardware mouse on the console.
It ships as the KBM edition (`tools/edge-spoof.sh` / `tools/release.sh`), but its
premise is still unverified on real hardware — check the host log for `physical
mouse button received` after deploying. Do **not** drop `Tiramisu-EdgeSpoof.msix`
from a release while public builds still cannot enable hardware mouse.
