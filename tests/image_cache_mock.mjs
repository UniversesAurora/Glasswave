#!/usr/bin/env node

import { mkdirSync, renameSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { createServer } from 'node:http';

const args = process.argv.slice(2);
const option = (name) => {
    const index = args.indexOf(name);
    return index >= 0 && index + 1 < args.length ? args[index + 1] : null;
};

const controlPath = option('--control');
const countersPath = option('--counters');
if (!controlPath || !countersPath) {
    console.error('Usage: node image_cache_mock.mjs --control <path> --counters <path>');
    process.exit(2);
}

const pngA = Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGPgEpH7DwABpAE8k4sOtwAAAABJRU5ErkJggg==',
    'base64'
);
const pngB = Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGPQMLL5DwACsgGWiwRo7AAAAABJRU5ErkJggg==',
    'base64'
);
const pngC = Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNwC4j6DwADwAHw4MV2LAAAAABJRU5ErkJggg==',
    'base64'
);

const cases = [
    'cached-first',
    'changed',
    'mislabelled-mime',
    'bad-html',
    'truncated',
    'no-store',
    'coalesce',
    'invalidate',
    'rotation',
    'shutdown-finish-0',
    'shutdown-finish-1',
    'shutdown-finish-2',
    'shutdown'
];

const counters = {
    requests: 0,
    conditionalRequests: 0,
    notModified: 0,
    changedResponses: 0,
    invalidResponses: 0,
    noStoreResponses: 0,
    byCase: Object.fromEntries(
        cases.map((name) => [
            name,
            {
                requests: 0,
                conditionalRequests: 0,
                notModified: 0,
                changedResponses: 0,
                invalidResponses: 0,
                noStoreResponses: 0
            }
        ])
    )
};

function writeJsonAtomic(path, value) {
    mkdirSync(dirname(path), { recursive: true });
    const temporary = `${path}.${process.pid}.tmp`;
    writeFileSync(temporary, JSON.stringify(value), { encoding: 'utf8', flag: 'w' });
    renameSync(temporary, path);
}

function delay(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function increment(name, field = 'requests') {
    counters[field] += 1;
    counters.byCase[name][field] += 1;
}

function send(response, status, body, headers = {}) {
    response.writeHead(status, {
        'Cache-Control': 'public, max-age=300',
        'Content-Length': Buffer.byteLength(body),
        ...headers
    });
    response.end(body);
}

function sendImage(response, bytes, etag, extraHeaders = {}) {
    send(response, 200, bytes, {
        'Content-Type': 'image/png',
        ETag: etag,
        ...extraHeaders
    });
}

const server = createServer(async (request, response) => {
    const requestUrl = new URL(request.url ?? '/', 'http://127.0.0.1');
    if (requestUrl.pathname === '/__health') {
        send(response, 200, '{"ok":true}', { 'Content-Type': 'application/json' });
        return;
    }
    if (requestUrl.pathname === '/__counters') {
        send(response, 200, JSON.stringify(counters), { 'Content-Type': 'application/json' });
        return;
    }
    if (requestUrl.pathname === '/__shutdown') {
        send(response, 200, '{"ok":true}', { 'Content-Type': 'application/json' });
        writeJsonAtomic(countersPath, counters);
        server.close(() => process.exit(0));
        setTimeout(() => process.exit(0), 1000).unref();
        return;
    }

    const components = requestUrl.pathname.split('/').filter(Boolean);
    if (components.length !== 2 || components[0] !== 'image' || !cases.includes(components[1])) {
        send(response, 404, 'not found', { 'Content-Type': 'text/plain' });
        return;
    }

    const name = components[1];
    increment(name);
    const conditional = request.headers['if-none-match'] ?? null;
    if (conditional) {
        increment(name, 'conditionalRequests');
    }

    const count = counters.byCase[name].requests;
    if (name.startsWith('shutdown-finish-')) {
        await delay(300);
        sendImage(response, pngA, `"${name}-a"`);
        return;
    }
    switch (name) {
    case 'cached-first':
        if (count === 1 && conditional === '"cached-first-a"') {
            await delay(900);
            increment(name, 'notModified');
            response.writeHead(304, {
                ETag: '"cached-first-b"',
                'Last-Modified': 'Wed, 11 Nov 2026 00:00:00 GMT',
                'Cache-Control': 'public, max-age=300'
            });
            response.end();
        } else {
            await delay(60);
            sendImage(response, pngA, '"cached-first-a"');
        }
        return;
    case 'changed':
        if (conditional === '"changed-a"' || count > 1) {
            await delay(140);
            increment(name, 'changedResponses');
            sendImage(response, pngB, '"changed-b"');
        } else {
            await delay(40);
            sendImage(response, pngA, '"changed-a"');
        }
        return;
    case 'mislabelled-mime':
        await delay(60);
        sendImage(response, pngA, '"mislabelled-mime-a"', { 'Content-Type': 'image/jpg' });
        return;
    case 'bad-html':
        if (conditional === '"bad-html-a"' || count > 1) {
            increment(name, 'invalidResponses');
            send(response, 200, '<!doctype html><title>not an image</title>', {
                'Content-Type': 'image/png'
            });
        } else {
            await delay(40);
            sendImage(response, pngA, '"bad-html-a"');
        }
        return;
    case 'truncated':
        if (conditional === '"truncated-a"' || count > 1) {
            increment(name, 'invalidResponses');
            sendImage(response, pngA.subarray(0, pngA.length - 12), '"truncated-b"');
        } else {
            await delay(40);
            sendImage(response, pngA, '"truncated-a"');
        }
        return;
    case 'no-store':
        if (conditional === '"no-store-a"' || count > 1) {
            await delay(100);
            increment(name, 'noStoreResponses');
            sendImage(response, pngB, '"no-store-b"', { 'Cache-Control': 'no-store' });
        } else {
            await delay(40);
            sendImage(response, pngA, '"no-store-a"');
        }
        return;
    case 'coalesce':
        await delay(350);
        sendImage(response, pngA, '"coalesce-a"');
        return;
    case 'invalidate':
        if (count === 1 && conditional === '"invalidate-a"') {
            await delay(850);
            sendImage(response, pngB, '"invalidate-b"');
        } else {
            await delay(60);
            sendImage(response, pngC, '"invalidate-c"');
        }
        return;
    case 'rotation':
        if (count === 1 && conditional === '"rotation-a"') {
            await delay(850);
            sendImage(response, pngB, '"rotation-b"');
        } else {
            await delay(60);
            sendImage(response, pngC, '"rotation-c"');
        }
        return;
    case 'shutdown':
        await delay(1000);
        sendImage(response, pngA, '"shutdown-a"');
        return;
    default:
        send(response, 500, 'unreachable', { 'Content-Type': 'text/plain' });
    }
});

server.listen(0, '127.0.0.1', () => {
    const address = server.address();
    writeJsonAtomic(controlPath, { port: address.port, ok: true });
});

server.on('error', (error) => {
    writeJsonAtomic(controlPath, { ok: false, error: error.message });
    process.exit(1);
});

function stop() {
    writeJsonAtomic(countersPath, counters);
    server.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 1000).unref();
}

process.on('SIGINT', stop);
process.on('SIGTERM', stop);
