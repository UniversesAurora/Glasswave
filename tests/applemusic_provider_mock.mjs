import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';

const args = process.argv.slice(2);
function arg(name) {
    const at = args.indexOf(name);
    return at >= 0 ? args[at + 1] : undefined;
}

const stateFile = arg('--state-file');
const bridgeFile = arg('--bridge-file');
const nonce = arg('--nonce');
const scenario = arg('--scenario') ?? 'baseline';
if (!stateFile || !bridgeFile || !nonce) {
    throw new Error('missing mock arguments');
}
if (!['baseline', 'mutation-reconciliation', 'catalog-regression', 'catalog-suggestions',
      'startup-cache'].includes(scenario)) {
    throw new Error('invalid scenario');
}

const token = crypto.randomBytes(32).toString('hex');
// A per-run prefix prevents list persistence or the catalog cache from warm-starting
// the records whose missing relationships this fixture must expose.
const enrichmentTag = nonce.slice(0, 16).toLowerCase();
const songCatalog = 'song.contract';
const songLibrary = 'i.song.contract';
const albumCatalog = 'album.contract';
const albumLibrary = 'l.album.contract';
const ownerPlaybackAlbumLibrary = 'l.rPurqtz';
const playlistCatalog = 'playlist.contract';
const playlistSaved = 'p.saved.contract';
const playlistFavorite = 'p.favorites.contract';
const playlistScratch = 'p.scratch.contract';
const playlistPerformance = 'p.performance.contract';
const playlistPerformanceCatalog = 'playlist.performance.catalog';
// Keep the streaming adapter regression separate from the legacy full-list performance
// fixture so their request counters cannot hide duplicate detail or page-zero reads.
const playlistPagedPerformance = 'p.glasswave-paged-performance';
const mutationAlbumCatalog = '1900000001';
const mutationAlbumLibrary = 'l.mutation.contract';
const mutationCatalogSongId = '1900002600';
const mutationPlaylistCatalog = '1900000002';
const mutationPlaylistLibrary = 'p.mutation.contract';
const favoriteArtistCatalog = '1542902428';
const favoriteArtistLibrary = 'r.1542902428';
const sourcePageArtistCatalog = '1900002500';
const sourcePageArtistLibrary = 'r.sourcepage.contract';
const staleArtistCatalog = '1900002501';
const staleArtistLibrary = 'r.staleread.contract';
const playlistEnrichment = `p.glasswave-enrichment.${enrichmentTag}`;
const catalogArtistAll = '159260351';
const catalogArtistEmpty = '1147783278';
const catalogArtistUnknown = '1900003998';
const catalogArtistUnauthorized = '1900003997';
const catalogArtistNetworkError = '1900003996';
const catalogArtistContinuationError = '1900003995';
const catalogArtistConcurrentEmpty = '1900003994';
const startupAccountA = `startup-account-${enrichmentTag}.a`;
const startupAccountB = `startup-account-${enrichmentTag}.b`;
const startupPlaylistA = `p.startup.${enrichmentTag}.a`;
const startupPlaylistB = `p.startup.${enrichmentTag}.b`;
const startupFavoriteA = `p.favorites.${enrichmentTag}.a`;
const performanceTrackCount = 359;
const performanceUniqueCount = 356;
const performancePageSize = 100;
const enrichmentPageLimit = 20;
const enrichmentKnownIndex = 0;
// Row 0 is complete and must be skipped. Row 2 is repeated to verify occurrence order.
const enrichmentIndexes = [2, 0, 2, 1, 3];
const enrichmentUniqueIndexes = [0, 1, 2, 3];
const enrichmentHydrationIndexes = [2, 1, 3];
const enrichmentHydrationDelayMs = 180;
const hydrationDelayMs = 80;
const detailDelayMs = 80;
const mutationDelayMs = 650;

function waitFor(predicate, timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
        const deadline = Date.now() + timeoutMs;
        const poll = () => {
            if (predicate()) return resolve();
            if (Date.now() >= deadline) return reject(new Error('mock wait timed out'));
            setTimeout(poll, 10);
        };
        poll();
    });
}

function regionalAlbumCatalog() {
    return `album.glasswave-regional.${enrichmentTag}`;
}
function regionalAlbumLibrary() {
    return `l.album.glasswave-regional.${enrichmentTag}`;
}
function regionalUploadedSongCatalog() {
    return `song.glasswave-regional.${enrichmentTag}.uploaded`;
}
function regionalUploadedSongLibrary() {
    return `i.song.glasswave-regional.${enrichmentTag}.uploaded`;
}
function regionalLibraryOnlySong() {
    return `i.song.glasswave-regional.${enrichmentTag}.library-only`;
}
function regionalAlbumArtistCatalog() {
    return `artist.glasswave-regional.${enrichmentTag}.album`;
}
function regionalAlbumArtistLibrary() {
    return `r.artist.glasswave-regional.${enrichmentTag}.album`;
}
function regionalLibraryOnlyArtist() {
    return `r.artist.glasswave-regional.${enrichmentTag}.library-only`;
}
function regionalMissingArtistCatalog() {
    return `artist.glasswave-regional.${enrichmentTag}.missing`;
}
function regionalRetryArtistCatalog() {
    return `artist.glasswave-ordered.${enrichmentTag}.retry`;
}
function regionalRetryArtistLibrary() {
    return `r.artist.glasswave-regional.${enrichmentTag}.retry`;
}
function regionalDetailAlbumCatalog() {
    return `album.glasswave-ordered.${enrichmentTag}`;
}
function regionalDetailUploadedSongCatalog() {
    return `song.glasswave-ordered.${enrichmentTag}.uploaded`;
}
function regionalDetailUploadedSongLibrary() {
    return `i.song.glasswave-ordered.${enrichmentTag}.uploaded`;
}
function regionalDetailLibraryOnlySong() {
    return `i.song.glasswave-ordered.${enrichmentTag}.library-only`;
}
function regionalDetailArtistCatalog() {
    return `artist.glasswave-ordered.${enrichmentTag}.album`;
}
function regionalDetailLibraryOnlyArtist() {
    return `r.artist.glasswave-ordered.${enrichmentTag}.library-only`;
}

let scratch = null;
let favorite = false;
let albumSaved = false;
let playlistIsSaved = false;
let mutationAlbumSaved = scenario === 'mutation-reconciliation';
let mutationPlaylistSaved = false;
const artistFavorites = {
    [favoriteArtistCatalog]: false,
    [sourcePageArtistCatalog]: false,
    [staleArtistCatalog]: false,
};
let failArtistRefreshAfterMutation = 0;
let failArtistRefreshAfterPlaylistSave = 0;
let accountSwitched = false;
let releaseMutationGate;
const mutationGate = new Promise((resolve) => {
    releaseMutationGate = resolve;
});
const mutationFixture = {
    active: false,
    staleReadArmed: false,
    preAlbumSnapshotSeen: false,
    preArtistSnapshotSeen: false,
};
const playlistAuthority = {
    staleArmed: false,
    staleServed: false,
    absentServed: false,
    externalPresent: false,
};
let releaseArtistPreferenceGate;
const artistPreferenceGate = new Promise((resolve) => {
    releaseArtistPreferenceGate = resolve;
});
const artistFixture = {
    originalLibraryReads: 0,
    canonicalReads: {
        [favoriteArtistCatalog]: 0,
        [sourcePageArtistCatalog]: 0,
        [staleArtistCatalog]: 0,
    },
    staleReadArmed: false,
    staleReadHeld: false,
    staleReadCompleted: false,
    writes: [],
};
const accountControl = {
    armRequested: false,
    accountRequestsAfterArm: 0,
    switchRequested: false,
    oldArtistHeld: false,
};
const mutationWrites = {
    albumRemove: 0,
    albumSave: 0,
    playlistSave: 0,
    playlistRemove: 0,
    artistFavoriteAdd: 0,
    artistFavoriteRemove: 0,
};
const mutationResponses = [];
let hydrationActive = 0;
let enrichmentHydrationActive = 0;
let detailActive = 0;
let performanceDetailMode = 'projected';
const hydrationIds = new Set();
const performance = {
    detailRequests: 0,
    projectedDetailRequests: 0,
    fallbackDetailRequests: 0,
    invalidDetailRequests: 0,
    metadataOnlyRequests: 0,
    catalogDetailRequests: 0,
    invalidCatalogDetailRequests: 0,
    maxDetailConcurrency: 0,
    hydrationsBeforeFallback: -1,
    pagesBeforeFallback: -1,
    trackPageRequests: 0,
    zeroOffsetPageRequests: 0,
    trackPageRows: 0,
    pageOffsets: [],
    invalidTrackPageRequests: 0,
    hydrationRequests: 0,
    hydrationBatchSizes: [],
    hydrationDuplicateIds: 0,
    invalidHydrationRequests: 0,
    maxHydrationConcurrency: 0,
    primaryEmbeddedRows: performancePageSize,
};
const paged = {
    primaryRequests: 0,
    pageRequests: 0,
    zeroOffsetRequests: 0,
    offsets: [],
    exactNestedIncludes: 0,
    opaqueQueryRetained: 0,
    invalidPrimaryRequests: 0,
    invalidPageRequests: 0,
    pageRows: 0, // Primary embedded rows plus every continuation-page row.
};
const enrichment = {
    tag: enrichmentTag,
    trackRequests: 0,
    invalidTrackRequests: 0,
    trackRows: 0,
    hydrationRequests: 0,
    hydrationRows: 0,
    invalidHydrationRequests: 0,
    knownIdRequests: 0,
    hydrationDuplicateIds: 0,
    maxHydrationConcurrency: 0,
    activeHydrationRequests: 0,
    hydrationDelayMs: enrichmentHydrationDelayMs,
    hydrationBatchSizes: [],
    hydrationIds: [],
};
const regional = {
    sourceLibraryAlbumRequests: 0,
    sourceLibraryAlbumTrackRequests: 0,
    sourceLibraryArtistRequests: 0,
    sourceCatalogArtistRequests: 0,
    sourceRegionalAlbumRequests: 0,
    sourceRegionalArtistRequests: 0,
    sourceRetryArtistRequests: 0,
    sourceArtistAlbumRequests: 0,
    sourceSharedArtistAlbumRequests: 0,
    sourceRetryArtistAlbumRequests: 0,
    invalidSourceArtistAlbumRequests: 0,
    japanAlbumRequests: 0,
    japanSongRequests: 0,
    japanArtistRequests: 0,
    chinaAlbumRequests: 0,
    chinaSongRequests: 0,
    chinaArtistRequests: 0,
    retryArtistRequests: 0,
    retryArtistFailures: 0,
    retryArtistSuccesses: 0,
    invalidRequests: 0,
};
const isolation = {
    favoriteTrackReads: 0,
    backgroundAlbumArtReads: 0,
    catalogPlaylistReads: 0,
    invalidCatalogPlaylistReads: 0,
    favoriteArtistAlbumCountReads: 0,
    mutationCatalogAlbumReads: 0,
    mutationLibraryAlbumReads: 0,
    mutationLibraryAlbumTrackReads: 0,
    playlistDisplayReads: 0,
    unknownPlaylistReads: 0,
    invalidFavoriteTrackReads: 0,
};
const catalog = {
    artistRoots: {},
    sectionRequests: {},
    sectionContinuations: {},
    sectionPaths: [],
    sectionOffsets: [],
    sectionLimits: [],
    concurrentEmptyCallbacks: 0,
    searchRequests: {},
    searchContinuations: {},
    searchQueries: [],
    invalidSearchRequests: 0,
    invalidSearchContinuations: 0,
    suggestionRequests: 0,
    suggestionTerms: [],
    invalidSuggestionRequests: 0,
};
const startup = {
    accountMode: scenario === 'startup-cache' ? 'seed' : 'normal',
    phase: 0,
    accountRequests: 0,
    accountCompleted: 0,
    accountInFlight: 0,
    authCompleted: false,
    drmRequests: 0,
    drmCompleted: false,
    playlistCollectionCompleted: false,
    albumCollectionCompleted: false,
    artistCollectionCompleted: false,
    playlistRequests: 0,
    playlistPageRequests: 0,
    playlistPageAfterAuthBeforeMembership: 0,
    favoritePageRequests: 0,
    favoritePageAfterAuthBeforeMembership: 0,
    displaySongRequests: 0,
    displaySongMissingCounterparts: 0,
    invalidDisplaySongRequests: 0,
    listRequests: 0,
    enrichmentRequests: 0,
};
const writes = {
    favoriteAdd: 0,
    favoriteRemove: 0,
    playlistCreate: 0,
    renameFailure: 0,
    playlistRename: 0,
    playlistAdd: 0,
    playlistRemove: 0,
    playlistDelete: 0,
    albumSave: 0,
    albumRemove: 0,
    playlistSave: 0,
    playlistRemoveFromLibrary: 0,
};
const errors = [];
const unexpectedCalls = [];

function publicState() {
    return {
        ready: true,
        writes,
        errors,
        unexpectedCalls,
        rollbackRejected: writes.renameFailure === 1,
        performance: {
            ...performance,
            uniqueHydrationIds: hydrationIds.size,
        },
        paged,
        enrichment,
        regional,
        isolation,
        catalog,
        startup,
        scenario,
        mutation: {
            ...mutationFixture,
            artistFixture,
            writes: mutationWrites,
            responses: mutationResponses,
            albumSaved: mutationAlbumSaved,
            playlistSaved: mutationPlaylistSaved,
            playlistAuthority,
            artistFavorites,
            artistFavorite: artistFavorites[favoriteArtistCatalog],
            failArtistRefreshAfterMutation,
            failArtistRefreshAfterPlaylistSave,
        },
        accountControl: {
            ...accountControl,
            switched: accountSwitched,
        },
        account: scenario === 'startup-cache'
            ? {
                accountId: startup.accountMode === 'account-b' ? startupAccountB : startupAccountA,
                name: startup.accountMode === 'signed-out' ? '' : 'Apple Startup Contract',
                storefront: startup.accountMode === 'signed-out' ? '' : 'US',
            }
            : {
                accountId: accountSwitched ? 'mock-account-b' : 'mock-account-a',
                name: 'Provider Contract Mock',
                storefront: 'US',
            },
        finalState: {
            favorite,
            albumSaved,
            playlistSaved: playlistIsSaved,
            mutationAlbumSaved,
            mutationPlaylistSaved,
            accountSwitched,
            scratchExists: scratch !== null,
            scratchTrackCount: scratch?.tracks.length ?? 0,
        },
    };
}

function writeJsonFile(path, value) {
    const temporary = `${path}.${process.pid}.tmp`;
    fs.writeFileSync(temporary, JSON.stringify(value), { encoding: 'utf8', mode: 0o600 });
    // Windows readers may briefly open a readiness/state file without delete
    // sharing. A transient sharing violation must not crash the fixture server.
    for (let attempt = 0; ; attempt++) {
        try { fs.renameSync(temporary, path); break; }
        catch (error) {
            if (!['EPERM', 'EACCES'].includes(error.code) || attempt >= 19) throw error;
            Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 10);
        }
    }
}

function saveState() {
    writeJsonFile(stateFile, publicState());
}

function mockError(message) {
    if (!errors.includes(message)) errors.push(message);
    saveState();
}

function unexpected(method, pathname) {
    unexpectedCalls.push(`${method} ${pathname}`);
    saveState();
}

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function song(library = true) {
    const item = {
        id: library ? songLibrary : songCatalog,
        type: library ? 'library-songs' : 'songs',
        attributes: {
            name: 'Provider Contract Song',
            albumName: 'Provider Contract Album',
            artistName: 'Mock Artist',
            durationInMillis: 180000,
            genreNames: ['Rock'],
            composerName: 'Mock Composer',
            playParams: { id: library ? songLibrary : songCatalog, kind: 'song', catalogId: songCatalog },
        },
        relationships: {
            albums: {
                data: [{
                    id: library ? albumLibrary : albumCatalog,
                    type: library ? 'library-albums' : 'albums',
                }],
            },
            artists: {
                data: [{
                    id: library ? 'r.artist.contract' : 'artist.contract',
                    type: 'artists',
                    attributes: { name: 'Mock Artist' },
                }],
            },
        },
    };
    return item;
}

function regionalArtistResource(region = 'source') {
    const localized = region === 'jp'
        ? { name: '日本語のバイオアーティスト', bio: '日本語のアーティスト紹介' }
        : region === 'cn'
            ? { name: '中文简介艺术家', bio: '中文艺术家简介' }
            : { name: 'Regional Source Album Artist', bio: 'Regional source artist biography' };
    return {
        id: regionalAlbumArtistCatalog(),
        type: 'artists',
        attributes: {
            name: localized.name,
            editorialNotes: localized.bio,
            albumCount: 3,
            trackCount: 7,
        },
    };
}

function regionalSourceAlbum() {
    const album = {
        id: regionalAlbumLibrary(),
        type: 'library-albums',
        attributes: {
            name: 'Regional Source Album',
            artistName: 'Regional Source Album Artist',
            editorialNotes: 'Regional source album description',
            trackCount: 2,
            releaseDate: '2021-05-04',
        },
        relationships: {
            catalog: { data: [{ id: regionalAlbumCatalog(), type: 'albums' }] },
            artists: {
                data: [{
                    id: regionalAlbumArtistLibrary(),
                    type: 'artists',
                    attributes: { name: 'Regional Source Album Artist' },
                }],
            },
        },
    };
    return album;
}

function regionalSourceArtist() {
    const catalog = regionalArtistResource();
    return {
        id: regionalAlbumArtistLibrary(),
        type: 'library-artists',
        attributes: {
            name: 'Regional Source Album Artist',
            editorialNotes: 'Regional source artist biography',
            albumCount: 3,
            trackCount: 7,
        },
        relationships: {
            catalog: {
                data: [{
                    ...catalog,
                    attributes: {
                        ...catalog.attributes,
                        editorialNotes: 'Regional source artist biography',
                    },
                }],
            },
        },
    };
}

function regionalSourceRetryArtist() {
    return {
        id: regionalRetryArtistLibrary(),
        type: 'library-artists',
        attributes: {
            name: 'Regional Source Retry Artist',
            editorialNotes: 'Regional source retry artist biography',
            albumCount: 2,
            trackCount: 5,
        },
        relationships: {
            catalog: {
                data: [{
                    id: regionalRetryArtistCatalog(),
                    type: 'artists',
                    attributes: {
                        name: 'Regional Source Retry Artist',
                        editorialNotes: 'Regional source retry artist biography',
                        albumCount: 2,
                        trackCount: 5,
                    },
                }],
            },
        },
        relationships: {
            catalog: {
                data: [{
                    id: regionalRetryArtistCatalog(),
                    type: 'artists',
                    attributes: {
                        name: 'Regional Source Retry Artist',
                        editorialNotes: 'Regional source retry artist biography',
                        albumCount: 2,
                        trackCount: 5,
                    },
                }],
            },
        },
    };
}

function regionalRetryCatalogSourceArtist() {
    return {
        id: regionalRetryArtistCatalog(),
        type: 'artists',
        attributes: {
            name: 'Regional Source Retry Artist',
            editorialNotes: 'Regional source retry artist biography',
            albumCount: 2,
            trackCount: 5,
        },
        relationships: {
            catalog: {
                data: [{
                    id: regionalRetryArtistCatalog(),
                    type: 'artists',
                    attributes: {
                        name: 'Regional Source Retry Artist',
                        editorialNotes: 'Regional source retry artist biography',
                        albumCount: 2,
                        trackCount: 5,
                    },
                }],
            },
        },
    };
}

function favoriteArtistResource() {
    return {
        id: favoriteArtistLibrary,
        type: 'library-artists',
        attributes: {
            name: 'Favorite Artist Contract',
            inFavorites: artistFavorites[favoriteArtistCatalog],
            albumCount: 1,
        },
        relationships: {
            catalog: {
                data: [{
                    id: favoriteArtistCatalog,
                    type: 'artists',
                    attributes: { name: 'Favorite Artist Contract' },
                }],
            },
        },
    };
}

function favoriteCatalogArtistResource() {
    return {
        id: favoriteArtistCatalog,
        type: 'artists',
        attributes: {
            name: 'Favorite Artist Contract',
            inFavorites: artistFavorites[favoriteArtistCatalog],
            albumCount: 1,
            trackCount: 1,
        },
    };
}

function sourcePageArtistResource() {
    return {
        id: sourcePageArtistLibrary,
        type: 'library-artists',
        attributes: {
            name: 'Source Page Artist Contract',
            inFavorites: artistFavorites[sourcePageArtistCatalog],
            albumCount: 0,
        },
        relationships: {
            catalog: {
                data: [{
                    id: sourcePageArtistCatalog,
                    type: 'artists',
                    attributes: { name: 'Source Page Artist Contract', albumCount: 0 },
                }],
            },
        },
    };
}

function sourcePageCatalogArtistResource() {
    return {
        id: sourcePageArtistCatalog,
        type: 'artists',
        attributes: {
            name: 'Source Page Artist Contract',
            inFavorites: artistFavorites[sourcePageArtistCatalog],
            albumCount: 0,
            trackCount: 0,
        },
    };
}

function staleArtistResource() {
    return {
        id: staleArtistLibrary,
        type: 'library-artists',
        attributes: {
            name: 'Stale Read Artist Contract',
            inFavorites: artistFavorites[staleArtistCatalog],
            albumCount: 0,
        },
        relationships: {
            catalog: {
                data: [{
                    id: staleArtistCatalog,
                    type: 'artists',
                    attributes: { name: 'Stale Read Artist Contract', albumCount: 0 },
                }],
            },
        },
    };
}

function staleCatalogArtistResource() {
    return {
        id: staleArtistCatalog,
        type: 'artists',
        attributes: {
            name: 'Stale Read Artist Contract',
            inFavorites: artistFavorites[staleArtistCatalog],
            albumCount: 0,
            trackCount: 0,
        },
    };
}

function favoriteArtistAlbum() {
    return {
        id: `${favoriteArtistCatalog}.album.1`,
        type: 'albums',
        attributes: {
            name: 'Favorite Artist Contract Album',
            artistName: 'Favorite Artist Contract',
            trackCount: 1,
            releaseDate: '2026-03-03',
        },
    };
}

function regionalSourceArtistAlbum(catalogArtist, index) {
    return {
        id: `${catalogArtist}.album.${index}`,
        type: 'albums',
        attributes: {
            name: `Regional Artist Album ${index}`,
            artistName: 'Regional Artist',
            trackCount: 1,
            releaseDate: '2021-05-04',
        },
    };
}

function regionalDetailSourceUploadedSong() {
    return {
        id: regionalDetailUploadedSongLibrary(),
        type: 'library-songs',
        attributes: {
            name: 'Regional Source Uploaded Song',
            albumName: 'Regional Source Album',
            artistName: 'Regional Source Album Artist',
            durationInMillis: 245678,
            genreNames: ['Regional'],
            composerName: 'Regional source composer',
            playParams: {
                id: regionalDetailUploadedSongLibrary(),
                kind: 'song',
                catalogId: regionalDetailUploadedSongCatalog(),
            },
        },
        relationships: {
            catalog: {
                data: [{
                    id: regionalDetailUploadedSongCatalog(),
                    type: 'songs',
                    attributes: {
                        name: 'Regional Source Uploaded Song',
                        albumName: 'Regional Source Album',
                    },
                }],
            },
            albums: {
                data: [{ id: regionalDetailAlbumCatalog(), type: 'albums' }],
            },
            artists: {
                data: [{
                    id: regionalDetailArtistCatalog(),
                    type: 'artists',
                    attributes: { name: 'Regional Source Album Artist' },
                }],
            },
        },
    };
}

function regionalDetailSourceLibraryOnlySong() {
    return {
        id: regionalDetailLibraryOnlySong(),
        type: 'library-songs',
        attributes: {
            name: 'Regional Source Library-only Song',
            albumName: 'Regional Source Album',
            artistName: 'Regional Source Library-only Artist',
            durationInMillis: 198765,
            genreNames: ['Regional'],
            playParams: {
                id: regionalDetailLibraryOnlySong(),
                kind: 'song',
            },
        },
        relationships: {
            albums: {
                data: [{ id: regionalDetailAlbumCatalog(), type: 'albums' }],
            },
            artists: {
                data: [{
                    id: regionalDetailLibraryOnlyArtist(),
                    type: 'artists',
                    attributes: { name: 'Regional Source Library-only Artist' },
                }],
            },
        },
    };
}

function regionalDetailSourceAlbum() {
    return {
        id: regionalDetailAlbumCatalog(),
        type: 'albums',
        attributes: {
            name: 'Regional Source Album',
            artistName: 'Regional Source Album Artist',
            editorialNotes: 'Regional source album description',
            trackCount: 2,
            releaseDate: '2021-05-04',
        },
        relationships: {
            tracks: {
                data: [
                    regionalDetailSourceUploadedSong(),
                    regionalDetailSourceLibraryOnlySong(),
                ],
            },
            artists: {
                data: [{
                    id: regionalDetailArtistCatalog(),
                    type: 'artists',
                    attributes: { name: 'Regional Source Album Artist' },
                }],
            },
        },
    };
}

function regionalDetailSourceArtist() {
    return {
        id: regionalDetailArtistCatalog(),
        type: 'artists',
        attributes: {
            name: 'Regional Source Album Artist',
            editorialNotes: 'Regional source artist biography',
            albumCount: 3,
            trackCount: 7,
        },
        relationships: {
            catalog: {
                data: [{
                    id: regionalDetailArtistCatalog(),
                    type: 'artists',
                    attributes: {
                        name: 'Regional Source Album Artist',
                        editorialNotes: 'Regional source artist biography',
                        albumCount: 3,
                        trackCount: 7,
                    },
                }],
            },
        },
    };
}

function regionalSourceUploadedSong() {
    return {
        id: regionalUploadedSongLibrary(),
        type: 'library-songs',
        attributes: {
            name: 'Regional Source Uploaded Song',
            albumName: 'Regional Source Album',
            artistName: 'Regional Source Album Artist',
            durationInMillis: 245678,
            genreNames: ['Regional'],
            composerName: 'Regional source composer',
            playParams: {
                id: regionalUploadedSongLibrary(),
                kind: 'song',
                catalogId: regionalUploadedSongCatalog(),
            },
        },
        relationships: {
            catalog: {
                data: [{
                    id: regionalUploadedSongCatalog(),
                    type: 'songs',
                    attributes: {
                        name: 'Regional Source Uploaded Song',
                        albumName: 'Regional Source Album',
                    },
                }],
            },
            albums: {
                data: [{ id: regionalAlbumLibrary(), type: 'library-albums' }],
            },
            artists: {
                data: [{
                    id: regionalAlbumArtistLibrary(),
                    type: 'artists',
                    attributes: { name: 'Regional Source Album Artist' },
                }],
            },
        },
    };
}

function regionalSourceLibraryOnlySong() {
    return {
        id: regionalLibraryOnlySong(),
        type: 'library-songs',
        attributes: {
            name: 'Regional Source Library-only Song',
            albumName: 'Regional Source Album',
            artistName: 'Regional Source Library-only Artist',
            durationInMillis: 198765,
            genreNames: ['Regional'],
            playParams: {
                id: regionalLibraryOnlySong(),
                kind: 'song',
            },
        },
        relationships: {
            albums: {
                data: [{ id: regionalAlbumLibrary(), type: 'library-albums' }],
            },
            artists: {
                data: [{
                    id: regionalLibraryOnlyArtist(),
                    type: 'artists',
                    attributes: { name: 'Regional Source Library-only Artist' },
                }],
            },
        },
    };
}

function regionalLocalizedAlbum(region) {
    const localized = region === 'jp'
        ? {
            name: '日本地域アルバム',
            description: '日本語のアルバム説明',
            artist: '日本語のアルバムアーティスト',
        }
        : {
            name: '中国地区专辑',
            description: '中文专辑介绍',
            artist: '中文专辑艺术家',
        };
    return {
        id: regionalDetailAlbumCatalog(),
        type: 'albums',
        attributes: {
            name: localized.name,
            artistName: localized.artist,
            editorialNotes: localized.description,
            trackCount: 2,
            releaseDate: '2021-05-04',
        },
        relationships: {
            artists: {
                data: [{
                    id: regionalDetailArtistCatalog(),
                    type: 'artists',
                    attributes: { name: localized.artist },
                }],
            },
        },
    };
}

function regionalLocalizedSong(region) {
    const localized = region === 'jp'
        ? {
            title: '日本語のアップロード曲',
            album: '日本地域アルバム',
            artist: '日本語のアルバムアーティスト',
            composer: '日本語の作曲者',
        }
        : {
            title: '中文上传歌曲',
            album: '中国地区专辑',
            artist: '中文专辑艺术家',
            composer: '中文作曲者',
        };
    return {
        id: regionalDetailUploadedSongCatalog(),
        type: 'songs',
        attributes: {
            name: localized.title,
            albumName: localized.album,
            artistName: localized.artist,
            durationInMillis: 1,
            genreNames: ['Regional'],
            composerName: localized.composer,
            playParams: {
                id: regionalDetailUploadedSongCatalog(),
                kind: 'song',
                catalogId: regionalDetailUploadedSongCatalog(),
            },
        },
        relationships: {
            albums: {
                data: [{ id: regionalDetailAlbumCatalog(), type: 'albums' }],
            },
            artists: {
                data: [{
                    id: regionalDetailArtistCatalog(),
                    type: 'artists',
                    attributes: { name: localized.artist },
                }],
            },
        },
    };
}

function regionalLocalizedArtist(region) {
    const localized = region === 'jp'
        ? { name: '日本語のバイオアーティスト', bio: '日本語のアーティスト紹介' }
        : { name: '中文简介艺术家', bio: '中文艺术家简介' };
    return {
        id: regionalDetailArtistCatalog(),
        type: 'artists',
        attributes: {
            name: localized.name,
            editorialNotes: localized.bio,
            albumCount: 3,
            trackCount: 7,
        },
    };
}

function regionalLocalizedRetryArtist(region) {
    return region === 'jp'
        ? {
            id: regionalRetryArtistCatalog(),
            type: 'artists',
            attributes: {
                name: '日本語の再試行アーティスト',
                editorialNotes: '日本語の再試行アーティスト紹介',
                albumCount: 2,
                trackCount: 5,
            },
        }
        : {
            id: regionalRetryArtistCatalog(),
            type: 'artists',
            attributes: {
                name: 'Retry Artist',
                editorialNotes: 'Retry artist biography',
                albumCount: 2,
                trackCount: 5,
            },
        };
}

function regionalMissingSourceArtist() {
    return {
        id: regionalMissingArtistCatalog(),
        type: 'artists',
        attributes: {
            name: 'Regional Source Missing Artist',
            editorialNotes: 'Regional missing source artist biography',
            albumCount: 4,
            trackCount: 9,
        },
        relationships: {
            catalog: {
                data: [{
                    id: regionalMissingArtistCatalog(),
                    type: 'artists',
                    attributes: {
                        name: 'Regional Source Missing Artist',
                        editorialNotes: 'Regional missing source artist biography',
                        albumCount: 4,
                        trackCount: 9,
                    },
                }],
            },
        },
    };
}

function performanceIndexes() {
    return [
        ...Array.from({ length: performanceUniqueCount }, (_, index) => index),
        7,
        128,
        277,
    ];
}

function performanceSong(index, hydrated = false) {
    const item = {
        id: `i.song.performance.${index}`,
        type: 'library-songs',
        attributes: {
            name: `Performance Track ${index}`,
            albumName: hydrated ? `Performance Album ${index}` : '',
            artistName: hydrated ? `Performance Artist ${index}` : '',
            durationInMillis: 180000 + index,
            genreNames: ['Test'],
            playParams: {
                id: `i.song.performance.${index}`,
                kind: 'song',
                catalogId: `song.performance.${index}`,
            },
        },
    };
    if (hydrated) {
        item.relationships = {
            albums: {
                data: [{ id: `l.album.performance.${index}`, type: 'library-albums' }],
            },
            artists: {
                data: [{
                    id: `r.artist.performance.${index}`,
                    type: 'artists',
                    attributes: { name: `Performance Artist ${index}` },
                }],
            },
        };
    }
    return item;
}

function performanceNext(offset) {
    const nextOffset = offset + performancePageSize;
    if (nextOffset >= performanceTrackCount) return null;
    return `/v1/me/library/playlists/${playlistPerformance}/tracks` +
        `?offset=${nextOffset}&include=tracks`;
}

function performancePlaylist(includeTracks = true, hydrated = false) {
    const tracks = includeTracks
        ? performanceIndexes()
            .slice(0, performancePageSize)
            .map((index) => performanceSong(index, hydrated))
        : [];
    const item = playlistItem(
        playlistPerformance,
        'Provider Performance Playlist',
        false,
        [],
        tracks,
    );
    item.attributes.trackCount = performanceTrackCount;
    if (includeTracks) {
        item.relationships.tracks.next =
            `/v1/me/library/playlists/${playlistPerformance}/tracks` +
            `?offset=${performancePageSize}&include=tracks`;
    }
    return item;
}

function performanceTrackPage(offset, hydrated) {
    const tracks = performanceIndexes()
        .slice(offset, offset + performancePageSize)
        .map((index) => performanceSong(index, hydrated));
    return {
        data: tracks,
        next: performanceNext(offset),
    };
}

function pagedIndexes() {
    return performanceIndexes();
}

function pagedSong(index) {
    const suffix = String(index);
    return {
        id: `i.song.glasswave-paged-performance.${suffix}`,
        type: 'library-songs',
        attributes: {
            name: `Glasswave Paged Track ${suffix}`,
            albumName: `Glasswave Paged Album ${suffix}`,
            artistName: `Glasswave Paged Artist ${suffix}`,
            durationInMillis: 200000 + index,
            genreNames: ['Paged'],
            playParams: {
                id: `i.song.glasswave-paged-performance.${suffix}`,
                kind: 'song',
                catalogId: `song.glasswave-paged-performance.${suffix}`,
            },
        },
        relationships: {
            albums: {
                data: [{ id: `l.album.glasswave-paged-performance.${suffix}`, type: 'library-albums' }],
            },
            artists: {
                data: [{
                    id: `r.artist.glasswave-paged-performance.${suffix}`,
                    type: 'artists',
                    attributes: { name: `Glasswave Paged Artist ${suffix}` },
                }],
            },
        },
    };
}

function pagedNext(offset) {
    const nextOffset = offset + performancePageSize;
    if (nextOffset >= performanceTrackCount) return null;
    return `/v1/me/library/playlists/${playlistPagedPerformance}/tracks` +
        `?offset=${nextOffset}&opaque=glasswave-paged-${nextOffset}&include=tracks`;
}

function pagedTrackPage(offset) {
    const tracks = pagedIndexes()
        .slice(offset, offset + performancePageSize)
        .map(pagedSong);
    return {
        data: tracks,
        next: pagedNext(offset),
    };
}

function enrichmentBaseSong(index, known = false) {
    const suffix = String(index);
    const item = {
        id: `i.song.glasswave-enrichment.${enrichmentTag}.${suffix}`,
        type: 'library-songs',
        attributes: {
            name: `Glasswave Enrichment Track ${suffix}`,
            artistName: `Glasswave Enrichment Artist ${suffix}`,
            durationInMillis: 210000 + index,
            playParams: {
                id: `i.song.glasswave-enrichment.${enrichmentTag}.${suffix}`,
                kind: 'song',
                catalogId: `song.glasswave-enrichment.${enrichmentTag}.${suffix}`,
            },
        },
    };
    if (known) {
        item.attributes.albumName = `Glasswave Enrichment Album ${suffix}`;
        item.relationships = {
            albums: {
                data: [{
                    id: `l.album.glasswave-enrichment.${enrichmentTag}.${suffix}`,
                    type: 'library-albums',
                }],
            },
            artists: {
                data: [{
                    id: `r.artist.glasswave-enrichment.${enrichmentTag}.${suffix}`,
                    type: 'artists',
                    attributes: { name: `Glasswave Enrichment Artist ${suffix}` },
                }],
            },
        };
    }
    return item;
}

function enrichmentTrackPage() {
    return {
        data: enrichmentIndexes.map((index) => enrichmentBaseSong(index, index === enrichmentKnownIndex)),
        next: null,
    };
}

function catalogPerformanceSong(index) {
    return {
        id: `song.catalog.performance.${index}`,
        type: 'songs',
        attributes: {
            name: `Catalog Performance Track ${index}`,
            albumName: `Catalog Performance Album ${index}`,
            artistName: `Catalog Performance Artist ${index}`,
            durationInMillis: 190000 + index,
            playParams: {
                id: `song.catalog.performance.${index}`,
                kind: 'song',
                catalogId: `song.catalog.performance.${index}`,
            },
        },
        relationships: {
            albums: {
                data: [{ id: `album.catalog.performance.${index}`, type: 'albums' }],
            },
            artists: {
                data: [{
                    id: `artist.catalog.performance.${index}`,
                    type: 'artists',
                    attributes: { name: `Catalog Performance Artist ${index}` },
                }],
            },
        },
    };
}

function catalogPerformancePlaylist() {
    const tracks = [0, 1, 2].map(catalogPerformanceSong);
    return {
        id: playlistPerformanceCatalog,
        type: 'playlists',
        attributes: {
            name: 'Catalog Performance Playlist',
            canEdit: false,
            trackCount: tracks.length,
        },
        relationships: { tracks: { data: tracks } },
    };
}

function libraryAlbum() {
    return {
        id: albumLibrary,
        type: 'library-albums',
        attributes: {
            name: 'Provider Contract Album',
            artistName: 'Mock Artist',
            trackCount: 1,
            releaseDate: '2026-01-01',
        },
        relationships: {
            catalog: { data: [{ id: albumCatalog, type: 'albums' }] },
        },
    };
}

function mutationAlbum() {
    return {
        id: mutationAlbumLibrary,
        type: 'library-albums',
        attributes: {
            name: 'Mutation Ledger Album',
            artistName: 'Mock Artist',
            trackCount: 1,
            releaseDate: '2026-02-02',
        },
        relationships: {
            catalog: { data: [{ id: mutationAlbumCatalog, type: 'albums' }] },
        },
    };
}

function mutationCatalogSong() {
    return {
        id: mutationCatalogSongId,
        type: 'songs',
        attributes: {
            name: 'Mutation Catalog Track',
            albumName: 'Mutation Ledger Album',
            artistName: 'Mock Artist',
            durationInMillis: 181000,
            playParams: { id: mutationCatalogSongId, kind: 'song' },
        },
        relationships: {
            albums: { data: [{ id: mutationAlbumCatalog, type: 'albums' }] },
            artists: {
                data: [{
                    id: favoriteArtistCatalog,
                    type: 'artists',
                    attributes: { name: 'Mock Artist' },
                }],
            },
        },
    };
}

function mutationCatalogAlbum() {
    return {
        id: mutationAlbumCatalog,
        type: 'albums',
        attributes: {
            name: 'Mutation Ledger Album',
            artistName: 'Mock Artist',
            trackCount: 1,
            releaseDate: '2026-02-02',
        },
        relationships: {
            tracks: { data: [mutationCatalogSong()] },
        },
    };
}

function playlistItem(id, name, own, tags, tracks = [], catalogId = null) {
    const item = {
        id,
        type: 'library-playlists',
        attributes: {
            name,
            canEdit: own,
            canRename: own,
            canDelete: own || id === playlistSaved,
            trackCount: tracks.length,
            tags,
        },
        relationships: { tracks: { data: tracks } },
    };
    if (catalogId) item.relationships.catalog = { data: [{ id: catalogId, type: 'playlists' }] };
    return item;
}

function favoritePlaylist() {
    return playlistItem(playlistFavorite, 'Favorite Songs', false, ['favorited'], favorite ? [song()] : []);
}

function savedPlaylist() {
    return playlistItem(playlistSaved, 'Provider Contract Saved Playlist', false, [], [], playlistCatalog);
}

function catalogPlaylist() {
    return {
        id: playlistCatalog,
        type: 'playlists',
        attributes: {
            name: 'Provider Contract Catalog Playlist',
            description: 'Provider contract playlist display metadata',
            trackCount: 0,
            canEdit: false,
        },
        relationships: {
            tracks: { data: [] },
        },
    };
}

function mutationCatalogPlaylist() {
    return {
        id: mutationPlaylistCatalog,
        type: 'playlists',
        attributes: {
            name: 'Mutation Ledger Playlist',
            description: 'Mutation ledger playlist display metadata',
            trackCount: 0,
            canEdit: false,
        },
        relationships: {
            tracks: { data: [] },
        },
    };
}

function mutationPlaylist() {
    const item = playlistItem(mutationPlaylistLibrary, 'Mutation Ledger Playlist', false, [], [],
                              mutationPlaylistCatalog);
    item.attributes.canDelete = true;
    return item;
}

function scratchPlaylist() {
    return playlistItem(playlistScratch, scratch.name, true, [], scratch.tracks.map(() => song()));
}

function libraryPlaylists() {
    if (scenario === 'startup-cache') {
        const id = startup.accountMode === 'b'
            ? startupPlaylistB
            : startup.accountMode === 'signed-out' ? null : startupPlaylistA;
        if (!id) return [];
        const rows = [playlistItem(id, `Startup playlist ${id === startupPlaylistA ? 'A' : 'B'}`,
                                   true, ['startup'], [], null)];
        if (id === startupPlaylistA)
            rows.unshift(playlistItem(startupFavoriteA, 'Favorite Songs A', false,
                                      ['favorited', 'startup'], [], null));
        return rows;
    }
    const rows = [favoritePlaylist()];
    rows.push(performancePlaylist(false));
    if (playlistIsSaved) rows.push(savedPlaylist());
    if (mutationPlaylistSaved && !accountSwitched) rows.push(mutationPlaylist());
    if (scratch) rows.push(scratchPlaylist());
    return rows;
}

function catalogArtistResource(id, name) {
    return {
        id,
        type: 'artists',
        attributes: {
            name,
            artwork: { url: `https://example.invalid/{w}x{h}bb.jpg` },
        },
    };
}

function catalogAlbumResource(id, name) {
    return {
        id,
        type: 'albums',
        attributes: {
            name,
            artistName: 'Catalog Regression Artist',
            trackCount: 1,
            releaseDate: '2026-01-01',
            artwork: { url: `https://example.invalid/{w}x{h}bb.jpg` },
        },
    };
}

function catalogSongResource(id, name) {
    return {
        id,
        type: 'songs',
        attributes: {
            name,
            artistName: 'Catalog Regression Artist',
            albumName: 'Catalog Regression Album',
            durationInMillis: 180000,
            artwork: { url: `https://example.invalid/{w}x{h}bb.jpg` },
        },
        relationships: {
            albums: { data: [catalogAlbumResource('catalog.album.context', 'Catalog Regression Album')] },
            artists: { data: [catalogArtistResource('catalog.artist.context', 'Catalog Regression Artist')] },
        },
    };
}

function catalogPlaylistResource(id, name) {
    return {
        id,
        type: 'playlists',
        attributes: {
            name,
            canEdit: false,
            artwork: { url: `https://example.invalid/{w}x{h}bb.jpg` },
        },
    };
}

const catalogSectionIds = [
    'top-songs',
    'featured-albums',
    'full-albums',
    'singles',
    'live-albums',
    'appears-on-albums',
    'compilation-albums',
    'featured-playlists',
];

function catalogSectionRows(section) {
    if (section === 'top-songs')
        return [0, 1, 2, 3].map((index) =>
            catalogSongResource(`catalog.top.${index}`, `Catalog Top Song ${index}`));
    if (section === 'featured-playlists')
        return [0, 1, 2].map((index) =>
            catalogPlaylistResource(`catalog.playlist.${index}`, `Catalog Playlist ${index}`));
    return [0, 1, 2].map((index) =>
        catalogAlbumResource(`catalog.${section}.${index}`, `Catalog ${section} ${index}`));
}

function categorySearchRows(kind, page) {
    const suffix = page === 0 ? 'first' : 'second';
    const id = page === 0 ? '777' : '778';
    switch (kind) {
        case 'songs':
            return [catalogSongResource(id, `Search song ${suffix}`)];
        case 'artists':
            return [catalogArtistResource(id, `Search artist ${suffix}`)];
        case 'albums':
            return [catalogAlbumResource(id, `Search album ${suffix}`)];
        case 'playlists':
            return [catalogPlaylistResource(id, `Search playlist ${suffix}`)];
        default:
            return [];
    }
}

function resourceResolution(params) {
    const resources = {};
    if (params.get('ids[albums]') === albumCatalog) {
        resources.albums = {
            [albumCatalog]: {
                id: albumCatalog,
                type: 'albums',
                attributes: { inLibrary: albumSaved },
                relationships: {
                    library: { data: albumSaved ? [{ id: albumLibrary, type: 'library-albums' }] : [] },
                },
            },
        };
    }
    if (params.get('ids[playlists]') === playlistCatalog) {
        resources.playlists = {
            [playlistCatalog]: {
                id: playlistCatalog,
                type: 'playlists',
                attributes: { inLibrary: playlistIsSaved },
                relationships: {
                    library: { data: playlistIsSaved ? [{ id: playlistSaved, type: 'library-playlists' }] : [] },
                },
            },
        };
    }
    return { resources };
}

function reply(res, status, body) {
    if (status === 204 || body === undefined) {
        res.writeHead(status);
        res.end();
        return;
    }
    const payload = JSON.stringify(body ?? {});
    res.writeHead(status, { 'content-type': 'application/json; charset=utf-8', 'content-length': Buffer.byteLength(payload) });
    res.end(payload);
}

// What the provider asks of an artist page's own record (the whole profile) and of an album.
const kArtistProfileExtend = 'inFavorites,artistBio,bornOrFormed,origin,isGroup,plainEditorialNotes';
const kAlbumExtend = 'inFavorites,audioTraits';

function expectRegionalParams(params, expected, message) {
    const actualKeys = [...params.keys()].sort();
    const expectedKeys = Object.keys(expected).sort();
    const matches = actualKeys.join('|') === expectedKeys.join('|') &&
        expectedKeys.every((key) => params.get(key) === expected[key]);
    if (!matches) {
        regional.invalidRequests++;
        mockError(`${message}: ${params.toString()}`);
    }
    return matches;
}

function bump(map, key) {
    map[key] = (map[key] ?? 0) + 1;
}

function startupSlowMembership() {
    return scenario === 'startup-cache' &&
        ['verify-slow', 'account-b'].includes(startup.accountMode);
}

function CatalogSearchNext(term, types, withValue, offset) {
    const query = new URLSearchParams({
        term,
        types,
        with: withValue,
        offset: String(offset),
        limit: '25',
    });
    return `/v1/catalog/US/search?${query.toString()}`;
}

async function dispatchCatalog(method, path, params) {
    if (!['catalog-regression', 'catalog-suggestions'].includes(scenario) || method !== 'GET')
        return null;

    const artistMatch = path.match(/^\/v1\/catalog\/US\/artists\/([^/]+)$/);
    if (artistMatch) {
        const artistId = decodeURIComponent(artistMatch[1]);
        bump(catalog.artistRoots, artistId);
        if (artistId === catalogArtistUnknown) return [404, { errors: [{ status: '404' }] }];
        if (artistId === catalogArtistConcurrentEmpty) await delay(180);
        return [200, { data: [catalogArtistResource(artistId, `Catalog Artist ${artistId}`)] }];
    }

    const viewMatch = path.match(/^\/v1\/catalog\/US\/artists\/([^/]+)\/view\/([^/]+)$/);
    if (viewMatch) {
        const artistId = decodeURIComponent(viewMatch[1]);
        const section = decodeURIComponent(viewMatch[2]);
        if (!catalogSectionIds.includes(section)) {
            catalog.invalidSearchRequests++;
            return [404, { error: 'unknown artist section' }];
        }
        bump(catalog.sectionRequests, section);
        catalog.sectionPaths.push(path);
        const offset = Number.parseInt(params.get('offset') ?? '0', 10);
        const limit = Number.parseInt(params.get('limit') ?? '5', 10);
        catalog.sectionOffsets.push(offset);
        catalog.sectionLimits.push(limit);
        if (offset > 0) bump(catalog.sectionContinuations, section);

        if (artistId === catalogArtistUnknown) return [404, { errors: [{ status: '404' }] }];
        if (artistId === catalogArtistUnauthorized) return [401, { errors: [{ status: '401' }] }];
        if (artistId === catalogArtistNetworkError) return [503, { error: 'injected network failure' }];
        if (artistId === catalogArtistContinuationError && offset > 0)
            return [404, { errors: [{ status: '404', title: 'continuation disappeared' }] }];
        if (artistId === catalogArtistEmpty &&
            ['featured-albums', 'live-albums', 'compilation-albums', 'featured-playlists'].includes(section))
            return [404, { errors: [{ status: '404', title: 'normal absent view' }] }];
        if (artistId === catalogArtistConcurrentEmpty &&
            ['featured-albums', 'full-albums', 'singles', 'live-albums'].includes(section))
            return [404, { errors: [{ status: '404', title: 'concurrent absent view' }] }];

        const rows = catalogSectionRows(section);
        const page = rows.slice(offset, offset + limit);
        const nextOffset = offset + page.length;
        const body = { data: page };
        if (nextOffset < rows.length) {
            body.next =
                `/v1/catalog/US/artists/${encodeURIComponent(artistId)}/view/${encodeURIComponent(section)}` +
                `?offset=${nextOffset}&limit=${limit}`;
        }
        return [200, body];
    }

    if (path === '/v1/catalog/US/search/suggestions') {
        catalog.suggestionRequests++;
        const term = params.get('term') ?? '';
        catalog.suggestionTerms.push(term);
        if (params.get('kinds') !== 'terms,topResults' ||
            params.get('types') !== 'songs,artists,albums,playlists') {
            catalog.invalidSuggestionRequests++;
            mockError(`invalid suggestion query: ${params.toString()}`);
        }
        if (term === 'suggest-stale-region' || term === 'suggest-stale-account')
            await delay(800);
        const suggestions = [
            {
                kind: 'terms',
                searchTerm: 'search-one',
                displayTerm: 'Display One',
            },
            {
                kind: 'topResults',
                content: catalogArtistResource('777', 'Suggestion Artist'),
            },
            {
                kind: 'terms',
                searchTerm: 'search-two',
                displayTerm: 'Display Two',
            },
            {
                kind: 'topResults',
                content: { id: 'ignored', type: 'stations', attributes: { name: 'Ignored station' } },
            },
            {
                kind: 'topResults',
                content: catalogAlbumResource('888', 'Suggestion Album'),
            },
            {
                kind: 'topResults',
                content: catalogPlaylistResource('999', 'Suggestion Playlist'),
            },
            {
                kind: 'topResults',
                content: catalogSongResource('777', 'Suggestion Song'),
            },
        ];
        return [200, { results: { suggestions } }];
    }

    if (path === '/v1/catalog/US/search') {
        const term = params.get('term') ?? '';
        const types = params.get('types') ?? '';
        const withValue = params.get('with') ?? '';
        const offset = Number.parseInt(params.get('offset') ?? '0', 10);
        bump(catalog.searchRequests, types || 'missing');
        catalog.searchQueries.push(params.toString());
        if (offset > 0) bump(catalog.searchContinuations, `${term}|${types}|${withValue}`);
        if (params.get('limit') !== '25') {
            catalog.invalidSearchRequests++;
            mockError(`search ${types} requested limit ${params.get('limit')}`);
        }
        const top = types === 'songs,artists,albums,playlists';
        if (withValue !== (top ? 'topResults' : '') ||
            (types === 'songs') !== (params.get('include') === 'albums')) {
            catalog.invalidSearchRequests++;
            mockError(`invalid search query: ${params.toString()}`);
        }
        if (offset > 0 &&
            ((term === 'paged-search' &&
              (!['songs', 'artists', 'albums', 'playlists'].includes(types) || withValue !== '')) ||
             (term === 'mixed-rank' &&
              (types !== 'songs,artists,albums,playlists' || withValue !== 'topResults')) ||
             !['paged-search', 'mixed-rank'].includes(term))) {
            catalog.invalidSearchContinuations++;
            return [400, { error: 'search continuation query was not rebound' }];
        }

        if (term === 'paged-search') {
            if (!['songs', 'artists', 'albums', 'playlists'].includes(types)) {
                catalog.invalidSearchContinuations++;
                return [400, { error: 'search continuation was not rebound' }];
            }
            if (offset > 0) {
                return [200, { results: { [types]: { data: categorySearchRows(types, 1) } } }];
            }
            return [200, {
                results: {
                    [types]: {
                        data: categorySearchRows(types, 0),
                        next: CatalogSearchNext('wrong-term', 'songs', 'wrong', 1),
                    },
                },
            }];
        }

        if (term === 'mixed-rank') {
            if (offset > 0) {
                return [200, {
                    results: {
                        topResults: {
                            data: [
                                catalogArtistResource('778', 'Mixed continuation artist'),
                                catalogSongResource('778', 'Mixed continuation song'),
                            ],
                        },
                    },
                }];
            }
            return [200, {
                results: {
                    topResults: {
                        data: [
                            catalogArtistResource('777', 'Mixed artist'),
                            { id: 'ignored', type: 'stations', attributes: { name: 'Ignored mixed item' } },
                            catalogSongResource('777', 'Mixed song'),
                            catalogPlaylistResource('777', 'Mixed playlist'),
                            catalogAlbumResource('777', 'Mixed album'),
                        ],
                        next: CatalogSearchNext('wrong-term', 'songs', 'wrong', 5),
                    },
                },
            }];
        }

        if (term === 'terminal-mixed') {
            return [200, {
                results: {
                    topResults: {
                        data: [
                            catalogSongResource('991', 'Terminal song'),
                            catalogArtistResource('991', 'Terminal artist'),
                            catalogAlbumResource('991', 'Terminal album'),
                            catalogPlaylistResource('991', 'Terminal playlist'),
                        ],
                    },
                },
            }];
        }

        if (term === 'missing-category')
            return [200, { results: { songs: { data: [] } } }];
        return [200, { results: { [types]: { data: categorySearchRows(types, offset > 0 ? 1 : 0) } } }];
    }

    return null;
}

async function dispatchApple(method, rawPath, body) {
    const url = new URL(rawPath, 'http://127.0.0.1');
    const path = url.pathname;
    const params = url.searchParams;

    const catalogResult = await dispatchCatalog(method, path, params);
    if (catalogResult) return catalogResult;

    if (method === 'GET') {
        if (scenario === 'startup-cache' && startupSlowMembership() &&
            ['/v1/me/library/playlists', '/v1/me/library/albums',
             '/v1/me/library/artists'].includes(path)) {
            const phase = startup.phase;
            await delay(path === '/v1/me/library/playlists' ? 900 : 1800);
            if (phase === startup.phase) {
                if (path === '/v1/me/library/playlists') startup.playlistCollectionCompleted = true;
                if (path === '/v1/me/library/albums') startup.albumCollectionCompleted = true;
                if (path === '/v1/me/library/artists') startup.artistCollectionCompleted = true;
            }
            saveState();
        }
        if (path === '/v1/me/library/playlists') {
            const rows = libraryPlaylists();
            if (scenario === 'mutation-reconciliation' && playlistAuthority.staleArmed) {
                if (!playlistAuthority.staleServed) {
                    playlistAuthority.staleServed = true;
                    rows.push(mutationPlaylist());
                    saveState();
                    await delay(mutationDelayMs);
                } else if (!playlistAuthority.externalPresent) {
                    playlistAuthority.absentServed = true;
                    saveState();
                } else if (!rows.some((row) => row.id === mutationPlaylistLibrary)) {
                    rows.push(mutationPlaylist());
                    saveState();
                }
            }
            return [200, { data: rows, next: null }];
        }
        const startupPlaylistMatch =
            path.match(/^\/v1\/me\/library\/playlists\/(p\.(?:startup|favorites)\.[^/]+)\/tracks$/);
        if (scenario === 'startup-cache' && startupPlaylistMatch) {
            const playlistId = decodeURIComponent(startupPlaylistMatch[1]);
            const favorite = playlistId === startupFavoriteA;
            if (favorite) startup.favoritePageRequests++;
            else startup.playlistPageRequests++;
            startup.listRequests++;
            if (startup.authCompleted && !startup.playlistCollectionCompleted &&
                !startup.albumCollectionCompleted && !startup.artistCollectionCompleted) {
                if (favorite) startup.favoritePageAfterAuthBeforeMembership++;
                else startup.playlistPageAfterAuthBeforeMembership++;
            }
            saveState();
            return [200, { data: [song(true)], next: null }];
        }
        if (path === `/v1/catalog/US/albums/${regionalDetailAlbumCatalog()}`) {
            regional.sourceRegionalAlbumRequests++;
            expectRegionalParams(params, {
                include: 'tracks',
                extend: kAlbumExtend,
                l: 'zh-Hans-CN',
            }, 'regional source catalog album request did not match');
            return [200, { data: [regionalDetailSourceAlbum()] }];
        }
        if (path === `/v1/catalog/US/albums/${mutationAlbumCatalog}`) {
            isolation.mutationCatalogAlbumReads++;
            const actualKeys = [...params.keys()].sort().join('|');
            if (actualKeys !== 'extend|include|l' || params.get('include') !== 'tracks' ||
                params.get('extend') !== kAlbumExtend || params.get('l') !== 'zh-Hans-CN') {
                mockError(`mutation catalog album request did not match: ${params.toString()}`);
            }
            saveState();
            return [200, { data: [mutationCatalogAlbum()] }];
        }
        if (path === `/v1/catalog/US/artists/${regionalDetailArtistCatalog()}`) {
            regional.sourceRegionalArtistRequests++;
            expectRegionalParams(params, {
                extend: kArtistProfileExtend,
                platform: 'web',
                l: 'zh-Hans-CN',
            }, 'regional source catalog artist request did not match');
            return [200, { data: [regionalDetailSourceArtist()] }];
        }
        if (path === `/v1/catalog/US/artists/${regionalRetryArtistCatalog()}`) {
            regional.sourceRetryArtistRequests++;
            expectRegionalParams(params, {
                extend: kArtistProfileExtend,
                platform: 'web',
                l: 'zh-Hans-CN',
            }, 'regional retry catalog artist request did not match');
            return [200, { data: [regionalRetryCatalogSourceArtist()] }];
        }
        if (path === `/v1/me/library/albums/${regionalAlbumLibrary()}`) {
            regional.sourceLibraryAlbumRequests++;
            expectRegionalParams(params, {
                include: 'tracks,catalog,artists',
                'include[library-songs]': 'catalog,artists,albums',
                l: 'zh-Hans-CN',
            }, 'regional source library album request did not match');
            return [200, { data: [regionalSourceAlbum()] }];
        }
        if (path === `/v1/me/library/albums/${regionalAlbumLibrary()}/tracks`) {
            regional.sourceLibraryAlbumTrackRequests++;
            expectRegionalParams(params, {
                limit: '100',
                'include[library-songs]': 'catalog,artists,albums',
                l: 'zh-Hans-CN',
            }, 'regional source library album tracks request did not match');
            return [200, {
                data: [regionalSourceUploadedSong(), regionalSourceLibraryOnlySong()],
                next: null,
            }];
        }
        if (path === `/v1/me/library/artists/${regionalAlbumArtistLibrary()}`) {
            regional.sourceLibraryArtistRequests++;
            expectRegionalParams(params, {
                include: 'catalog',
                l: 'zh-Hans-CN',
            }, 'regional source library artist request did not match');
            return [200, { data: [regionalSourceArtist()] }];
        }
        if (path === `/v1/me/library/artists/${regionalRetryArtistLibrary()}`) {
            regional.sourceLibraryArtistRequests++;
            expectRegionalParams(params, {
                include: 'catalog',
                l: 'zh-Hans-CN',
            }, 'regional retry library artist request did not match');
            return [200, { data: [regionalSourceRetryArtist()] }];
        }
        if (path === `/v1/catalog/US/artists/${regionalAlbumArtistCatalog()}/albums` ||
            path === `/v1/catalog/US/artists/${regionalRetryArtistCatalog()}/albums`) {
            regional.sourceArtistAlbumRequests++;
            const shared = path.includes(regionalAlbumArtistCatalog());
            if (shared) regional.sourceSharedArtistAlbumRequests++;
            else regional.sourceRetryArtistAlbumRequests++;
            if (!expectRegionalParams(params, {
                limit: '100',
                offset: '0',
                l: 'zh-Hans-CN',
            }, 'regional source artist albums request did not match'))
                regional.invalidSourceArtistAlbumRequests++;
            const catalogArtist = shared ? regionalAlbumArtistCatalog() : regionalRetryArtistCatalog();
            const count = shared ? 3 : 2;
            return [200, {
                data: Array.from(
                    { length: count },
                    (_, index) => regionalSourceArtistAlbum(catalogArtist, index),
                ),
                next: null,
            }];
        }
        if (path === `/v1/catalog/US/artists/${regionalMissingArtistCatalog()}`) {
            regional.sourceCatalogArtistRequests++;
            expectRegionalParams(params, {
                extend: kArtistProfileExtend,
                platform: 'web',
                l: 'zh-Hans-CN',
            }, 'regional source catalog artist request did not match');
            return [200, { data: [regionalMissingSourceArtist()] }];
        }
        if (path === `/v1/catalog/US/artists/${favoriteArtistCatalog}/albums`) {
            isolation.favoriteArtistAlbumCountReads++;
            const actualKeys = [...params.keys()].sort().join('|');
            if (actualKeys !== 'l|limit|offset' ||
                params.get('limit') !== '100' || params.get('offset') !== '0' ||
                params.get('l') !== 'zh-Hans-CN') {
                mockError(`favorite artist album-count request did not match: ${params.toString()}`);
            }
            saveState();
            return [200, { data: [favoriteArtistAlbum()], next: null }];
        }
        if (scenario === 'mutation-reconciliation' &&
            (path === `/v1/catalog/US/artists/${sourcePageArtistCatalog}/albums` ||
             path === `/v1/catalog/US/artists/${staleArtistCatalog}/albums`)) {
            // The library refresh enriches album counts for these two fixtures,
            // whose artist resources both declare albumCount=0.
            expectRegionalParams(params, {
                limit: '100',
                offset: '0',
                l: 'zh-Hans-CN',
            }, 'focused artist album-count request did not match');
            return [200, { data: [], next: null }];
        }
        if (path === `/v1/catalog/US/artists/${favoriteArtistCatalog}`) {
            const actualKeys = [...params.keys()].sort().join('|');
            if (actualKeys !== 'extend|l|platform' ||
                params.get('extend') !== 'inFavorites' ||
                params.get('platform') !== 'web' ||
                params.get('l') !== 'zh-Hans-CN') {
                mockError(`favorite catalog artist request did not match: ${params.toString()}`);
            }
            saveState();
            return [200, { data: [favoriteCatalogArtistResource()] }];
        }
        if (path === `/v1/catalog/US/artists/${sourcePageArtistCatalog}` ||
            path === `/v1/catalog/US/artists/${staleArtistCatalog}`) {
            const id = path.endsWith(sourcePageArtistCatalog)
                ? sourcePageArtistCatalog
                : staleArtistCatalog;
            artistFixture.canonicalReads[id]++;
            const actualKeys = [...params.keys()].sort().join('|');
            if (actualKeys !== 'extend|l|platform' ||
                params.get('extend') !== 'inFavorites' ||
                params.get('platform') !== 'web' ||
                params.get('l') !== 'zh-Hans-CN') {
                mockError(`focused catalog artist request did not match: ${params.toString()}`);
            }
            saveState();
            return [200, {
                data: [id === sourcePageArtistCatalog
                    ? sourcePageCatalogArtistResource()
                    : staleCatalogArtistResource()],
            }];
        }
        if (path === '/v1/me/library/artists' && params.has('ids')) {
            artistFixture.originalLibraryReads++;
            saveState();
            return [404, { errors: [{ status: '404', title: 'Original library artist is stale' }] }];
        }
        if (path === '/v1/catalog/jp/albums' || path === '/v1/catalog/cn/albums') {
            const japan = path.includes('/jp/');
            const region = japan ? 'jp' : 'cn';
            if (japan) regional.japanAlbumRequests++;
            else regional.chinaAlbumRequests++;
            expectRegionalParams(params, {
                ids: regionalDetailAlbumCatalog(),
                include: 'artists',
                platform: 'web',
                l: japan ? 'ja-JP' : 'zh-Hans-CN',
            }, `regional ${region} album request did not match`);
            return [200, { data: [regionalLocalizedAlbum(region)] }];
        }
        if (path === '/v1/catalog/jp/songs' || path === '/v1/catalog/cn/songs') {
            const japan = path.includes('/jp/');
            const region = japan ? 'jp' : 'cn';
            if (scenario === 'startup-cache' && params.get('ids') === songCatalog) {
                const expected = {
                    ids: songCatalog,
                    include: 'albums,artists',
                    platform: 'web',
                    l: 'ja-JP',
                };
                const actualKeys = [...params.keys()].sort();
                const expectedKeys = Object.keys(expected).sort();
                if (!japan || actualKeys.join('|') !== expectedKeys.join('|') ||
                    !expectedKeys.every((key) => params.get(key) === expected[key])) {
                    startup.invalidDisplaySongRequests++;
                    mockError(`startup display song request did not match: ${params.toString()}`);
                    return [400, { error: 'invalid startup display song request' }];
                }
                startup.displaySongRequests++;
                startup.displaySongMissingCounterparts++;
                saveState();
                return [200, { data: [] }];
            }
            if (japan) regional.japanSongRequests++;
            else regional.chinaSongRequests++;
            expectRegionalParams(params, {
                ids: regionalDetailUploadedSongCatalog(),
                include: 'albums,artists',
                platform: 'web',
                l: japan ? 'ja-JP' : 'zh-Hans-CN',
            }, `regional ${region} song request did not match`);
            return [200, { data: [regionalLocalizedSong(region)] }];
        }
        if (path === '/v1/catalog/jp/artists' || path === '/v1/catalog/cn/artists') {
            const japan = path.includes('/jp/');
            const region = japan ? 'jp' : 'cn';
            if (japan) regional.japanArtistRequests++;
            else regional.chinaArtistRequests++;
            const ids = (params.get('ids') ?? '').split(',').filter(Boolean);
            if (ids.length !== 1 ||
                ![
                    regionalDetailArtistCatalog(),
                    regionalMissingArtistCatalog(),
                    regionalRetryArtistCatalog(),
                ].includes(ids[0])) {
                regional.invalidRequests++;
                mockError(`regional ${region} artist lookup used an unexpected id`);
            }
            expectRegionalParams(params, {
                ids: ids.join(','),
                extend: 'inFavorites,artistBio,plainEditorialNotes',
                platform: 'web',
                l: japan ? 'ja-JP' : 'zh-Hans-CN',
            }, `regional ${region} artist request did not match`);
            if (ids[0] === regionalRetryArtistCatalog()) {
                regional.retryArtistRequests++;
                if (regional.retryArtistRequests === 1) {
                    regional.retryArtistFailures++;
                    return [503, { error: 'one-shot regional artist failure' }];
                }
                regional.retryArtistSuccesses++;
                return [200, { data: [regionalLocalizedRetryArtist(region)] }];
            }
            return [200, {
                data: ids[0] === regionalDetailArtistCatalog()
                    ? [regionalLocalizedArtist(region)]
                    : [],
            }];
        }
        if (path === `/v1/me/library/playlists/${playlistEnrichment}/tracks`) {
            enrichment.trackRequests++;
            const keys = [...params.keys()].sort().join('|');
            const expectedKeys =
                ['include', 'include[library-songs]', 'include[songs]', 'limit', 'l'].sort().join('|');
            if (keys !== expectedKeys ||
                params.get('limit') !== String(enrichmentPageLimit) ||
                params.get('include') !== 'catalog,artists' ||
                params.get('include[library-songs]') !== 'catalog,artists,albums' ||
                params.get('include[songs]') !== 'albums') {
                enrichment.invalidTrackRequests++;
                mockError(
                    `glasswave enrichment playlist track request did not match: ${params.toString()}`,
                );
            }
            const page = enrichmentTrackPage();
            enrichment.trackRows += page.data.length;
            saveState();
            return [200, page];
        }
        if (path === `/v1/me/library/playlists/${playlistPagedPerformance}/tracks`) {
            if (!params.has('offset')) {
                paged.primaryRequests++;
                const nested = params.get('include[library-songs]') === 'catalog,artists,albums';
                if (nested) paged.exactNestedIncludes++;
                if (params.get('limit') !== String(performancePageSize) ||
                    params.get('include') !== 'catalog,artists' ||
                    !nested ||
                    params.has('opaque')) {
                    paged.invalidPrimaryRequests++;
                    mockError('glasswave paged playlist primary request did not match');
                }
                const page = pagedTrackPage(0);
                paged.pageRows += page.data.length;
                saveState();
                return [200, page];
            }
            const offset = Number(params.get('offset'));
            paged.pageRequests++;
            paged.offsets.push(Number.isSafeInteger(offset) ? offset : -1);
            if (offset === 0) paged.zeroOffsetRequests++;
            const nested = params.get('include[library-songs]') === 'catalog,artists,albums';
            if (nested) paged.exactNestedIncludes++;
            if (params.get('opaque') === `glasswave-paged-${offset}`) paged.opaqueQueryRetained++;
            if (!Number.isSafeInteger(offset) || offset < performancePageSize ||
                offset % performancePageSize !== 0 ||
                params.get('limit') !== String(performancePageSize) ||
                params.get('include') !== 'catalog,artists' ||
                !nested ||
                params.get('opaque') !== `glasswave-paged-${offset}`) {
                paged.invalidPageRequests++;
                mockError('glasswave paged playlist page request did not match');
            }
            const page = pagedTrackPage(offset);
            paged.pageRows += page.data.length;
            saveState();
            return [200, page];
        }
        if (path === `/v1/me/library/playlists/${playlistFavorite}/tracks`) {
            isolation.favoriteTrackReads++;
            const limit = Number(params.get('limit'));
            const hasLimit = params.has('limit');
            const include = params.get('include');
            const nested = params.get('include[library-songs]');
            if ((hasLimit && (!Number.isSafeInteger(limit) || limit < 1 || limit > 100)) ||
                (params.has('include') && include !== 'catalog,artists' && include !== 'tracks') ||
                (params.has('include[library-songs]') && nested !== 'catalog,artists,albums')) {
                isolation.invalidFavoriteTrackReads++;
                mockError('favorite tracks isolation request had an invalid shape');
            }
            saveState();
            return [200, { data: favorite ? [song()] : [], next: null }];
        }
        if (path === `/v1/me/library/playlists/${playlistPerformance}`) {
            const include = params.get('include') ?? '';
            if (!include.split(',').includes('tracks')) {
                performance.metadataOnlyRequests++;
                saveState();
                return [200, { data: [performancePlaylist(false)] }];
            }
            performance.detailRequests++;
            const projected = params.get('include[library-songs]') === 'catalog,artists,albums';
            if (include !== 'tracks' || !projected) {
                performance.invalidDetailRequests++;
                mockError('performance playlist primary projection did not match');
            }
            performanceDetailMode = performance.detailRequests === 1 ? 'projected' : 'fallback';
            const hydrated = performanceDetailMode === 'projected';
            if (performanceDetailMode === 'projected')
                performance.projectedDetailRequests++;
            else {
                performance.fallbackDetailRequests++;
                performance.hydrationsBeforeFallback = performance.hydrationRequests;
                performance.pagesBeforeFallback = performance.trackPageRequests;
            }
            detailActive++;
            performance.maxDetailConcurrency =
                Math.max(performance.maxDetailConcurrency, detailActive);
            saveState();
            await delay(detailDelayMs);
            detailActive--;
            saveState();
            return [200, {
                data: [performancePlaylist(true, hydrated)],
            }];
        }
        if (path === `/v1/me/library/playlists/${playlistPerformance}/tracks`) {
            const offset = Number(params.get('offset'));
            performance.trackPageRequests++;
            performance.pageOffsets.push(Number.isSafeInteger(offset) ? offset : -1);
            if (!Number.isSafeInteger(offset) || offset < 0 || offset % performancePageSize !== 0 ||
                params.get('limit') !== String(performancePageSize) ||
                params.get('include') !== 'catalog,artists' ||
                params.get('include[library-songs]') !== 'catalog,artists,albums') {
                performance.invalidTrackPageRequests++;
                mockError('performance playlist page request did not match');
            }
            if (offset === 0) performance.zeroOffsetPageRequests++;
            const page = performanceTrackPage(offset, performanceDetailMode === 'projected');
            performance.trackPageRows += page.data.length;
            saveState();
            return [200, page];
        }
        if (path === `/v1/catalog/US/playlists/${playlistPerformanceCatalog}`) {
            performance.catalogDetailRequests++;
            if (params.get('include') !== 'tracks,catalog,artists' ||
                params.get('include[songs]') !== 'albums') {
                performance.invalidCatalogDetailRequests++;
                mockError('catalog performance playlist projection did not match');
            }
            return [200, { data: [catalogPerformancePlaylist()] }];
        }
        if (path === `/v1/catalog/JP/playlists/${playlistCatalog}`) {
            const actualKeys = [...params.keys()].sort().join('|');
            if (actualKeys !== 'l|platform' ||
                params.get('l') !== 'ja-JP' || params.get('platform') !== 'web') {
                mockError(`catalog playlist display request did not match: ${params.toString()}`);
            }
            isolation.playlistDisplayReads++;
            saveState();
            return [200, { data: [catalogPlaylist()] }];
        }
        if (path === `/v1/catalog/US/playlists/${playlistCatalog}`) {
            isolation.catalogPlaylistReads++;
            const actualKeys = [...params.keys()].sort().join('|');
            // Apple answers a resource request carrying "limit" with 400 ("Limit may not be
            // supplied on this request", observed 2026-11-13).
            const detailKeys = ['include', 'include[songs]', 'l'].sort().join('|');
            const artworkKeys = ['l'].sort().join('|');
            const detail = actualKeys === detailKeys;
            const artwork = actualKeys === artworkKeys;
            if ((!detail && !artwork) ||
                (actualKeys !== artworkKeys &&
                 (params.get('include') !== 'tracks,catalog,artists' ||
                  params.get('include[songs]') !== 'albums')) ||
                params.get('l') !== 'zh-Hans-CN') {
                isolation.invalidCatalogPlaylistReads++;
                mockError(`catalog playlist detail request did not match: ${params.toString()}`);
            }
            saveState();
            return [200, { data: [catalogPlaylist()] }];
        }
        if (path === `/v1/catalog/US/playlists/${mutationPlaylistCatalog}`) {
            const actualKeys = [...params.keys()].sort().join('|');
            if (actualKeys !== 'l' || params.get('l') !== 'zh-Hans-CN') {
                mockError(
                    `mutation catalog playlist request did not match: ${params.toString()}`,
                );
            }
            saveState();
            return [200, { data: [mutationCatalogPlaylist()] }];
        }
        if (path === `/v1/me/library/playlists/${playlistFavorite}`) return [200, { data: [favoritePlaylist()] }];
        if (path === `/v1/me/library/playlists/${playlistSaved}`)
            return playlistIsSaved ? [200, { data: [savedPlaylist()] }] : [404, {}];
        if (path === `/v1/me/library/playlists/${mutationPlaylistLibrary}`)
            return mutationPlaylistSaved && !accountSwitched
                ? [200, { data: [mutationPlaylist()] }]
                : [404, {}];
        if (path === `/v1/me/library/playlists/${playlistScratch}`)
            return scratch ? [200, { data: [scratchPlaylist()] }] : [404, {}];
        if (path === `/v1/me/library/artists/${favoriteArtistLibrary}`) {
            if (params.get('include') !== 'catalog' || params.get('platform') !== 'web') {
                mockError('favorite artist detail request did not match');
                return [400, { error: 'invalid favorite artist detail request' }];
            }
            return [200, { data: [favoriteArtistResource()] }];
        }
        if (path === '/v1/me/library/songs') {
            const ids = (params.get('ids') ?? '').split(',').filter(Boolean);
            if (ids.some((id) => id.startsWith(`i.song.glasswave-enrichment.${enrichmentTag}.`))) {
                enrichment.hydrationRequests++;
                enrichment.hydrationBatchSizes.push(ids.length);
                enrichment.hydrationIds.push(...ids);
                enrichmentHydrationActive++;
                enrichment.activeHydrationRequests = enrichmentHydrationActive;
                enrichment.maxHydrationConcurrency =
                    Math.max(enrichment.maxHydrationConcurrency, enrichmentHydrationActive);

                const keys = [...params.keys()].sort().join('|');
                const expectedKeys =
                    ['ids', 'include', 'include[songs]', 'platform', 'l'].sort().join('|');
                const expectedIds = enrichmentHydrationIndexes.map((index) =>
                    `i.song.glasswave-enrichment.${enrichmentTag}.${index}`);
                if (keys !== expectedKeys || ids.length !== expectedIds.length ||
                    ids.join(',') !== expectedIds.join(',') ||
                    params.get('include') !== 'catalog,albums,artists' ||
                    params.get('include[songs]') !== 'albums' ||
                    params.get('platform') !== 'web') {
                    enrichment.invalidHydrationRequests++;
                    mockError(
                        `glasswave enrichment hydration request did not match: ${params.toString()}`,
                    );
                }

                const seen = new Set();
                const rows = [];
                for (const id of ids) {
                    if (seen.has(id)) {
                        enrichment.hydrationDuplicateIds++;
                        enrichment.invalidHydrationRequests++;
                        mockError('glasswave enrichment hydration repeated an id');
                    }
                    seen.add(id);
                    const prefix = `i.song.glasswave-enrichment.${enrichmentTag}.`;
                    const suffix = id.startsWith(prefix) ? id.slice(prefix.length) : '';
                    const index = /^(0|[1-9][0-9]*)$/.test(suffix) ? Number(suffix) : -1;
                    if (!enrichmentUniqueIndexes.includes(index)) {
                        enrichment.invalidHydrationRequests++;
                        mockError('glasswave enrichment hydration used an unexpected id');
                        continue;
                    }
                    if (index === enrichmentKnownIndex) {
                        enrichment.knownIdRequests++;
                        enrichment.invalidHydrationRequests++;
                        mockError('glasswave enrichment requested a fully known row');
                    }
                    rows.push(enrichmentBaseSong(index, true));
                }
                enrichment.hydrationRows += rows.length;
                saveState();
                await delay(enrichmentHydrationDelayMs);
                enrichmentHydrationActive--;
                enrichment.activeHydrationRequests = enrichmentHydrationActive;
                saveState();
                return [200, { data: rows, next: null }];
            }
            if (ids.some((id) => id.startsWith('i.song.performance.'))) {
                performance.hydrationRequests++;
                performance.hydrationBatchSizes.push(ids.length);
                hydrationActive++;
                performance.maxHydrationConcurrency =
                    Math.max(performance.maxHydrationConcurrency, hydrationActive);
                if (ids.length < 1 || ids.length > 20 ||
                    params.get('include') !== 'catalog,albums,artists' ||
                    params.get('platform') !== 'web') {
                    performance.invalidHydrationRequests++;
                    mockError('performance album hydration request did not match');
                }
                const rows = [];
                for (const id of ids) {
                    const match = /^i\.song\.performance\.(0|[1-9][0-9]*)$/.exec(id);
                    const index = match ? Number(match[1]) : -1;
                    if (index < 0 || index >= performanceUniqueCount) {
                        performance.invalidHydrationRequests++;
                        mockError('performance album hydration used an unexpected song id');
                        continue;
                    }
                    if (hydrationIds.has(id)) performance.hydrationDuplicateIds++;
                    hydrationIds.add(id);
                    rows.push(performanceSong(index, true));
                }
                saveState();
                await delay(hydrationDelayMs);
                hydrationActive--;
                saveState();
                return [200, { data: rows, next: null }];
            }
            return [200, {
                data: [
                    song(),
                    regionalSourceUploadedSong(),
                    regionalSourceLibraryOnlySong(),
                ],
                next: null,
            }];
        }
        if (path === '/v1/me/library/albums') {
            if (scenario === 'mutation-reconciliation' && mutationFixture.staleReadArmed &&
                !mutationFixture.active &&
                !mutationFixture.preAlbumSnapshotSeen) {
                const oldData = [regionalSourceAlbum()];
                if (albumSaved) oldData.unshift(libraryAlbum());
                if (mutationAlbumSaved) oldData.unshift(mutationAlbum());
                mutationFixture.preAlbumSnapshotSeen = true;
                saveState();
                await mutationGate;
                await delay(mutationDelayMs);
                return [200, { data: oldData, next: null }];
            }
            const data = [regionalSourceAlbum()];
            if (albumSaved) data.unshift(libraryAlbum());
            if (scenario === 'mutation-reconciliation' && mutationAlbumSaved && !accountSwitched)
                data.unshift(mutationAlbum());
            return [200, { data, next: null }];
        }
        if (path === `/v1/me/library/albums/${mutationAlbumLibrary}`) {
            isolation.mutationLibraryAlbumReads++;
            saveState();
            return [404, { errors: [{ status: '404', title: 'Stale library album' }] }];
        }
        if (path === `/v1/me/library/albums/${mutationAlbumLibrary}/tracks`) {
            isolation.mutationLibraryAlbumTrackReads++;
            saveState();
            return [404, { errors: [{ status: '404', title: 'Stale library album tracks' }] }];
        }
        if (path === `/v1/me/library/albums/${albumLibrary}`) {
            if (!albumSaved || params.get('include') !== 'tracks,catalog,artists' ||
                params.get('include[library-songs]') !== 'catalog,artists,albums') {
                mockError('library album detail request did not match');
                return [404, {}];
            }
            return [200, { data: [libraryAlbum()] }];
        }
        if (path === `/v1/me/library/albums/${albumLibrary}/tracks`) {
            if (!albumSaved || params.get('limit') !== '100' ||
                params.get('include[library-songs]') !== 'catalog,artists,albums' ||
                params.get('include[songs]') !== 'albums') {
                mockError('library album tracks request did not match');
                return [404, {}];
            }
            return [200, { data: [song()], next: null }];
        }
        // The restored owner Now Playing item can request its real album cover even
        // while the provider contract bridge is isolated. Accept only that exact
        // opaque art-resolver shape; album/provider detail uses a different include.
        if (path === `/v1/me/library/albums/${ownerPlaybackAlbumLibrary}` &&
            [...params.keys()].sort().join('|') === 'include|l' &&
            params.get('include') === 'catalog' && params.get('l') === 'zh-Hans-CN') {
            isolation.backgroundAlbumArtReads++;
            saveState();
            return [404, {}];
        }
        if (path === '/v1/me/library/artists') {
            const rows = [regionalSourceArtist(), regionalSourceRetryArtist()];
            if (scenario === 'mutation-reconciliation') {
                rows.push(sourcePageArtistResource(), staleArtistResource(),
                          favoriteArtistResource());
            }
            const oldData = {
                data: rows,
                next: null,
            };
            if (scenario === 'mutation-reconciliation' && artistFixture.staleReadArmed &&
                !artistFixture.staleReadHeld) {
                artistFixture.staleReadHeld = true;
                saveState();
                await artistPreferenceGate;
                await delay(mutationDelayMs);
                artistFixture.staleReadCompleted = true;
                saveState();
                return [200, oldData];
            }
            if (scenario === 'mutation-reconciliation' && mutationFixture.staleReadArmed &&
                !mutationFixture.active &&
                !mutationFixture.preArtistSnapshotSeen) {
                mutationFixture.preArtistSnapshotSeen = true;
                saveState();
                await mutationGate;
                await delay(mutationDelayMs);
                return [200, oldData];
            }
            if (accountControl.armRequested && !accountSwitched &&
                accountControl.accountRequestsAfterArm > 0) {
                accountControl.oldArtistHeld = true;
                saveState();
                await waitFor(() => accountSwitched, 5000);
                await delay(100);
            }
            if (scenario === 'mutation-reconciliation' && mutationFixture.active) {
                if (failArtistRefreshAfterMutation > 0) {
                    failArtistRefreshAfterMutation--;
                    saveState();
                    return [400, { errors: [{ status: '400', title: 'Injected mutation refresh failure' }] }];
                }
                if (failArtistRefreshAfterPlaylistSave > 0) {
                    failArtistRefreshAfterPlaylistSave--;
                    saveState();
                    return [400, { errors: [{ status: '400', title: 'Injected playlist refresh failure' }] }];
                }
            }
            return [200, oldData];
        }
        if (path.startsWith(`/v1/me/library/playlists/${playlistEnrichment}`)) {
            enrichment.invalidTrackRequests++;
            mockError('glasswave enrichment used an unexpected playlist endpoint');
            return [404, { data: [] }];
        }
        const unknownPlaylistPrefix = '/v1/me/library/playlists/';
        if (path.startsWith(unknownPlaylistPrefix) &&
            !path.slice(unknownPlaylistPrefix.length).includes('/')) {
            isolation.unknownPlaylistReads++;
            saveState();
            return [404, { data: [] }];
        }
        if (path === '/v1/catalog/US') return [200, resourceResolution(params)];
        // The dev profile can restore artwork requests for its previous playing track.
        if (path.startsWith('/v1/catalog/US/songs/')) return [404, {}];
    }
    if (path === '/v1/me/favorites') {
        const artistId = params.get('ids[artists]');
        if (artistId !== null) {
            const locale = params.get('l');
            if (!Object.hasOwn(artistFavorites, artistId) || body !== null ||
                params.get('platform') !== 'web' ||
                !['POST', 'DELETE'].includes(method) || params.size !== 3 ||
                !['en-US', 'zh-Hans-CN'].includes(locale)) {
                mockError('favorite artist mutation used an unexpected identity or query');
                return [400, { error: 'invalid artist favorite request' }];
            }
            if (method === 'POST') {
                mutationWrites.artistFavoriteAdd++;
                artistFavorites[artistId] = true;
                artistFixture.writes.push({ id: artistId, method, status: 202 });
                saveState();
                return [202];
            }
            if (method === 'DELETE') {
                mutationWrites.artistFavoriteRemove++;
                artistFavorites[artistId] = false;
                artistFixture.writes.push({ id: artistId, method, status: 204 });
                saveState();
                return [204];
            }
        }
        const id = params.get('ids[songs]');
        if (id !== songCatalog || params.get('platform') !== 'web' || body !== null) {
            mockError('favorite mutation used an unexpected song id');
            return [400, { error: 'invalid song id' }];
        }
        if (method === 'POST') {
            writes.favoriteAdd++;
            favorite = true;
            return [202];
        }
        if (method === 'DELETE') {
            writes.favoriteRemove++;
            favorite = false;
            return [204];
        }
    }
    if (method === 'POST' && path === '/v1/me/library/playlists') {
        if (body === null || Object.keys(body).join(',') !== 'attributes' ||
            Object.keys(body.attributes ?? {}).join(',') !== 'name' ||
            body.attributes?.name !== 'provider-contract-scratch') {
            mockError('playlist create body did not match');
            return [400, { error: 'invalid create body' }];
        }
        writes.playlistCreate++;
        scratch = { name: body.attributes.name, tracks: [] };
        return [201, { data: [scratchPlaylist()] }];
    }
    if (method === 'PATCH' && path === `/v1/me/library/playlists/${playlistScratch}`) {
        const name = body?.attributes?.name;
        if (!scratch || params.get('platform') !== 'web' ||
            body === null || Object.keys(body).join(',') !== 'attributes' ||
            Object.keys(body.attributes ?? {}).join(',') !== 'name' ||
            typeof name !== 'string') {
            mockError('playlist rename body did not match');
            return [400, { error: 'invalid rename body' }];
        }
        if (name === 'fail-rename') {
            writes.renameFailure++;
            saveState();
            return [409, { error: 'injected rename failure' }];
        }
        if (name !== 'provider-contract-renamed') {
            mockError('unexpected successful rename name');
            return [400, { error: 'invalid rename name' }];
        }
        writes.playlistRename++;
        scratch.name = name;
        return [200, { data: [scratchPlaylist()] }];
    }
    if (method === 'POST' && path === `/v1/me/library/playlists/${playlistScratch}/tracks`) {
        const data = body?.data;
        if (!scratch || body === null || Object.keys(body).join(',') !== 'data' ||
            !Array.isArray(data) || data.length !== 1 ||
            data[0]?.id !== songCatalog || data[0]?.type !== 'songs') {
            mockError('playlist add body did not match');
            return [400, { error: 'invalid add body' }];
        }
        writes.playlistAdd++;
        scratch.tracks.push(songLibrary);
        return [201, {}];
    }
    if (method === 'DELETE' && path === `/v1/me/library/playlists/${playlistScratch}/tracks`) {
        if (!scratch || body !== null || params.get('ids[library-songs]') !== songLibrary ||
            params.get('mode') !== 'all') {
            mockError('playlist remove query did not match');
            return [400, { error: 'invalid remove query' }];
        }
        writes.playlistRemove++;
        scratch.tracks = scratch.tracks.filter((id) => id !== songLibrary);
        return [204];
    }
    if (method === 'DELETE' && path === `/v1/me/library/playlists/${playlistScratch}`) {
        if (!scratch || body !== null) {
            mockError('scratch playlist was already absent');
            return [404, { error: 'missing scratch playlist' }];
        }
        writes.playlistDelete++;
        scratch = null;
        return [204];
    }
    if (method === 'POST' && path === '/v1/me/library' && params.get('ids[albums]') === albumCatalog) {
        if (body !== null) {
            mockError('album save unexpectedly included a body');
            return [400, { error: 'unexpected body' }];
        }
        writes.albumSave++;
        albumSaved = true;
        return [202];
    }
    if (method === 'POST' && path === '/v1/me/library' &&
        params.get('ids[albums]') === mutationAlbumCatalog) {
        if (body !== null) {
            mockError('mutation album save unexpectedly included a body or library id');
            return [400, { error: 'unexpected mutation album body' }];
        }
        mutationWrites.albumSave++;
        mutationAlbumSaved = true;
        return [202];
    }
    if (method === 'DELETE' && path === `/v1/me/library/albums/${albumLibrary}`) {
        if (body !== null || params.size !== 0) {
            mockError('album remove query or body did not match');
            return [400, { error: 'invalid album remove' }];
        }
        writes.albumRemove++;
        albumSaved = false;
        return [204];
    }
    if (method === 'DELETE' && path === `/v1/me/library/albums/${mutationAlbumLibrary}`) {
        if (body !== null || params.size !== 0) {
            mockError('mutation album remove query or body did not match');
            return [400, { error: 'invalid mutation album remove' }];
        }
        await waitFor(() => mutationFixture.preAlbumSnapshotSeen &&
                            mutationFixture.preArtistSnapshotSeen, 5000);
        mutationWrites.albumRemove++;
        mutationFixture.active = true;
        mutationAlbumSaved = false;
        failArtistRefreshAfterMutation = 1;
        releaseMutationGate();
        saveState();
        return [204];
    }
    if (method === 'POST' && path === '/v1/me/library' && params.get('ids[playlists]') === playlistCatalog) {
        if (body !== null) {
            mockError('playlist save unexpectedly included a body');
            return [400, { error: 'unexpected body' }];
        }
        writes.playlistSave++;
        playlistIsSaved = true;
        return [202];
    }
    if (method === 'POST' && path === '/v1/me/library' &&
        params.get('ids[playlists]') === mutationPlaylistCatalog) {
        if (body !== null) {
            mockError('mutation playlist save unexpectedly included body');
            return [400, { error: 'unexpected mutation playlist body' }];
        }
        mutationWrites.playlistSave++;
        mutationPlaylistSaved = true;
        failArtistRefreshAfterPlaylistSave = 1;
        return [202];
    }
    if (method === 'DELETE' && path === `/v1/me/library/playlists/${playlistSaved}`) {
        if (body !== null || params.size !== 0) {
            mockError('playlist remove query or body did not match');
            return [400, { error: 'invalid playlist remove' }];
        }
        writes.playlistRemoveFromLibrary++;
        playlistIsSaved = false;
        return [204];
    }
    if (method === 'DELETE' && path ===
        `/v1/me/library/playlists/${mutationPlaylistLibrary}`) {
        if (body !== null || params.size !== 0) {
            mockError('mutation playlist remove query or body did not match');
            return [400, { error: 'invalid mutation playlist remove' }];
        }
        mutationWrites.playlistRemove++;
        mutationPlaylistSaved = false;
        return [204];
    }
    return null;
}

async function readJson(req) {
    let bytes = '';
    for await (const chunk of req) {
        bytes += chunk;
        if (bytes.length > 65536) throw new Error('request body too large');
    }
    return bytes ? JSON.parse(bytes) : null;
}

const server = http.createServer(async (req, res) => {
    saveState();
    if (req.headers['x-api-token'] !== token) {
        reply(res, 403, { error: 'unauthorized' });
        return;
    }
    const url = new URL(req.url ?? '/', 'http://127.0.0.1');
    if (req.method === 'GET' && url.pathname === '/api/v1/glasswave/testing') {
        reply(res, 200, { ok: true, nonce, ...publicState() });
        return;
    }
    if (req.method === 'POST' && url.pathname === '/api/v1/glasswave/testing/arm-account-switch') {
        accountControl.armRequested = true;
        accountControl.accountRequestsAfterArm = 0;
        saveState();
        reply(res, 200, { ok: true });
        return;
    }
    if (req.method === 'POST' &&
        url.pathname === '/api/v1/glasswave/testing/arm-mutation-stale-reads') {
        if (scenario !== 'mutation-reconciliation') {
            reply(res, 409, { error: 'mutation stale reads require mutation-reconciliation' });
            return;
        }
        mutationFixture.staleReadArmed = true;
        mutationFixture.preAlbumSnapshotSeen = false;
        mutationFixture.preArtistSnapshotSeen = false;
        saveState();
        reply(res, 200, { ok: true });
        return;
    }
    if (req.method === 'POST' &&
        url.pathname === '/api/v1/glasswave/testing/arm-artist-preference-stale-read') {
        if (scenario !== 'mutation-reconciliation' || artistFixture.staleReadHeld) {
            reply(res, 409, { error: 'artist stale read cannot be armed' });
            return;
        }
        artistFixture.staleReadArmed = true;
        saveState();
        reply(res, 200, { ok: true });
        return;
    }
    if (req.method === 'POST' &&
        url.pathname === '/api/v1/glasswave/testing/arm-playlist-remove-stale-snapshot') {
        if (scenario !== 'mutation-reconciliation' || playlistAuthority.staleArmed) {
            reply(res, 409, { error: 'playlist stale snapshot cannot be armed' });
            return;
        }
        playlistAuthority.staleArmed = true;
        playlistAuthority.staleServed = false;
        playlistAuthority.absentServed = false;
        playlistAuthority.externalPresent = false;
        saveState();
        reply(res, 200, { ok: true });
        return;
    }
    if (req.method === 'POST' &&
        url.pathname === '/api/v1/glasswave/testing/set-playlist-external-present') {
        if (scenario !== 'mutation-reconciliation' || !playlistAuthority.absentServed) {
            reply(res, 409, { error: 'playlist external present cannot be set yet' });
            return;
        }
        playlistAuthority.externalPresent = true;
        mutationPlaylistSaved = true;
        saveState();
        reply(res, 200, { ok: true });
        return;
    }
    if (req.method === 'POST' &&
        url.pathname === '/api/v1/glasswave/testing/release-artist-preference-stale-read') {
        if (!artistFixture.staleReadHeld) {
            reply(res, 409, { error: 'artist stale read was not held' });
            return;
        }
        releaseArtistPreferenceGate();
        saveState();
        reply(res, 200, { ok: true });
        return;
    }
    if (req.method === 'POST' && url.pathname === '/api/v1/glasswave/testing/switch-account') {
        if (!accountControl.armRequested) {
            reply(res, 409, { error: 'account switch was not armed' });
            return;
        }
        accountControl.switchRequested = true;
        accountSwitched = true;
        saveState();
        reply(res, 200, { ok: true });
        return;
    }
    const startupMode = url.pathname.match(/^\/api\/v1\/glasswave\/testing\/startup-mode\/([^/]+)$/);
    if (req.method === 'POST' && startupMode) {
        const mode = decodeURIComponent(startupMode[1]);
        if (scenario !== 'startup-cache' ||
            !['seed', 'verify-slow', 'account-b', 'signed-out', 'transient'].includes(mode)) {
            reply(res, 409, { error: 'invalid startup mode transition' });
            return;
        }
        startup.accountMode = mode;
        startup.phase++;
        startup.accountRequests = 0;
        startup.accountCompleted = 0;
        startup.accountInFlight = 0;
        startup.authCompleted = false;
        startup.drmRequests = 0;
        startup.drmCompleted = false;
        startup.playlistCollectionCompleted = false;
        startup.albumCollectionCompleted = false;
        startup.artistCollectionCompleted = false;
        startup.playlistRequests = 0;
        startup.playlistPageRequests = 0;
        startup.playlistPageAfterAuthBeforeMembership = 0;
        startup.favoritePageRequests = 0;
        startup.favoritePageAfterAuthBeforeMembership = 0;
        startup.displaySongRequests = 0;
        startup.displaySongMissingCounterparts = 0;
        startup.invalidDisplaySongRequests = 0;
        startup.listRequests = 0;
        startup.enrichmentRequests = 0;
        saveState();
        reply(res, 200, { ok: true, mode });
        return;
    }
    if (req.method === 'GET' && url.pathname === '/api/v1/glasswave/account') {
        if (scenario === 'startup-cache') {
            startup.accountRequests++;
            startup.accountInFlight++;
            saveState();
            const mode = startup.accountMode;
            const phase = startup.phase;
            await delay(400);
            if (phase === startup.phase) {
                startup.authCompleted = true;
                startup.accountInFlight--;
                startup.accountCompleted++;
            }
            saveState();
            if (mode === 'transient') {
                reply(res, 503, { error: 'injected account verification failure' });
                return;
            }
            const signedIn = mode !== 'signed-out';
            reply(res, 200, {
                signedIn,
                accountId: signedIn ? (mode === 'account-b' ? startupAccountB : startupAccountA) : '',
                storefront: signedIn ? 'US' : '',
                name: signedIn ? 'Apple Startup Contract' : '',
            });
            return;
        }
        if (accountControl.armRequested && !accountSwitched)
            accountControl.accountRequestsAfterArm++;
        saveState();
        reply(res, 200, {
            signedIn: true,
            accountId: accountSwitched ? 'mock-account-b' : 'mock-account-a',
            storefront: 'US',
            name: 'Provider Contract Mock',
        });
        return;
    }
    if (req.method === 'POST' && url.pathname === '/api/v1/drm/logout') {
        reply(res, 200, { ok: true });
        return;
    }
    // The dev instance's current track may ask for lyrics while the contract runs; that is
    // not a provider operation, so it is answered with no results instead of being counted.
    if (req.method === 'POST' && url.pathname === '/api/v1/glasswave/lyrics') {
        reply(res, 200, { results: [] });
        return;
    }
    // foobar2000 owns playback-state restoration. This library fixture has no audio.
    if (req.method === 'POST' && url.pathname === '/api/v1/playback') {
        reply(res, 503, { error: 'audio disabled in provider contract fixture' });
        return;
    }
    if (req.method === 'GET' && url.pathname === '/api/v1/drm/status') {
        if (scenario === 'startup-cache') {
            startup.drmRequests++;
            const phase = startup.phase;
            if (startupSlowMembership()) await delay(1500);
            else await delay(100);
            if (phase === startup.phase) startup.drmCompleted = true;
            saveState();
        }
        reply(res, 200, {
            available: true,
            state: {
                manager: 'mock',
                process: 'running',
                authentication: 'logged_in',
                fairplay: 'ready',
                session: 'mock-session',
            },
        });
        return;
    }
    if (req.method === 'POST' && url.pathname === '/api/v1/glasswave/apple') {
        let wrapper;
        try {
            wrapper = await readJson(req);
        } catch {
            mockError('invalid Apple proxy JSON');
            reply(res, 400, { error: 'invalid JSON' });
            return;
        }
        const method = String(wrapper?.method ?? '').toUpperCase();
        const path = String(wrapper?.path ?? '');
        if (scenario === 'startup-cache') {
            const requestUrl = new URL(path, 'http://127.0.0.1');
            if (requestUrl.pathname === '/v1/me/library/playlists') {
                startup.playlistRequests++;
                startup.listRequests++;
            } else if (requestUrl.searchParams.has('ids[songs]') ||
                       requestUrl.searchParams.has('ids[albums]') ||
                       requestUrl.searchParams.has('ids[artists]')) {
                startup.enrichmentRequests++;
            }
        }
        const result = await dispatchApple(method, path, wrapper?.body ?? null);
        if (result) {
            const requestUrl = new URL(path, 'http://127.0.0.1');
            const requestPath = requestUrl.pathname;
            if (scenario === 'mutation-reconciliation' &&
                ['POST', 'DELETE'].includes(method) &&
                (requestPath === '/v1/me/favorites' || requestPath === '/v1/me/library' ||
                 requestPath.startsWith('/v1/me/library/'))) {
                mutationResponses.push({
                    method,
                    path: requestPath,
                    resourceId: requestUrl.searchParams.get('ids[artists]') ??
                        requestUrl.searchParams.get('ids[albums]') ??
                        requestUrl.searchParams.get('ids[playlists]') ??
                        requestUrl.searchParams.get('ids[songs]') ??
                        requestPath.slice(requestPath.lastIndexOf('/') + 1),
                    status: result[0],
                    bodyBytes: result[1] === undefined ? 0 : Buffer.byteLength(JSON.stringify(result[1])),
                });
            }
            reply(res, result[0], result[1]);
        } else {
            unexpected(method, new URL(path, 'http://127.0.0.1').pathname);
            reply(res, 404, { error: 'unexpected Apple mock request' });
        }
        saveState();
        return;
    }
    unexpected(req.method ?? 'UNKNOWN', url.pathname);
    reply(res, 404, { error: 'unexpected mock request' });
});

server.listen(0, '127.0.0.1', () => {
    const port = server.address().port;
    writeJsonFile(bridgeFile, { url: `http://127.0.0.1:${port}`, token });
    saveState();
});

const shutdown = () => server.close(() => process.exit(0));
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
