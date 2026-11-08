import { existsSync, readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import type { Plugin } from "vite";
import { defineConfig } from "vitest/config";

const backend = "http://127.0.0.1:8080";

/**
 * Writes dist/third-party-licenses.txt with the licence text of every
 * production npm package bundled into the map (MapLibre GL JS and its
 * dependencies). The minified bundle drops their licence comments, and BSD/MIT
 * licences ask for the notice to travel with redistributed copies.
 */
function thirdPartyLicenses(): Plugin {
  return {
    name: "sema-third-party-licenses",
    apply: "build",
    generateBundle() {
      const lock = JSON.parse(readFileSync("package-lock.json", "utf8")) as {
        packages: Record<string, { version?: string; license?: string; dev?: boolean }>;
      };
      const parts: string[] = [
        "Semâ web map: third-party software bundled in this site.\n" +
          "Semâ itself is MIT licensed: https://github.com/mustafagulturk/sema-airspace-monitor\n",
      ];
      const entries = Object.entries(lock.packages)
        .filter(([path, p]) => path.startsWith("node_modules/") && !p.dev)
        .sort(([a], [b]) => a.localeCompare(b));
      for (const [path, p] of entries) {
        const name = path.slice(path.lastIndexOf("node_modules/") + "node_modules/".length);
        const files = existsSync(path) ? readdirSync(path).filter((f) => /^(licen[cs]e|copying|notice)/i.test(f)) : [];
        const text = files.map((f) => readFileSync(join(path, f), "utf8").trim()).join("\n\n");
        parts.push(`${"=".repeat(78)}\n${name} ${p.version ?? ""} (${p.license ?? "see text"})\n\n${text || "(no licence file in package)"}\n`);
      }
      this.emitFile({ type: "asset", fileName: "third-party-licenses.txt", source: parts.join("\n") });
    },
  };
}

export default defineConfig({
  plugins: [thirdPartyLicenses()],
  server: {
    proxy: {
      "/ws": { target: backend, ws: true, changeOrigin: true },
      "/boundary.geojson": { target: backend, changeOrigin: true },
      "/healthz": { target: backend, changeOrigin: true },
      "/api": { target: backend, changeOrigin: true },
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
