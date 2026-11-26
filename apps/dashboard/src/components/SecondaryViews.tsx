import React, { useState } from 'react';
import {
  Play,
  Search,
  HardDrive,
  FolderOpen,
  Sliders,
  Plus,
  ArrowUp,
  ArrowDown,
  Gamepad2,
  RefreshCw,
  Wifi,
  Star,
  Check,
  Trash2,
  Download,
} from 'lucide-react';
import {
  GameItem,
  ModItem,
  DevModeSettings,
  filterGamesByQuery,
} from '../utils/dashboardLogic';
import { GamepadTelemetry } from '../utils/useXboxGamepad';

interface SecondaryViewsProps {
  activeTab: 'library' | 'mods' | 'tools' | 'settings';
  allGames: GameItem[];
  mods: ModItem[];
  searchQuery: string;
  onSearchChange: (q: string) => void;
  onLaunchGame: (game: GameItem) => void;
  onOpenGameChapters: (game: GameItem) => void;
  onOpenGameConfig: (game: GameItem) => void;
  onToggleInstallGame: (gameId: string) => void;
  onToggleFavoriteGame: (gameId: string) => void;
  onToggleMod: (modId: string) => void;
  onMoveModOrder: (modId: string, direction: 'up' | 'down') => void;
  onOpenInstallModWindow: (gameId: string) => void;
  onOpenRegisterPackageWindow: () => void;
  onOpenStorageWindow: () => void;
  onOpenNetworkWindow: () => void;
  devSettings: DevModeSettings;
  onUpdateDevSettings: (patch: Partial<DevModeSettings>) => void;
  gamepadTelemetry: GamepadTelemetry;
}

export const SecondaryViews: React.FC<SecondaryViewsProps> = ({
  activeTab,
  allGames,
  mods,
  searchQuery,
  onSearchChange,
  onLaunchGame,
  onOpenGameChapters,
  onOpenGameConfig,
  onToggleInstallGame,
  onToggleFavoriteGame,
  onToggleMod,
  onMoveModOrder,
  onOpenInstallModWindow,
  onOpenRegisterPackageWindow,
  onOpenStorageWindow,
  onOpenNetworkWindow,
  devSettings,
  onUpdateDevSettings,
  gamepadTelemetry,
}) => {
  // Library state
  const [libraryFilter, setLibraryFilter] = useState<
    'all' | 'installed' | 'internal' | 'usb' | 'favorites'
  >('all');
  const [inspectedGameId, setInspectedGameId] = useState<string>(allGames[0]?.id || 'gow3');

  // Mods state
  const [selectedModGameId, setSelectedModGameId] = useState<string>('gow3');

  // Tools state
  const [activeToolPane, setActiveToolPane] = useState<
    'devportal' | 'scanner' | 'controller' | 'shadercache'
  >('devportal');
  const [scanLog, setScanLog] = useState<string[]>([
    '[UWP.IO] Mounted D:\\Tiramisu\\Games (NTFS USB 3.2 Gen 1)',
    '[UWP.IO] Mounted E:\\DevelopmentFiles\\WindowsApps (Internal NVMe)',
    '[AppX] Verified 12 registered game manifests (0 corrupted)',
  ]);
  const [simulatedButton, setSimulatedButton] = useState<string | null>(null);

  // Settings state
  const [settingsCategory, setSettingsCategory] = useState<
    'sandbox' | 'display' | 'controller' | 'shell'
  >('sandbox');

  const filteredLibraryGames = filterGamesByQuery(allGames, searchQuery, libraryFilter);
  const inspectedGame =
    allGames.find((g) => g.id === inspectedGameId) ||
    filteredLibraryGames[0] ||
    allGames[0];

  /* ============================================================================
   * 1. LIBRARY MENU — 2-Pane Xbox Dev Mode Game Browser + Top Inspector Window
   * ========================================================================== */
  if (activeTab === 'library') {
    const counts = {
      all: allGames.length,
      installed: allGames.filter((g) => g.installed).length,
      internal: allGames.filter((g) => g.storageDrive === 'Internal NVMe (E:\\)').length,
      usb: allGames.filter((g) => g.storageDrive === 'External USB 3.2 (D:\\)').length,
      favorites: allGames.filter((g) => g.favorite).length,
    };

    return (
      <div className="grid grid-cols-1 lg:grid-cols-12 gap-4 items-stretch min-h-[540px]">
        {/* Left Rail: Drive & Partition Filter */}
        <aside className="lg:col-span-3 tiramisu-glass rounded-[26px] p-5 border border-white/10 flex flex-col justify-between gap-4">
          <div className="space-y-4">
            <div className="px-1">
              <h1 className="text-[18px] font-bold tracking-tight text-white">
                Game Library
              </h1>
              <p className="text-xs text-[#9e9699] mt-0.5 tabular-nums">
                {counts.installed} Installed · {counts.all} Total Packages
              </p>
            </div>

            {/* Search Input */}
            <label className="relative flex items-center">
              <Search className="w-4 h-4 text-[#9e9699] absolute left-3.5 pointer-events-none" />
              <input
                type="search"
                value={searchQuery}
                onChange={(e) => onSearchChange(e.target.value)}
                placeholder="Search title or Title ID..."
                aria-label="Filter game library"
                className="w-full pl-9 pr-3 py-2 rounded-full bg-white/[0.05] border border-white/10 text-xs text-white placeholder:text-[#867e81] focus:outline-none focus:border-[#cbbab5]"
              />
            </label>

            {/* Filter Navigation Buttons */}
            <nav aria-label="Library drive filters" className="space-y-1">
              {[
                { id: 'all', label: 'All Titles', count: counts.all },
                { id: 'installed', label: 'Installed & Ready', count: counts.installed },
                { id: 'internal', label: 'Internal NVMe (E:\\)', count: counts.internal },
                { id: 'usb', label: 'USB 3.2 Drive (D:\\)', count: counts.usb },
                { id: 'favorites', label: 'Pinned Favorites', count: counts.favorites },
              ].map((item) => {
                const active = libraryFilter === item.id;
                return (
                  <button
                    key={item.id}
                    type="button"
                    onClick={() =>
                      setLibraryFilter(
                        item.id as 'all' | 'installed' | 'internal' | 'usb' | 'favorites'
                      )
                    }
                    className={`w-full flex items-center justify-between px-4 py-2.5 rounded-full text-xs font-semibold transition-colors cursor-pointer ${
                      active
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:bg-white/[0.06] hover:text-white'
                    }`}
                  >
                    <span>{item.label}</span>
                    <span
                      className={`tabular-nums font-mono ${
                        active ? 'text-[#171316]/80 font-bold' : 'text-[#8e8689]'
                      }`}
                    >
                      {item.count}
                    </span>
                  </button>
                );
              })}
            </nav>
          </div>

          <div className="pt-3 border-t border-white/10 space-y-2">
            <button
              type="button"
              onClick={onOpenRegisterPackageWindow}
              className="w-full flex items-center justify-center gap-2 py-2.5 px-4 rounded-full bg-white/[0.07] hover:bg-white/12 border border-white/10 text-xs font-semibold text-white transition-colors cursor-pointer"
            >
              <Plus className="w-3.5 h-3.5 text-[#cbbab5]" />
              <span>Register AppX / Game Path</span>
            </button>
            <button
              type="button"
              onClick={onOpenStorageWindow}
              className="w-full flex items-center justify-center gap-2 py-2 px-4 rounded-full bg-transparent hover:bg-white/[0.05] text-xs font-medium text-[#b5aeb0] hover:text-white transition-colors cursor-pointer"
            >
              <HardDrive className="w-3.5 h-3.5" />
              <span>Manage Drives (412.6 GB / 1.0 TB)</span>
            </button>
          </div>
        </aside>

        {/* Right Workspace: Selected Game Detail Banner + Real Game Cover Grid */}
        <section className="lg:col-span-9 flex flex-col gap-4">
          {/* Top Inspector Strip for Selected Game */}
          {inspectedGame && (
            <div className="relative rounded-[26px] overflow-hidden border border-white/12 bg-[#141218] min-h-[188px] p-6 flex flex-col justify-between shadow-xl">
              <img
                src={inspectedGame.heroImage}
                alt={inspectedGame.title}
                referrerPolicy="no-referrer"
                className="absolute right-0 top-0 h-full w-[62%] object-cover object-center opacity-80 pointer-events-none"
              />
              <div
                className="absolute inset-0 pointer-events-none"
                style={{
                  background:
                    'linear-gradient(90deg, #141218 0%, #141218 38%, rgba(20,18,24,0.82) 58%, rgba(20,18,24,0.2) 100%)',
                }}
              />

              <div className="relative z-10 flex items-start justify-between gap-4">
                <div>
                  <div className="flex items-center gap-2 text-xs text-[#cbbab5] font-mono tabular-nums">
                    <span>TITLE ID: {inspectedGame.titleId}</span>
                    <span aria-hidden="true">·</span>
                    <span>{inspectedGame.storageDrive}</span>
                    <span aria-hidden="true">·</span>
                    <span>{inspectedGame.uwpPackageStatus}</span>
                  </div>
                  <h2 className="text-2xl font-bold text-white tracking-tight mt-1">
                    {inspectedGame.title}
                  </h2>
                  <p className="text-xs font-mono text-[#a8a0a3] mt-0.5 truncate max-w-lg">
                    {inspectedGame.executablePath}
                  </p>
                </div>

                <button
                  type="button"
                  onClick={() => onToggleFavoriteGame(inspectedGame.id)}
                  aria-label={`Toggle favorite for ${inspectedGame.title}`}
                  className={`p-2.5 rounded-full border transition-colors cursor-pointer ${
                    inspectedGame.favorite
                      ? 'bg-[#cbbab5]/20 border-[#cbbab5] text-[#cbbab5]'
                      : 'bg-white/5 border-white/10 text-white/60 hover:text-white'
                  }`}
                >
                  <Star
                    className={`w-4 h-4 ${inspectedGame.favorite ? 'fill-[#cbbab5]' : ''}`}
                  />
                </button>
              </div>

              <div className="relative z-10 flex flex-wrap items-center justify-between gap-4 pt-4">
                <div className="flex items-center gap-2.5">
                  {inspectedGame.installed ? (
                    <button
                      type="button"
                      onClick={() => onLaunchGame(inspectedGame)}
                      className="tiramisu-pill-accent flex items-center gap-2.5 px-7 py-2.5 rounded-full text-sm font-bold text-[#171316] hover:brightness-105 cursor-pointer"
                    >
                      <Play className="w-4 h-4 fill-[#171316]" />
                      <span>Play</span>
                    </button>
                  ) : (
                    <button
                      type="button"
                      onClick={() => onToggleInstallGame(inspectedGame.id)}
                      className="tiramisu-pill-accent flex items-center gap-2 px-6 py-2.5 rounded-full text-sm font-bold text-[#171316] hover:brightness-105 cursor-pointer"
                    >
                      <Download className="w-4 h-4" />
                      <span>Mount & Install ({inspectedGame.sizeGb} GB)</span>
                    </button>
                  )}

                  <button
                    type="button"
                    onClick={() => onOpenGameChapters(inspectedGame)}
                    className="px-4 py-2.5 rounded-full bg-[#25232a]/90 hover:bg-[#312e38] border border-white/12 text-xs font-semibold text-white cursor-pointer"
                  >
                    Save States ({inspectedGame.chapters.length})
                  </button>

                  <button
                    type="button"
                    onClick={() => onOpenGameConfig(inspectedGame)}
                    className="px-4 py-2.5 rounded-full bg-[#25232a]/90 hover:bg-[#312e38] border border-white/12 text-xs font-semibold text-white cursor-pointer"
                  >
                    Launch Config
                  </button>

                  {inspectedGame.installed && (
                    <button
                      type="button"
                      onClick={() => onToggleInstallGame(inspectedGame.id)}
                      className="px-3.5 py-2.5 rounded-full bg-white/[0.06] hover:bg-white/12 text-xs font-medium text-[#b5aeb0] hover:text-white cursor-pointer"
                    >
                      Unmount
                    </button>
                  )}
                </div>

                <div className="flex items-center gap-3 text-xs text-[#d0c9cb] tabular-nums">
                  <span>{inspectedGame.sizeGb} GB</span>
                  <span aria-hidden="true">·</span>
                  <span>{inspectedGame.resolution}</span>
                  <span aria-hidden="true">·</span>
                  <span>{inspectedGame.fpsTarget} Hz VRR</span>
                  <span aria-hidden="true">·</span>
                  <span>{inspectedGame.playTimeHours}h Played</span>
                </div>
              </div>
            </div>
          )}

          {/* Game Cover Art Grid (Real cover art for all 12 games) */}
          <div className="tiramisu-glass rounded-[26px] p-5 border border-white/10 flex-1">
            {filteredLibraryGames.length === 0 ? (
              <div className="py-12 text-center">
                <p className="text-sm text-[#b5aeb0]">
                  No titles match filter "{searchQuery}".
                </p>
                <button
                  type="button"
                  onClick={() => {
                    onSearchChange('');
                    setLibraryFilter('all');
                  }}
                  className="mt-3 px-4 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
                >
                  Show All Games
                </button>
              </div>
            ) : (
              <div className="grid grid-cols-2 sm:grid-cols-3 xl:grid-cols-4 gap-3.5">
                {filteredLibraryGames.map((game) => {
                  const isFocused = inspectedGame?.id === game.id;
                  return (
                    <button
                      key={game.id}
                      type="button"
                      onClick={() => {
                        if (isFocused && game.installed) {
                          onLaunchGame(game);
                        } else {
                          setInspectedGameId(game.id);
                        }
                      }}
                      className={`group relative rounded-[18px] overflow-hidden bg-[#16141b] text-left transition-all duration-200 cursor-pointer focus:outline-none ${
                        isFocused
                          ? 'ring-2 ring-white shadow-[0_0_22px_rgba(255,255,255,0.22)]'
                          : 'ring-1 ring-white/10 hover:ring-white/35'
                      }`}
                    >
                      <div className="relative aspect-[16/9] w-full overflow-hidden bg-[#121016]">
                        <img
                          src={game.cardImage}
                          alt={game.title}
                          referrerPolicy="no-referrer"
                          className={`w-full h-full object-cover transition-transform duration-300 group-hover:scale-105 ${
                            game.installed ? 'opacity-100' : 'opacity-55'
                          }`}
                        />
                        <div className="absolute inset-0 bg-gradient-to-t from-black/85 via-black/15 to-transparent" />
                        <div className="absolute bottom-2.5 left-3 right-3 flex items-end justify-between gap-2">
                          <div className="min-w-0">
                            <div className="text-xs font-bold text-white truncate">
                              {game.title}
                            </div>
                            <div className="text-[11px] text-[#c2babc] tabular-nums mt-0.5">
                              {game.installed ? `${game.sizeGb} GB · Ready` : `${game.sizeGb} GB · Unmounted`}
                            </div>
                          </div>
                        </div>
                      </div>
                    </button>
                  );
                })}
              </div>
            )}
          </div>
        </section>
      </div>
    );
  }

  /* ============================================================================
   * 2. MODS MENU — 2-Pane Xbox Dev Mode Virtual Filesystem Mod Load Order Manager
   * ========================================================================== */
  if (activeTab === 'mods') {
    const gamesWithMods = allGames.slice(0, 7);
    const activeModGame =
      allGames.find((g) => g.id === selectedModGameId) || allGames[0];
    const gameMods = mods
      .filter((m) => m.gameId === activeModGame.id)
      .sort((a, b) => a.loadOrder - b.loadOrder);

    return (
      <div className="grid grid-cols-1 lg:grid-cols-12 gap-4 items-stretch min-h-[540px]">
        {/* Left Game Target Selector */}
        <aside className="lg:col-span-4 tiramisu-glass rounded-[26px] p-5 border border-white/10 flex flex-col justify-between gap-4">
          <div>
            <div className="px-1 mb-3">
              <h1 className="text-[18px] font-bold tracking-tight text-white">
                Virtual FS Mod Loader
              </h1>
              <p className="text-xs text-[#9e9699] mt-0.5">
                Select an installed title to manage D:\Tiramisu\Mods injection order
              </p>
            </div>

            <div className="space-y-1.5">
              {gamesWithMods.map((game) => {
                const isSelected = game.id === activeModGame.id;
                const count = mods.filter((m) => m.gameId === game.id && m.enabled).length;
                return (
                  <button
                    key={game.id}
                    type="button"
                    onClick={() => setSelectedModGameId(game.id)}
                    className={`w-full flex items-center gap-3 p-2 rounded-2xl transition-colors text-left cursor-pointer ${
                      isSelected
                        ? 'bg-[#cbbab5] text-[#171316]'
                        : 'hover:bg-white/[0.06] text-white'
                    }`}
                  >
                    <img
                      src={game.cardImage}
                      alt={game.title}
                      referrerPolicy="no-referrer"
                      className="w-14 h-9 rounded-lg object-cover shrink-0 border border-black/20"
                    />
                    <div className="min-w-0 flex-1">
                      <div className="text-xs font-bold truncate">{game.title}</div>
                      <div
                        className={`text-[11px] font-mono tabular-nums ${
                          isSelected ? 'text-[#171316]/75' : 'text-[#9e9699]'
                        }`}
                      >
                        Title ID {game.titleId} · {count} Active
                      </div>
                    </div>
                  </button>
                );
              })}
            </div>
          </div>

          <div className="pt-3 border-t border-white/10">
            <button
              type="button"
              onClick={() => onOpenInstallModWindow(activeModGame.id)}
              className="w-full flex items-center justify-center gap-2 py-2.5 px-4 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
            >
              <Plus className="w-4 h-4" />
              <span>Install Mod Package (.pak / .asi)</span>
            </button>
          </div>
        </aside>

        {/* Right Load Order Table */}
        <section className="lg:col-span-8 tiramisu-glass rounded-[26px] p-6 border border-white/10 flex flex-col justify-between gap-4">
          <div>
            <div className="flex flex-wrap items-center justify-between gap-4 pb-4 border-b border-white/10">
              <div className="flex items-center gap-3.5">
                <img
                  src={activeModGame.cardImage}
                  alt={activeModGame.title}
                  referrerPolicy="no-referrer"
                  className="w-20 h-12 rounded-xl object-cover border border-white/15"
                />
                <div>
                  <h2 className="text-lg font-bold text-white">{activeModGame.title}</h2>
                  <p className="text-xs font-mono text-[#a8a0a3] mt-0.5">
                    Target: {activeModGame.executablePath}
                  </p>
                </div>
              </div>

              <div className="flex items-center gap-2">
                <button
                  type="button"
                  onClick={() => onOpenInstallModWindow(activeModGame.id)}
                  className="px-4 py-2 rounded-full bg-white/[0.08] hover:bg-white/14 border border-white/10 text-xs font-semibold text-white cursor-pointer"
                >
                  + Add Mod File
                </button>
                <button
                  type="button"
                  onClick={() => onLaunchGame(activeModGame)}
                  className="px-5 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer flex items-center gap-1.5"
                >
                  <Play className="w-3.5 h-3.5 fill-current" />
                  <span>Launch with Mods</span>
                </button>
              </div>
            </div>

            {/* Mod Load Order Rows */}
            {gameMods.length === 0 ? (
              <div className="py-16 text-center">
                <p className="text-sm text-[#b5aeb0]">
                  No mods registered in D:\Tiramisu\Mods\{activeModGame.id}\
                </p>
                <button
                  type="button"
                  onClick={() => onOpenInstallModWindow(activeModGame.id)}
                  className="mt-3 px-4 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
                >
                  Mount First Mod Package
                </button>
              </div>
            ) : (
              <div className="divide-y divide-white/10 mt-2">
                {gameMods.map((mod, idx) => (
                  <div
                    key={mod.id}
                    className="py-3.5 flex items-center justify-between gap-4"
                  >
                    <div className="flex items-center gap-3.5 min-w-0">
                      <span className="font-mono text-xs text-[#9e9699] tabular-nums w-7">
                        #{String(idx + 1).padStart(2, '0')}
                      </span>
                      <div className="min-w-0">
                        <div className="text-sm font-semibold text-white truncate">
                          {mod.name}
                        </div>
                        <div className="text-xs font-mono text-[#9e9699] truncate mt-0.5 tabular-nums">
                          {mod.installPath} · v{mod.version} · {mod.sizeMb} MB
                        </div>
                      </div>
                    </div>

                    <div className="flex items-center gap-2 shrink-0">
                      <button
                        type="button"
                        onClick={() => onMoveModOrder(mod.id, 'up')}
                        aria-label={`Move ${mod.name} up in load order`}
                        className="p-2 rounded-full bg-white/[0.05] hover:bg-white/12 text-white/80 cursor-pointer"
                      >
                        <ArrowUp className="w-3.5 h-3.5" />
                      </button>
                      <button
                        type="button"
                        onClick={() => onMoveModOrder(mod.id, 'down')}
                        aria-label={`Move ${mod.name} down in load order`}
                        className="p-2 rounded-full bg-white/[0.05] hover:bg-white/12 text-white/80 cursor-pointer"
                      >
                        <ArrowDown className="w-3.5 h-3.5" />
                      </button>
                      <button
                        type="button"
                        onClick={() => onToggleMod(mod.id)}
                        className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer min-w-[84px] ${
                          mod.enabled
                            ? 'tiramisu-pill-accent'
                            : 'bg-white/10 text-[#b5aeb0] hover:text-white'
                        }`}
                      >
                        {mod.enabled ? 'Enabled' : 'Disabled'}
                      </button>
                    </div>
                  </div>
                ))}
              </div>
            )}
          </div>

          <div className="pt-3 border-t border-white/10 flex items-center justify-between text-xs text-[#9e9699] font-mono">
            <span>HOOK METHOD: UWP Win32 FileRedirection Driver</span>
            <span>SANDBOX: {devSettings.sandboxId}</span>
          </div>
        </section>
      </div>
    );
  }

  /* ============================================================================
   * 3. TOOLS MENU — Xbox Dev Mode Utilities (Device Portal, AppX Scanner, XInput)
   * ========================================================================== */
  if (activeTab === 'tools') {
    return (
      <div className="grid grid-cols-1 lg:grid-cols-12 gap-4 items-stretch min-h-[540px]">
        {/* Left Rail: Xbox Dev Mode Utility Selector */}
        <aside className="lg:col-span-3 tiramisu-glass rounded-[26px] p-5 border border-white/10 flex flex-col justify-between gap-4">
          <div className="space-y-2">
            <div className="px-1 mb-3">
              <h1 className="text-[18px] font-bold tracking-tight text-white">
                Dev Mode Utilities
              </h1>
              <p className="text-xs text-[#9e9699] mt-0.5">
                Xbox Series X|S Retail Dev Kit & UWP Bridge Tools
              </p>
            </div>

            {[
              {
                id: 'devportal',
                label: 'Device Portal & SMB',
                sub: '192.168.1.84:11443',
                Icon: Wifi,
              },
              {
                id: 'scanner',
                label: 'AppX & Drive Scanner',
                sub: 'D:\\ & E:\\ Game Indexer',
                Icon: FolderOpen,
              },
              {
                id: 'controller',
                label: 'XInput Controller Test',
                sub: 'W3C Gamepad / Impulse',
                Icon: Gamepad2,
              },
              {
                id: 'shadercache',
                label: 'DX12 Shader & NVMe',
                sub: 'RDNA2 Pipeline Cache',
                Icon: HardDrive,
              },
            ].map((tool) => {
              const active = activeToolPane === tool.id;
              const IconComp = tool.Icon;
              return (
                <button
                  key={tool.id}
                  type="button"
                  onClick={() =>
                    setActiveToolPane(
                      tool.id as 'devportal' | 'scanner' | 'controller' | 'shadercache'
                    )
                  }
                  className={`w-full flex items-center gap-3 px-4 py-3 rounded-2xl text-left transition-colors cursor-pointer ${
                    active
                      ? 'tiramisu-pill-accent'
                      : 'text-[#d5cecc] hover:bg-white/[0.06] hover:text-white'
                  }`}
                >
                  <IconComp className="w-4 h-4 shrink-0" />
                  <div className="min-w-0">
                    <div className="text-xs font-bold truncate">{tool.label}</div>
                    <div
                      className={`text-[11px] font-mono truncate ${
                        active ? 'text-[#171316]/75' : 'text-[#8e8689]'
                      }`}
                    >
                      {tool.sub}
                    </div>
                  </div>
                </button>
              );
            })}
          </div>

          <div className="pt-3 border-t border-white/10">
            <button
              type="button"
              onClick={onOpenNetworkWindow}
              className="w-full py-2.5 px-4 rounded-full bg-white/[0.07] hover:bg-white/12 border border-white/10 text-xs font-semibold text-white cursor-pointer"
            >
              Open Full Network Window
            </button>
          </div>
        </aside>

        {/* Right Utility Workspace */}
        <section className="lg:col-span-9 tiramisu-glass rounded-[26px] p-6 border border-white/10 flex flex-col justify-between">
          {activeToolPane === 'devportal' && (
            <div className="space-y-5">
              <div className="flex items-center justify-between border-b border-white/10 pb-4">
                <div>
                  <h2 className="text-xl font-bold text-white">
                    Xbox Device Portal (WDP) & SMB File Server
                  </h2>
                  <p className="text-xs text-[#a8a0a3] mt-0.5">
                    Deploy loose Win32/UWP game folders directly from your PC over LAN to Xbox Series X|S
                  </p>
                </div>
                <button
                  type="button"
                  onClick={onOpenNetworkWindow}
                  className="px-4 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
                >
                  Configure Credentials
                </button>
              </div>

              <div className="divide-y divide-white/10 text-sm">
                <div className="py-3.5 flex items-center justify-between">
                  <div>
                    <div className="font-semibold text-white">Xbox Device Portal HTTPS Endpoint</div>
                    <div className="text-xs font-mono text-[#9e9699] mt-0.5">
                      https://192.168.1.84:{devSettings.devPortalPort} (SSL Pinning Enabled)
                    </div>
                  </div>
                  <button
                    type="button"
                    onClick={() =>
                      onUpdateDevSettings({ devPortalHttps: !devSettings.devPortalHttps })
                    }
                    className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                      devSettings.devPortalHttps
                        ? 'tiramisu-pill-accent'
                        : 'bg-white/10 text-white'
                    }`}
                  >
                    {devSettings.devPortalHttps ? 'Listening' : 'Stopped'}
                  </button>
                </div>

                <div className="py-3.5 flex items-center justify-between">
                  <div>
                    <div className="font-semibold text-white">SMB Network Share (DevelopmentFiles)</div>
                    <div className="text-xs font-mono text-[#9e9699] mt-0.5">
                      \\192.168.1.84\DevelopmentFiles · Mapped to E:\ & D:\Tiramisu
                    </div>
                  </div>
                  <button
                    type="button"
                    onClick={() =>
                      onUpdateDevSettings({ smbShareEnabled: !devSettings.smbShareEnabled })
                    }
                    className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                      devSettings.smbShareEnabled
                        ? 'tiramisu-pill-accent'
                        : 'bg-white/10 text-white'
                    }`}
                  >
                    {devSettings.smbShareEnabled ? 'Mounted' : 'Unmounted'}
                  </button>
                </div>

                <div className="py-3.5 flex items-center justify-between">
                  <div>
                    <div className="font-semibold text-white">Local FTP Game Transfer Daemon</div>
                    <div className="text-xs font-mono text-[#9e9699] mt-0.5">
                      ftp://xboxdev@192.168.1.84:{devSettings.ftpPort}/D/Tiramisu/Games
                    </div>
                  </div>
                  <button
                    type="button"
                    onClick={() =>
                      onUpdateDevSettings({ ftpServerEnabled: !devSettings.ftpServerEnabled })
                    }
                    className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                      devSettings.ftpServerEnabled
                        ? 'tiramisu-pill-accent'
                        : 'bg-white/10 text-white'
                    }`}
                  >
                    {devSettings.ftpServerEnabled ? 'Port 2121 Active' : 'Disabled'}
                  </button>
                </div>
              </div>
            </div>
          )}

          {activeToolPane === 'scanner' && (
            <div className="space-y-5">
              <div className="flex items-center justify-between border-b border-white/10 pb-4">
                <div>
                  <h2 className="text-xl font-bold text-white">
                    UWP AppX & USB 3.2 Game Directory Scanner
                  </h2>
                  <p className="text-xs text-[#a8a0a3] mt-0.5">
                    Scans D:\Tiramisu\Games and E:\DevelopmentFiles\WindowsApps for executables and cover art
                  </p>
                </div>
                <div className="flex items-center gap-2">
                  <button
                    type="button"
                    onClick={() =>
                      setScanLog((prev) => [
                        ...prev,
                        `[Scanner] Indexed D:\\Tiramisu\\Games — Found ${allGames.length} titles (412.6 GB verified)`,
                      ])
                    }
                    className="px-4 py-2 rounded-full bg-white/10 hover:bg-white/15 text-xs font-semibold text-white cursor-pointer flex items-center gap-1.5"
                  >
                    <RefreshCw className="w-3.5 h-3.5" />
                    <span>Rescan Drives</span>
                  </button>
                  <button
                    type="button"
                    onClick={onOpenRegisterPackageWindow}
                    className="px-4 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
                  >
                    + Register Manual Path
                  </button>
                </div>
              </div>

              <div className="rounded-2xl bg-[#100f14] border border-white/10 p-4 font-mono text-xs text-[#d5cecc] space-y-1.5 max-h-64 overflow-y-auto">
                {scanLog.map((line, i) => (
                  <div key={i} className="flex items-center gap-2">
                    <span className="text-[#cbbab5]">›</span>
                    <span>{line}</span>
                  </div>
                ))}
              </div>
            </div>
          )}

          {activeToolPane === 'controller' && (
            <div className="space-y-5">
              <div className="flex items-center justify-between border-b border-white/10 pb-4">
                <div>
                  <h2 className="text-xl font-bold text-white">
                    Xbox Wireless Controller (XInput & W3C Gamepad API)
                  </h2>
                  <p className="text-xs text-[#a8a0a3] mt-0.5">
                    Live input bridge for Xbox Series X|S Dev Mode WebView2 & native game passthrough
                  </p>
                </div>
                <span className="text-xs font-mono text-[#cbbab5] tabular-nums">
                  {gamepadTelemetry.connected
                    ? `Hardware Connected: ${gamepadTelemetry.id}`
                    : 'Port 0: Xbox Wireless Controller Ready'}
                </span>
              </div>

              <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                <div className="p-4 rounded-2xl bg-white/[0.03] border border-white/10 space-y-3">
                  <div className="text-xs font-semibold text-[#b5aeb0]">
                    Interactive Button Passthrough Test
                  </div>
                  <div className="flex flex-wrap gap-2">
                    {['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'D-Pad', 'Menu'].map(
                      (btn) => {
                        const isPressed =
                          simulatedButton === btn ||
                          gamepadTelemetry.pressedButtons.includes(btn);
                        return (
                          <button
                            key={btn}
                            type="button"
                            onClick={() => setSimulatedButton(btn)}
                            className={`px-3.5 py-2 rounded-full text-xs font-bold transition-colors cursor-pointer ${
                              isPressed
                                ? 'tiramisu-pill-accent'
                                : 'bg-white/10 text-white hover:bg-white/15'
                            }`}
                          >
                            {btn}
                          </button>
                        );
                      }
                    )}
                  </div>
                  <p className="text-xs font-mono text-[#9e9699] tabular-nums">
                    Last Input Event:{' '}
                    {gamepadTelemetry.pressedButtons.join(', ') ||
                      simulatedButton ||
                      'None (Press controller button or click above)'}
                  </p>
                </div>

                <div className="p-4 rounded-2xl bg-white/[0.03] border border-white/10 space-y-3 text-xs tabular-nums">
                  <div className="font-semibold text-[#b5aeb0]">
                    Analog Sticks & Impulse Triggers
                  </div>
                  <div className="flex justify-between">
                    <span className="text-[#9e9699]">Left Stick (LX / LY)</span>
                    <span className="font-mono text-white">
                      {gamepadTelemetry.leftStickX} / {gamepadTelemetry.leftStickY}
                    </span>
                  </div>
                  <div className="flex justify-between">
                    <span className="text-[#9e9699]">Right Stick (RX / RY)</span>
                    <span className="font-mono text-white">
                      {gamepadTelemetry.rightStickX} / {gamepadTelemetry.rightStickY}
                    </span>
                  </div>
                  <div className="flex justify-between">
                    <span className="text-[#9e9699]">Deadzone Threshold</span>
                    <span className="font-mono text-white">
                      {devSettings.controllerDeadzonePct}%
                    </span>
                  </div>
                </div>
              </div>
            </div>
          )}

          {activeToolPane === 'shadercache' && (
            <div className="space-y-5">
              <div className="flex items-center justify-between border-b border-white/10 pb-4">
                <div>
                  <h2 className="text-xl font-bold text-white">
                    DirectX 12 RDNA2 Shader Cache & Storage Partitions
                  </h2>
                  <p className="text-xs text-[#a8a0a3] mt-0.5">
                    Manage precompiled DXIL shader archives on E:\ and D:\Tiramisu\Cache
                  </p>
                </div>
                <button
                  type="button"
                  onClick={onOpenStorageWindow}
                  className="px-4 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
                >
                  Open Partition Manager Window
                </button>
              </div>

              <div className="divide-y divide-white/10 text-sm">
                {allGames.slice(0, 5).map((g) => (
                  <div key={g.id} className="py-3 flex items-center justify-between">
                    <div>
                      <div className="font-semibold text-white">{g.title}</div>
                      <div className="text-xs font-mono text-[#9e9699]">
                        D:\Tiramisu\Cache\{g.titleId}_dx12.pso · Verified RDNA2 Wave64
                      </div>
                    </div>
                    <span className="text-xs font-mono text-[#cbbab5] tabular-nums">
                      Compiled
                    </span>
                  </div>
                ))}
              </div>
            </div>
          )}

          <div className="pt-4 border-t border-white/10 flex items-center justify-between text-xs text-[#9e9699] font-mono">
            <span>TARGET HARDWARE: {devSettings.consoleModel.toUpperCase()}</span>
            <span>UWP EXCLUSIVE GAME MODE (13.5 GB RAM)</span>
          </div>
        </section>
      </div>
    );
  }

  /* ============================================================================
   * 4. SETTINGS MENU — Authentic 2-Column Xbox Console Settings Menu
   * ========================================================================== */
  return (
    <div className="grid grid-cols-1 lg:grid-cols-12 gap-4 items-stretch min-h-[540px]">
      {/* Left Category List */}
      <aside className="lg:col-span-3 tiramisu-glass rounded-[26px] p-5 border border-white/10 flex flex-col justify-between">
        <div className="space-y-1.5">
          <div className="px-1 mb-3">
            <h1 className="text-[18px] font-bold tracking-tight text-white">
              System Settings
            </h1>
            <p className="text-xs text-[#9e9699] mt-0.5">
              Tiramisu UWP Shell & Xbox Dev Mode Config
            </p>
          </div>

          {[
            { id: 'sandbox', label: 'Dev Mode & Sandbox' },
            { id: 'display', label: 'Display, VRR & HDR' },
            { id: 'controller', label: 'Controller & Input' },
            { id: 'shell', label: 'Dashboard Appearance' },
          ].map((cat) => {
            const active = settingsCategory === cat.id;
            return (
              <button
                key={cat.id}
                type="button"
                onClick={() =>
                  setSettingsCategory(
                    cat.id as 'sandbox' | 'display' | 'controller' | 'shell'
                  )
                }
                className={`w-full flex items-center justify-between px-4 py-3 rounded-full text-xs font-bold transition-colors cursor-pointer ${
                  active
                    ? 'tiramisu-pill-accent'
                    : 'text-[#d5cecc] hover:bg-white/[0.06] hover:text-white'
                }`}
              >
                <span>{cat.label}</span>
              </button>
            );
          })}
        </div>

        <div className="pt-3 border-t border-white/10 text-xs font-mono text-[#9e9699] px-1">
          <div>BUILD: Tiramisu UWP 2.4.0</div>
          <div className="mt-0.5">ARCH: x86_64 · DirectX 12.2</div>
        </div>
      </aside>

      {/* Right Settings Rows */}
      <section className="lg:col-span-9 tiramisu-glass rounded-[26px] p-6 border border-white/10 flex flex-col justify-between">
        {settingsCategory === 'sandbox' && (
          <div className="divide-y divide-white/10">
            <div className="pb-4">
              <h2 className="text-xl font-bold text-white">Xbox Dev Mode & Hardware Target</h2>
              <p className="text-xs text-[#a8a0a3] mt-0.5">
                Configure retail Dev Mode memory allocation and default NTFS USB 3.2 mount points
              </p>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Target Console Hardware Profile</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Adjusts default internal render resolution and memory ceiling (16GB Series X vs 10GB Series S)
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {(['Xbox Series X', 'Xbox Series S'] as const).map((model) => (
                  <button
                    key={model}
                    type="button"
                    onClick={() =>
                      onUpdateDevSettings({
                        consoleModel: model,
                        outputResolution: model === 'Xbox Series X' ? '2160p 4K' : '1440p QHD',
                      })
                    }
                    className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                      devSettings.consoleModel === model
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {model}
                  </button>
                ))}
              </div>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Default Game Installation Drive</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Primary volume for loose PC game directories and virtual mod mounts
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {(['External USB 3.2 (D:\\)', 'Internal NVMe (E:\\)'] as const).map((drv) => (
                  <button
                    key={drv}
                    type="button"
                    onClick={() => onUpdateDevSettings({ defaultGamesDrive: drv })}
                    className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                      devSettings.defaultGamesDrive === drv
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {drv}
                  </button>
                ))}
              </div>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Auto-Mount USB 3.2 NTFS Media on Boot</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Automatically grants UWP broadFileSystemAccess permission to D:\Tiramisu
                </div>
              </div>
              <button
                type="button"
                onClick={() =>
                  onUpdateDevSettings({ autoMountUsb: !devSettings.autoMountUsb })
                }
                className={`px-5 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                  devSettings.autoMountUsb
                    ? 'tiramisu-pill-accent'
                    : 'bg-white/10 text-white'
                }`}
              >
                {devSettings.autoMountUsb ? 'Enabled' : 'Disabled'}
              </button>
            </div>
          </div>
        )}

        {settingsCategory === 'display' && (
          <div className="divide-y divide-white/10">
            <div className="pb-4">
              <h2 className="text-xl font-bold text-white">Display, VRR & HDMI 2.1 Output</h2>
              <p className="text-xs text-[#a8a0a3] mt-0.5">
                Configure DirectX 12 swapchain presentation resolution, 120Hz VRR, and HDR10 luminance
              </p>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Swapchain Output Resolution</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Target backbuffer resolution passed to CoreWindow / SwapChainPanel
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {(['2160p 4K', '1440p QHD', '1080p FHD'] as const).map((res) => (
                  <button
                    key={res}
                    type="button"
                    onClick={() => onUpdateDevSettings({ outputResolution: res })}
                    className={`px-3.5 py-1.5 rounded-full text-xs font-bold cursor-pointer tabular-nums ${
                      devSettings.outputResolution === res
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {res}
                  </button>
                ))}
              </div>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Refresh Rate & VRR Ceiling</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Requires HDMI 2.1 or 120Hz display mode enabled in Xbox OS TV & Display Options
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {([120, 60] as const).map((hz) => (
                  <button
                    key={hz}
                    type="button"
                    onClick={() => onUpdateDevSettings({ refreshRateHz: hz })}
                    className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer tabular-nums ${
                      devSettings.refreshRateHz === hz
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {hz} Hz
                  </button>
                ))}
              </div>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Xbox Auto HDR & HGIG Tone Mapping</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Upconverts SDR DirectX 11/12 swapchains to 10-bit HDR10 Rec.2020
                </div>
              </div>
              <button
                type="button"
                onClick={() => onUpdateDevSettings({ autoHdr: !devSettings.autoHdr })}
                className={`px-5 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                  devSettings.autoHdr ? 'tiramisu-pill-accent' : 'bg-white/10 text-white'
                }`}
              >
                {devSettings.autoHdr ? 'Active' : 'Off'}
              </button>
            </div>
          </div>
        )}

        {settingsCategory === 'controller' && (
          <div className="divide-y divide-white/10">
            <div className="pb-4">
              <h2 className="text-xl font-bold text-white">Controller & Navigation</h2>
              <p className="text-xs text-[#a8a0a3] mt-0.5">
                Configure Xbox Wireless Controller stick deadzones and Impulse Trigger haptics
              </p>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Analog Stick Inner Deadzone</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Filters micro-drift before forwarding axes to WebView2 and XInput
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {[5, 8, 12, 15].map((pct) => (
                  <button
                    key={pct}
                    type="button"
                    onClick={() => onUpdateDevSettings({ controllerDeadzonePct: pct })}
                    className={`px-3.5 py-1.5 rounded-full text-xs font-bold cursor-pointer tabular-nums ${
                      devSettings.controllerDeadzonePct === pct
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {pct}%
                  </button>
                ))}
              </div>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Impulse Trigger & Rumble Passthrough</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Maps Windows.Gaming.Input 4-motor trigger vibration to PC XInput calls
                </div>
              </div>
              <button
                type="button"
                onClick={() =>
                  onUpdateDevSettings({
                    controllerVibration: !devSettings.controllerVibration,
                  })
                }
                className={`px-5 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                  devSettings.controllerVibration
                    ? 'tiramisu-pill-accent'
                    : 'bg-white/10 text-white'
                }`}
              >
                {devSettings.controllerVibration ? 'Enabled' : 'Disabled'}
              </button>
            </div>
          </div>
        )}

        {settingsCategory === 'shell' && (
          <div className="divide-y divide-white/10">
            <div className="pb-4">
              <h2 className="text-xl font-bold text-white">Dashboard Appearance & Gooey Physics</h2>
              <p className="text-xs text-[#a8a0a3] mt-0.5">
                Tune SVG metaball viscosity and ambient horizon ribbon animations
              </p>
            </div>

            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="text-sm font-semibold text-white">Navigation & Carousel Gooey Viscosity</div>
                <div className="text-xs text-[#9e9699] mt-0.5">
                  Controls SVG feGaussianBlur + feColorMatrix alpha threshold on sliding pills
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {(['standard', 'ultra'] as const).map((mode) => (
                  <button
                    key={mode}
                    type="button"
                    onClick={() => onUpdateDevSettings({ gooeyFilterMode: mode })}
                    className={`px-4 py-1.5 rounded-full text-xs font-bold capitalize cursor-pointer ${
                      devSettings.gooeyFilterMode === mode
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {mode}
                  </button>
                ))}
              </div>
            </div>
          </div>
        )}

        <div className="pt-4 border-t border-white/10 flex items-center justify-between text-xs text-[#9e9699]">
          <span>Changes are saved immediately to LocalFolder\tiramisu_config.json</span>
          <span className="font-mono text-[#cbbab5]">[B] Return Home</span>
        </div>
      </section>
    </div>
  );
};
