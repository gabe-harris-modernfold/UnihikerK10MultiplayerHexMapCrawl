// Bump this whenever a file under /img/ changes in place (e.g. the item
// placeholder icons) — the fetch handler below is cache-first and never
// revalidates, so clients keep the old bytes until the cache name changes.
// (The tile atlas doesn't need a bump: its pages are requested ?v=<hash>.)
// v5: the per-file hex tiles gave way to the atlas, and v4's handler had
// cached /img/tiles.json cache-first, which must not outlive it.
// v7: hexGlassFields2 repainted in place (the placeholder gave way to real art).
// v8: hexMarsh0-3 repainted in place (hexMarsh4-5 are new).
// v9: hexSettlement0-7 redrawn in place (hexSettlement8-13 are new).
const CACHE = 'img-v10';   // v10: ui_glyphs.png grew glyph 21 (TRAP)

// Take over from an older worker at once instead of on the next visit: an
// old one still serving /img/tiles.json cache-first would pin the first
// atlas it ever saw.
self.addEventListener('install', () => self.skipWaiting());

// Drop stale image caches from earlier versions on activate.
self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', event => {
  if (!event.request.url.includes('/img/')) return;
  const url = new URL(event.request.url);
  // /img/tiles.json names the current tile atlas build, so it has to be
  // fresh: straight to the network (the board answers it with an ETag).
  if (url.pathname.endsWith('.json')) return;
  event.respondWith(
    caches.open(CACHE).then(cache =>
      cache.match(event.request).then(hit => {
        if (hit) return hit;
        return fetch(event.request).then(resp => {
          // Only a real 200 is worth keeping forever: the board answers an
          // image missing from its PSRAM cache with a 204.
          if (resp.status !== 200) return resp;
          cache.put(event.request, resp.clone());
          // A versioned asset (?v=<hash>: the tile atlas pages) replaces its
          // older builds instead of piling up beside them.
          if (url.searchParams.has('v')) {
            cache.keys().then(keys => keys.forEach(k => {
              const ku = new URL(k.url);
              if (ku.pathname === url.pathname && ku.search !== url.search) cache.delete(k);
            }));
          }
          return resp;
        });
      })
    )
  );
});
