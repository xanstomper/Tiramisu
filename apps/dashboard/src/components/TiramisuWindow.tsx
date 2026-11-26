import React, { useState } from 'react';
import { X, Maximize2, Minimize2, Terminal } from 'lucide-react';

interface TiramisuWindowProps {
  title: string;
  subtitle?: string;
  systemPath?: string;
  onClose: () => void;
  maxWidthClass?: string;
  footerHints?: { keyLabel: string; action: string }[];
  headerRight?: React.ReactNode;
  children: React.ReactNode;
}

export const TiramisuWindow: React.FC<TiramisuWindowProps> = ({
  title,
  subtitle,
  systemPath,
  onClose,
  maxWidthClass = 'max-w-4xl',
  footerHints = [
    { keyLabel: 'A', action: 'Confirm' },
    { keyLabel: 'B', action: 'Close Window' },
  ],
  headerRight,
  children,
}) => {
  const [isMaximized, setIsMaximized] = useState(false);

  return (
    <div className="fixed inset-0 z-50 bg-black/75 backdrop-blur-xl flex items-center justify-center p-4 sm:p-6">
      <div
        role="dialog"
        aria-modal="true"
        aria-label={title}
        className={`tiramisu-glass border border-white/15 flex flex-col overflow-hidden transition-all duration-200 shadow-[0_32px_90px_rgba(0,0,0,0.92)] ${
          isMaximized
            ? 'w-full h-full rounded-[20px]'
            : `w-full ${maxWidthClass} max-h-[86vh] rounded-[26px]`
        }`}
      >
        {/* Top Window Chrome Bar */}
        <div className="flex items-center justify-between gap-4 px-6 py-4 bg-[#121116]/90 border-b border-white/10 shrink-0">
          <div className="min-w-0 flex items-center gap-3.5">
            <div className="w-9 h-9 rounded-xl bg-white/[0.06] border border-white/10 flex items-center justify-center text-[#cbbab5] shrink-0">
              <Terminal className="w-4 h-4" />
            </div>
            <div className="min-w-0">
              <div className="flex items-center gap-2.5">
                <h2 className="text-[17px] font-bold tracking-tight text-white truncate">
                  {title}
                </h2>
                {systemPath && (
                  <span className="hidden sm:inline-block font-mono text-[11px] text-[#b5aeb0] bg-white/[0.05] px-2.5 py-0.5 rounded-md border border-white/10 truncate max-w-xs">
                    {systemPath}
                  </span>
                )}
              </div>
              {subtitle && (
                <p className="text-xs text-[#9e9699] truncate mt-0.5">{subtitle}</p>
              )}
            </div>
          </div>

          <div className="flex items-center gap-2 shrink-0">
            {headerRight}
            <button
              type="button"
              onClick={() => setIsMaximized((prev) => !prev)}
              aria-label={isMaximized ? 'Restore window size' : 'Maximize window'}
              className="w-9 h-9 rounded-full bg-white/[0.06] hover:bg-white/15 border border-white/10 flex items-center justify-center text-white/80 hover:text-white transition-colors cursor-pointer"
            >
              {isMaximized ? (
                <Minimize2 className="w-4 h-4" />
              ) : (
                <Maximize2 className="w-4 h-4" />
              )}
            </button>
            <button
              type="button"
              onClick={onClose}
              aria-label={`Close ${title} window`}
              className="w-9 h-9 rounded-full bg-white/[0.06] hover:bg-[#b92b27] border border-white/10 flex items-center justify-center text-white/90 hover:text-white transition-colors cursor-pointer"
            >
              <X className="w-4 h-4" />
            </button>
          </div>
        </div>

        {/* Window Body Content */}
        <div className="flex-1 overflow-y-auto p-6 bg-[#151319]/60">{children}</div>

        {/* Bottom Console Controller Bar */}
        <div className="px-6 py-3 bg-[#100f14]/95 border-t border-white/10 flex items-center justify-between gap-4 text-xs text-[#b5aeb0] shrink-0">
          <div className="flex items-center gap-2 font-mono text-[11px] text-[#9e9699]">
            <span>XBOX DEV MODE UWP SHELL</span>
            <span aria-hidden="true">·</span>
            <span>DIRECTX 12 SWAPCHAIN READY</span>
          </div>

          <div className="flex items-center gap-4">
            {footerHints.map((hint) => (
              <div key={hint.keyLabel} className="flex items-center gap-1.5">
                <span className="inline-flex items-center justify-center min-w-[22px] h-[22px] px-1.5 rounded-full bg-white/15 text-white font-bold text-[11px] border border-white/20">
                  {hint.keyLabel}
                </span>
                <span className="text-[#d5cecc] font-medium">{hint.action}</span>
              </div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
};
