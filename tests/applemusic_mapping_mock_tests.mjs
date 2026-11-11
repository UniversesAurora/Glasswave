import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const testRoot = path.join(root, '.local', 'tests');
const mockPath = path.join(root, 'tests', 'applemusic_mock.mjs');
const fixtures = {
    ALAC: path.join(testRoot, 'alac-24-192-fragmented.m4a'),
    AAC: path.join(testRoot, 'aac-48-faststart.m4a')
};
const staleAssetId = '1237064886';
const libraryId = 'i.GlasswaveStaleFixture';
const reportingId = '1865440142';

// Catalog AAC emits its index before media. A tail-moov upload
// must be tested through the real Go normalizer, not substituted into this mock.
function assertCatalogAacLayout(file) {
    const media = fs.readFileSync(file);
    const boxes = [];
    let offset = 0;
    while (offset + 8 <= media.length && boxes.length < 64) {
        const size = media.readUInt32BE(offset);
        assert.ok(size >= 8 && offset + size <= media.length, 'invalid AAC fixture box');
        boxes.push(media.toString('ascii', offset + 4, offset + 8));
        offset += size;
    }
    const moov = boxes.indexOf('moov');
    const moof = boxes.indexOf('moof');
    const mdat = boxes.indexOf('mdat');
    assert.equal(boxes[0], 'ftyp');
    assert.ok(moov > 0 && mdat > moov, 'catalog AAC fixture must place its index before media');
    if (moof !== -1) assert.ok(moof > moov && mdat > moof, 'AAC fragment precedes its init segment');
}

assertCatalogAacLayout(fixtures.AAC);

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

async function waitFor(condition, description, timeoutMs = 10000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        const value = await condition();
        if (value) return value;
        await delay(25);
    }
    throw new Error(`timed out waiting for ${description}`);
}

async function readJson(file) {
    try {
        return JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch {
        return null;
    }
}

async function callJson(url, token, method = 'GET', body, transactionTag) {
    const response = await fetch(url, {
        method,
        headers: {
            'X-Api-Token': token,
            ...(transactionTag ? { 'X-Glasswave-Probe-Transaction': transactionTag } : {}),
            ...(body === undefined ? {} : { 'Content-Type': 'application/json' })
        },
        body: body === undefined ? undefined : JSON.stringify(body)
    });
    const text = await response.text();
    return { status: response.status, body: text ? JSON.parse(text) : null };
}

async function runCase({ name, codec, mappingMode, verify }) {
    const fixture = fixtures[codec];
    assert.ok(fs.existsSync(fixture), `${name}: missing ${codec} fixture`);
    const caseRoot = fs.mkdtempSync(path.join(testRoot, `applemusic-mapping-${name}-`));
    const bridgePath = path.join(caseRoot, 'bridge.json');
    const statePath = path.join(caseRoot, 'state.json');
    const child = spawn(process.execPath, [
        mockPath,
        fixture,
        bridgePath,
        statePath,
        codec,
        'Normal',
        `mapping-${name}`,
        mappingMode
    ], { stdio: ['ignore', 'ignore', 'pipe'] });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });

    try {
        const bridge = await waitFor(() => readJson(bridgePath), `${name} bridge`);
        assert.equal(bridge.fixtureAssetId, 'glasswave-test');
        assert.equal(bridge.fixturePlaybackAssetId, staleAssetId);
        assert.equal(bridge.libraryId, libraryId);
        assert.match(bridge.fixtureTransactionTag, /^[0-9a-f]{32}$/);
        const baseUrl = new URL(bridge.url);
        const playbackUrl = new URL('/api/v1/playback', baseUrl).toString();
        const appleUrl = new URL('/api/v1/glasswave/apple', baseUrl).toString();
        const capabilities = { lossless: true, hiRes: true };
        const callProbe = (url, method = 'GET', body) =>
            callJson(url, bridge.token, method, body, bridge.fixtureTransactionTag);
        assert.equal((await callJson(playbackUrl, 'invalid-token', 'POST', {
            assetId: staleAssetId, capabilities
        }, bridge.fixtureTransactionTag)).status, 401, `${name}: transaction tag must not bypass auth`);

        // Owner resume/prefetch may request even the same catalog IDs. Without the
        // explicit probe transaction they cannot advance the fixture or create a session.
        for (const assetId of ['1601728963', staleAssetId, reportingId]) {
            const background = await callJson(playbackUrl, bridge.token, 'POST', { assetId, capabilities });
            assert.equal(background.status, 403, `${name}: background asset must stay isolated`);
        }

        const first = await callProbe(playbackUrl, 'POST', {
            assetId: staleAssetId,
            capabilities,
            preferAtmos: false
        });
        assert.equal(first.status, 404, `${name}: stale catalog request must fail`);

        const backgroundMapping = await callJson(appleUrl, bridge.token, 'POST', {
            method: 'GET', path: `/v1/me/library/songs/${libraryId}?include=catalog`, body: null
        });
        assert.equal(backgroundMapping.status, 403);
        const mapping = await callProbe(appleUrl, 'POST', {
            method: 'GET',
            path: `/v1/me/library/songs/${libraryId}?include=catalog&l=en-US`,
            body: null
        });
        assert.equal(mapping.status, 200, `${name}: authoritative library mapping must succeed`);
        const backgroundReporting = await callJson(playbackUrl, bridge.token, 'POST', {
            assetId: reportingId, capabilities
        });
        assert.equal(backgroundReporting.status, 403);

        if (mappingMode === 'WrongLibrary') {
            assert.notEqual(mapping.body.data[0].id, libraryId);
        } else if (mappingMode === 'WrongReportingId') {
            assert.equal(mapping.body.data[0].attributes.playParams.reportingId, staleAssetId);
        } else {
            assert.equal(mapping.body.data[0].id, libraryId);
            assert.equal(mapping.body.data[0].attributes.playParams.catalogId, staleAssetId);
            assert.equal(mapping.body.data[0].attributes.playParams.reportingId, reportingId);

            const accepted = await callProbe(playbackUrl, 'POST', {
                assetId: reportingId,
                capabilities,
                preferAtmos: false
            });
            assert.equal(accepted.status, 200, `${name}: remapped playback must be accepted`);
            assert.ok(accepted.body.sessionId, `${name}: accepted session needs an id`);
            verify(accepted.body);

            const audioUrl = new URL(accepted.body.streams.audio, baseUrl).toString();
            assert.equal((await callJson(audioUrl, bridge.token)).status, 403);
            const sessionUrl = new URL(`/api/v1/playback/${accepted.body.sessionId}`, baseUrl).toString();
            assert.equal((await callJson(sessionUrl, bridge.token, 'DELETE')).status, 403);

            const deleted = await callProbe(
                new URL(`/api/v1/playback/${accepted.body.sessionId}`, baseUrl).toString(),
                'DELETE'
            );
            assert.equal(deleted.status, 200, `${name}: created session must be deleted`);
        }

        const state = await waitFor(async () => {
            const value = await readJson(statePath);
            const expectedDeletes = mappingMode === 'WrongLibrary' || mappingMode === 'WrongReportingId' ? 0 : 1;
            return value?.deleteCount === expectedDeletes && value?.activeSessionCount === 0 ? value : null;
        }, `${name} final state`);
        const createsSession = !['WrongLibrary', 'WrongReportingId'].includes(mappingMode);
        assert.deepEqual(state.playbackAttempts, createsSession
            ? [staleAssetId, reportingId]
            : [staleAssetId]);
        assert.deepEqual(state.playbackAssetIds, createsSession ? [reportingId] : []);
        assert.equal(state.rejectedAssetIds.includes(staleAssetId), true);
        assert.equal(state.libraryLookupCount, 1);
        assert.equal(state.audioGetCount, 0);
        assert.equal(state.fixtureAssetId, 'glasswave-test');
        assert.equal(state.fixturePlaybackAssetId, staleAssetId);
        assert.equal(state.playbackSessionCount, createsSession ? 1 : 0);
        assert.equal(state.deleteCount, createsSession ? 1 : 0);
        assert.equal(state.successfulDeleteCount, createsSession ? 1 : 0);
        assert.equal(state.rejectedBackgroundRequests.length, createsSession ? 7 : 5);
        assert.ok(state.rejectedBackgroundRequests.every((item) => item.status === 403));
        assert.ok(state.rejectedBackgroundRequests.some((item) => item.assetId === staleAssetId));
        assert.ok(state.rejectedBackgroundRequests.some((item) => item.assetId === reportingId));
    } finally {
        if (child.exitCode === null && child.signalCode === null) {
            child.kill();
            await new Promise((resolve) => child.once('exit', resolve));
        }
        if (stderr.trim()) process.stderr.write(stderr);
        assert.ok(caseRoot.startsWith(testRoot + path.sep), 'test cleanup escaped .local/tests');
        fs.rmSync(caseRoot, { recursive: true, force: true });
    }
}

await runCase({
    name: 'full-alac',
    codec: 'ALAC',
    mappingMode: 'StaleCatalog',
    verify(session) {
        assert.equal(session.codec, 'alac');
        assert.equal(session.durationMs, 3000);
    }
});

await runCase({
    name: 'full-aac',
    codec: 'AAC',
    mappingMode: 'StaleCatalog',
    verify(session) {
        assert.equal(session.codec, 'aac');
        assert.equal(session.durationMs, 3000);
    }
});

await runCase({
    name: 'invalid-session-metadata',
    codec: 'ALAC',
    mappingMode: 'InvalidSessionMetadata',
    verify(session) {
        assert.equal(session.durationMs, 0);
        assert.equal(session.streams.audio, '/api/v1/playback/invalid/audio');
    }
});

await runCase({
    name: 'preview',
    codec: 'AAC',
    mappingMode: 'PreviewRejected',
    verify(session) {
        assert.equal(session.durationMs, 100);
    }
});

await runCase({
    name: 'wrong-library',
    codec: 'ALAC',
    mappingMode: 'WrongLibrary',
    verify() {}
});

await runCase({
    name: 'wrong-reporting-id',
    codec: 'ALAC',
    mappingMode: 'WrongReportingId',
    verify() {}
});

process.stdout.write('Apple Music mapping mock checks passed: 6\n');
