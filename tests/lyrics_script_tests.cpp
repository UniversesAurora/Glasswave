#include "lyrics/script_host.h"
#include <miniz.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace glasswave::lyrics;
namespace fs = std::filesystem;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary);
    output << text;
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "Expected repository root");
        const fs::path repo(argv[1]);
        const auto root = repo / L".local/tests/lyrics-host-contract";
        Write(root / L"lib/common.js", "export const shared = 7;");
        Write(root / L"searcher/relative.js", "export const relative = 3;");
        Write(root / L"lib/classic.js", "globalThis.classicValue = 11;");
        Write(root / L"parser/test.js", R"JS(
export function getConfig(c) { c.name = 'Test parser'; c.fileType = 'test'; c.parsePlainText = false; }
export function parseLyric(c) { c.lyricText = '[00:01.00]' + arrayBufferToString(c.lyricData); }
)JS");
        Write(root / L"searcher/test.js", R"JS(
import { shared } from 'common.js';
import { relative } from './relative.js';
import * as decoder from 'parser_ext.so';
function check(value, message) { if (!value) throw new Error(message); }
export function getConfig(c) { c.name = 'Fixture'; c.useRawMeta = true; }
export function getLyrics(meta, man) {
    check(shared + relative === 10, 'ES module resolution');
    evalLib('classic.js'); check(classicValue === 11, 'classic library');
    check(decoder.decodeQrc(new ArrayBuffer(0)) === null, 'native QRC fallback');
    check(meta.duration === 123 && meta.subSong === 2 && meta.rawTitle === 'raw', 'track metadata');
    check(atob(btoa(String.fromCharCode(0, 127, 128, 255))) === String.fromCharCode(0, 127, 128, 255), 'base64 string round trip');
    check(btoa('中文') === '5Lit5paH' && atob('5Lit5paH') === '中文', 'ESLyric Unicode base64');
    check(arrayBufferToString(stringToArrayBuffer('中文 🌟')) === '中文 🌟', 'UTF-8 buffers');
    const doc = mxml.loadString('<root><songinfo id="a"><content><![CDATA[first]]></content></songinfo><group><songinfo id="b"/></group></root>');
    const matches = [...doc.findElement('songinfo')];
    check(matches.length === 2 && matches[0].getAttr('id') === 'a' && matches[1].getAttr('missing') === null, 'XML iteration');
    check(matches[0].findElement('content').getFirstChild().getCDATA() === 'first', 'XML CDATA');
    check(mxml.loadString('<!DOCTYPE root SYSTEM "file:///missing"><root/>') === null, 'external DTD');
    check(mxml.loadString('<broken>') === null, 'invalid XML');
    const xml = mxml.loadString('<root a="A&amp;B"><value>text &lt; value</value></root>');
    check(xml.findElement('root').getAttr('a') === 'A&B' && xml.findElement('value').getText() === 'text < value', 'XML entities');
    let callback = false;
    request({url:'https://fixture.invalid/text', method:'post', body:'payload', headers:{Referer:'test'}}, (err, res, body) => {
        check(err === null && res.statusCode === 200 && res.headers['x-fixture'] === 'yes' && body === 'response', 'HTTP response');
        callback = true;
    });
    check(callback, 'HTTP callback runs before request returns');
    request({url:'https://fixture.invalid/raw', raw:true}, (err, res, body) => {
        check(body instanceof ArrayBuffer && new Uint8Array(body)[1] === 255, 'raw HTTP ArrayBuffer');
    });
    request('https://fixture.invalid/zlib', (err, res, body) => {
        const bytes = Uint8Array.from(atob(body), x => x.charCodeAt(0));
        check(arrayBufferToString(zlib.uncompress(bytes)) === 'compressed fixture', 'zlib decompression');
        check(zlib.uncompress(new Uint8Array([1,2,3])) === null, 'invalid zlib');
    });
    man.setSvcData('token', 'saved'); check(man.getSvcData('token') === 'saved', 'service data');
    check(man.getSvcData('missing') === '' && !man.checkAbort(), 'defaults');
    const lyric = man.createLyric();
    lyric.title = 'first'; lyric.lyricText = '[00:01.00]Fixture'; man.addLyric(lyric);
    lyric.title = 'second'; lyric.lyricText = '[00:02.00]Changed'; man.addLyric(lyric);
    lyric.title = 'parsed'; lyric.fileType = 'test'; lyric.lyricText = ''; lyric.lyricData = stringToArrayBuffer('Parsed'); man.addLyric(lyric);
    Promise.resolve().then(() => {
        lyric.fileType = 'lrc'; lyric.title = 'promise'; lyric.lyricText = '[00:03.00]Promise'; man.addLyric(lyric);
    });
    messageLoop(0);
}
)JS");
        std::string zipped(256, 0);
        mz_ulong zipSize = static_cast<mz_ulong>(zipped.size());
        const std::string plain = "compressed fixture";
        Check(mz_compress(reinterpret_cast<unsigned char*>(zipped.data()), &zipSize,
                          reinterpret_cast<const unsigned char*>(plain.data()),
                          static_cast<mz_ulong>(plain.size())) == MZ_OK,
              "compress fixture");
        zipped.resize(zipSize);
        // Generate base64 in the fixture itself, without a second encoder in the test.
        std::string base64Script =
            "export function getConfig(c){c.name='zip';} export function getLyrics(m,s){let b='';";
        for (unsigned char c : zipped)
            base64Script += "b+=String.fromCharCode(" + std::to_string(c) + ");";
        base64Script += "const l=s.createLyric();l.lyricText=btoa(b);s.addLyric(l);}";
        Write(root / L"searcher/zip.js", base64Script);
        ScriptEnvironment env;
        env.roots = {root};
        std::vector<ScriptLyric> results;
        env.found = [&](ScriptLyric lyric) { results.push_back(std::move(lyric)); };
        ScriptHost zipHost(env);
        auto zip = zipHost.Configure(root / L"searcher/zip.js");
        zipHost.Search(zip, {}, 0, 0);
        Check(results.size() == 1, "zip encoder fixture");
        const std::string encoded = results.front().text;
        results.clear();
        std::map<std::string, std::string> data;
        env.getData = [&](const std::string& key) { return data[key]; };
        env.setData = [&](const std::string& key, const std::string& value) { data[key] = value; };
        env.request = [&](const ScriptRequest& request) {
            ScriptResponse response;
            response.status = 200;
            response.headers["x-fixture"] = "yes";
            if (request.url.ends_with("/raw"))
                response.body = std::string("\0\xff", 2);
            else if (request.url.ends_with("/zlib"))
                response.body = encoded;
            else {
                Check(request.method == "post" && request.body == "payload" &&
                          request.headers.at("Referer") == "test",
                      "HTTP options");
                response.body = "response";
            }
            return response;
        };
        ScriptHost configHost(env);
        auto parser = configHost.Configure(root / L"parser/test.js");
        env.parsers.push_back(parser);
        ScriptHost host(env);
        auto script = host.Configure(root / L"searcher/test.js");
        Check(script.name == "Fixture" && script.rawMeta, "getConfig");
        host.Search(script, {{"rawTitle", "raw"}}, 2, 123);
        Check(results.size() == 4, "copy-on-add, parser dispatch and promise jobs");
        Check(results[0].title == "first" && results[1].title == "second" &&
                  results[2].text == "[00:01.00]Parsed",
              "result contents");
        Write(root / L"searcher/loop.js",
              "export function getConfig(c){c.name='Loop';} export function getLyrics(){while(true){}}");
        std::atomic<bool> aborted = false;
        env.aborted = [&] { return aborted.load(); };
        ScriptHost loopHost(env);
        auto loop = loopHost.Configure(root / L"searcher/loop.js");
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            aborted = true;
        });
        bool interrupted = false;
        try {
            loopHost.Search(loop, {}, 0, 0);
        } catch (const std::exception&) {
            interrupted = true;
        }
        cancel.join();
        Check(interrupted, "CPU-bound script cancellation");
        std::cout << "Lyrics host contract checks passed\n";

        // Read-only compatibility checks against the owner's unmodified reference scripts.
        size_t configured = 0;
        for (const auto& source : {repo / L".local/refs/lyrics/eslyric-pkg/emb_5923208/scripts",
                                   repo / L".local/refs/lyrics/ESLyric-LyricsSource/current/qrc",
                                   repo / L".local/refs/lyrics/ESLyric-LyricsSource/current/yrc",
                                   repo / L".local/refs/lyrics/ESLyric-LyricsSource/current/krc"}) {
            if (!fs::exists(source)) continue;
            for (const auto* type : {L"searcher", L"parser"}) {
                const auto folder = source / type;
                if (!fs::exists(folder)) continue;
                for (const auto& file : fs::directory_iterator(folder)) {
                    if (file.path().extension() != L".js") continue;
                    ScriptEnvironment reference;
                    reference.roots = {repo / L".local/refs/lyrics/eslyric-pkg/emb_5923208/scripts", source};
                    ScriptHost referenceHost(reference);
                    const auto config = referenceHost.Configure(file.path());
                    Check(!config.name.empty(), "reference script config");
                    ++configured;
                }
            }
        }
        std::cout << "Configured " << configured << " unmodified reference scripts\n";
        const auto bridgeRoot = root / L"qrc-fixture";
        Write(bridgeRoot / L"lib/qrc-decryptor/qrc-decryptor.js", R"JS(
export function decryptQrcBytes(hex) {
    if (hex !== '01020304') throw new Error('QRC binary adapter payload');
    return stringToArrayBuffer('bridge fixture');
}
export function decryptQrc() { return null; }
)JS");
        Write(bridgeRoot / L"searcher/bridge.js", R"JS(
import { decodeQrc } from 'parser_ext.so';
export function getConfig(c) { c.name = 'Bridge fixture'; }
export function getLyrics(meta, man) {
    const prefix = '[offset:0]\n';
    const bytes = Uint8Array.from([...prefix].map(c => c.charCodeAt(0)).concat([1,2,3,4]));
    if (arrayBufferToString(decodeQrc(bytes.buffer)) !== 'bridge fixture') throw new Error('QRC binary adapter');
}
)JS");
        ScriptEnvironment bridgeEnvironment;
        bridgeEnvironment.roots = {bridgeRoot};
        ScriptHost bridgeHost(bridgeEnvironment);
        const auto bridge = bridgeHost.Configure(bridgeRoot / L"searcher/bridge.js");
        bridgeHost.Search(bridge, {}, 0, 0);

        const auto qqOriginal =
            repo / L".local/refs/lyrics/ESLyric-LyricsSource/current/qrc/searcher/qqmusic_ex.js";
        if (fs::exists(qqOriginal)) {
            ScriptEnvironment qqEnvironment;
            qqEnvironment.roots = {repo / L".local/refs/lyrics/eslyric-pkg/emb_5923208/scripts", bridgeRoot};
            size_t qqPosts = 0;
            qqEnvironment.request = [&](const ScriptRequest& request) {
                ScriptResponse response;
                response.status = 200;
                if (request.url.find("fcg_search_pc_lrc") != std::string::npos)
                    response.body = "<root><songname><![CDATA[fixture]]></"
                                    "songname><singer><![CDATA[%E4%B8%AD%E6%96%87]]></singer>"
                                    "<songinfo "
                                    "id='1'><name><![CDATA[]]></name><singername><![CDATA[]]></"
                                    "singername><albumname><![CDATA[]]></albumname></songinfo></root>";
                else if (request.method == "post") {
                    Check(request.body.find("5Lit5paH") != std::string::npos,
                          "original QQ source Unicode metadata");
                    ++qqPosts;
                    response.body = "{\"code\":1}";
                } else
                    response.body = "<root/>";
                return response;
            };
            ScriptHost qqHost(qqEnvironment);
            const auto qq = qqHost.Configure(qqOriginal);
            qqHost.Search(qq, {{"title", "fixture"}, {"artist", "中文"}}, 0, 123);
            Check(qqPosts == 1, "unmodified QQ source reaches lyric request");
        }
        std::cout << "Unicode QQ metadata and original QRC parser adapter checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
