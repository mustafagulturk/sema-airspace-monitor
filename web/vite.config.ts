import { defineConfig } from "vitest/config";

const backend = "http://127.0.0.1:8080";

export default defineConfig({
  server: {
    proxy: {
      "/ws": { target: backend, ws: true, changeOrigin: true },
      "/boundary.geojson": { target: backend, changeOrigin: true },
      "/healthz": { target: backend, changeOrigin: true },
    },
  },
  build: {
    outDir: "dist",
    emptyOutDir: true,
    sourcemap: true,
    chunkSizeWarningLimit: 1500,
  },
  test: {
    include: ["src/**/*.test.ts"],
    environment: "node",
  },
});
