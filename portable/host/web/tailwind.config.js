/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        // 深色底：接近 #0b1020 的三层
        ink: {
          900: '#070a16',
          800: '#0b1020',
          700: '#111834',
          600: '#182046',
        },
        neon: {
          cyan: '#22d3ee',
          violet: '#a78bfa',
          mint: '#34d399',
          amber: '#fbbf24',
          rose: '#fb7185',
          sky: '#38bdf8',
          pink: '#f472b6',
          lime: '#a3e635',
        },
      },
      fontFamily: {
        sans: ['"Segoe UI"', '"Microsoft YaHei"', 'Inter', 'system-ui', 'sans-serif'],
        mono: ['"Cascadia Mono"', '"JetBrains Mono"', 'Consolas', 'ui-monospace', 'monospace'],
      },
      borderRadius: {
        card: '16px',
      },
      boxShadow: {
        glow: '0 0 0 1px rgba(34,211,238,0.25), 0 8px 30px -12px rgba(34,211,238,0.45)',
        card: '0 18px 40px -24px rgba(0,0,0,0.9)',
      },
      keyframes: {
        pulseDot: {
          '0%,100%': { opacity: '1', transform: 'scale(1)' },
          '50%': { opacity: '0.35', transform: 'scale(0.82)' },
        },
        sweep: {
          '0%': { transform: 'translateX(-100%)' },
          '100%': { transform: 'translateX(100%)' },
        },
        riseIn: {
          '0%': { opacity: '0', transform: 'translateY(6px)' },
          '100%': { opacity: '1', transform: 'translateY(0)' },
        },
      },
      animation: {
        pulseDot: 'pulseDot 1.6s ease-in-out infinite',
        sweep: 'sweep 2.4s linear infinite',
        riseIn: 'riseIn 180ms ease-out',
      },
      transitionDuration: {
        DEFAULT: '150ms',
      },
    },
  },
  plugins: [],
}
