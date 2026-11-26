export type NavTabId = 'home' | 'library' | 'mods' | 'tools' | 'settings';
export type ConsoleProfile = 'Xbox Series X' | 'Xbox Series S';
export type StorageLocation = 'Internal NVMe (E:\\)' | 'External USB 3.2 (D:\\)';

export interface GameItem {
  id: string;
  title: string;
  shortTitle: string;
  subtitle?: string;
  lastPlayed: string;
  playTimeHours: number;
  sizeGb: number;
  cardImage: string;
  heroImage: string;
  logoImage?: string;
  accentColor: string;
  glowColor: string;
  executablePath: string;
  titleId: string;
  storageDrive: StorageLocation;
  resolution: '2160p 4K' | '1440p QHD' | '1080p FHD';
  fpsTarget: 60 | 120;
  hdrEnabled: boolean;
  vrrEnabled: boolean;
  uwpPackageStatus: 'Registered (AppX)' | 'Loose Folder (D:\\)';
  installed: boolean;
  favorite: boolean;
  launchArgs: string;
  activeModsCount: number;
  chapters: { id: string; name: string; timestamp: string; slot: string; progress: number }[];
}

export interface ModItem {
  id: string;
  gameId: string;
  gameName: string;
  name: string;
  fileName: string;
  installPath: string;
  author: string;
  version: string;
  category: 'Framerate' | 'Graphics' | 'Gameplay' | 'Controller';
  enabled: boolean;
  loadOrder: number;
  sizeMb: number;
}

export interface DevModeSettings {
  consoleModel: ConsoleProfile;
  sandboxId: string;
  devPortalHttps: boolean;
  devPortalPort: number;
  smbShareEnabled: boolean;
  ftpServerEnabled: boolean;
  ftpPort: number;
  defaultGamesDrive: StorageLocation;
  autoMountUsb: boolean;
  outputResolution: '2160p 4K' | '1440p QHD' | '1080p FHD';
  refreshRateHz: 60 | 120;
  allowVariableRefreshRate: boolean;
  autoHdr: boolean;
  controllerDeadzonePct: number;
  controllerVibration: boolean;
  gooeyFilterMode: 'standard' | 'ultra';
}

export const INITIAL_DEV_SETTINGS: DevModeSettings = {
  consoleModel: 'Xbox Series X',
  sandboxId: 'RETAIL-DEV.1',
  devPortalHttps: true,
  devPortalPort: 11443,
  smbShareEnabled: true,
  ftpServerEnabled: true,
  ftpPort: 2121,
  defaultGamesDrive: 'External USB 3.2 (D:\\)',
  autoMountUsb: true,
  outputResolution: '2160p 4K',
  refreshRateHz: 120,
  allowVariableRefreshRate: true,
  autoHdr: true,
  controllerDeadzonePct: 8,
  controllerVibration: true,
  gooeyFilterMode: 'standard',
};

export const INITIAL_GAMES: GameItem[] = [
  {
    id: 'gow3',
    title: 'God of War III',
    shortTitle: 'GOD OF WAR',
    subtitle: 'III',
    lastPlayed: '2h 17m ago',
    playTimeHours: 34.5,
    sizeGb: 42.8,
    cardImage: '/assets/covers/card_gow3_hd.png',
    heroImage: '/assets/covers/kratos_right_hd.png',
    logoImage: '/assets/tiramisu/logo_gow3.png',
    accentColor: '#b92b27',
    glowColor: 'rgba(203, 186, 181, 0.35)',
    executablePath: 'D:\\Tiramisu\\Games\\GodOfWar3\\GOW3_x64.exe',
    titleId: '474F5733',
    storageDrive: 'Internal NVMe (E:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Registered (AppX)',
    installed: true,
    favorite: true,
    launchArgs: '-dx12 -noborder -fpscap 120',
    activeModsCount: 2,
    chapters: [
      { id: 'c1', name: 'Mount Olympus — Gaia Ascent', timestamp: 'Nov 22 · 5:24 PM', slot: 'Slot 01 (Quick Resume)', progress: 100 },
      { id: 'c2', name: 'Realm of Hades — Palace of Hades', timestamp: 'Nov 21 · 11:10 PM', slot: 'Slot 02 (AutoSave)', progress: 100 },
      { id: 'c3', name: 'The Labyrinth — Daedalus Chamber', timestamp: 'Nov 22 · 5:18 PM', slot: 'Slot 03 (Manual)', progress: 78 },
    ],
  },
  {
    id: 'sekiro',
    title: 'Sekiro: Shadows Die Twice',
    shortTitle: 'SEKIRO',
    subtitle: 'SHADOWS DIE TWICE',
    lastPlayed: 'Yesterday',
    playTimeHours: 68.2,
    sizeGb: 25.4,
    cardImage: '/assets/covers/card_sekiro_hd.png',
    heroImage: '/assets/covers/sekiro_hero.jpg',
    accentColor: '#d97736',
    glowColor: 'rgba(217, 119, 54, 0.32)',
    executablePath: 'D:\\Tiramisu\\Games\\Sekiro\\sekiro.exe',
    titleId: '53454B49',
    storageDrive: 'Internal NVMe (E:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Registered (AppX)',
    installed: true,
    favorite: true,
    launchArgs: '-dx12 -unlockfps -rawinput',
    activeModsCount: 2,
    chapters: [
      { id: 's1', name: 'Ashina Castle — Upper Tower Dojo', timestamp: 'Nov 21 · 9:14 PM', slot: 'Slot 01 (Quick Resume)', progress: 100 },
      { id: 's2', name: 'Sunken Valley — Gun Fort Shrine', timestamp: 'Nov 19 · 8:42 PM', slot: 'Slot 02 (Manual)', progress: 82 },
      { id: 's3', name: 'Fountainhead Palace — Vermilion Bridge', timestamp: 'Nov 21 · 8:55 PM', slot: 'Slot 03 (AutoSave)', progress: 64 },
    ],
  },
  {
    id: 'zelda-totk',
    title: 'The Legend of Zelda: Tears of the Kingdom',
    shortTitle: 'ZELDA',
    subtitle: 'TEARS OF THE KINGDOM',
    lastPlayed: '3d ago',
    playTimeHours: 112.0,
    sizeGb: 18.6,
    cardImage: '/assets/covers/card_zelda_hd.png',
    heroImage: '/assets/covers/card_zelda_hd.png',
    accentColor: '#3ec6a8',
    glowColor: 'rgba(62, 198, 168, 0.32)',
    executablePath: 'E:\\DevelopmentFiles\\WindowsApps\\TotkNative_x64\\totk_uwp.exe',
    titleId: '5A454C44',
    storageDrive: 'Internal NVMe (E:\\)',
    resolution: '2160p 4K',
    fpsTarget: 60,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Registered (AppX)',
    installed: true,
    favorite: true,
    launchArgs: '-d3d12 -shadercache D:\\Cache\\totk.bin',
    activeModsCount: 2,
    chapters: [
      { id: 'z1', name: 'Great Sky Island — Temple of Time', timestamp: 'Nov 19 · 6:20 PM', slot: 'Slot 01 (Quick Resume)', progress: 100 },
      { id: 'z2', name: 'Lookout Landing — Purah Pad Lab', timestamp: 'Nov 18 · 4:15 PM', slot: 'Slot 02 (AutoSave)', progress: 88 },
    ],
  },
  {
    id: 'rdr2',
    title: 'Red Dead Redemption II',
    shortTitle: 'RED DEAD REDEMPTION',
    subtitle: 'II',
    lastPlayed: '5d ago',
    playTimeHours: 146.8,
    sizeGb: 119.4,
    cardImage: '/assets/covers/card_rdr2_hd.png',
    heroImage: '/assets/covers/rdr2_hero.jpg',
    accentColor: '#dc2626',
    glowColor: 'rgba(220, 38, 38, 0.32)',
    executablePath: 'D:\\Tiramisu\\Games\\RedDeadRedemption2\\RDR2.exe',
    titleId: '52445232',
    storageDrive: 'External USB 3.2 (D:\\)',
    resolution: '2160p 4K',
    fpsTarget: 60,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Loose Folder (D:\\)',
    installed: true,
    favorite: true,
    launchArgs: '-dx12 -cpuLoadRebalancing -ignorepipelinecache',
    activeModsCount: 1,
    chapters: [
      { id: 'r1', name: 'Chapter 2 — Horseshoe Overlook', timestamp: 'Nov 17 · 10:05 PM', slot: 'Slot 01 (Quick Resume)', progress: 100 },
      { id: 'r2', name: 'Chapter 4 — Saint Denis Manor', timestamp: 'Nov 16 · 7:30 PM', slot: 'Slot 02 (Manual)', progress: 54 },
    ],
  },
  {
    id: 'cyberpunk2077',
    title: 'Cyberpunk 2077',
    shortTitle: 'CYBERPUNK',
    subtitle: '2077',
    lastPlayed: '1w ago',
    playTimeHours: 89.4,
    sizeGb: 94.2,
    cardImage: '/assets/covers/card_cyberpunk_hd.png',
    heroImage: '/assets/covers/cyberpunk_hero.jpg',
    accentColor: '#facc15',
    glowColor: 'rgba(250, 204, 21, 0.32)',
    executablePath: 'D:\\Tiramisu\\Games\\Cyberpunk2077\\bin\\x64\\Cyberpunk2077.exe',
    titleId: '43503737',
    storageDrive: 'External USB 3.2 (D:\\)',
    resolution: '1440p QHD',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Loose Folder (D:\\)',
    installed: true,
    favorite: true,
    launchArgs: '-skipStartScreen -fsr3 -directstorage',
    activeModsCount: 2,
    chapters: [
      { id: 'cp1', name: 'Act II — Afterlife Lounge', timestamp: 'Nov 15 · 9:00 PM', slot: 'Slot 01 (Quick Resume)', progress: 100 },
      { id: 'cp2', name: 'Phantom Liberty — Dogtown Stadium', timestamp: 'Nov 14 · 11:45 PM', slot: 'Slot 02 (AutoSave)', progress: 72 },
    ],
  },
  {
    id: 'eldenring',
    title: 'Elden Ring',
    shortTitle: 'ELDEN RING',
    subtitle: 'SHADOW OF THE ERDTREE',
    lastPlayed: '1w ago',
    playTimeHours: 164.0,
    sizeGb: 61.5,
    cardImage: '/assets/covers/eldenring_header.jpg',
    heroImage: '/assets/covers/eldenring_hero.jpg',
    accentColor: '#d4af37',
    glowColor: 'rgba(212, 175, 55, 0.3)',
    executablePath: 'D:\\Tiramisu\\Games\\EldenRing\\Game\\eldenring.exe',
    titleId: '454C444E',
    storageDrive: 'External USB 3.2 (D:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Registered (AppX)',
    installed: true,
    favorite: true,
    launchArgs: '-eac-nop-loaded -dx12 -unlockfps',
    activeModsCount: 1,
    chapters: [
      { id: 'er1', name: 'Leyndell, Royal Capital — Elden Throne', timestamp: 'Nov 13 · 10:12 PM', slot: 'Slot 01 (Quick Resume)', progress: 92 },
    ],
  },
  {
    id: 'ghost',
    title: "Ghost of Tsushima Director's Cut",
    shortTitle: 'GHOST OF TSUSHIMA',
    subtitle: "DIRECTOR'S CUT",
    lastPlayed: '2w ago',
    playTimeHours: 52.3,
    sizeGb: 50.7,
    cardImage: '/assets/covers/ghost_header.jpg',
    heroImage: '/assets/covers/ghost_hero.jpg',
    accentColor: '#e11d48',
    glowColor: 'rgba(225, 29, 72, 0.28)',
    executablePath: 'D:\\Tiramisu\\Games\\GhostOfTsushima\\GhostOfTsushima.exe',
    titleId: '47485354',
    storageDrive: 'External USB 3.2 (D:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Loose Folder (D:\\)',
    installed: true,
    favorite: false,
    launchArgs: '-dx12 -fsr3 -hdr',
    activeModsCount: 1,
    chapters: [
      { id: 'gh1', name: 'Act II — Toyotama Castle Siege', timestamp: 'Nov 10 · 8:00 PM', slot: 'Slot 01 (AutoSave)', progress: 68 },
    ],
  },
  {
    id: 'witcher3',
    title: 'The Witcher 3: Wild Hunt',
    shortTitle: 'THE WITCHER 3',
    subtitle: 'WILD HUNT — COMPLETE EDITION',
    lastPlayed: '2w ago',
    playTimeHours: 128.5,
    sizeGb: 54.0,
    cardImage: '/assets/covers/witcher3_header.jpg',
    heroImage: '/assets/covers/witcher3_hero.jpg',
    accentColor: '#ef4444',
    glowColor: 'rgba(239, 68, 68, 0.28)',
    executablePath: 'D:\\Tiramisu\\Games\\Witcher3\\bin\\x64_dx12\\witcher3.exe',
    titleId: '57544348',
    storageDrive: 'External USB 3.2 (D:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Loose Folder (D:\\)',
    installed: false,
    favorite: false,
    launchArgs: '-dx12 -nohmd',
    activeModsCount: 0,
    chapters: [
      { id: 'w1', name: 'Kaer Morhen — Battle Preparation', timestamp: 'Nov 08 · 7:40 PM', slot: 'Slot 01 (Manual)', progress: 75 },
    ],
  },
  {
    id: 'doometernal',
    title: 'DOOM Eternal',
    shortTitle: 'DOOM ETERNAL',
    subtitle: 'ID TECH 7 ENGINE',
    lastPlayed: '3w ago',
    playTimeHours: 41.2,
    sizeGb: 88.5,
    cardImage: '/assets/covers/doometernal_header.jpg',
    heroImage: '/assets/covers/doometernal_hero.jpg',
    accentColor: '#f97316',
    glowColor: 'rgba(249, 115, 22, 0.3)',
    executablePath: 'D:\\Tiramisu\\Games\\DOOMEternal\\DOOMEternalx64vk.exe',
    titleId: '444F4F4D',
    storageDrive: 'External USB 3.2 (D:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Registered (AppX)',
    installed: false,
    favorite: false,
    launchArgs: '+com_skipIntroVideo 1 +r_swapInterval 0',
    activeModsCount: 0,
    chapters: [
      { id: 'dm1', name: 'Super Gore Nest — Master Level', timestamp: 'Nov 02 · 9:15 PM', slot: 'Slot 01 (Checkpoint)', progress: 100 },
    ],
  },
  {
    id: 'hollowknight',
    title: 'Hollow Knight',
    shortTitle: 'HOLLOW KNIGHT',
    subtitle: 'VOIDHEART EDITION',
    lastPlayed: '1m ago',
    playTimeHours: 62.0,
    sizeGb: 7.4,
    cardImage: '/assets/covers/hollowknight_header.jpg',
    heroImage: '/assets/covers/hollowknight_hero.jpg',
    accentColor: '#94a3b8',
    glowColor: 'rgba(148, 163, 184, 0.3)',
    executablePath: 'E:\\DevelopmentFiles\\WindowsApps\\HollowKnight_UWP\\hollow_knight.exe',
    titleId: '484C4C57',
    storageDrive: 'Internal NVMe (E:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: false,
    vrrEnabled: true,
    uwpPackageStatus: 'Registered (AppX)',
    installed: false,
    favorite: true,
    launchArgs: '-force-d3d12',
    activeModsCount: 0,
    chapters: [
      { id: 'hk1', name: 'City of Tears — Watcher Spire', timestamp: 'Oct 28 · 6:00 PM', slot: 'Bench Save 01', progress: 100 },
    ],
  },
  {
    id: 'hades',
    title: 'Hades',
    shortTitle: 'HADES',
    subtitle: 'UNDERWORLD ESCAPE',
    lastPlayed: '1m ago',
    playTimeHours: 77.5,
    sizeGb: 15.2,
    cardImage: '/assets/covers/hades_header.jpg',
    heroImage: '/assets/covers/hades_hero.jpg',
    accentColor: '#ef4444',
    glowColor: 'rgba(239, 68, 68, 0.3)',
    executablePath: 'E:\\DevelopmentFiles\\WindowsApps\\Hades_UWP\\x64\\Hades.exe',
    titleId: '48414453',
    storageDrive: 'Internal NVMe (E:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Registered (AppX)',
    installed: false,
    favorite: false,
    launchArgs: '/UseDX12=true /SwapInterval=0',
    activeModsCount: 0,
    chapters: [
      { id: 'hd1', name: 'House of Hades — Courtyard Chamber', timestamp: 'Oct 24 · 11:20 PM', slot: 'Run #84 Save', progress: 100 },
    ],
  },
  {
    id: 'gtav',
    title: 'Grand Theft Auto V',
    shortTitle: 'GRAND THEFT AUTO V',
    subtitle: 'LOS SANTOS',
    lastPlayed: '1m ago',
    playTimeHours: 210.4,
    sizeGb: 108.0,
    cardImage: '/assets/covers/gtav_header.jpg',
    heroImage: '/assets/covers/gtav_hero.jpg',
    accentColor: '#22c55e',
    glowColor: 'rgba(34, 197, 94, 0.28)',
    executablePath: 'D:\\Tiramisu\\Games\\GTAV\\GTA5.exe',
    titleId: '47544135',
    storageDrive: 'External USB 3.2 (D:\\)',
    resolution: '2160p 4K',
    fpsTarget: 120,
    hdrEnabled: true,
    vrrEnabled: true,
    uwpPackageStatus: 'Loose Folder (D:\\)',
    installed: false,
    favorite: false,
    launchArgs: '-DX12 -frameLimit 120 -noInGameDOF',
    activeModsCount: 0,
    chapters: [
      { id: 'gt1', name: 'Franklin & Lamar — Vinewood Hills', timestamp: 'Oct 19 · 8:45 PM', slot: 'Autosave 01', progress: 100 },
    ],
  },
];

export const INITIAL_MODS: ModItem[] = [
  {
    id: 'mod-1',
    gameId: 'gow3',
    gameName: 'God of War III',
    name: '120 FPS Unlocked VBlank Patch',
    fileName: 'gow3_120hz_vblank.asi',
    installPath: 'D:\\Tiramisu\\Mods\\gow3\\gow3_120hz_vblank.asi',
    author: 'TiramisuCore',
    version: '2.4.1',
    category: 'Framerate',
    enabled: true,
    loadOrder: 1,
    sizeMb: 14,
  },
  {
    id: 'mod-2',
    gameId: 'gow3',
    gameName: 'God of War III',
    name: 'Xbox Series X|S Button Prompts & Impulse Triggers',
    fileName: 'xbox_ui_prompts_4k.pak',
    installPath: 'D:\\Tiramisu\\Mods\\gow3\\xbox_ui_prompts_4k.pak',
    author: 'NikoUWP',
    version: '1.8.0',
    category: 'Controller',
    enabled: true,
    loadOrder: 2,
    sizeMb: 68,
  },
  {
    id: 'mod-3',
    gameId: 'sekiro',
    gameName: 'Sekiro: Shadows Die Twice',
    name: 'Unlocked 120Hz Frame Pacing & FOV Override',
    fileName: 'sekiro_fpsunlock_uwp.dll',
    installPath: 'D:\\Tiramisu\\Mods\\sekiro\\sekiro_fpsunlock_uwp.dll',
    author: 'AshinaDev',
    version: '3.1.0',
    category: 'Framerate',
    enabled: true,
    loadOrder: 1,
    sizeMb: 8,
  },
  {
    id: 'mod-4',
    gameId: 'sekiro',
    gameName: 'Sekiro: Shadows Die Twice',
    name: 'Native Xbox Controller HUD Icons',
    fileName: 'sekiro_xinput_icons.dcx',
    installPath: 'D:\\Tiramisu\\Mods\\sekiro\\sekiro_xinput_icons.dcx',
    author: 'AshinaDev',
    version: '1.2.0',
    category: 'Controller',
    enabled: true,
    loadOrder: 2,
    sizeMb: 24,
  },
  {
    id: 'mod-5',
    gameId: 'zelda-totk',
    gameName: 'The Legend of Zelda: Tears of the Kingdom',
    name: '60/120 FPS Dynamic Time-Step Hook',
    fileName: 'totk_dynamic_fps_v4.pchtxt',
    installPath: 'D:\\Tiramisu\\Mods\\totk\\totk_dynamic_fps_v4.pchtxt',
    author: 'ChuckUWP',
    version: '4.0.2',
    category: 'Framerate',
    enabled: true,
    loadOrder: 1,
    sizeMb: 6,
  },
  {
    id: 'mod-6',
    gameId: 'zelda-totk',
    gameName: 'The Legend of Zelda: Tears of the Kingdom',
    name: 'Xbox ABXY Layout & 4K Shadow Cascade',
    fileName: 'totk_xbox_ui_4k.romfs',
    installPath: 'D:\\Tiramisu\\Mods\\totk\\totk_xbox_ui_4k.romfs',
    author: 'HyruleDev',
    version: '2.1.0',
    category: 'Graphics',
    enabled: true,
    loadOrder: 2,
    sizeMb: 312,
  },
  {
    id: 'mod-7',
    gameId: 'rdr2',
    gameName: 'Red Dead Redemption II',
    name: 'FSR 3.1 Native RDNA2 Upscaler Injection',
    fileName: 'nvngx_fsr3_uwp.dll',
    installPath: 'D:\\Tiramisu\\Mods\\rdr2\\nvngx_fsr3_uwp.dll',
    author: 'ValentinDX',
    version: '1.4.0',
    category: 'Graphics',
    enabled: true,
    loadOrder: 1,
    sizeMb: 42,
  },
  {
    id: 'mod-8',
    gameId: 'cyberpunk2077',
    gameName: 'Cyberpunk 2077',
    name: 'Cyber Engine Tweaks UWP Sandbox Bridge',
    fileName: 'cet_uwp_bridge.asi',
    installPath: 'D:\\Tiramisu\\Mods\\cp2077\\cet_uwp_bridge.asi',
    author: 'YamashiUWP',
    version: '1.32.0',
    category: 'Gameplay',
    enabled: true,
    loadOrder: 1,
    sizeMb: 55,
  },
  {
    id: 'mod-9',
    gameId: 'eldenring',
    gameName: 'Elden Ring',
    name: '120Hz VRR & Chromatic Aberration Disable',
    fileName: 'er_uwp_120hz.dll',
    installPath: 'D:\\Tiramisu\\Mods\\eldenring\\er_uwp_120hz.dll',
    author: 'TechieW',
    version: '1.12.2',
    category: 'Framerate',
    enabled: true,
    loadOrder: 1,
    sizeMb: 11,
  },
];

export const LIBRARY_BAR_HEIGHTS = [24, 34, 48, 56, 86, 62, 68];

export function calculateStorageMetrics(games: GameItem[], baseSystemGb = 0.0, totalTb = 1.0) {
  const installedGamesGb = games
    .filter((g) => g.installed)
    .reduce((acc, item) => acc + item.sizeGb, 0);
  const usedGb = Number((installedGamesGb + baseSystemGb).toFixed(1));
  const totalGb = totalTb * 1000;
  const percentage = Math.min(100, Math.max(0, Number(((usedGb / totalGb) * 100).toFixed(1))));
  return {
    usedGb,
    totalTb: totalTb.toFixed(1),
    percentage,
    freeGb: Number((totalGb - usedGb).toFixed(1)),
  };
}

export function stepCardSelection(currentIndex: number, totalCount: number, delta: number): number {
  if (totalCount <= 0) return 0;
  return ((currentIndex + delta) % totalCount + totalCount) % totalCount;
}

export function filterGamesByQuery(
  games: GameItem[],
  query: string,
  filterMode: 'all' | 'installed' | 'internal' | 'usb' | 'favorites' = 'all'
): GameItem[] {
  const normalized = query.trim().toLowerCase();
  return games.filter((g) => {
    if (filterMode === 'installed' && !g.installed) return false;
    if (filterMode === 'internal' && g.storageDrive !== 'Internal NVMe (E:\\)') return false;
    if (filterMode === 'usb' && g.storageDrive !== 'External USB 3.2 (D:\\)') return false;
    if (filterMode === 'favorites' && !g.favorite) return false;

    if (!normalized) return true;
    return (
      g.title.toLowerCase().includes(normalized) ||
      g.executablePath.toLowerCase().includes(normalized) ||
      g.titleId.toLowerCase().includes(normalized)
    );
  });
}
