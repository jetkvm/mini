// Each build gives the lazy route chunks new hashed file names. A tab that was
// opened before a firmware update still asks for the old names, the device
// answers 404, and Vite reports the failed import as "vite:preloadError". One
// reload loads the current index.html and its chunks. A second failure within
// RELOAD_GUARD_MS means the file is missing from the current build too, so the
// error is shown instead of reloading again.

const STORAGE_KEY = "jetkvm.preloadErrorReloadAt";
export const RELOAD_GUARD_MS = 30_000;

export function shouldReload(lastReloadAt: string | null, now: number): boolean {
  const last = Number(lastReloadAt);
  if (lastReloadAt === null || !Number.isFinite(last)) return true;
  return now - last >= RELOAD_GUARD_MS || now < last;
}

export function reloadOnPreloadError(win: Window = window, now: () => number = Date.now) {
  win.addEventListener("vite:preloadError", () => {
    try {
      const at = now();
      if (!shouldReload(win.sessionStorage.getItem(STORAGE_KEY), at)) return;
      win.sessionStorage.setItem(STORAGE_KEY, String(at));
    } catch {
      // Without session storage there is no guard against a reload loop.
      return;
    }
    win.location.reload();
  });
}
