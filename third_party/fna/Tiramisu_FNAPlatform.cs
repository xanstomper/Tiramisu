#region License
/* Tiramisu - PE64 runtime for Xbox Series X|S Dev Mode
 * Tiramisu_FNAPlatform: FNA platform backend backed by the Tiramisu host
 * surface (CoreWindow) and the host input layer (CoreWindow keyboard /
 * pointer + XInput pads), instead of SDL2/SDL3. Path B (Celeste).
 *
 * This file lives in third_party/fna/ (NOT in the fna-src clone) and is
 * appended to the FNA core build by third_party/fna/build-fna.sh. The
 * FNAPlatform dispatch patch (FNAPlatform-tiramisu.patch) selects it when
 * TIRAMISU_FNA_PLATFORM=1 or FNA_PLATFORM_BACKEND=TIRAMISU.
 *
 * Native access is through Mono internal calls (registered by
 * trt_mono_run via trt_fna_register_internal_calls in
 * src/trt/fna_platform.cpp). The backend CANNOT run outside the Tiramisu
 * host: ProgramInit pings the bridge and fails loudly if the internal
 * calls are not registered. There is no P/Invoke and no SDL here.
 *
 * Headless mode (offline Linux harness): with no host window registered
 * the backend still initializes and answers queries from host state
 * (display size defaults to 1920x1080, no pads connected, no keys down).
 * A graphics device cannot be created headless (FNA3D needs a real
 * surface); the full Celeste boot stays on the SDL2 backend offline.
 * Anything that needs the real console (CoreWindow presentation, real
 * GameInput/XInput state) is console-verification-pending, NOT claimed
 * working here.
 */
#endregion

#region Using Statements
using System;
using System.IO;
using System.Text;
using System.Reflection;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;

using Microsoft.Xna.Framework.Graphics;
using Microsoft.Xna.Framework.Audio;
using Microsoft.Xna.Framework.Input;
using Microsoft.Xna.Framework.Input.Touch;
#endregion

namespace Microsoft.Xna.Framework
{
	internal static class Tiramisu_FNAPlatform
	{
		#region Native Bridge (Mono internal calls)

		/* Must match TRT_FNA_ABI_VERSION in src/trt/fna_platform.h.
		 * ProgramInit refuses to run on a mismatch.
		 */
		private const int ABI_VERSION = 1;

		/* Headless window sentinel: a non-zero handle that is never
		 * dereferenced. On the console CreateWindow returns the real
		 * CoreWindow (ICoreWindow*) instead.
		 */
		private static readonly IntPtr HeadlessWindowHandle = new IntPtr(1);

		/* Windows messages the host posts to the trt message ring
		 * (src/trt/input.cpp). PollEvents drains the ring through
		 * NativePollEvent and translates these; everything else is
		 * dropped (the managed game is the ring's only consumer).
		 */
		private const int WM_ACTIVATE = 0x0006;
		private const int WM_CLOSE = 0x0010;
		private const int WM_KEYDOWN = 0x0100;
		private const int WM_KEYUP = 0x0101;
		private const int WM_CHAR = 0x0102;
		private const int WM_LBUTTONDOWN = 0x0201;
		private const int WM_RBUTTONDOWN = 0x0204;
		private const int WM_MBUTTONDOWN = 0x0207;
		private const int WM_MOUSEWHEEL = 0x020A;
		private const int WM_XBUTTONDOWN = 0x020B;

		/* KEYDOWN lParam bit 30: the key was already down (auto-repeat). */
		private const long KEYEVENTF_PREVIOUS_DOWN = 0x40000000L;

		/* All internal calls use blittable types only (int, long, IntPtr,
		 * out ints) so no Mono marshalling helpers are needed natively.
		 */
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int NativePing();

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern IntPtr NativeGetWindowHandle();

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NativeGetDisplaySize(out int w, out int h);

		/* Dequeues one message from the host message ring. Returns 1 when
		 * an event was dequeued, 0 when the ring is empty.
		 */
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int NativePollEvent(
			out int message,
			out long wparam,
			out long lparam
		);

		/* Mouse position + buttons. buttons bits: 0=L, 1=R, 2=M, 3=X1,
		 * 4=X2 (the trt mbtn layout).
		 */
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NativeGetMouseState(
			out int x,
			out int y,
			out int buttons
		);

		/* Accumulated mouse deltas since the last call (consumed). */
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NativeGetMouseDelta(out int dx, out int dy);

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NativeSetMousePosition(int x, int y);

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NativeSetCursorVisible(int visible);

		/* XInput pad state. Returns 1 when connected, 0 otherwise.
		 * Sticks are raw XInput range (-32768..32767, +Y is up);
		 * triggers are bytes (0..255); buttons is the XInput wButtons
		 * word, whose bits are identical to XNA's Buttons values.
		 */
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int NativeGetPadState(
			int index,
			out int lx,
			out int ly,
			out int rx,
			out int ry,
			out int buttons,
			out int tl,
			out int tr
		);

		/* Returns 1 when the pad is connected (request accepted). */
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int NativeSetPadVibration(
			int index,
			int leftMotor,
			int rightMotor
		);

		/* Pointer to a static NUL-terminated UTF-8 buffer holding the
		 * host storage root (UWP LocalState), or IntPtr.Zero when the
		 * host has not provided one (headless).
		 */
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern IntPtr NativeGetStorageRoot();

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int NativeIsHeadless();

		#endregion

		#region Static State

		/* FNA keyboard state is event-fed (see PollEvents). Win32 virtual
		 * keys are numerically identical to XNA Keys values, so the
		 * translation is a validity check, not a table.
		 */
		private static readonly bool[] ValidVK = BuildValidVKTable();

		private static bool[] BuildValidVKTable()
		{
			bool[] table = new bool[256];
			foreach (Keys k in Enum.GetValues(typeof(Keys)))
			{
				int v = (int) k;
				if (v >= 0 && v < table.Length)
				{
					table[v] = true;
				}
			}
			return table;
		}

		private static bool textInputActive = false;
		private static bool relativeMouseMode = false;
		private static bool warnedWindowed = false;

		/* Previous GamePad states, for PacketNumber change tracking
		 * (same scheme as the SDL backends).
		 */
		private static GamePadState[] INTERNAL_states =
			new GamePadState[GamePad.GAMEPAD_COUNT];

		#endregion

		#region Init/Exit Methods

		public static string ProgramInit(LaunchParameters args)
		{
			/* The native bridge is the Tiramisu host process itself.
			 * If the internal calls are not registered this backend
			 * cannot run: fail loudly instead of limping on nulls.
			 */
			int abi;
			try
			{
				abi = NativePing();
			}
			catch (Exception e)
			{
				Console.Error.WriteLine(
					"TIRAMISU FNA BACKEND FAILURE: native internal " +
					"calls are not registered. This backend only " +
					"runs under the Tiramisu host (trt_mono_run); " +
					"use the SDL2 backend outside it."
				);
				throw new InvalidOperationException(
					"Tiramisu_FNAPlatform requires the Tiramisu " +
					"host: internal calls are not registered.",
					e
				);
			}
			if (abi != ABI_VERSION)
			{
				throw new InvalidOperationException(
					"Tiramisu_FNAPlatform: native ABI mismatch " +
					"(host=" + abi + ", expected=" + ABI_VERSION + ")"
				);
			}

			if (NativeIsHeadless() != 0)
			{
				FNALoggerEXT.LogInfo(
					"Tiramisu_FNAPlatform: headless mode (no host " +
					"window); windowing/input calls report host " +
					"state only."
				);
			}
			else
			{
				FNALoggerEXT.LogInfo(
					"Tiramisu_FNAPlatform: host window attached."
				);
			}

			/* The host sets the AppDomain base directory to the game's
			 * directory (mono_host.cpp), so this is the title location.
			 */
			return AppDomain.CurrentDomain.BaseDirectory;
		}

		public static void ProgramExit(object sender, EventArgs e)
		{
			/* Nothing to shut down: the host owns the window, the
			 * input layer, and the Mono runtime.
			 */
		}

		#endregion

		#region Memory

		public static IntPtr Malloc(int size)
		{
			return Marshal.AllocHGlobal(size);
		}

		public static void Free(IntPtr ptr)
		{
			Marshal.FreeHGlobal(ptr);
		}

		#endregion

		#region Environment

		/* FNA3D reads its FNA3D_* settings from the process environment
		 * (SDL_GetHint falls back to getenv), so a plain env write is
		 * the genuine equivalent of SDL's hint store here.
		 */
		public static void SetEnv(string name, string value)
		{
			Environment.SetEnvironmentVariable(name, value);
		}

		#endregion

		#region Window Methods

		public static GameWindow CreateWindow()
		{
			IntPtr hostHandle = NativeGetWindowHandle();
			IntPtr handle = (hostHandle != IntPtr.Zero) ?
				hostHandle :
				HeadlessWindowHandle;

			/* The window IS the display: mouse coordinates are display
			 * pixels, so the window size for Mouse.GetState scaling is
			 * the display size.
			 */
			int w, h;
			NativeGetDisplaySize(out w, out h);
			Mouse.INTERNAL_WindowWidth = w;
			Mouse.INTERNAL_WindowHeight = h;

			/* We hide the mouse cursor by default. */
			OnIsMouseVisibleChanged(false);

			string title =
				MonoGame.Utilities.AssemblyHelper.GetDefaultWindowTitle();
			return new FNAWindow(
				handle,
				@"\\.\\DISPLAY1",
				title
			);
		}

		public static void DisposeWindow(GameWindow window)
		{
			if (Mouse.WindowHandle == window.Handle)
			{
				Mouse.WindowHandle = IntPtr.Zero;
			}

			if (TouchPanel.WindowHandle == window.Handle)
			{
				TouchPanel.WindowHandle = IntPtr.Zero;
			}

			if (TextInputEXT.WindowHandle == window.Handle)
			{
				TextInputEXT.WindowHandle = IntPtr.Zero;
			}

			/* The host owns the CoreWindow; there is nothing to destroy.
			 * Headless handles are sentinels, not allocations.
			 */
		}

		public static void ApplyWindowChanges(
			IntPtr window,
			int clientWidth,
			int clientHeight,
			bool wantsFullscreen,
			string screenDeviceName,
			ref string resultDeviceName
		) {
			/* The host surface is the fullscreen CoreWindow: it cannot
			 * be resized, moved, or windowed. The backbuffer size is
			 * the graphics device's business (it scales to the
			 * display); the window stays put.
			 */
			resultDeviceName = @"\\.\\DISPLAY1";
			if (!wantsFullscreen && !warnedWindowed)
			{
				warnedWindowed = true;
				FNALoggerEXT.LogWarn(
					"Tiramisu_FNAPlatform: windowed mode is not " +
					"supported on the host surface; staying fullscreen."
				);
			}
		}

		public static void ScaleForWindow(IntPtr window, bool invert, ref int w, ref int h)
		{
			/* Scale factor is 1: no HiDPI on the host surface. */
		}

		public static Rectangle GetWindowBounds(IntPtr window)
		{
			int w, h;
			NativeGetDisplaySize(out w, out h);
			return new Rectangle(0, 0, w, h);
		}

		public static bool GetWindowResizable(IntPtr window)
		{
			return false;
		}

		public static void SetWindowResizable(IntPtr window, bool resizable)
		{
			/* The host surface is fixed fullscreen; nothing to resize. */
		}

		public static bool GetWindowBorderless(IntPtr window)
		{
			/* A CoreWindow has no border or title bar. */
			return true;
		}

		public static void SetWindowBorderless(IntPtr window, bool borderless)
		{
			/* Always borderless; nothing to change. */
		}

		public static void SetWindowTitle(IntPtr window, string title)
		{
			/* UWP apps have no settable window title (the app title
			 * comes from the package manifest). Genuine no-op.
			 */
		}

		public static bool IsScreenKeyboardShown(IntPtr window)
		{
			/* No on-screen keyboard is surfaced by this backend. */
			return false;
		}

		public static IntPtr WrapWindow(IntPtr handle)
		{
			return handle;
		}

		public static IntPtr UnwrapWindow(IntPtr handle)
		{
			return handle;
		}

		#endregion

		#region Event Loop

		public static GraphicsAdapter RegisterGame(Game game)
		{
			/* The CoreWindow is already visible; nothing to show.
			 * Single display, single adapter.
			 */
			return GraphicsAdapter.Adapters[0];
		}

		public static void UnregisterGame(Game game)
		{
		}

		public static void PollEvents(
			Game game,
			ref GraphicsAdapter currentAdapter,
			bool[] textInputControlDown,
			ref bool textInputSuppress
		) {
			/* Drain the host message ring. The ring is also the native
			 * guests' message pump, but a managed game is its only
			 * consumer, so dropping unhandled messages is safe.
			 * The iteration cap bounds a pathological flood; a normal
			 * frame drains a handful of events.
			 */
			for (int i = 0; i < 1024; i += 1)
			{
				int message;
				long wparam;
				long lparam;
				if (NativePollEvent(out message, out wparam, out lparam) == 0)
				{
					break;
				}

				/* Window activation (posted by the host's CoreWindow
				 * Activated handler while a game runs).
				 */
				if (message == WM_ACTIVATE)
				{
					if (game != null)
					{
						game.IsActive = (wparam & 0xFFFF) != 0;
					}
				}
				else if (message == WM_CLOSE)
				{
					if (game != null)
					{
						game.Exit();
					}
					return;
				}
				else if (message == WM_KEYDOWN)
				{
					Keys key = ToXNAKey((int) wparam);
					if (key == Keys.None)
					{
						continue;
					}
					bool repeat = (lparam & KEYEVENTF_PREVIOUS_DOWN) != 0;
					if (Keyboard.keys.IsKeyUp(key))
					{
						Keyboard.keys.AddPressedKey((int) key);
						int textIndex;
						if (FNAPlatform.TextInputBindings.TryGetValue(key, out textIndex))
						{
							textInputControlDown[textIndex] = true;
							TextInputEXT.OnTextInput(FNAPlatform.TextInputCharacters[textIndex]);
						}
						else if ((Keyboard.keys.IsKeyDown(Keys.LeftControl) || Keyboard.keys.IsKeyDown(Keys.RightControl))
							&& Keyboard.keys.IsKeyUp(Keys.LeftAlt)
							&& key == Keys.V)
						{
							textInputControlDown[6] = true;
							TextInputEXT.OnTextInput(FNAPlatform.TextInputCharacters[6]);
							textInputSuppress = true;
						}
					}
					else if (repeat)
					{
						int textIndex;
						if (FNAPlatform.TextInputBindings.TryGetValue(key, out textIndex))
						{
							TextInputEXT.OnTextInput(FNAPlatform.TextInputCharacters[textIndex]);
						}
						else if ((Keyboard.keys.IsKeyDown(Keys.LeftControl) || Keyboard.keys.IsKeyDown(Keys.RightControl))
							&& key == Keys.V)
						{
							TextInputEXT.OnTextInput(FNAPlatform.TextInputCharacters[6]);
						}
					}
				}
				else if (message == WM_KEYUP)
				{
					Keys key = ToXNAKey((int) wparam);
					if (key == Keys.None)
					{
						continue;
					}
					if (Keyboard.keys.IsKeyDown(key))
					{
						Keyboard.keys.RemovePressedKey((int) key);
						int value;
						if (FNAPlatform.TextInputBindings.TryGetValue(key, out value))
						{
							textInputControlDown[value] = false;
						}
						else if (((Keyboard.keys.IsKeyUp(Keys.LeftControl) && Keyboard.keys.IsKeyUp(Keys.RightControl)) && textInputControlDown[6])
							|| key == Keys.V)
						{
							textInputControlDown[6] = false;
							textInputSuppress = false;
						}
					}
				}
				else if (message == WM_CHAR)
				{
					/* Printable characters only, and only while the
					 * game has text input started (SDL_TEXTINPUT
					 * semantics). Control characters are covered by
					 * the KEYDOWN path above; forwarding them here
					 * would double-fire.
					 */
					if (!textInputActive || textInputSuppress)
					{
						continue;
					}
					char c = (char) wparam;
					bool isControl = false;
					foreach (char control in FNAPlatform.TextInputCharacters)
					{
						if (c == control)
						{
							isControl = true;
							break;
						}
					}
					if (!isControl)
					{
						TextInputEXT.OnTextInput(c);
					}
				}
				else if (message == WM_LBUTTONDOWN)
				{
					Mouse.INTERNAL_onClicked(0);
				}
				else if (message == WM_MBUTTONDOWN)
				{
					Mouse.INTERNAL_onClicked(1);
				}
				else if (message == WM_RBUTTONDOWN)
				{
					Mouse.INTERNAL_onClicked(2);
				}
				else if (message == WM_XBUTTONDOWN)
				{
					/* wParam high word: 1 = XBUTTON1, 2 = XBUTTON2. */
					Mouse.INTERNAL_onClicked(((wparam >> 16) & 0xFFFF) == 1 ? 3 : 4);
				}
				else if (message == WM_MOUSEWHEEL)
				{
					/* wParam high word is the wheel delta in WHEEL_DELTA
					 * (120) units, matching SDL's preciseY * 120.
					 */
					Mouse.INTERNAL_MouseWheel += (short) (wparam >> 16);
				}
				/* WM_MOUSEMOVE / WM_INPUT / pointer messages and the
				 * rest: ignored. Mouse position is polled directly
				 * (GetMouseState); raw input has no FNA consumer.
				 */
			}
		}

		/* Win32 virtual keys are numerically identical to XNA Keys
		 * values (Back=8, Escape=27, Space=32, A=65, F1=112,
		 * LeftShift=160, ...), so the translation is a validity check.
		 */
		private static Keys ToXNAKey(int vk)
		{
			if (vk <= 0 || vk >= ValidVK.Length || !ValidVK[vk])
			{
				return Keys.None;
			}
			return (Keys) vk;
		}

		public static bool NeedsPlatformMainLoop()
		{
			return false;
		}

		public static void RunPlatformMainLoop(Game game)
		{
			/* Never called: NeedsPlatformMainLoop returns false. */
		}

		#endregion

		#region Display Methods

		public static GraphicsAdapter[] GetGraphicsAdapters()
		{
			int w, h;
			NativeGetDisplaySize(out w, out h);
			List<DisplayMode> modes = new List<DisplayMode>();
			modes.Add(
				new DisplayMode(
					w,
					h,
					SurfaceFormat.Color
				)
			);
			GraphicsAdapter[] adapters = new GraphicsAdapter[1];
			adapters[0] = new GraphicsAdapter(
				new DisplayModeCollection(modes),
				@"\\.\\DISPLAY1",
				"Tiramisu Display"
			);
			return adapters;
		}

		public static DisplayMode GetCurrentDisplayMode(int adapterIndex)
		{
			int w, h;
			NativeGetDisplaySize(out w, out h);
			return new DisplayMode(
				w,
				h,
				SurfaceFormat.Color
			);
		}

		public static IntPtr GetMonitorHandle(int adapterIndex)
		{
			return new IntPtr(adapterIndex);
		}

		#endregion

		#region Keyboard Methods

		/* PS/2 set-1 scan code -> XNA key. The main key path uses
		 * virtual keys (unambiguous); this EXT helper is for callers
		 * that only have a scan code. E0-prefixed keys (arrows, Insert,
		 * Delete, Home/End, PgUp/PgDn, right Ctrl/Alt, Windows keys)
		 * share scan codes with numpad keys and cannot be told apart
		 * from the code alone, so they map to their numpad twins here
		 * (documented limitation; Celeste does not use this path).
		 */
		public static Keys GetKeyFromScancode(Keys scancode)
		{
			switch ((int) scancode)
			{
				case 0x01: return Keys.Escape;
				case 0x02: return Keys.D1;
				case 0x03: return Keys.D2;
				case 0x04: return Keys.D3;
				case 0x05: return Keys.D4;
				case 0x06: return Keys.D5;
				case 0x07: return Keys.D6;
				case 0x08: return Keys.D7;
				case 0x09: return Keys.D8;
				case 0x0A: return Keys.D9;
				case 0x0B: return Keys.D0;
				case 0x0C: return Keys.OemMinus;
				case 0x0D: return Keys.OemPlus;
				case 0x0E: return Keys.Back;
				case 0x0F: return Keys.Tab;
				case 0x10: return Keys.Q;
				case 0x11: return Keys.W;
				case 0x12: return Keys.E;
				case 0x13: return Keys.R;
				case 0x14: return Keys.T;
				case 0x15: return Keys.Y;
				case 0x16: return Keys.U;
				case 0x17: return Keys.I;
				case 0x18: return Keys.O;
				case 0x19: return Keys.P;
				case 0x1A: return Keys.OemOpenBrackets;
				case 0x1B: return Keys.OemCloseBrackets;
				case 0x1C: return Keys.Enter;
				case 0x1D: return Keys.LeftControl;
				case 0x1E: return Keys.A;
				case 0x1F: return Keys.S;
				case 0x20: return Keys.D;
				case 0x21: return Keys.F;
				case 0x22: return Keys.G;
				case 0x23: return Keys.H;
				case 0x24: return Keys.J;
				case 0x25: return Keys.K;
				case 0x26: return Keys.L;
				case 0x27: return Keys.OemSemicolon;
				case 0x28: return Keys.OemQuotes;
				case 0x29: return Keys.OemTilde;
				case 0x2A: return Keys.LeftShift;
				case 0x2B: return Keys.OemPipe;
				case 0x2C: return Keys.Z;
				case 0x2D: return Keys.X;
				case 0x2E: return Keys.C;
				case 0x2F: return Keys.V;
				case 0x30: return Keys.B;
				case 0x31: return Keys.N;
				case 0x32: return Keys.M;
				case 0x33: return Keys.OemComma;
				case 0x34: return Keys.OemPeriod;
				case 0x35: return Keys.OemQuestion;
				case 0x36: return Keys.RightShift;
				case 0x37: return Keys.Multiply;
				case 0x38: return Keys.LeftAlt;
				case 0x39: return Keys.Space;
				case 0x3A: return Keys.CapsLock;
				case 0x3B: return Keys.F1;
				case 0x3C: return Keys.F2;
				case 0x3D: return Keys.F3;
				case 0x3E: return Keys.F4;
				case 0x3F: return Keys.F5;
				case 0x40: return Keys.F6;
				case 0x41: return Keys.F7;
				case 0x42: return Keys.F8;
				case 0x43: return Keys.F9;
				case 0x44: return Keys.F10;
				case 0x45: return Keys.NumLock;
				case 0x46: return Keys.Scroll;
				case 0x47: return Keys.NumPad7;
				case 0x48: return Keys.NumPad8;
				case 0x49: return Keys.NumPad9;
				case 0x4A: return Keys.Subtract;
				case 0x4B: return Keys.NumPad4;
				case 0x4C: return Keys.NumPad5;
				case 0x4D: return Keys.NumPad6;
				case 0x4E: return Keys.Add;
				case 0x4F: return Keys.NumPad1;
				case 0x50: return Keys.NumPad2;
				case 0x51: return Keys.NumPad3;
				case 0x52: return Keys.NumPad0;
				case 0x53: return Keys.Decimal;
				case 0x56: return Keys.OemBackslash;
				case 0x57: return Keys.F11;
				case 0x58: return Keys.F12;
				default: return Keys.None;
			}
		}

		#endregion

		#region Text Input Methods

		/* Text input state is tracked in managed code; there is no
		 * on-screen keyboard to show on the console, but physical
		 * keyboard WM_CHAR events still flow through PollEvents while
		 * text input is started. Named gap: no OSK.
		 */
		public static bool IsTextInputActive(IntPtr window)
		{
			return textInputActive;
		}

		public static void StartTextInput(IntPtr window)
		{
			textInputActive = true;
		}

		public static void StopTextInput(IntPtr window)
		{
			textInputActive = false;
		}

		public static void SetTextInputRectangle(IntPtr window, Rectangle rectangle)
		{
			/* No on-screen keyboard to position. */
		}

		#endregion

		#region Mouse Methods

		public static void GetMouseState(
			IntPtr window,
			out int x,
			out int y,
			out ButtonState left,
			out ButtonState middle,
			out ButtonState right,
			out ButtonState x1,
			out ButtonState x2
		) {
			int buttons;
			if (relativeMouseMode)
			{
				/* Relative mode reports motion, not position (same as
				 * SDL_GetRelativeMouseState). The accumulator covers
				 * both real-mouse deltas and pointer moves.
				 */
				NativeGetMouseDelta(out x, out y);
				int px, py;
				NativeGetMouseState(out px, out py, out buttons);
			}
			else
			{
				NativeGetMouseState(out x, out y, out buttons);
			}
			left =		((buttons & 1) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
			right =		((buttons & 2) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
			middle =	((buttons & 4) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
			x1 =		((buttons & 8) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
			x2 =		((buttons & 16) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
		}

		public static void SetMousePosition(IntPtr window, int x, int y)
		{
			NativeSetMousePosition(x, y);
		}

		public static void OnIsMouseVisibleChanged(bool visible)
		{
			NativeSetCursorVisible(visible ? 1 : 0);
		}

		public static bool GetRelativeMouseMode(IntPtr window)
		{
			return relativeMouseMode;
		}

		public static void SetRelativeMouseMode(IntPtr window, bool enable)
		{
			if (enable && !relativeMouseMode)
			{
				/* Drain stale motion so enabling does not jump. */
				int dx, dy;
				NativeGetMouseDelta(out dx, out dy);
			}
			relativeMouseMode = enable;
		}

		#endregion

		#region GamePad Methods

		public static GamePadCapabilities GetGamePadCapabilities(int index)
		{
			int lx, ly, rx, ry, buttons, tl, tr;
			if (NativeGetPadState(index, out lx, out ly, out rx, out ry, out buttons, out tl, out tr) == 0)
			{
				return new GamePadCapabilities();
			}
			/* XInput pads are complete standard controllers: every
			 * face/shoulder/stick button, both sticks, both triggers,
			 * both rumble motors. No Guide button (XInput does not
			 * report it), no voice, no light bar, no gyro/accel, and
			 * no trigger rumble (that needs GameInput impulse triggers;
			 * the host feed is XInput).
			 */
			GamePadCapabilities caps = new GamePadCapabilities();
			caps.IsConnected = true;
			caps.GamePadType = GamePadType.GamePad;
			caps.HasAButton = true;
			caps.HasBButton = true;
			caps.HasXButton = true;
			caps.HasYButton = true;
			caps.HasBackButton = true;
			caps.HasStartButton = true;
			caps.HasDPadUpButton = true;
			caps.HasDPadDownButton = true;
			caps.HasDPadLeftButton = true;
			caps.HasDPadRightButton = true;
			caps.HasLeftShoulderButton = true;
			caps.HasRightShoulderButton = true;
			caps.HasLeftStickButton = true;
			caps.HasRightStickButton = true;
			caps.HasBigButton = false;
			caps.HasLeftXThumbStick = true;
			caps.HasLeftYThumbStick = true;
			caps.HasRightXThumbStick = true;
			caps.HasRightYThumbStick = true;
			caps.HasLeftTrigger = true;
			caps.HasRightTrigger = true;
			caps.HasLeftVibrationMotor = true;
			caps.HasRightVibrationMotor = true;
			caps.HasVoiceSupport = false;
			return caps;
		}

		public static GamePadState GetGamePadState(int index, GamePadDeadZone deadZoneMode)
		{
			int lx, ly, rx, ry, buttons, tl, tr;
			if (NativeGetPadState(index, out lx, out ly, out rx, out ry, out buttons, out tl, out tr) == 0)
			{
				return new GamePadState();
			}

			/* XInput sticks are +Y-up, matching XNA (SDL's axes are
			 * +Y-down, which is why the SDL backends negate Y).
			 */
			Vector2 stickLeft = new Vector2(
				(float) lx / 32767.0f,
				(float) ly / 32767.0f
			);
			Vector2 stickRight = new Vector2(
				(float) rx / 32767.0f,
				(float) ry / 32767.0f
			);

			float triggerLeft = (float) tl / 255.0f;
			float triggerRight = (float) tr / 255.0f;

			/* XInput wButtons bits are numerically identical to XNA's
			 * Buttons values (DPad 0x1-0x8, Start 0x10, Back 0x20,
			 * sticks 0x40/0x80, shoulders 0x100/0x200, A/B/X/Y
			 * 0x1000-0x8000), so the word casts directly.
			 */
			Buttons gc_buttonState = (Buttons) buttons;

			ButtonState dpadUp =	((buttons & 0x0001) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
			ButtonState dpadDown =	((buttons & 0x0002) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
			ButtonState dpadLeft =	((buttons & 0x0004) != 0) ?
				ButtonState.Pressed : ButtonState.Released;
			ButtonState dpadRight =	((buttons & 0x0008) != 0) ?
				ButtonState.Pressed : ButtonState.Released;

			/* Build the GamePadState, increment PacketNumber if state changed. */
			GamePadState gc_builtState = new GamePadState(
				new GamePadThumbSticks(stickLeft, stickRight, deadZoneMode),
				new GamePadTriggers(triggerLeft, triggerRight, deadZoneMode),
				new GamePadButtons(gc_buttonState),
				new GamePadDPad(dpadUp, dpadDown, dpadLeft, dpadRight)
			);
			gc_builtState.IsConnected = true;
			gc_builtState.PacketNumber = INTERNAL_states[index].PacketNumber;
			if (gc_builtState != INTERNAL_states[index])
			{
				gc_builtState.PacketNumber += 1;
				INTERNAL_states[index] = gc_builtState;
			}

			return gc_builtState;
		}

		public static bool SetGamePadVibration(int index, float leftMotor, float rightMotor)
		{
			/* The host consumes the request on its pad-poll tick and
			 * calls XInputSetState (src/host/main.cpp). The dashboard
			 * pad-vibration kill-switch applies downstream.
			 */
			return NativeSetPadVibration(
				index,
				(int) (MathHelper.Clamp(leftMotor, 0.0f, 1.0f) * 0xFFFF),
				(int) (MathHelper.Clamp(rightMotor, 0.0f, 1.0f) * 0xFFFF)
			) != 0;
		}

		public static bool SetGamePadTriggerVibration(int index, float leftTrigger, float rightTrigger)
		{
			/* XInput has no trigger rumble. The host feed is XInput;
			 * impulse triggers would need the GameInput path.
			 * Named gap, not a silent success: returns false.
			 */
			return false;
		}

		public static string GetGamePadGUID(int index)
		{
			/* XInput exposes no per-device GUID; the slot is the
			 * stable identifier. Empty when disconnected.
			 */
			int lx, ly, rx, ry, buttons, tl, tr;
			if (NativeGetPadState(index, out lx, out ly, out rx, out ry, out buttons, out tl, out tr) == 0)
			{
				return String.Empty;
			}
			return "XInput-Pad-" + index;
		}

		public static void SetGamePadLightBar(int index, Color color)
		{
			/* Xbox controllers have no light bar. Genuine no-op. */
		}

		public static bool GetGamePadGyro(int index, out Vector3 gyro)
		{
			/* No gyro on XInput controllers. */
			gyro = Vector3.Zero;
			return false;
		}

		public static bool GetGamePadAccelerometer(int index, out Vector3 accel)
		{
			/* No accelerometer on XInput controllers. */
			accel = Vector3.Zero;
			return false;
		}

		#endregion

		#region Storage Methods

		public static string GetStorageRoot()
		{
			/* The per-game folder name comes from the entry
			 * assembly, NOT the AppDomain friendly name: the host
			 * names the domain ("tiramisu-mono"), so FriendlyName
			 * is the same for every game. mono_jit_exec sets the
			 * entry assembly (that is why the host uses it), so
			 * this is "Celeste" for Celeste and
			 * "probe_fna_platform" for the probe.
			 */
			string exeName = null;
			Assembly entry = Assembly.GetEntryAssembly();
			if (entry != null)
			{
				exeName = entry.GetName().Name;
			}
			if (String.IsNullOrEmpty(exeName))
			{
				exeName = Path.GetFileNameWithoutExtension(
					AppDomain.CurrentDomain.FriendlyName
				).Replace(".vshost", "");
			}

			/* Console: the host's LocalState folder, handed to the
			 * native layer at boot (src/host/main.cpp boot_all).
			 */
			IntPtr rootPtr = NativeGetStorageRoot();
			if (rootPtr != IntPtr.Zero)
			{
				string hostRoot = ReadUTF8Z(rootPtr);
				if (!String.IsNullOrEmpty(hostRoot))
				{
					return Path.Combine(hostRoot, exeName);
				}
			}

			/* Headless / host did not provide a root: XDG data home,
			 * like the SDL backend on Linux.
			 */
			string osConfigDir = Environment.GetEnvironmentVariable("XDG_DATA_HOME");
			if (String.IsNullOrEmpty(osConfigDir))
			{
				osConfigDir = Environment.GetEnvironmentVariable("HOME");
				if (String.IsNullOrEmpty(osConfigDir))
				{
					return ".";
				}
				osConfigDir += "/.local/share";
			}
			return Path.Combine(osConfigDir, exeName);
		}

		private static string ReadUTF8Z(IntPtr ptr)
		{
			List<byte> bytes = new List<byte>();
			for (int i = 0; i < 4096; i += 1)
			{
				byte b = Marshal.ReadByte(ptr, i);
				if (b == 0)
				{
					break;
				}
				bytes.Add(b);
			}
			return Encoding.UTF8.GetString(bytes.ToArray());
		}

		public static DriveInfo GetDriveInfo(string storageRoot)
		{
			DriveInfo result;
			try
			{
				result = new DriveInfo(Path.GetPathRoot(storageRoot));
			}
			catch (Exception e)
			{
				FNALoggerEXT.LogError("Failed to get DriveInfo: " + e.ToString());
				result = null;
			}
			return result;
		}

		public static IntPtr ReadFileToPointer(string path, out IntPtr size)
		{
			size = IntPtr.Zero;
			try
			{
				byte[] data = File.ReadAllBytes(path);
				IntPtr ptr = Marshal.AllocHGlobal(data.Length);
				Marshal.Copy(data, 0, ptr, data.Length);
				size = new IntPtr(data.Length);
				return ptr;
			}
			catch
			{
				/* Missing/unreadable file: NULL, like SDL_LoadFile. */
				return IntPtr.Zero;
			}
		}

		public static void FreeFilePointer(IntPtr file)
		{
			Marshal.FreeHGlobal(file);
		}

		#endregion

		#region Logging/Messaging Methods

		public static void ShowRuntimeError(GameWindow gameWindow, string message)
		{
			/* No message box on the console: the host log + stderr
			 * are the error surface (visible in the web UI).
			 */
			string title = (gameWindow != null) ? gameWindow.Title : "Tiramisu";
			string line = "TIRAMISU RUNTIME ERROR [" + title + "]: " + message;
			Console.Error.WriteLine(line);
			if (FNALoggerEXT.LogError != null)
			{
				FNALoggerEXT.LogError(line);
			}
		}

		#endregion

		#region Microphone Implementation

		/* No microphone API is surfaced by this backend. An empty
		 * device list is the genuine answer (same as an SDL host with
		 * no capture devices), not a stub.
		 */
		public static Microphone[] GetMicrophones()
		{
			return new Microphone[0];
		}

		public static int GetMicrophoneSamples(
			IntPtr handle,
			byte[] buffer,
			int offset,
			int count
		) {
			return 0;
		}

		public static int GetMicrophoneQueuedBytes(IntPtr handle)
		{
			return 0;
		}

		public static void StartMicrophone(IntPtr handle)
		{
		}

		public static void StopMicrophone(IntPtr handle)
		{
		}

		#endregion

		#region Touch Methods

		/* No touch screen on the console. Genuine "not present". */
		public static TouchPanelCapabilities GetTouchCapabilities()
		{
			return new TouchPanelCapabilities();
		}

		public static void UpdateTouchPanelState()
		{
		}

		public static int GetNumTouchFingers()
		{
			return 0;
		}

		#endregion

		#region Orientation Methods

		public static bool SupportsOrientationChanges()
		{
			return false;
		}

		#endregion
	}
}
