/**
 * Vite plugin: ship webfonts as woff2 only.
 *
 * Every @fontsource stylesheet lists each face twice -- woff2, then woff as a
 * fallback for browsers that predate woff2 (2016). Nothing we support is in
 * that set, and the service worker (public/sw.js precacheShellAssets) fetches
 * every url() a bundled stylesheet names, so the unused .woff half was
 * downloaded on a first visit and never read. Iosevka's alone is ~1.9 MB per
 * weight. Dropping the fallback from the CSS drops it from the precache, and
 * deleting the files keeps them out of the deploy.
 */
export function woff2Only() {
  const woffFallback = /,\s*url\([^)]*?\.woff\)\s*format\(\s*["']?woff["']?\s*\)/g;
  return {
    name: 'woff2-only',
    apply: 'build',
    generateBundle(_opts, bundle) {
      if (this.environment && this.environment.name !== 'client') return;
      const dropped = new Set();
      for (const [name, item] of Object.entries(bundle)) {
        if (item.type !== 'asset' || !name.endsWith('.css')) continue;
        const css = String(item.source);
        const next = css.replace(woffFallback, '');
        if (next !== css) item.source = next;
      }
      for (const name of Object.keys(bundle)) {
        if (/\.woff$/.test(name)) { delete bundle[name]; dropped.add(name); }
      }
      // A .woff still named by some stylesheet would 404 at runtime; say so.
      for (const item of Object.values(bundle)) {
        if (item.type !== 'asset' || !String(item.fileName).endsWith('.css')) continue;
        for (const d of dropped) {
          if (String(item.source).includes(d.split('/').pop())) {
            this.error(`${item.fileName} still references dropped ${d}`);
          }
        }
      }
    },
  };
}
