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

/*
 * Save-game icons, by title ID: <PLATFORM>/<TITLEID>/<icon0>. Mind the case —
 * PS3 and PSP store ICON0.PNG, everything else icon0.png (see ICON_FILE in
 * tools.js).
 *
 * Fetched per card rather than built in — they are PNGs of real box art and
 * there are hundreds of them. Coverage is partial, so a card has to look
 * right without one; tools.js hides the image on error rather than reserving
 * space for a placeholder.
 *
 * @master, not @main: that repository's default branch is master, and jsDelivr
 * answers 404 for a branch that does not exist rather than falling back.
 */
export const SAVES_CDN = 'https://cdn.jsdelivr.net/gh/bucanero/apollo-saves@master';

/*
 * PS4 title metadata, mirrored from Sony's TMDB as static JSON per title ID:
 * <PSNDB>/<TITLEID>/<TITLEID>_00.json, with the icon under `icons[0].icon`.
 *
 * This is the mirror apollo-ps4 itself reads (source/exec_cmd.c). Going
 * through it rather than tmdb.np.dl.playstation.net directly buys two things a
 * browser needs: no HMAC-SHA1 of the title ID to compute, and an
 * `access-control-allow-origin: *` that Sony's endpoint does not send.
 *
 * PS4 only — there are no PS3, PSP or Vita entries in it, which is why
 * apollo-saves stays the first source for those.
 */
export const PSNDB = 'https://bucanero.github.io/psndb';

/*
 * Sony's own title metadata, for PS3 icons:
 *
 *   <TMDB>/<TITLEID>_00_<HMAC>/ICON0.PNG
 *   HMAC = HMAC-SHA1(TMDB_KEY, "<TITLEID>_00"), uppercase hex
 *
 * The scheme is the one bucanero/pkgi-ps3 uses (source/pkgi_download.c,
 * pkgi_download_icon) and the key is its tmdb_hmac_key verbatim. Unlike the
 * PS4 half of TMDB this needs no JSON hop — the digest IS the path — and the
 * host answers over https, so there is no mixed-content rewrite either.
 *
 * PS3 only. The same endpoint returns 404 for every PS4, PSP and Vita title
 * tried, which is why the other platforms go through apollo-saves and, for
 * PS4, the psndb mirror above.
 *
 * It earns nothing on the games listed today — apollo-saves already covers all
 * 20 PS3 cards — and a great deal on the ones that come next: of the 837 PS3
 * patches in the catalogue with no apollo-saves art, a sample says about 82%
 * have art here.
 */
export const TMDB = 'https://tmdb.np.dl.playstation.net/tmdb';
export const TMDB_KEY =
    'F5DE66D2680E255B2DF79E74F890EBF349262F618BCAE2A9ACCDEE5156CE8DF2' +
    'CDF2D48C71173CDC2594465B87405D197CF1AED3B7E9671EEB56CA6753C2E6B0';
