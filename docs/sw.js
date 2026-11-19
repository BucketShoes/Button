// Service worker: lets the control page open with no internet (game sites often have no reception)
// and be installed as an app. Bluetooth doesn't go through here at all.
//
// Network first, cache as the fallback: with a connection you always get the newest page (it must match
// the buttons' firmware), and without one you get the copy from the last visit.
"use strict";
const CACHE = "capture-buttons-v1";
const FILES = ["./", "index.html", "demo.js", "manifest.webmanifest", "bc-logo-300.avif",
               "icons/icon-192.png", "icons/icon-512.png", "icons/maskable-512.png",
               "icons/apple-touch-icon.png", "icons/favicon-32.png"];

self.addEventListener("install", e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", e => {
  e.waitUntil(caches.keys()
    .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener("fetch", e => {
  const req = e.request;
  if (req.method !== "GET" || new URL(req.url).origin !== location.origin) return;
  e.respondWith(
    fetch(req, { cache: "no-cache" })  // check with the server every time; the browser's HTTP cache could be minutes stale
      .then(res => {
        if (res.ok) { const copy = res.clone(); caches.open(CACHE).then(c => c.put(req, copy)); }
        return res;
      })
      .catch(() => caches.match(req, { ignoreSearch: true }).then(hit => hit || caches.match("index.html")))
  );
});
