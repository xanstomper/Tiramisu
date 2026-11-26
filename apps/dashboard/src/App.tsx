/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

import React, { useState, useEffect, useCallback } from 'react';
import {
  Play,
  List,
  Settings,
  Box,
  Layers,
  ChevronRight,
  Bookmark,
  HardDrive,
  FolderOpen,
  Plus,
  Check,
  RefreshCw,
} from 'lucide-react';
import { AmbientWaveBackground } from './components/AmbientWaveBackground';
import { TopNavigation } from './components/TopNavigation';
import { SecondaryViews } from './components/SecondaryViews';
import { TiramisuWindow } from './components/TiramisuWindow';
import { useXboxGamepad } from './utils/useXboxGamepad';
import {
  NavTabId,
  GameItem,
  ModItem,
  DevModeSettings,
  INITIAL_GAMES,
  INITIAL_MODS,
  INITIAL_DEV_SETTINGS,
  LIBRARY_BAR_HEIGHTS,
  calculateStorageMetrics,
  stepCardSelection,
} from './utils/dashboardLogic';

const NAV_ORDER: NavTabId[] = ['home', 'library', 'mods', 'tools', 'settings'];

export default function App() {
  const [activeTab, setActiveTab] = useState<NavTabId>('home');
  const [games, setGames] = useState<GameItem[]>(INITIAL_GAMES);
  const [mods, setMods] = useState<ModItem[]>(INITIAL_MODS);
  const [devSettings, setDevSettings] = useState<DevModeSettings>(INITIAL_DEV_SETTINGS);
  const [selectedCardIndex, setSelectedCardIndex] = useState<number>(0);
  const [heroSlideIndex, setHeroSlideIndex] = useState<number>(0);
  const [searchQuery, setSearchQuery] = useState<string>('');

  // Real Windows State
  const [launchingGame, setLaunchingGame] = useState<GameItem | null>(null);
  const [chaptersWindowGameId, setChaptersWindowGameId] = useState<string | null>(null);
  const [configWindowGameId, setConfigWindowGameId] = useState<string | null>(null);
  const [showStorageWindow, setShowStorageWindow] = useState<boolean>(false);
  const [showNetworkWindow, setShowNetworkWindow] = useState<boolean>(false);
  const [showDevAccountWindow, setShowDevAccountWindow] = useState<boolean>(false);
  const [installModTargetGameId, setInstallModTargetGameId] = useState<string | null>(null);

  // Form states inside windows
  const [newSnapshotName, setNewSnapshotName] = useState<string>('');
  const [newModName, setNewModName] = useState<string>('');
  const [newModFileName, setNewModFileName] = useState<string>('');
  const [newModCategory, setNewModCategory] = useState<
    'Framerate' | 'Graphics' | 'Gameplay' | 'Controller'
  >('Framerate');
  const [modFormError, setModFormError] = useState<string>('');

  // Continue Playing shows the first 5 featured titles matching the concept layout
  const continuePlayingGames = games.slice(0, 5);
  const activeGame = continuePlayingGames[selectedCardIndex] || continuePlayingGames[0];
  const storageMetrics = calculateStorageMetrics(games, 0.0, 1.0);

  const chaptersWindowGame = games.find((g) => g.id === chaptersWindowGameId) || null;
  const configWindowGame = games.find((g) => g.id === configWindowGameId) || null;

  const hasOpenWindow = Boolean(
    launchingGame ||
      chaptersWindowGame ||
      configWindowGame ||
      showStorageWindow ||
      showNetworkWindow ||
      showDevAccountWindow ||
      installModTargetGameId
  );

  const closeAllWindows = useCallback(() => {
    setLaunchingGame(null);
    setChaptersWindowGameId(null);
    setConfigWindowGameId(null);
    setShowStorageWindow(false);
    setShowNetworkWindow(false);
    setShowDevAccountWindow(false);
    setInstallModTargetGameId(null);
  }, []);

  // Real W3C Gamepad API Integration for Xbox Series X|S Controller Navigation
  const gamepadTelemetry = useXboxGamepad({
    onButtonPress: (btn) => {
      if (btn === 'B') {
        if (hasOpenWindow) {
          closeAllWindows();
        } else if (activeTab !== 'home') {
          setActiveTab('home');
        }
      } else if (btn === 'LB' && !hasOpenWindow) {
        const curIdx = NAV_ORDER.indexOf(activeTab);
        setActiveTab(NAV_ORDER[(curIdx - 1 + NAV_ORDER.length) % NAV_ORDER.length]);
      } else if (btn === 'RB' && !hasOpenWindow) {
        const curIdx = NAV_ORDER.indexOf(activeTab);
        setActiveTab(NAV_ORDER[(curIdx + 1) % NAV_ORDER.length]);
      } else if (btn === 'DPadRight' && activeTab === 'home' && !hasOpenWindow) {
        setSelectedCardIndex((prev) =>
          stepCardSelection(prev, continuePlayingGames.length, 1)
        );
      } else if (btn === 'DPadLeft' && activeTab === 'home' && !hasOpenWindow) {
        setSelectedCardIndex((prev) =>
          stepCardSelection(prev, continuePlayingGames.length, -1)
        );
      } else if (btn === 'A' && activeTab === 'home' && !hasOpenWindow) {
        setLaunchingGame(activeGame);
      } else if (btn === 'X' && activeTab === 'home' && !hasOpenWindow) {
        setChaptersWindowGameId(activeGame.id);
      } else if (btn === 'Y' && activeTab === 'home' && !hasOpenWindow) {
        setConfigWindowGameId(activeGame.id);
      }
    },
  });

  // Keyboard Navigation (ArrowLeft / ArrowRight / Enter / Escape)
  const handleKeyDown = useCallback(
    (e: KeyboardEvent) => {
      if (hasOpenWindow) {
        if (e.key === 'Escape') {
          closeAllWindows();
        }
        return;
      }

      if (
        document.activeElement?.tagName === 'INPUT' ||
        document.activeElement?.tagName === 'TEXTAREA'
      ) {
        return;
      }

      if (e.key === 'ArrowRight' && activeTab === 'home') {
        e.preventDefault();
        setSelectedCardIndex((prev) =>
          stepCardSelection(prev, continuePlayingGames.length, 1)
        );
      } else if (e.key === 'ArrowLeft' && activeTab === 'home') {
        e.preventDefault();
        setSelectedCardIndex((prev) =>
          stepCardSelection(prev, continuePlayingGames.length, -1)
        );
      } else if (e.key === 'Enter' && activeTab === 'home') {
        setLaunchingGame(activeGame);
      }
    },
    [hasOpenWindow, closeAllWindows, activeTab, continuePlayingGames.length, activeGame]
  );

  useEffect(() => {
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [handleKeyDown]);

  useEffect(() => {
    setHeroSlideIndex(selectedCardIndex % 3);
  }, [selectedCardIndex]);

  const handleUpdateDevSettings = (patch: Partial<DevModeSettings>) => {
    setDevSettings((prev) => ({ ...prev, ...patch }));
  };

  const handleUpdateGame = (gameId: string, patch: Partial<GameItem>) => {
    setGames((prev) =>
      prev.map((g) => (g.id === gameId ? { ...g, ...patch } : g))
    );
  };

  const handleToggleInstallGame = (gameId: string) => {
    setGames((prev) =>
      prev.map((g) => (g.id === gameId ? { ...g, installed: !g.installed } : g))
    );
  };

  const handleToggleFavoriteGame = (gameId: string) => {
    setGames((prev) =>
      prev.map((g) => (g.id === gameId ? { ...g, favorite: !g.favorite } : g))
    );
  };

  const handleToggleMod = (modId: string) => {
    setMods((prev) =>
      prev.map((m) => (m.id === modId ? { ...m, enabled: !m.enabled } : m))
    );
  };

  const handleMoveModOrder = (modId: string, direction: 'up' | 'down') => {
    setMods((prev) => {
      const target = prev.find((m) => m.id === modId);
      if (!target) return prev;
      const sameGameMods = prev
        .filter((m) => m.gameId === target.gameId)
        .sort((a, b) => a.loadOrder - b.loadOrder);
      const idx = sameGameMods.findIndex((m) => m.id === modId);
      const swapIdx = direction === 'up' ? idx - 1 : idx + 1;
      if (swapIdx < 0 || swapIdx >= sameGameMods.length) return prev;

      const neighbor = sameGameMods[swapIdx];
      return prev.map((m) => {
        if (m.id === target.id) return { ...m, loadOrder: neighbor.loadOrder };
        if (m.id === neighbor.id) return { ...m, loadOrder: target.loadOrder };
        return m;
      });
    });
  };

  const handleCreateSaveSnapshot = (gameId: string) => {
    const label = newSnapshotName.trim() || 'Manual Quick Resume State';
    setGames((prev) =>
      prev.map((g) =>
        g.id === gameId
          ? {
              ...g,
              chapters: [
                {
                  id: `snap-${Date.now()}`,
                  name: label,
                  timestamp: 'Just now · 7:42 PM',
                  slot: `Slot 0${g.chapters.length + 1} (Quick Resume)`,
                  progress: 100,
                },
                ...g.chapters,
              ],
            }
          : g
      )
    );
    setNewSnapshotName('');
  };

  const handleAddModSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (!installModTargetGameId) return;
    if (!newModName.trim() || !newModFileName.trim()) {
      setModFormError('Please provide both a Mod Title and a valid package filename (.pak, .asi, or .dll).');
      return;
    }

    const targetGame = games.find((g) => g.id === installModTargetGameId) || games[0];
    const cleanFile = newModFileName.trim();
    const existingCount = mods.filter((m) => m.gameId === targetGame.id).length;

    const created: ModItem = {
      id: `mod-${Date.now()}`,
      gameId: targetGame.id,
      gameName: targetGame.title,
      name: newModName.trim(),
      fileName: cleanFile,
      installPath: `D:\\Tiramisu\\Mods\\${targetGame.id}\\${cleanFile}`,
      author: 'LocalDevKit',
      version: '1.0.0',
      category: newModCategory,
      enabled: true,
      loadOrder: existingCount + 1,
      sizeMb: 28,
    };

    setMods((prev) => [...prev, created]);
    setNewModName('');
    setNewModFileName('');
    setModFormError('');
    setInstallModTargetGameId(null);
  };

  return (
    <div
      data-state={launchingGame ? 'playing' : activeTab}
      className="relative min-h-screen w-full bg-[#08070a] text-white flex flex-col justify-between overflow-x-hidden select-none"
    >
      {/* Ambient Sweeping Aurora Light Waves & Global SVG Gooey Metaball Filters */}
      <AmbientWaveBackground
        accentGlow={activeGame.glowColor}
        gooeyIntensity={devSettings.gooeyFilterMode}
      />

      {/* Main 16:9 Xbox Series X|S Dashboard Viewport Container */}
      <div className="relative z-10 w-full max-w-[1440px] mx-auto px-8 py-6 my-auto flex flex-col justify-center gap-5">
        {/* Top Row: Capsule Navigation Bar + Clock + Wi-Fi + Tiramisu Bear Avatar */}
        <TopNavigation
          activeTab={activeTab}
          onSelectTab={(tab) => setActiveTab(tab)}
          clockTime="7:42 PM"
          clockDate="Sun, Nov 22"
          onOpenNetworkModal={() => setShowNetworkWindow(true)}
          onOpenArchModal={() => setShowDevAccountWindow(true)}
        />

        <p className="text-xs text-[#cbb8b4]">Design preview · Sample library</p>

        {/* Center & Bottom Workspace */}
        {activeTab !== 'home' ? (
          <main className="flex flex-col justify-start my-1">
            <SecondaryViews
              activeTab={activeTab}
              allGames={games}
              mods={mods}
              searchQuery={searchQuery}
              onSearchChange={setSearchQuery}
              onLaunchGame={(game) => setLaunchingGame(game)}
              onOpenGameChapters={(game) => setChaptersWindowGameId(game.id)}
              onOpenGameConfig={(game) => setConfigWindowGameId(game.id)}
              onToggleInstallGame={handleToggleInstallGame}
              onToggleFavoriteGame={handleToggleFavoriteGame}
              onToggleMod={handleToggleMod}
              onMoveModOrder={handleMoveModOrder}
              onOpenInstallModWindow={(gameId) => {
                setModFormError('');
                setInstallModTargetGameId(gameId);
              }}
              onOpenRegisterPackageWindow={() => setShowDevAccountWindow(true)}
              onOpenStorageWindow={() => setShowStorageWindow(true)}
              onOpenNetworkWindow={() => setShowNetworkWindow(true)}
              devSettings={devSettings}
              onUpdateDevSettings={handleUpdateDevSettings}
              gamepadTelemetry={gamepadTelemetry}
            />
          </main>
        ) : (
          <main className="flex flex-col justify-start gap-6">
            {/* Upper Split Grid: Hero Spotlight (Left ~74%) + Library & Storage Cards (Right ~26%) */}
            <div className="grid grid-cols-1 lg:grid-cols-12 gap-4 items-stretch">
              {/* Left Hero Banner Card */}
              <section
                aria-label="Featured game spotlight"
                className="lg:col-span-9 relative rounded-[28px] overflow-hidden border border-white/[0.11] bg-[#151419] min-h-[315px] lg:min-h-[346px] flex flex-col justify-between p-7 shadow-[0_24px_60px_rgba(0,0,0,0.8)]"
              >
                {/* Dynamic Hero Artwork Layer */}
                <div
                  key={activeGame.id}
                  className="absolute inset-0 pointer-events-none transition-opacity duration-300"
                >
                  {activeGame.id === 'gow3' ? (
                    <>
                      <img
                        src={activeGame.heroImage}
                        alt="God of War III Kratos Hero"
                        referrerPolicy="no-referrer"
                        className="absolute right-0 top-0 h-full w-[66%] object-cover object-left-top scale-[1.09] origin-top-left"
                      />
                      <div
                        className="absolute inset-0"
                        style={{
                          background:
                            'linear-gradient(90deg, #151419 0%, #151419 33%, rgba(21,20,25,0.82) 45%, rgba(21,20,25,0.0) 66%), radial-gradient(circle at 18% 25%, rgba(65,55,68,0.24) 0%, transparent 55%)',
                        }}
                      />
                    </>
                  ) : (
                    <>
                      <img
                        src={activeGame.heroImage}
                        alt={activeGame.title}
                        referrerPolicy="no-referrer"
                        className="absolute right-0 top-0 h-full w-[72%] object-cover object-center opacity-90"
                      />
                      <div
                        className="absolute inset-0"
                        style={{
                          background:
                            'linear-gradient(90deg, #141217 0%, #141217 34%, rgba(20,18,23,0.84) 54%, rgba(20,18,23,0.12) 100%)',
                        }}
                      />
                    </>
                  )}
                </div>

                {/* Top Left: Resume Action Pill */}
                <div className="relative z-10 flex items-center justify-between">
                  <button
                    type="button"
                    onClick={() => setLaunchingGame(activeGame)}
                    className="inline-flex items-center gap-2.5 px-4 py-2 rounded-full bg-[#252329]/90 hover:bg-[#302d35] border border-white/12 text-[14px] font-semibold text-white/95 shadow-sm transition-colors duration-150 cursor-pointer"
                  >
                    <Play className="w-3.5 h-3.5 fill-white text-white" />
                    <span>Resume</span>
                  </button>
                </div>

                {/* Middle Left: Game Emblem / Title Lockup + Last Played Metadata */}
                <div className="relative z-10 my-auto pt-2">
                  {activeGame.id === 'gow3' ? (
                    <div className="w-[360px] sm:w-[415px] max-w-full select-none -ml-1">
                      <img
                        src="/assets/tiramisu/logo_gow3.png"
                        alt="GOD OF WAR III"
                        referrerPolicy="no-referrer"
                        className="w-full h-auto object-contain drop-shadow-[0_8px_20px_rgba(0,0,0,0.85)]"
                        style={{
                          maskImage:
                            'radial-gradient(ellipse 95% 84% at 50% 50%, black 68%, transparent 100%)',
                          WebkitMaskImage:
                            'radial-gradient(ellipse 95% 84% at 50% 50%, black 68%, transparent 100%)',
                        }}
                      />
                    </div>
                  ) : (
                    <div className="max-w-lg">
                      <h1
                        className="text-4xl sm:text-5xl font-extrabold tracking-wider text-white uppercase drop-shadow-[0_4px_20px_rgba(0,0,0,0.8)]"
                        style={{ fontFamily: 'var(--font-serif)' }}
                      >
                        {activeGame.shortTitle}
                      </h1>
                      {activeGame.subtitle && (
                        <p className="text-sm font-bold tracking-[0.25em] text-[#cbbab5] mt-1 uppercase">
                          {activeGame.subtitle}
                        </p>
                      )}
                    </div>
                  )}

                  {/* Clean Unboxed Metadata with Subtle Dot Separator */}
                  <div className="flex items-center gap-2 text-[15px] font-medium text-[#e0dadd]/90 mt-2.5">
                    <span>Last Played</span>
                    <span aria-hidden="true">·</span>
                    <span className="tabular-nums">{activeGame.lastPlayed}</span>
                  </div>
                </div>

                {/* Bottom Row: Fixed-Geometry Play Button (NO HOVER EXPAND) + Chapters Window Button + Config Window Button + Gooey Carousel Dots */}
                <div className="relative z-10 flex items-end justify-between gap-4 pt-3">
                  <div className="relative flex items-center gap-3.5">
                    <button
                      type="button"
                      onClick={() => setLaunchingGame(activeGame)}
                      className="tiramisu-pill-accent relative flex items-center justify-center gap-3 px-11 py-3.5 rounded-full text-[17px] font-bold text-[#171316] transition-colors duration-150 hover:brightness-105 active:brightness-95 cursor-pointer whitespace-nowrap shrink-0 focus:outline-none focus-visible:ring-2 focus-visible:ring-white"
                    >
                      <Play className="w-5 h-5 fill-[#171316] text-[#171316]" />
                      <span>Play</span>
                    </button>

                    <button
                      type="button"
                      onClick={() => setChaptersWindowGameId(activeGame.id)}
                      aria-label={`Open Save States and Chapters window for ${activeGame.title}`}
                      className="w-12 h-12 rounded-full bg-[#27242c]/90 hover:bg-[#34303b] border border-white/14 flex items-center justify-center text-white/90 hover:text-white transition-colors duration-150 cursor-pointer shrink-0 focus:outline-none focus-visible:ring-2 focus-visible:ring-[#cbbab5]"
                    >
                      <List className="w-5 h-5" />
                    </button>

                    <button
                      type="button"
                      onClick={() => setConfigWindowGameId(activeGame.id)}
                      aria-label={`Open UWP Launch Configuration window for ${activeGame.title}`}
                      className="w-12 h-12 rounded-full bg-[#27242c]/90 hover:bg-[#34303b] border border-white/14 flex items-center justify-center text-white/90 hover:text-white transition-colors duration-150 cursor-pointer shrink-0 focus:outline-none focus-visible:ring-2 focus-visible:ring-[#cbbab5]"
                    >
                      <Settings className="w-5 h-5" />
                    </button>
                  </div>

                  {/* Bottom-Right Gooey Morphing Carousel Indicator Dots */}
                  <div
                    role="group"
                    aria-label="Featured spotlight slide selector"
                    className="flex items-center gap-2 gooey-filter pb-1 pr-1"
                  >
                    {[0, 1, 2].map((dotIdx) => {
                      const isActiveDot = heroSlideIndex === dotIdx;
                      return (
                        <button
                          key={dotIdx}
                          type="button"
                          aria-label={`Featured spotlight slide ${dotIdx + 1}`}
                          onClick={() => {
                            setHeroSlideIndex(dotIdx);
                            setSelectedCardIndex(dotIdx);
                          }}
                          className="relative h-2.5 flex items-center justify-center cursor-pointer focus:outline-none"
                        >
                          <span
                            className={`block rounded-full transition-all duration-500 ease-[cubic-bezier(0.22,1.35,0.36,1)] ${
                              isActiveDot
                                ? 'w-7 h-2 bg-white shadow-[0_0_10px_rgba(255,255,255,0.85)]'
                                : 'w-2 h-2 bg-white/35 hover:bg-white/60'
                            }`}
                          />
                        </button>
                      );
                    })}
                  </div>
                </div>
              </section>

              {/* Right Column: Library Stat Card + Storage Bar Card */}
              <div className="lg:col-span-3 flex flex-col justify-between gap-4">
                {/* Top Card: Library 184 Games + 7-Bar Equalizer */}
                <button
                  type="button"
                  onClick={() => setActiveTab('library')}
                  aria-label="Open Library of 184 Games"
                  className="tiramisu-glass group flex-1 rounded-[26px] p-5 border border-white/10 flex flex-col justify-between text-left transition-colors duration-200 hover:border-white/25 cursor-pointer focus:outline-none focus-visible:ring-2 focus-visible:ring-[#cbbab5]"
                >
                  <div className="flex items-center justify-between w-full">
                    <div className="flex items-center gap-2.5">
                      <Box className="w-5 h-5 text-[#d7d0d2] stroke-[1.9]" />
                      <span className="text-[17px] font-semibold text-white tracking-tight">
                        Library
                      </span>
                    </div>
                    <ChevronRight className="w-5 h-5 text-white/60 group-hover:text-white group-hover:translate-x-0.5 transition-all" />
                  </div>

                  <div className="flex items-end justify-between gap-3 pt-3">
                    <div>
                      <div className="text-[44px] font-bold leading-none tracking-tight text-white tabular-nums">
                        184
                      </div>
                      <div className="text-[14px] font-medium text-[#b5aeb0] mt-1.5">
                        Games
                      </div>
                    </div>

                    {/* 7 Vertical Pill Bars matching the concept */}
                    <div
                      aria-hidden="true"
                      className="flex items-end gap-2 h-16 pb-0.5 pr-1"
                    >
                      {LIBRARY_BAR_HEIGHTS.map((heightPct, idx) => {
                        const isHighlighted = idx >= 2 && idx <= 4;
                        return (
                          <span
                            key={idx}
                            style={{
                              height: `${heightPct}%`,
                              transformOrigin: 'bottom',
                            }}
                            className={`w-[7px] rounded-full transition-all duration-300 ${
                              isHighlighted
                                ? 'bg-white shadow-[0_0_10px_rgba(255,255,255,0.45)]'
                                : 'bg-white/25 group-hover:bg-white/35'
                            }`}
                          />
                        );
                      })}
                    </div>
                  </div>
                </button>

                {/* Bottom Card: Storage 412.6 GB / 1.0 TB + Gooey Liquid Progress Pill */}
                <button
                  type="button"
                  onClick={() => setShowStorageWindow(true)}
                  aria-label={`Open Storage Partition Manager Window: ${storageMetrics.usedGb} GB of ${storageMetrics.totalTb} TB used`}
                  className="tiramisu-glass group flex-1 rounded-[26px] p-5 border border-white/10 flex flex-col justify-between text-left transition-colors duration-200 hover:border-white/25 cursor-pointer focus:outline-none focus-visible:ring-2 focus-visible:ring-[#cbbab5]"
                >
                  <div className="flex items-center justify-between w-full">
                    <div className="flex items-center gap-2.5">
                      <Layers className="w-5 h-5 text-[#d7d0d2] stroke-[1.9]" />
                      <span className="text-[17px] font-semibold text-white tracking-tight">
                        Storage
                      </span>
                    </div>
                    <ChevronRight className="w-5 h-5 text-white/60 group-hover:text-white group-hover:translate-x-0.5 transition-all" />
                  </div>

                  <div className="pt-2">
                    <div className="text-[17px] tracking-tight tabular-nums mb-3">
                      <span className="font-bold text-white">
                        {storageMetrics.usedGb} GB
                      </span>{' '}
                      <span className="font-medium text-[#b5aeb0]">
                        / {storageMetrics.totalTb} TB
                      </span>
                    </div>

                    {/* Thick Rounded Progress Track with Gooey White Fill */}
                    <div className="w-full h-[22px] rounded-full bg-white/[0.18] overflow-hidden relative p-[2px]">
                      <div className="w-full h-full flex items-center gooey-soft-filter">
                        <div
                          style={{ width: `${storageMetrics.percentage}%` }}
                          className="h-full rounded-full bg-[#f2eff0] relative flex items-center justify-end transition-all duration-700 ease-[cubic-bezier(0.22,1.35,0.36,1)]"
                        >
                          <span className="w-4 h-4 rounded-full bg-white animate-goo-blob mr-0.5" />
                        </div>
                      </div>
                    </div>
                  </div>
                </button>
              </div>
            </div>

            {/* Bottom Section: Continue Playing Row with 5 Exact Game Cards (Clean Header matching concept) */}
            <section aria-label="Continue Playing" className="pt-1">
              <div className="flex items-center justify-between mb-3.5 px-1">
                <div className="flex items-center gap-2.5">
                  <Bookmark className="w-5 h-5 text-[#d5cecc] stroke-[2]" />
                  <h2 className="text-[20px] font-bold tracking-tight text-white">
                    Continue Playing
                  </h2>
                </div>
              </div>

              <div className="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-5 gap-4">
                {continuePlayingGames.map((game, idx) => {
                  const isSelected = idx === selectedCardIndex;
                  return (
                    <button
                      key={game.id}
                      type="button"
                      onClick={() => {
                        if (isSelected) {
                          setLaunchingGame(game);
                        } else {
                          setSelectedCardIndex(idx);
                        }
                      }}
                      aria-label={`${game.title}, last played ${game.lastPlayed}`}
                      aria-pressed={isSelected}
                      className={`group relative aspect-[16/10.2] w-full rounded-[22px] overflow-hidden bg-[#16141a] cursor-pointer focus:outline-none transition-all duration-200 ${
                        isSelected
                          ? 'ring-[2.5px] ring-white shadow-[0_0_28px_rgba(255,255,255,0.28)]'
                          : 'ring-1 ring-white/10 hover:ring-white/40'
                      }`}
                    >
                      <img
                        src={game.cardImage}
                        alt={game.title}
                        referrerPolicy="no-referrer"
                        className="w-full h-full object-cover transition-transform duration-300 group-hover:scale-[1.03]"
                      />
                    </button>
                  );
                })}
              </div>
            </section>
          </main>
        )}
      </div>

      {/* ============================================================================
       * WINDOW 1: Game Runner & UWP Swapchain Host Window
       * ========================================================================== */}
      {launchingGame && (
        <TiramisuWindow
          title={`${launchingGame.title} — UWP Exclusive Game Host`}
          subtitle={`Title ID: ${launchingGame.titleId} · ${launchingGame.storageDrive}`}
          systemPath={launchingGame.executablePath}
          onClose={() => setLaunchingGame(null)}
          maxWidthClass="max-w-3xl"
          footerHints={[
            { keyLabel: 'A', action: 'Enter Exclusive Fullscreen' },
            { keyLabel: 'X', action: 'Save States' },
            { keyLabel: 'B', action: 'Suspend to Quick Resume' },
          ]}
        >
          <div className="space-y-5">
            <div className="rounded-2xl overflow-hidden aspect-[16/9] relative border border-white/12 bg-[#100e14]">
              <img
                src={launchingGame.heroImage}
                alt={launchingGame.title}
                referrerPolicy="no-referrer"
                className="w-full h-full object-cover"
              />
              <div className="absolute inset-0 bg-gradient-to-t from-black/90 via-black/30 to-transparent flex flex-col justify-between p-5">
                <div className="flex items-center justify-between">
                  <span className="px-3 py-1 rounded-full bg-black/60 border border-white/15 font-mono text-xs text-[#cbbab5]">
                    {devSettings.consoleModel} · DirectX 12 SwapChain Active
                  </span>
                  <span className="px-3 py-1 rounded-full bg-black/60 border border-white/15 font-mono text-xs text-white tabular-nums">
                    {launchingGame.fpsTarget} FPS · {launchingGame.resolution}
                  </span>
                </div>

                <div className="flex items-end justify-between gap-4">
                  <div>
                    <div className="text-xs font-mono text-[#b5aeb0]">
                      Command Line: {launchingGame.executablePath} {launchingGame.launchArgs}
                    </div>
                    <div className="text-lg font-bold text-white mt-0.5">
                      Ready in UWP Exclusive Game Container (13.5 GB Allocated)
                    </div>
                  </div>
                </div>
              </div>
            </div>

            <div className="flex flex-wrap items-center justify-between gap-3">
              <div className="flex items-center gap-2.5">
                <button
                  type="button"
                  onClick={() => {
                    const targetId = launchingGame.id;
                    setLaunchingGame(null);
                    setChaptersWindowGameId(targetId);
                  }}
                  className="px-4 py-2 rounded-full bg-white/10 hover:bg-white/15 text-xs font-semibold text-white cursor-pointer"
                >
                  Manage Save States ({launchingGame.chapters.length})
                </button>
                <button
                  type="button"
                  onClick={() => {
                    const targetId = launchingGame.id;
                    setLaunchingGame(null);
                    setConfigWindowGameId(targetId);
                  }}
                  className="px-4 py-2 rounded-full bg-white/10 hover:bg-white/15 text-xs font-semibold text-white cursor-pointer"
                >
                  Launch Arguments & Display
                </button>
              </div>

              <button
                type="button"
                onClick={() => setLaunchingGame(null)}
                className="px-6 py-2.5 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
              >
                Suspend to Dashboard
              </button>
            </div>
          </div>
        </TiramisuWindow>
      )}

      {/* ============================================================================
       * WINDOW 2: Save States, Chapters & Quick Resume Manager Window
       * ========================================================================== */}
      {chaptersWindowGame && (
        <TiramisuWindow
          title={`${chaptersWindowGame.title} — Save States & Quick Resume`}
          subtitle={`Total Playtime: ${chaptersWindowGame.playTimeHours} hrs · NVMe Save Container`}
          systemPath={`E:\\DevelopmentFiles\\Saves\\${chaptersWindowGame.titleId}\\`}
          onClose={() => setChaptersWindowGameId(null)}
          maxWidthClass="max-w-3xl"
          footerHints={[
            { keyLabel: 'A', action: 'Load Selected State' },
            { keyLabel: 'X', action: 'Create Snapshot' },
            { keyLabel: 'B', action: 'Close Window' },
          ]}
        >
          <div className="space-y-5">
            {/* Create New Snapshot Bar */}
            <div className="p-4 rounded-2xl bg-white/[0.04] border border-white/10 flex flex-wrap items-center justify-between gap-3">
              <input
                type="text"
                value={newSnapshotName}
                onChange={(e) => setNewSnapshotName(e.target.value)}
                placeholder="New snapshot label (e.g. Boss Checkpoint - Phase 2)..."
                aria-label="New save state snapshot label"
                className="flex-1 min-w-[220px] px-4 py-2 rounded-full bg-black/40 border border-white/15 text-xs text-white placeholder:text-[#867e81] focus:outline-none focus:border-[#cbbab5]"
              />
              <button
                type="button"
                onClick={() => handleCreateSaveSnapshot(chaptersWindowGame.id)}
                className="px-5 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer flex items-center gap-1.5"
              >
                <Plus className="w-3.5 h-3.5" />
                <span>Create Quick Resume Snapshot</span>
              </button>
            </div>

            {/* Save Slots Table */}
            <div className="divide-y divide-white/10">
              {chaptersWindowGame.chapters.map((ch) => (
                <div
                  key={ch.id}
                  className="py-3.5 flex items-center justify-between gap-4"
                >
                  <div>
                    <div className="flex items-center gap-2.5">
                      <span className="text-sm font-bold text-white">{ch.name}</span>
                      <span className="text-xs font-mono text-[#cbbab5]">
                        · {ch.slot}
                      </span>
                    </div>
                    <div className="text-xs text-[#9e9699] mt-0.5 tabular-nums">
                      Saved: {ch.timestamp} · Chapter Completion: {ch.progress}%
                    </div>
                  </div>

                  <button
                    type="button"
                    onClick={() => {
                      const target = chaptersWindowGame;
                      setChaptersWindowGameId(null);
                      setLaunchingGame(target);
                    }}
                    className="px-5 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer shrink-0"
                  >
                    Load State
                  </button>
                </div>
              ))}
            </div>
          </div>
        </TiramisuWindow>
      )}

      {/* ============================================================================
       * WINDOW 3: Per-Game UWP Launch & Display Configuration Window
       * ========================================================================== */}
      {configWindowGame && (
        <TiramisuWindow
          title={`${configWindowGame.title} — Game Launch Configuration`}
          subtitle={`Package Status: ${configWindowGame.uwpPackageStatus} · Title ID ${configWindowGame.titleId}`}
          systemPath={configWindowGame.executablePath}
          onClose={() => setConfigWindowGameId(null)}
          maxWidthClass="max-w-3xl"
          footerHints={[
            { keyLabel: 'A', action: 'Save & Apply' },
            { keyLabel: 'Y', action: 'Test Launch' },
            { keyLabel: 'B', action: 'Close Window' },
          ]}
        >
          <div className="divide-y divide-white/10 text-sm">
            {/* Executable & Launch Args */}
            <div className="pb-4 space-y-2">
              <label className="block text-xs font-semibold text-[#cbbab5]">
                Executable Command-Line Launch Arguments
              </label>
              <input
                type="text"
                value={configWindowGame.launchArgs}
                onChange={(e) =>
                  handleUpdateGame(configWindowGame.id, { launchArgs: e.target.value })
                }
                aria-label="Executable command line arguments"
                className="w-full px-4 py-2.5 rounded-xl bg-black/45 border border-white/15 font-mono text-xs text-white focus:outline-none focus:border-[#cbbab5]"
              />
            </div>

            {/* Internal Resolution */}
            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="font-semibold text-white">Internal DirectX 12 Backbuffer Resolution</div>
                <div className="text-xs text-[#9e9699]">
                  Overrides UWP CoreWindow scale factor on {devSettings.consoleModel}
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {(['2160p 4K', '1440p QHD', '1080p FHD'] as const).map((res) => (
                  <button
                    key={res}
                    type="button"
                    onClick={() =>
                      handleUpdateGame(configWindowGame.id, { resolution: res })
                    }
                    className={`px-3.5 py-1.5 rounded-full text-xs font-bold cursor-pointer tabular-nums ${
                      configWindowGame.resolution === res
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {res}
                  </button>
                ))}
              </div>
            </div>

            {/* Frame Rate Target */}
            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="font-semibold text-white">Frame Rate & VRR SwapInterval Target</div>
                <div className="text-xs text-[#9e9699]">
                  Unlocks 120Hz presentation when display supports HDMI 2.1 VRR
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {([120, 60] as const).map((fps) => (
                  <button
                    key={fps}
                    type="button"
                    onClick={() =>
                      handleUpdateGame(configWindowGame.id, { fpsTarget: fps })
                    }
                    className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer tabular-nums ${
                      configWindowGame.fpsTarget === fps
                        ? 'tiramisu-pill-accent'
                        : 'text-[#d5cecc] hover:text-white'
                    }`}
                  >
                    {fps} FPS
                  </button>
                ))}
              </div>
            </div>

            {/* Drive Location */}
            <div className="py-4 flex items-center justify-between gap-4">
              <div>
                <div className="font-semibold text-white">Mounted Storage Volume</div>
                <div className="text-xs text-[#9e9699]">
                  Relocate package between Internal Dev Mode NVMe (E:\) and USB 3.2 Media Drive (D:\)
                </div>
              </div>
              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {(['Internal NVMe (E:\\)', 'External USB 3.2 (D:\\)'] as const).map(
                  (drive) => (
                    <button
                      key={drive}
                      type="button"
                      onClick={() =>
                        handleUpdateGame(configWindowGame.id, { storageDrive: drive })
                      }
                      className={`px-3.5 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                        configWindowGame.storageDrive === drive
                          ? 'tiramisu-pill-accent'
                          : 'text-[#d5cecc] hover:text-white'
                      }`}
                    >
                      {drive}
                    </button>
                  )
                )}
              </div>
            </div>

            <div className="pt-4 flex items-center justify-end gap-3">
              <button
                type="button"
                onClick={() => {
                  const target = configWindowGame;
                  setConfigWindowGameId(null);
                  setLaunchingGame(target);
                }}
                className="px-6 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
              >
                Save & Launch Game
              </button>
            </div>
          </div>
        </TiramisuWindow>
      )}

      {/* ============================================================================
       * WINDOW 4: Storage & Partition Manager Window
       * ========================================================================== */}
      {showStorageWindow && (
        <TiramisuWindow
          title="Xbox Dev Mode Storage & Partition Manager"
          subtitle={`${storageMetrics.usedGb} GB Used · ${storageMetrics.freeGb} GB Free of ${storageMetrics.totalTb} TB Total`}
          systemPath="E:\DevelopmentFiles & D:\Tiramisu"
          onClose={() => setShowStorageWindow(false)}
          maxWidthClass="max-w-4xl"
          footerHints={[
            { keyLabel: 'A', action: 'Toggle Mount / Install' },
            { keyLabel: 'X', action: 'Switch Drive (E:\\ ↔ D:\\)' },
            { keyLabel: 'B', action: 'Close Window' },
          ]}
        >
          <div className="space-y-5">
            {/* Volume Summary Strip */}
            <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
              <div className="p-4 rounded-2xl bg-white/[0.04] border border-white/10">
                <div className="flex items-center justify-between text-xs font-semibold text-[#cbbab5]">
                  <span>INTERNAL DEV MODE NVME (E:\DevelopmentFiles)</span>
                  <HardDrive className="w-4 h-4" />
                </div>
                <div className="text-lg font-bold text-white mt-1 tabular-nums">
                  {games
                    .filter(
                      (g) => g.installed && g.storageDrive === 'Internal NVMe (E:\\)'
                    )
                    .reduce((acc, g) => acc + g.sizeGb, 0)
                    .toFixed(1)}{' '}
                  GB Allocated
                </div>
                <p className="text-xs text-[#9e9699] mt-0.5">
                  PCIe 4.0 Xbox Velocity Architecture · DirectStorage 1.2 Queue
                </p>
              </div>

              <div className="p-4 rounded-2xl bg-white/[0.04] border border-white/10">
                <div className="flex items-center justify-between text-xs font-semibold text-[#cbbab5]">
                  <span>EXTERNAL USB 3.2 NTFS MEDIA (D:\Tiramisu\Games)</span>
                  <FolderOpen className="w-4 h-4" />
                </div>
                <div className="text-lg font-bold text-white mt-1 tabular-nums">
                  {games
                    .filter(
                      (g) => g.installed && g.storageDrive === 'External USB 3.2 (D:\\)'
                    )
                    .reduce((acc, g) => acc + g.sizeGb, 0)
                    .toFixed(1)}{' '}
                  GB Allocated
                </div>
                <p className="text-xs text-[#9e9699] mt-0.5">
                  broadFileSystemAccess Enabled · Hot-Swappable Media Volume
                </p>
              </div>
            </div>

            {/* Game Packages Table */}
            <div className="divide-y divide-white/10">
              {games.map((g) => (
                <div
                  key={g.id}
                  className="py-3 flex flex-wrap items-center justify-between gap-4"
                >
                  <div className="flex items-center gap-3 min-w-0">
                    <img
                      src={g.cardImage}
                      alt={g.title}
                      referrerPolicy="no-referrer"
                      className="w-14 h-9 rounded-lg object-cover shrink-0 border border-white/10"
                    />
                    <div className="min-w-0">
                      <div className="text-sm font-bold text-white truncate">
                        {g.title}
                      </div>
                      <div className="text-xs font-mono text-[#9e9699] truncate tabular-nums">
                        {g.storageDrive} · {g.sizeGb} GB · Title ID {g.titleId}
                      </div>
                    </div>
                  </div>

                  <div className="flex items-center gap-2 shrink-0">
                    <button
                      type="button"
                      onClick={() =>
                        handleUpdateGame(g.id, {
                          storageDrive:
                            g.storageDrive === 'Internal NVMe (E:\\)'
                              ? 'External USB 3.2 (D:\\)'
                              : 'Internal NVMe (E:\\)',
                        })
                      }
                      className="px-3.5 py-1.5 rounded-full bg-white/[0.06] hover:bg-white/12 text-xs font-medium text-[#d5cecc] cursor-pointer"
                    >
                      Move to {g.storageDrive === 'Internal NVMe (E:\\)' ? 'D:\\' : 'E:\\'}
                    </button>
                    <button
                      type="button"
                      onClick={() => handleToggleInstallGame(g.id)}
                      className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer min-w-[96px] ${
                        g.installed
                          ? 'tiramisu-pill-accent'
                          : 'bg-white/10 text-[#b5aeb0] hover:text-white'
                      }`}
                    >
                      {g.installed ? 'Mounted' : 'Mount Package'}
                    </button>
                  </div>
                </div>
              ))}
            </div>
          </div>
        </TiramisuWindow>
      )}

      {/* ============================================================================
       * WINDOW 5: Xbox Device Portal & Network SMB Share Window
       * ========================================================================== */}
      {showNetworkWindow && (
        <TiramisuWindow
          title="Xbox Device Portal (WDP) & Remote File Shares"
          subtitle="Manage LAN deployment endpoints for Xbox Series X|S Dev Mode"
          systemPath="https://192.168.1.84:11443"
          onClose={() => setShowNetworkWindow(false)}
          maxWidthClass="max-w-2xl"
        >
          <div className="divide-y divide-white/10 text-sm">
            <div className="py-3.5 flex items-center justify-between">
              <div>
                <div className="font-semibold text-white">Xbox Wireless LAN Interface</div>
                <div className="text-xs font-mono text-[#9e9699]">
                  IPv4: 192.168.1.84 · Gateway: 192.168.1.1 · Wi-Fi 6E (5 GHz)
                </div>
              </div>
              <span className="font-mono text-xs text-[#cbbab5]">Connected</span>
            </div>

            <div className="py-3.5 flex items-center justify-between">
              <div>
                <div className="font-semibold text-white">Windows Device Portal (HTTPS :11443)</div>
                <div className="text-xs text-[#9e9699]">
                  Remote AppX sideloading and real-time Xbox performance graph
                </div>
              </div>
              <button
                type="button"
                onClick={() =>
                  handleUpdateDevSettings({
                    devPortalHttps: !devSettings.devPortalHttps,
                  })
                }
                className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                  devSettings.devPortalHttps
                    ? 'tiramisu-pill-accent'
                    : 'bg-white/10 text-white'
                }`}
              >
                {devSettings.devPortalHttps ? 'Enabled' : 'Disabled'}
              </button>
            </div>

            <div className="py-3.5 flex items-center justify-between">
              <div>
                <div className="font-semibold text-white">
                  SMB Network Folder (\\192.168.1.84\DevelopmentFiles)
                </div>
                <div className="text-xs text-[#9e9699]">
                  Direct read/write access from Windows File Explorer over LAN
                </div>
              </div>
              <button
                type="button"
                onClick={() =>
                  handleUpdateDevSettings({
                    smbShareEnabled: !devSettings.smbShareEnabled,
                  })
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
                <div className="font-semibold text-white">
                  FTP Server Daemon (ftp://192.168.1.84:2121)
                </div>
                <div className="text-xs text-[#9e9699]">
                  High-speed bulk transfer for D:\Tiramisu\Games & Mods
                </div>
              </div>
              <button
                type="button"
                onClick={() =>
                  handleUpdateDevSettings({
                    ftpServerEnabled: !devSettings.ftpServerEnabled,
                  })
                }
                className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                  devSettings.ftpServerEnabled
                    ? 'tiramisu-pill-accent'
                    : 'bg-white/10 text-white'
                }`}
              >
                {devSettings.ftpServerEnabled ? 'Active' : 'Stopped'}
              </button>
            </div>
          </div>
        </TiramisuWindow>
      )}

      {/* ============================================================================
       * WINDOW 6: Xbox Dev Mode Account, Hardware Profile & UWP Shell Bridge Window
       * ========================================================================== */}
      {showDevAccountWindow && (
        <TiramisuWindow
          title="Tiramisu Xbox Dev Mode Sandbox & Package Manager"
          subtitle={`Active Sandbox: ${devSettings.sandboxId} · Hardware: ${devSettings.consoleModel}`}
          systemPath="ms-appx:///Tiramisu.UWP/"
          onClose={() => setShowDevAccountWindow(false)}
          maxWidthClass="max-w-3xl"
        >
          <div className="space-y-5 text-sm">
            <div className="p-4 rounded-2xl bg-white/[0.04] border border-white/10 flex flex-wrap items-center justify-between gap-4">
              <div className="flex items-center gap-3.5">
                <img
                  src="/assets/tiramisu/avatar_bear.png"
                  alt="Tiramisu Developer Profile"
                  referrerPolicy="no-referrer"
                  className="w-12 h-12 rounded-full border border-white/20"
                />
                <div>
                  <div className="font-bold text-white text-base">
                    Tiramisu DevKit Operator
                  </div>
                  <div className="text-xs font-mono text-[#cbbab5]">
                    Partner Center Dev Mode Activated · Exclusive Game Resources (13.5 GB)
                  </div>
                </div>
              </div>

              <div className="flex items-center gap-1.5 bg-white/[0.05] p-1 rounded-full border border-white/10">
                {(['Xbox Series X', 'Xbox Series S'] as const).map((model) => (
                  <button
                    key={model}
                    type="button"
                    onClick={() => handleUpdateDevSettings({ consoleModel: model })}
                    className={`px-3.5 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
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

            <div className="p-4 rounded-2xl bg-[#100e14] border border-white/10 space-y-2">
              <div className="text-xs font-bold text-[#cbbab5] uppercase tracking-wider">
                Xbox Series X|S UWP WebView2 + DirectX 12 SwapChain Architecture
              </div>
              <p className="text-xs text-[#b5aeb0] leading-relaxed">
                This frontend runs natively inside an Xbox Series S|X UWP <strong>WebView2</strong> shell (`ms-appx-web:///dist/index.html`) declared with `&lt;uap10:ExpandedResources /&gt;` and `&lt;rescap:Capability Name="broadFileSystemAccess" /&gt;` in `Package.appxmanifest`. W3C Gamepad API polls Xbox Wireless Controllers directly at 120Hz, and launching a title passes the `D:\Tiramisu\Games\...` path via `chrome.webview.postMessage` to the C++/WinRT DirectX 12 SwapChain host.
              </p>
            </div>
          </div>
        </TiramisuWindow>
      )}

      {/* ============================================================================
       * WINDOW 7: Install New Mod Package (.pak / .asi / .dll) Window
       * ========================================================================== */}
      {installModTargetGameId && (
        <TiramisuWindow
          title="Mount Virtual Filesystem Mod Package"
          subtitle={`Target Title: ${
            games.find((g) => g.id === installModTargetGameId)?.title || ''
          }`}
          systemPath={`D:\\Tiramisu\\Mods\\${installModTargetGameId}\\`}
          onClose={() => setInstallModTargetGameId(null)}
          maxWidthClass="max-w-xl"
        >
          <form onSubmit={handleAddModSubmit} className="space-y-4 text-sm">
            {modFormError && (
              <div className="p-3 rounded-xl bg-[#b92b27]/20 border border-[#b92b27] text-xs text-white">
                {modFormError}
              </div>
            )}

            <div>
              <label className="block text-xs font-semibold text-[#cbbab5] mb-1.5">
                Mod Display Name
              </label>
              <input
                type="text"
                value={newModName}
                onChange={(e) => setNewModName(e.target.value)}
                placeholder="e.g. 120Hz VRR Dynamic Resolution Unlocker"
                className="w-full px-4 py-2.5 rounded-xl bg-black/45 border border-white/15 text-xs text-white focus:outline-none focus:border-[#cbbab5]"
              />
            </div>

            <div>
              <label className="block text-xs font-semibold text-[#cbbab5] mb-1.5">
                Package Filename in D:\Tiramisu\Mods\{installModTargetGameId}\
              </label>
              <input
                type="text"
                value={newModFileName}
                onChange={(e) => setNewModFileName(e.target.value)}
                placeholder="e.g. vrr_unlock_120hz.asi"
                className="w-full px-4 py-2.5 rounded-xl bg-black/45 border border-white/15 font-mono text-xs text-white focus:outline-none focus:border-[#cbbab5]"
              />
            </div>

            <div>
              <span className="block text-xs font-semibold text-[#cbbab5] mb-1.5">
                Injection Category
              </span>
              <div className="flex flex-wrap gap-2">
                {(['Framerate', 'Graphics', 'Gameplay', 'Controller'] as const).map(
                  (cat) => (
                    <button
                      key={cat}
                      type="button"
                      onClick={() => setNewModCategory(cat)}
                      className={`px-4 py-1.5 rounded-full text-xs font-bold cursor-pointer ${
                        newModCategory === cat
                          ? 'tiramisu-pill-accent'
                          : 'bg-white/10 text-white'
                      }`}
                    >
                      {cat}
                    </button>
                  )
                )}
              </div>
            </div>

            <div className="pt-3 flex items-center justify-end gap-2">
              <button
                type="button"
                onClick={() => setInstallModTargetGameId(null)}
                className="px-4 py-2 rounded-full bg-white/10 text-xs font-semibold text-white cursor-pointer"
              >
                Cancel
              </button>
              <button
                type="submit"
                className="px-6 py-2 rounded-full tiramisu-pill-accent text-xs font-bold cursor-pointer"
              >
                Mount Mod to Load Order
              </button>
            </div>
          </form>
        </TiramisuWindow>
      )}
    </div>
  );
}
