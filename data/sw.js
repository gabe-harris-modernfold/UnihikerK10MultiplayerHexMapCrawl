// Bump this whenever a file under /img/ changes in place (e.g. the item
// placeholder icons) — the fetch handler below is cache-first and never
// revalidates, so clients keep the old bytes until the cache name changes.
const CACHE = 'img-v4';

// Drop stale image caches from earlier versions on activate.
self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k)))
    )
  );
});

self.addEventListener('fetch', event => {
  if (!event.request.url.includes('/img/')) return;
  event.respondWith(
    caches.open(CACHE).then(cache =>
      cache.match(event.request).then(hit => {
        if (hit) return hit;
        return fetch(event.request).then(resp => {
          cache.put(event.request, resp.clone());
          return resp;
        });
      })
    )
  );
});
