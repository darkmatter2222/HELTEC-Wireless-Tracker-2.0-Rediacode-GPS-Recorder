import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  server: { port: 5173, host: true },
  base: '/tracker/',
  build: { outDir: 'dist', sourcemap: false },
});
