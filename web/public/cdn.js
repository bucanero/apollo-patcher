/*
 * Where the patch database is fetched from at run time, shared by the page and
 * the worker.
 *
 * jsDelivr rather than raw.githubusercontent.com: the latter answers 503 to
 * cross-origin requests from the Pages origin (measured, not assumed).
 *
 * `@main` rather than a pinned commit, so a patch or helper module fixed
 * upstream reaches users without redeploying this site. The tradeoff is that
 * the generated index only lists what existed at build time, and jsDelivr
 * serves `@main` from cache for up to 12 hours.
 */
export const CDN = 'https://cdn.jsdelivr.net/gh/bucanero/apollo-patches@main';
