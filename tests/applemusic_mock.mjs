// Credential-free integration fixture for the real foobar2000 Apple input.
import http from 'node:http';
import fs from 'node:fs';
import crypto from 'node:crypto';

const [
    fixture,
    bridgePath,
    statePath,
    codecArgument = 'ALAC',
    slowArgument = '',
    runIdArgument = '',
    mappingMode = 'Normal'
] = process.argv.slice(2);
if (!fixture || !bridgePath || !statePath) throw new Error('fixture, bridge and state output required');
const codec = codecArgument.toUpperCase();
if (codec !== 'ALAC' && codec !== 'AAC' && codec !== 'ATMOS')
    throw new Error('codec must be ALAC, AAC or ATMOS');
if (slowArgument && slowArgument !== 'Normal' && slowArgument !== 'SlowStreamTest')
    throw new Error('stream mode must be Normal or SlowStreamTest');
const slowStreamTest = slowArgument === 'SlowStreamTest';
if (runIdArgument && !/^[A-Za-z0-9_-]{1,128}$/.test(runIdArgument))
    throw new Error('mock run id is invalid');
const proxyAudioUrl = (process.env.GLASSWAVE_UPLOAD_MP4_UPSTREAM_URL ?? '').trim();
const proxyAudioToken = (process.env.GLASSWAVE_UPLOAD_MP4_UPSTREAM_TOKEN ?? '').trim();
const proxySourceVersion = (process.env.GLASSWAVE_UPLOAD_MP4_SOURCE_VERSION ?? '').trim();
const proxyDurationMs = Number(process.env.GLASSWAVE_UPLOAD_MP4_DURATION_MS ?? '0');
const proxyMode = proxyAudioUrl !== '';
if (proxyMode) {
    const parsed = new URL(proxyAudioUrl);
    if (parsed.protocol !== 'http:' || !['127.0.0.1', '::1', 'localhost'].includes(parsed.hostname) ||
        proxyAudioToken === '' || proxySourceVersion === '' ||
        !Number.isSafeInteger(proxyDurationMs) || proxyDurationMs <= 0)
        throw new Error('progressive proxy requires a loopback HTTP URL, token, source version and duration');
}
const media = proxyMode ? null : fs.readFileSync(fixture);
const token = crypto.randomBytes(32).toString('hex');
const fixtureTransactionTag = crypto.randomBytes(16).toString('hex');
const mockRunId = runIdArgument || `process-${process.pid}-${Date.now().toString(36)}`;
const sourceVersion = proxyMode ? proxySourceVersion : `glasswave-mock-v1:${mockRunId}`;
const fixtureAssetId = 'glasswave-test';
const fixtureAccountId = '9000000001';
const fixtureAccountName = 'Glasswave Fixture';
const fixtureStorefront = 'US';
// Opt-in fixture only: the client must perform recovery itself.
const mappingModes = [
    'Normal',
    'StaleCatalog',
    'InvalidSessionMetadata',
    'PreviewRejected',
    'WrongLibrary',
    'WrongReportingId'
];
if (!mappingModes.includes(mappingMode))
    throw new Error('invalid mapping mode');
if (mappingMode !== 'Normal' && codec === 'ATMOS') throw new Error('mapping fixtures support ALAC or AAC');
const staleAssetId = '1237064886';
const libraryId = 'i.GlasswaveStaleFixture';
const reportingId = '1865440142';
const fixturePlaybackAssetId = mappingMode === 'Normal' ? fixtureAssetId : staleAssetId;
const libraryPath = `/v1/me/library/songs/${libraryId}`;
const libraryLookupPaths = [];
const rejectedAssetIds = [];
const playbackAttempts = [];
const rejectedBackgroundRequests = [];
let authoritativeMappingReturned = false;
let mappingAccepted = false;
let deleteCount = 0;
let successfulDeleteCount = 0;
let unexpectedDeleteCount = 0;
let audioGetCount = 0;
let nextSession = 1;
const playbackAssetIds = [];
const activeSessions = new Set();
const deletedSessions = new Set();
const sessionRecords = [];
let firstAudioRead = null;
let serverDownloadFinished = null;
let clientDisconnected = null;
let preferAtmos = null;
const writeState = () => {
    fs.writeFileSync(statePath, JSON.stringify({
        deleteCount,
        successfulDeleteCount,
        unexpectedDeleteCount,
        audioGetCount,
        playbackSessionCount: sessionRecords.length,
        activeSessionCount: activeSessions.size,
        deletedSessionCount: deletedSessions.size,
        mockRunId,
        sourceVersion,
        fixtureAccountId,
        fixtureStorefront,
        sessionRecords,
        fixtureAssetId,
        fixturePlaybackAssetId,
        playbackAssetIds,
        mappingMode,
        staleAssetId,
        libraryId,
        reportingId,
        libraryLookupPaths,
        libraryLookupCount: libraryLookupPaths.length,
        rejectedAssetIds,
        playbackAttempts,
        rejectedBackgroundRequests,
        authoritativeMappingReturned,
        mappingAccepted,
        firstAudioRead,
        serverDownloadFinished,
        clientDisconnected,
        codec,
        preferAtmos,
        proxyMode,
        proxySourceVersion: proxyMode ? proxySourceVersion : null
    }), 'utf8');
};
const waitUntil = (targetMs) => new Promise((resolve) => {
    const timeoutMs = Math.max(0, targetMs - Date.now());
    setTimeout(resolve, timeoutMs);
});
const endResponse = (res) => new Promise((resolve) => res.end(resolve));
const noteAudioRead = () => {
    if (firstAudioRead !== null) return;
    firstAudioRead = Date.now();
    writeState();
};
const proxySessionAudio = (req, res) => {
    if (req.method !== 'GET' || (req.headers.range ?? '') !== '') {
        res.writeHead(416, { 'Content-Range': 'bytes */*', 'Accept-Ranges': 'none' }).end();
        return;
    }
    noteAudioRead();
    let completed = false;
    const upstream = http.get(proxyAudioUrl, {
        headers: { 'X-Glasswave-Fixture': proxyAudioToken }
    }, (upstreamResponse) => {
        const headers = {};
        for (const name of ['content-type', 'accept-ranges', 'content-range']) {
            const value = upstreamResponse.headers[name];
            if (value !== undefined) headers[name] = value;
        }
        res.writeHead(upstreamResponse.statusCode ?? 502, headers);
        upstreamResponse.once('end', () => {
            completed = true;
            if (serverDownloadFinished === null) serverDownloadFinished = Date.now();
            writeState();
        });
        upstreamResponse.once('error', () => {
            if (!res.destroyed) res.destroy();
        });
        upstreamResponse.pipe(res);
    });
    upstream.once('error', () => {
        if (!res.headersSent) res.writeHead(502);
        if (!res.destroyed) res.end();
    });
    res.once('close', () => {
        if (completed || clientDisconnected !== null) return;
        clientDisconnected = Date.now();
        upstream.destroy();
        writeState();
    });
};
writeState();
const server = http.createServer(async (req, res) => {
    if (req.headers['x-api-token'] !== token) {
        res.writeHead(401).end();
        return;
    }
    const json = (value) => {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(value));
    };
    const requestUrl = new URL(req.url, 'http://127.0.0.1');
    const pathname = requestUrl.pathname;
    if (pathname === '/api/v1/glasswave/account') {
        return json({
            signedIn: true,
            accountId: fixtureAccountId,
            name: fixtureAccountName,
            storefront: fixtureStorefront
        });
    }
    if (pathname === '/api/v1/drm/status') return json({ available: false, state: {} });
    if (req.headers['x-glasswave-probe-transaction'] !== fixtureTransactionTag) {
        let body = '';
        for await (const bytes of req) {
            body += bytes;
            if (body.length > 8192) return res.writeHead(413).end();
        }
        let request = {};
        try { request = body ? JSON.parse(body) : {}; } catch { }
        rejectedBackgroundRequests.push({
            method: req.method,
            path: req.url,
            assetId: request.assetId ?? null,
            applePath: request.path ?? null,
            status: 403
        });
        writeState();
        return res.writeHead(403).end();
    }
    if (req.method === 'GET' && [
        '/v1/me/library/songs',
        '/v1/me/library/playlists',
        '/v1/me/library/albums',
        '/v1/me/library/artists'
    ].includes(pathname)) {
        return json({ data: [], next: null, meta: { total: 0 } });
    }
    if (mappingMode !== 'Normal' && pathname === '/api/v1/glasswave/apple' &&
        req.method === 'POST') {
        let body = '';
        for await (const bytes of req) {
            body += bytes;
            if (body.length > 8192) return res.writeHead(413).end();
        }
        const bridgeRequest = JSON.parse(body);
        const targetPath = String(bridgeRequest.path ?? '');
        const targetUrl = new URL(targetPath, 'http://127.0.0.1');
        libraryLookupPaths.push(targetPath);
        const validQuery = targetUrl.searchParams.get('include') === 'catalog' &&
            [...targetUrl.searchParams.keys()].every((key) => key === 'include' || key === 'l');
        if (bridgeRequest.method !== 'GET' || targetUrl.pathname !== libraryPath || !validQuery ||
            !rejectedAssetIds.includes(staleAssetId)) {
            writeState();
            return res.writeHead(404).end();
        }
        authoritativeMappingReturned = true;
        writeState();
        return json({ data: [{
            id: mappingMode === 'WrongLibrary' ? 'i.wrong.library.id' : libraryId,
            type: 'library-songs',
            attributes: {
                name: 'Glasswave metadata title',
                artistName: 'Glasswave metadata artist',
                playParams: {
                    id: libraryId,
                    kind: 'song',
                    isLibrary: true,
                    catalogId: staleAssetId,
                    reporting: true,
                    reportingId: mappingMode === 'WrongReportingId' ? staleAssetId : reportingId
                }
            },
            relationships: { catalog: { data: [] } }
        }] });
    }
    if (pathname === '/api/v1/playback' && req.method === 'POST') {
        let body = '';
        for await (const bytes of req) {
            body += bytes;
            if (body.length > 4096) return res.writeHead(413).end();
        }
        const request = JSON.parse(body);
        playbackAttempts.push(request.assetId);
        if (mappingMode !== 'Normal') {
            if (request.assetId === staleAssetId) {
                rejectedAssetIds.push(request.assetId);
                writeState();
                return res.writeHead(404).end();
            }
            if (request.assetId !== reportingId || !authoritativeMappingReturned ||
                playbackAttempts.length !== 2 || playbackAttempts[0] !== staleAssetId ||
                libraryLookupPaths.length !== 1 ||
                !['StaleCatalog', 'InvalidSessionMetadata', 'PreviewRejected'].includes(mappingMode)) {
                rejectedAssetIds.push(request.assetId);
                writeState();
                return res.writeHead(403).end();
            }
        } else if (request.assetId !== fixtureAssetId) {
            writeState();
            return res.writeHead(403).end();
        }
        if (!request.capabilities?.lossless || !request.capabilities?.hiRes)
            return res.writeHead(400).end();
        if (codec === 'ATMOS' && request.preferAtmos !== true) return res.writeHead(400).end();
        preferAtmos = request.preferAtmos === true;
        mappingAccepted = mappingMode !== 'Normal';
        const session = `glasswave-fixture-${nextSession++}`;
        activeSessions.add(session);
        playbackAssetIds.push(request.assetId);
        sessionRecords.push({
            sessionId: session,
            assetId: request.assetId,
            sourceVersion,
            preferAtmos: request.preferAtmos === true,
            deleted: false
        });
        writeState();
        const metadataInvalid = mappingMode === 'InvalidSessionMetadata';
        return json({
            sessionId: session,
            codec: codec === 'ATMOS' ? 'atmos' : codec.toLowerCase(),
            durationMs: metadataInvalid ? 0 : mappingMode === 'PreviewRejected' ? 100 :
                proxyMode ? proxyDurationMs : 3000,
            sampleRate: codec === 'ALAC' ? 192000 : 48000,
            bitDepth: codec === 'ALAC' ? 24 : 0,
            sourceVersion,
            streams: { audio: metadataInvalid ? '/api/v1/playback/invalid/audio'
                                                : `/api/v1/playback/${session}/audio` }
        });
    }
    const playbackDelete = /^\/api\/v1\/playback\/([^/]+)$/.exec(pathname);
    if (playbackDelete && req.method === 'DELETE') {
        const session = playbackDelete[1];
        deleteCount += 1;
        if (activeSessions.delete(session)) {
            successfulDeleteCount += 1;
            deletedSessions.add(session);
            const record = sessionRecords.find((item) => item.sessionId === session);
            if (record) record.deleted = true;
        } else {
            unexpectedDeleteCount += 1;
        }
        writeState();
        return json({});
    }
    const playbackRoute = /^\/api\/v1\/playback\/([^/]+)\/(touch|audio)$/.exec(pathname);
    if (!playbackRoute) return res.writeHead(404).end();
    const session = playbackRoute[1];
    if (!activeSessions.has(session)) return res.writeHead(404).end();
    if (playbackRoute[2] === 'touch') return json({});
    if (codec === 'ATMOS' && requestUrl.searchParams.get('container') !== 'ec3')
        return res.writeHead(400).end();
    if (codec !== 'ATMOS' && requestUrl.search.length)
        return res.writeHead(400).end();
    if (req.method === 'GET' || req.method === 'HEAD') {
        audioGetCount += 1;
        writeState();
    }
    if (proxyMode) return proxySessionAudio(req, res);
    if (req.method !== 'HEAD') noteAudioRead();
    const match = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? '');
    const start = match ? Number(match[1]) : 0;
    const end = match?.[2] ? Math.min(Number(match[2]), media.length - 1) : media.length - 1;
    if (start > end || start >= media.length) {
        return res.writeHead(416, { 'Content-Range': `bytes */${media.length}` }).end();
    }
    if (!match) {
        // The real headless engine streams the initial encoded response without
        // Content-Length. Exercise the input component's complete spool path.
        res.writeHead(200, { 'Content-Type': codec === 'ATMOS' ? 'audio/eac3' : 'audio/mp4' });
        if (req.method === 'HEAD') return res.end();
        let responseCompleted = false;
        res.once('close', () => {
            if (responseCompleted || clientDisconnected !== null) return;
            clientDisconnected = Date.now();
            writeState();
        });
        if (slowStreamTest) {
            const started = Date.now();
            const firstEnd = Math.min(media.length, 64 * 1024);
            await waitUntil(started + 100);
            if (res.destroyed) return;
            res.write(media.subarray(0, firstEnd));
            if (firstEnd < media.length) {
                const chunkCount = Math.ceil((media.length - firstEnd) / (64 * 1024));
                for (let index = 1; index <= chunkCount; index += 1) {
                    const target = started + 100 + Math.round((9900 * index) / chunkCount);
                    await waitUntil(target);
                    if (res.destroyed) return;
                    const offset = firstEnd + (index - 1) * 64 * 1024;
                    res.write(media.subarray(offset, Math.min(offset + 64 * 1024, media.length)));
                }
            }
            responseCompleted = true;
            serverDownloadFinished = Date.now();
            writeState();
            await endResponse(res);
            return;
        }
        for (let offset = 0; offset < media.length; offset += 64 * 1024)
            res.write(media.subarray(offset, Math.min(offset + 64 * 1024, media.length)));
        responseCompleted = true;
        serverDownloadFinished = Date.now();
        writeState();
        await endResponse(res);
        return;
    }
    const headers = {
        'Content-Type': codec === 'ATMOS' ? 'audio/eac3' : 'audio/mp4',
        'Content-Length': end - start + 1,
        'Accept-Ranges': 'bytes',
        'Content-Range': `bytes ${start}-${end}/${media.length}`
    };
    res.writeHead(206, headers);
    res.end(req.method === 'HEAD' ? undefined : media.subarray(start, end + 1));
});
server.listen(0, '127.0.0.1', () => {
    fs.writeFileSync(bridgePath, JSON.stringify({
        url: `http://127.0.0.1:${server.address().port}`,
        token,
        fixtureTransactionTag,
        fixtureAssetId,
        fixturePlaybackAssetId,
        libraryId,
        reportingId,
        mappingMode
    }));
});
