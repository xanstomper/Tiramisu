import React from 'react';
import { Home, Layers, Puzzle, Wrench, Settings, Wifi } from 'lucide-react';
import { NavTabId } from '../utils/dashboardLogic';

interface TopNavigationProps {
  activeTab: NavTabId;
  onSelectTab: (tab: NavTabId) => void;
  clockTime: string;
  clockDate: string;
  onOpenNetworkModal: () => void;
  onOpenArchModal: () => void;
}

const NAV_ITEMS: { id: NavTabId; label: string; Icon: React.FC<{ className?: string }> }[] = [
  { id: 'home', label: 'Home', Icon: Home },
  { id: 'library', label: 'Library', Icon: Layers },
  { id: 'mods', label: 'Mods', Icon: Puzzle },
  { id: 'tools', label: 'Tools', Icon: Wrench },
  { id: 'settings', label: 'Settings', Icon: Settings },
];

export const TopNavigation: React.FC<TopNavigationProps> = ({
  activeTab,
  onSelectTab,
  clockTime,
  clockDate,
  onOpenNetworkModal,
  onOpenArchModal,
}) => {
  const activeIndex = Math.max(0, NAV_ITEMS.findIndex((i) => i.id === activeTab));

  return (
    <header className="relative z-20 flex items-center justify-between gap-6 w-full">
      {/* Left: Main Capsule Navigation Bar with Gooey Morphing Pill & Under-Bar */}
      <div className="relative flex-1 max-w-[1000px]">
        <nav
          aria-label="Dashboard primary navigation"
          className="tiramisu-nav-shell relative grid grid-cols-5 items-center px-2 py-2 rounded-full border border-white/10 overflow-hidden"
        >
          {/* Gooey Liquid Sliding Active Pill (Pure CSS Spring Animation + SVG Gooey Filter) */}
          <div
            aria-hidden="true"
            className="absolute inset-y-2 left-2 right-2 pointer-events-none gooey-soft-filter"
          >
            <div
              className="h-full w-1/5 px-1 transition-transform duration-500 ease-[cubic-bezier(0.22,1.35,0.36,1)]"
              style={{
                transform: `translate3d(${activeIndex * 100}%, 0, 0)`,
              }}
            >
              <div className="w-full h-full rounded-full tiramisu-pill-accent relative">
                {/* Trailing liquid droplet for extra gooey stretch */}
                <span className="absolute inset-y-1.5 inset-x-3 rounded-full bg-[#cbbab5] animate-goo-blob opacity-90" />
              </div>
            </div>
          </div>

          {NAV_ITEMS.map((item, idx) => {
            const isActive = activeTab === item.id;
            const IconComponent = item.Icon;

            return (
              <div key={item.id} className="relative flex items-center justify-center">
                <button
                  type="button"
                  onClick={() => onSelectTab(item.id)}
                  className={`relative z-10 w-full flex items-center justify-center gap-2.5 py-2.5 px-4 rounded-full text-[15px] font-semibold tracking-tight transition-colors duration-200 whitespace-nowrap shrink-0 cursor-pointer focus:outline-none focus-visible:ring-2 focus-visible:ring-[#cbbab5] ${
                    isActive
                      ? 'text-[#171316]'
                      : 'text-[#e8e3e2]/90 hover:text-white'
                  }`}
                >
                  <IconComponent
                    className={`w-[18px] h-[18px] transition-transform duration-200 ${
                      isActive ? 'text-[#171316] scale-105' : 'text-[#d5cecc]'
                    }`}
                  />
                  <span>{item.label}</span>
                </button>

                {/* Vertical divider between nav items matching the concept */}
                {idx < NAV_ITEMS.length - 1 && (
                  <span
                    aria-hidden="true"
                    className="absolute right-0 h-5 w-[1px] bg-white/15 shrink-0 pointer-events-none"
                  />
                )}
              </div>
            );
          })}
        </nav>

        {/* Bottom glowing taupe bar beneath the navigation shell (centered under Mods / follows tab with gentle spring) */}
        <div className="relative h-3 w-full overflow-hidden pointer-events-none flex justify-center">
          <div
            className="mt-1.5 h-[4px] w-16 rounded-full bg-[#cbbab5] shadow-[0_0_12px_rgba(203,186,181,0.75)] transition-transform duration-500 ease-[cubic-bezier(0.22,1.35,0.36,1)]"
            style={{
              transform: `translate3d(${(activeIndex - 0) * 18}px, 0, 0)`,
            }}
          />
        </div>
      </div>

      {/* Right: Vertical Separator + Clock/Date + Wi-Fi + Tiramisu Mascot Avatar */}
      <div className="flex items-center gap-6 shrink-0 pb-2">
        <span aria-hidden="true" className="h-9 w-[1px] bg-white/20" />

        <div className="text-left leading-tight select-none">
          <div className="text-[21px] font-bold tracking-tight text-white tabular-nums whitespace-nowrap">
            {clockTime}
          </div>
          <div className="text-[13px] font-medium text-[#b5aeb0] whitespace-nowrap mt-0.5">
            {clockDate}
          </div>
        </div>

        <button
          type="button"
          onClick={onOpenNetworkModal}
          aria-label="Xbox Network and DirectStorage status"
          className="p-2 rounded-full text-white/90 hover:text-white hover:bg-white/10 transition-colors cursor-pointer focus:outline-none focus-visible:ring-2 focus-visible:ring-[#cbbab5]"
        >
          <Wifi className="w-7 h-7 stroke-[2.2]" />
        </button>

        <button
          type="button"
          onClick={onOpenArchModal}
          aria-label="Tiramisu Profile and Xbox Series X|S Tech Stack"
          className="relative group rounded-full p-0.5 transition-transform duration-200 hover:scale-105 cursor-pointer focus:outline-none focus-visible:ring-2 focus-visible:ring-[#cbbab5]"
        >
          <div className="w-11 h-11 rounded-full overflow-hidden ring-1 ring-white/20 bg-[#231d1b] flex items-center justify-center">
            <img
              src="/assets/tiramisu/avatar_bear.png"
              alt="Tiramisu Bear Profile"
              referrerPolicy="no-referrer"
              className="w-full h-full object-cover"
            />
          </div>
        </button>
      </div>
    </header>
  );
};
