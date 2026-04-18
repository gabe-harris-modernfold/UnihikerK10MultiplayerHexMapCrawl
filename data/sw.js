const CACHE = 'img-v1';

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
