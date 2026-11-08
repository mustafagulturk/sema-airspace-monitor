/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Base map style URL (MapLibre style JSON). */
  readonly VITE_MAP_STYLE?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
