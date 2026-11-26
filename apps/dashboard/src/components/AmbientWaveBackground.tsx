import React from 'react';

interface AmbientWaveBackgroundProps {
  accentGlow?: string;
  gooeyIntensity?: 'standard' | 'ultra';
}

export const AmbientWaveBackground: React.FC<AmbientWaveBackgroundProps> = ({
  accentGlow = 'rgba(225, 165, 172, 0.24)',
  gooeyIntensity = 'standard',
}) => {
  const stdDeviation = gooeyIntensity === 'ultra' ? '11' : '8';

  return (
    <>
      {/* Global SVG Filters for Gooey Metaball Liquid Animations */}
      <svg className="fixed w-0 h-0 pointer-events-none" aria-hidden="true">
        <defs>
          <filter id="tiramisu-goo" x="-30%" y="-30%" width="160%" height="160%">
            <feGaussianBlur in="SourceGraphic" stdDeviation={stdDeviation} result="blur" />
            <feColorMatrix
              in="blur"
              mode="matrix"
              values="1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  0 0 0 22 -9"
              result="goo"
            />
            <feComposite in="SourceGraphic" in2="goo" operator="atop" />
          </filter>

          <filter id="tiramisu-goo-soft" x="-25%" y="-25%" width="150%" height="150%">
            <feGaussianBlur in="SourceGraphic" stdDeviation="6" result="blur" />
            <feColorMatrix
              in="blur"
              mode="matrix"
              values="1 0 0 0 0  0 1 0 0 0  0 0 1 0 0  0 0 0 18 -7"
              result="goo"
            />
            <feComposite in="SourceGraphic" in2="goo" operator="atop" />
          </filter>

          <linearGradient id="ribbon-top-grad" x1="0%" y1="100%" x2="100%" y2="0%">
            <stop offset="0%" stopColor="#f5c6ce" stopOpacity="0.95" />
            <stop offset="45%" stopColor="#d99aa5" stopOpacity="0.55" />
            <stop offset="100%" stopColor="#cbbab5" stopOpacity="0" />
          </linearGradient>

          <linearGradient id="ribbon-bottom-grad" x1="0%" y1="50%" x2="100%" y2="50%">
            <stop offset="0%" stopColor="#cbbab5" stopOpacity="0.55" />
            <stop offset="30%" stopColor="#9e8589" stopOpacity="0.22" />
            <stop offset="68%" stopColor="#e2a6b0" stopOpacity="0.5" />
            <stop offset="88%" stopColor="#ffd8df" stopOpacity="0.95" />
            <stop offset="100%" stopColor="#f3b8c2" stopOpacity="0.4" />
          </linearGradient>

          <radialGradient id="left-horizon-bloom" cx="0%" cy="50%" r="70%">
            <stop offset="0%" stopColor="#f7bcc6" stopOpacity="0.55" />
            <stop offset="40%" stopColor="#b36e79" stopOpacity="0.22" />
            <stop offset="100%" stopColor="#09080c" stopOpacity="0" />
          </radialGradient>

          <radialGradient id="bottom-right-bloom" cx="85%" cy="100%" r="55%">
            <stop offset="0%" stopColor="#f4b5c1" stopOpacity="0.48" />
            <stop offset="45%" stopColor="#9d5c68" stopOpacity="0.16" />
            <stop offset="100%" stopColor="#09080c" stopOpacity="0" />
          </radialGradient>
        </defs>
      </svg>

      {/* Fixed Background Canvas Layer matching IMG_4524_3ca39770.png */}
      <div
        className="fixed inset-0 pointer-events-none overflow-hidden z-0 bg-[#08070a]"
        aria-hidden="true"
      >
        {/* Dynamic Game Accent Ambient Aura */}
        <div
          className="absolute -top-24 left-1/4 w-[680px] h-[420px] rounded-full blur-[130px] transition-all duration-700 animate-pulse-glow"
          style={{ background: accentGlow }}
        />

        {/* Mid-Left Pink Edge Flare right at the left edge of the Hero Card (y ~ 35%) */}
        <div
          className="absolute top-[22%] -left-[110px] w-[310px] h-[210px] rounded-full blur-[46px] animate-pulse-glow"
          style={{
            background:
              'radial-gradient(circle, rgba(246,192,202,0.52) 0%, rgba(188,112,126,0.22) 52%, rgba(9,8,12,0) 100%)',
          }}
        />

        {/* Bottom-Right Rose Horizon Glow */}
        <div
          className="absolute -bottom-[110px] right-[4%] w-[560px] h-[240px] rounded-full blur-[54px] animate-pulse-glow"
          style={{
            background:
              'radial-gradient(circle, rgba(247,188,199,0.46) 0%, rgba(176,101,115,0.2) 55%, rgba(9,8,12,0) 100%)',
          }}
        />

        {/* SVG Sweeping Light Ribbons matching the top-left and bottom waves in the concept */}
        <svg
          className="absolute inset-0 w-full h-full"
          viewBox="0 0 1440 810"
          preserveAspectRatio="none"
        >
          {/* Top-left glowing arc sweeping from mid-left edge up past the Home button */}
          <g className="animate-ribbon-top">
            <path
              d="M -40,262 C 15,260 55,68 235,-18"
              fill="none"
              stroke="url(#ribbon-top-grad)"
              strokeWidth="8"
              opacity="0.35"
              style={{ filter: 'blur(8px)' }}
            />
            <path
              d="M -40,262 C 15,260 55,68 235,-18"
              fill="none"
              stroke="url(#ribbon-top-grad)"
              strokeWidth="2.2"
            />
          </g>

          {/* Bottom sweeping wave across the floor of the dashboard */}
          <g className="animate-ribbon-bottom">
            <path
              d="M -60,722 C 180,775 490,815 840,795 C 1060,782 1240,730 1490,724"
              fill="none"
              stroke="url(#ribbon-bottom-grad)"
              strokeWidth="12"
              opacity="0.36"
              style={{ filter: 'blur(10px)' }}
            />
            <path
              d="M -60,722 C 180,775 490,815 840,795 C 1060,782 1240,730 1490,724"
              fill="none"
              stroke="url(#ribbon-bottom-grad)"
              strokeWidth="2.4"
            />
          </g>
        </svg>
      </div>
    </>
  );
};
