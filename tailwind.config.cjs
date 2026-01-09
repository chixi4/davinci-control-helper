/** @type {import('tailwindcss').Config} */
const defaultTheme = require("tailwindcss/defaultTheme");

module.exports = {
  content: ["./index.html", "./src/**/*.{js,jsx,ts,tsx}"],
  theme: {
    extend: {
      fontFamily: {
        mono: [
          ...defaultTheme.fontFamily.mono.slice(0, -1),
          '"Microsoft YaHei UI"',
          '"Microsoft YaHei"',
          '"PingFang SC"',
          '"Noto Sans SC"',
          '"Noto Sans CJK SC"',
          defaultTheme.fontFamily.mono[defaultTheme.fontFamily.mono.length - 1],
        ],
      },
    }
  },
  plugins: []
};

