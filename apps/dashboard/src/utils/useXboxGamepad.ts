import { useState, useEffect, useRef } from 'react';

export interface GamepadTelemetry {
  connected: boolean;
  id: string;
  leftStickX: number;
  leftStickY: number;
  rightStickX: number;
  rightStickY: number;
  leftTrigger: number;
  rightTrigger: number;
  pressedButtons: string[];
}

const BUTTON_NAMES: Record<number, string> = {
  0: 'A',
  1: 'B',
  2: 'X',
  3: 'Y',
  4: 'LB',
  5: 'RB',
  6: 'LT',
  7: 'RT',
  8: 'View',
  9: 'Menu',
  10: 'LS',
  11: 'RS',
  12: 'DPadUp',
  13: 'DPadDown',
  14: 'DPadLeft',
  15: 'DPadRight',
  16: 'XboxGuide',
};

interface UseXboxGamepadCallbacks {
  onButtonPress?: (buttonName: string) => void;
}

export function useXboxGamepad(callbacks?: UseXboxGamepadCallbacks): GamepadTelemetry {
  const [telemetry, setTelemetry] = useState<GamepadTelemetry>({
    connected: false,
    id: 'Xbox Wireless Controller (Standard XInput)',
    leftStickX: 0,
    leftStickY: 0,
    rightStickX: 0,
    rightStickY: 0,
    leftTrigger: 0,
    rightTrigger: 0,
    pressedButtons: [],
  });

  const callbacksRef = useRef(callbacks);
  callbacksRef.current = callbacks;
  const prevPressedRef = useRef<Set<number>>(new Set());

  useEffect(() => {
    let rafId: number;

    const pollGamepad = () => {
      const pads = navigator.getGamepads ? navigator.getGamepads() : [];
      const activePad = pads[0] || pads[1] || pads[2] || pads[3];

      if (activePad) {
        const currentPressed = new Set<number>();
        const pressedLabels: string[] = [];

        activePad.buttons.forEach((btn, idx) => {
          if (btn.pressed) {
            currentPressed.add(idx);
            const label = BUTTON_NAMES[idx] || `Btn${idx}`;
            pressedLabels.push(label);

            if (!prevPressedRef.current.has(idx)) {
              callbacksRef.current?.onButtonPress?.(label);
            }
          }
        });

        prevPressedRef.current = currentPressed;

        setTelemetry({
          connected: true,
          id: activePad.id || 'Xbox Wireless Controller',
          leftStickX: Number((activePad.axes[0] || 0).toFixed(2)),
          leftStickY: Number((activePad.axes[1] || 0).toFixed(2)),
          rightStickX: Number((activePad.axes[2] || 0).toFixed(2)),
          rightStickY: Number((activePad.axes[3] || 0).toFixed(2)),
          leftTrigger: Number((activePad.buttons[6]?.value || 0).toFixed(2)),
          rightTrigger: Number((activePad.buttons[7]?.value || 0).toFixed(2)),
          pressedButtons: pressedLabels,
        });
      }

      rafId = requestAnimationFrame(pollGamepad);
    };

    rafId = requestAnimationFrame(pollGamepad);
    return () => cancelAnimationFrame(rafId);
  }, []);

  return telemetry;
}
