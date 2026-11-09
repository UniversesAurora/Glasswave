#include "lyrics/script_host.h"

#include <windows.h>
#include <msxml6.h>
#include <wincrypt.h>
#include <wrl/client.h>
#include <quickjs.h>
#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>

#pragma comment(lib, "msxml6.lib")

namespace glasswave::lyrics {
namespace {

std::string ReadScript(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 8 * 1024 * 1024 || ec)
        throw std::runtime_error("Cannot read script or script is too large");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read script " + path.string());
    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (text.size() > 8 * 1024 * 1024) throw std::runtime_error("Script is too large");
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    return text;
}

std::wstring Wide(std::string_view text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string PathName(const std::filesystem::path& path) {
    const auto name = path.generic_u8string();
    return {reinterpret_cast<const char*>(name.data()), name.size()};
}

// An owning JS value. QuickJS's setters consume values, so release only at that boundary.
struct Value {
    JSContext* context;
    JSValue value;
    Value(JSContext* c, JSValue v) : context(c), value(v) {}
    ~Value() { JS_FreeValue(context, value); }
    Value(const Value&) = delete;
    operator JSValue() const { return value; }
    JSValue Release() {
        const JSValue result = value;
        value = JS_UNDEFINED;
        return result;
    }
};

std::string String(JSContext* ctx, JSValueConst value) {
    size_t size = 0;
    const char* text = JS_ToCStringLen(ctx, &size, value);
    if (!text) return {};
    std::string result(text, size);
    JS_FreeCString(ctx, text);
    return result;
}

std::string Property(JSContext* ctx, JSValueConst object, const char* name) {
    Value value(ctx, JS_GetPropertyStr(ctx, object, name));
    return JS_IsNull(value) || JS_IsUndefined(value) ? std::string() : String(ctx, value);
}

void Set(JSContext* ctx, JSValueConst object, const char* name, const std::string& text) {
    JS_SetPropertyStr(ctx, object, name, JS_NewStringLen(ctx, text.data(), text.size()));
}

int Number(JSContext* ctx, JSValueConst object, const char* name, int fallback) {
    Value value(ctx, JS_GetPropertyStr(ctx, object, name));
    int32_t number = fallback;
    if (!JS_IsUndefined(value)) JS_ToInt32(ctx, &number, value);
    return number;
}

std::string Bytes(JSContext* ctx, JSValueConst value) {
    size_t size = 0;
    if (JS_IsArrayBuffer(value)) {
        const uint8_t* data = JS_GetArrayBuffer(ctx, &size, value);
        return data ? std::string(reinterpret_cast<const char*>(data), size) : std::string();
    }
    size_t offset = 0, length = 0, elementSize = 0;
    Value buffer(ctx, JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &elementSize));
    if (JS_IsException(buffer)) {
        Value ignored(ctx, JS_GetException(ctx));
        return {};
    }
    const uint8_t* data = JS_GetArrayBuffer(ctx, &size, buffer);
    if (!data || offset > size || length > size - offset) return {};
    return {reinterpret_cast<const char*>(data + offset), length};
}

JSValue Buffer(JSContext* ctx, std::string_view bytes) {
    return JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

// Wrappers are our implementation of the documented Mini-XML subset. The tree itself is
// parsed by Windows MSXML, with DTDs and external resolution disabled.
constexpr char kGlobals[] = R"JS(
class XmlNode {
    constructor(data) { Object.assign(this, data); this.children = data.children.map(x => new XmlNode(x)); }
    findElement(name, mode = 1) {
        const matches = [];
        const walk = node => {
            for (const child of node.children) {
                if (child.name === name) matches.push(child);
                if (mode !== 0) walk(child);
            }
        };
        walk(this);
        if (!matches.length) return null;
        const first = Object.create(matches[0]);
        first[Symbol.iterator] = function* () { yield* matches; };
        return first;
    }
    getFirstChild() { return this.children[0] || null; }
    getCDATA() { return this.type === 4 ? this.text : null; }
    getText() { return this.text; }
    getAttr(name) { return Object.hasOwn(this.attrs, name) ? this.attrs[name] : null; }
}
globalThis.mxml = {
    MXML_DESCEND: 1, MXML_NO_DESCEND: 0, MXML_DESCEND_FIRST: 2,
    loadString(text) { const data = __xml(text); return data ? new XmlNode(data) : null; }
};
globalThis.console = { log: __log };
globalThis.utils = { createProfiler() { return { start() {}, stop() {}, reset() {}, print() {}, time: 0 }; } };
globalThis.debugWriteArrayBuffer = () => {};
globalThis.zlib = { uncompress: __uncompress };
)JS";

} // namespace

class ScriptHost::Impl {
  public:
    explicit Impl(ScriptEnvironment env) : environment(std::move(env)) {
        comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        runtime = JS_NewRuntime();
        if (!runtime) throw std::runtime_error("Cannot create JavaScript runtime");
        JS_SetMemoryLimit(runtime, 64 * 1024 * 1024);
        JS_SetMaxStackSize(runtime, 1024 * 1024);
        JS_SetInterruptHandler(runtime, Interrupt, this);
        JS_SetModuleLoaderFunc(runtime, Normalize, Load, this);
        context = JS_NewContext(runtime);
        if (!context) {
            JS_FreeRuntime(runtime);
            throw std::runtime_error("Cannot create JavaScript context");
        }
        JS_SetContextOpaque(context, this);
        Value global(context, JS_GetGlobalObject(context));
        constexpr const char* names[] = {"request",
                                         "messageLoop",
                                         "evalLib",
                                         "atob",
                                         "btoa",
                                         "arrayBufferToString",
                                         "stringToArrayBuffer",
                                         "__log",
                                         "__xml",
                                         "__uncompress",
                                         "__add",
                                         "__abort",
                                         "__get",
                                         "__set"};
        for (int i = 0; i < static_cast<int>(std::size(names)); ++i)
            JS_SetPropertyStr(context, global, names[i],
                              JS_NewCFunctionMagic(context, Native, names[i], 1, JS_CFUNC_generic_magic, i));
        Value initialized(context,
                          JS_Eval(context, kGlobals, sizeof(kGlobals) - 1, "globals", JS_EVAL_TYPE_GLOBAL));
        Check(initialized);
    }

    ~Impl() {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        if (comInitialized) CoUninitialize();
    }

    bool Stopped() const {
        return (environment.aborted && environment.aborted()) || std::chrono::steady_clock::now() >= deadline;
    }

    void Check(JSValueConst value) {
        if (!JS_IsException(value)) return;
        Value error(context, JS_GetException(context));
        const std::string message = String(context, error);
        throw std::runtime_error(message + " " + Property(context, error, "stack"));
    }

    void Jobs() {
        JSContext* jobContext = nullptr;
        while (!Stopped()) {
            const int result = JS_ExecutePendingJob(runtime, &jobContext);
            if (result < 0) {
                Check(JS_EXCEPTION);
                break;
            }
            if (!result) break;
        }
    }

    JSValue Module(const std::filesystem::path& path) {
        const std::string code = ReadScript(path), name = PathName(path);
        Value compiled(context, JS_Eval(context, code.data(), code.size(), name.c_str(),
                                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
        Check(compiled);
        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.value));
        Value evaluated(context, JS_EvalFunction(context, compiled.Release()));
        Check(evaluated);
        Jobs();
        return JS_GetModuleNamespace(context, module);
    }

    void Call(JSValueConst module, const char* name, JSValueConst argument,
              JSValueConst second = JS_UNDEFINED) {
        Value function(context, JS_GetPropertyStr(context, module, name));
        JSValue args[] = {argument, second};
        Value result(context, JS_Call(context, function, JS_UNDEFINED, JS_IsUndefined(second) ? 1 : 2, args));
        Check(result);
        Jobs();
    }

    ScriptConfig Configure(const std::filesystem::path& path) {
        Value module(context, Module(path));
        Value cfg(context, JS_NewObject(context));
        Call(module, "getConfig", cfg);
        ScriptConfig result;
        result.path = path;
        result.name = Property(context, cfg, "name");
        result.fileType = Property(context, cfg, "fileType");
        Value raw(context, JS_GetPropertyStr(context, cfg, "useRawMeta"));
        Value plain(context, JS_GetPropertyStr(context, cfg, "parsePlainText"));
        result.rawMeta = JS_ToBool(context, raw) > 0;
        result.plainText = JS_IsUndefined(plain) || JS_ToBool(context, plain) > 0;
        return result;
    }

    std::string Parse(const ScriptConfig& parser, std::string text, double seconds) {
        Value module(context, Module(parser.path));
        Value data(context, JS_NewObject(context));
        Set(context, data, "lyricText", parser.plainText ? text : std::string());
        JS_SetPropertyStr(context, data, "lyricData", Buffer(context, text));
        JS_SetPropertyStr(context, data, "duration", JS_NewFloat64(context, seconds));
        Call(module, "parseLyric", data);
        return Property(context, data, "lyricText");
    }

    void Search(const ScriptConfig& script, const std::map<std::string, std::string>& meta, unsigned subsong,
                double seconds) {
        duration = seconds;
        Value module(context, Module(script.path));
        Value track(context, JS_NewObject(context));
        for (const auto& [key, value] : meta)
            Set(context, track, key.c_str(), value);
        JS_SetPropertyStr(context, track, "subSong", JS_NewUint32(context, subsong));
        JS_SetPropertyStr(context, track, "duration", JS_NewFloat64(context, seconds));
        constexpr char managerCode[] =
            "({createLyric:()=>({fileType:'lrc',confidence:-1}),addLyric:__add,checkAbort:__abort,"
            "getSvcData:__get,setSvcData:__set})";
        Value manager(context,
                      JS_Eval(context, managerCode, sizeof(managerCode) - 1, "manager", JS_EVAL_TYPE_GLOBAL));
        Check(manager);
        Call(module, "getLyrics", track, manager);
    }

  private:
    static int Interrupt(JSContext*, void* opaque) { return static_cast<Impl*>(opaque)->Stopped(); }

    std::filesystem::path Resolve(std::string_view base, std::string_view name) const {
        const auto relative = std::filesystem::path(Wide(name));
        std::vector<std::filesystem::path> candidates;
        if (!base.empty()) candidates.push_back(std::filesystem::path(Wide(base)).parent_path() / relative);
        for (auto root = environment.roots.rbegin(); root != environment.roots.rend(); ++root)
            candidates.push_back(*root / L"lib" / relative);
        for (const auto& candidate : candidates) {
            const auto path = std::filesystem::weakly_canonical(candidate);
            for (const auto& root : environment.roots) {
                const auto rel = path.lexically_relative(std::filesystem::weakly_canonical(root));
                if (!rel.empty() && *rel.begin() != L".." && std::filesystem::is_regular_file(path))
                    return path;
            }
        }
        throw std::runtime_error("Cannot resolve lyric module " + std::string(name));
    }

    static char* Normalize(JSContext* ctx, const char* base, const char* name, void* opaque) {
        try {
            if (strcmp(name, "parser_ext.so") == 0) return js_strdup(ctx, name);
            const auto path = static_cast<Impl*>(opaque)->Resolve(base, name);
            return js_strdup(ctx, PathName(path).c_str());
        } catch (const std::exception& error) {
            JS_ThrowReferenceError(ctx, "%s", error.what());
            return nullptr;
        }
    }

    static JSModuleDef* Load(JSContext* ctx, const char* name, void* opaque) {
        try {
            std::string text;
            if (strcmp(name, "parser_ext.so") == 0) {
                text = "export function decodeQrc() { return null; }";
                try {
                    static_cast<Impl*>(opaque)->Resolve("", "qrc-decryptor/qrc-decryptor.js");
                    // Adapt the installed community module to the original parser's binary
                    // interface. The decryptor remains an external, unmodified script.
                    text = R"JS(
import { decryptQrcBytes } from 'qrc-decryptor/qrc-decryptor.js';
export function decodeQrc(data) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) :
        new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    const signature = '[offset:0]\n';
    const prefixed = bytes.length >= signature.length &&
        [...signature].every((character, index) => bytes[index] === character.charCodeAt(0));
    const payload = prefixed ? bytes.subarray(signature.length) : bytes;
    const hex = [...payload].map(byte => byte.toString(16).padStart(2, '0')).join('');
    return decryptQrcBytes(hex);
}
)JS";
                } catch (const std::filesystem::filesystem_error&) {
                } catch (const std::runtime_error&) {
                }
            } else
                text = ReadScript(std::filesystem::path(Wide(name)));
            const JSValue compiled =
                JS_Eval(ctx, text.data(), text.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(compiled)) return nullptr;
            auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
            JS_FreeValue(ctx, compiled);
            return module;
        } catch (const std::exception& error) {
            JS_ThrowReferenceError(ctx, "%s", error.what());
            return nullptr;
        }
    }

    JSValue Http(JSValueConst options, JSValueConst callback) {
        ScriptRequest request;
        bool raw = false;
        if (JS_IsString(options))
            request.url = String(context, options);
        else {
            request.url = Property(context, options, "url");
            if (auto method = Property(context, options, "method"); !method.empty()) request.method = method;
            request.body = Property(context, options, "body");
            request.timeoutMs = std::clamp(Number(context, options, "timeout", 6000), 1, 20000);
            Value rawValue(context, JS_GetPropertyStr(context, options, "raw"));
            raw = JS_ToBool(context, rawValue) > 0;
            Value headers(context, JS_GetPropertyStr(context, options, "headers"));
            JSPropertyEnum* properties = nullptr;
            uint32_t count = 0;
            if (JS_IsObject(headers) && JS_GetOwnPropertyNames(context, &properties, &count, headers,
                                                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
                for (uint32_t i = 0; i < count; ++i) {
                    const char* name = JS_AtomToCString(context, properties[i].atom);
                    if (name) request.headers[name] = Property(context, headers, name);
                    JS_FreeCString(context, name);
                }
                JS_FreePropertyEnum(context, properties, count);
            }
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count();
        request.timeoutMs =
            static_cast<int>(std::min<long long>(request.timeoutMs, std::max(1ll, remaining)));
        ScriptResponse response;
        if (Stopped())
            response.error = "Lyrics search cancelled";
        else if (environment.request)
            response = environment.request(request);
        else
            response.error = "HTTP is unavailable";
        Value res(context, JS_NewObject(context));
        JS_SetPropertyStr(context, res, "statusCode", JS_NewInt32(context, response.status));
        Set(context, res, "statusMessage", response.message);
        Value headers(context, JS_NewObject(context));
        for (const auto& [name, value] : response.headers)
            Set(context, headers, name.c_str(), value);
        JS_SetPropertyStr(context, res, "headers", headers.Release());
        Value error(context,
                    response.error.empty() ? JS_NULL : JS_NewString(context, response.error.c_str()));
        Value body(context, raw ? Buffer(context, response.body)
                                : JS_NewStringLen(context, response.body.data(), response.body.size()));
        JSValue args[] = {error, res, body};
        Value returned(context, JS_Call(context, callback, JS_UNDEFINED, 3, args));
        Check(returned);
        return JS_UNDEFINED;
    }

    JSValue Add(JSValueConst lyric) {
        ScriptLyric result;
        result.title = Property(context, lyric, "title");
        result.artist = Property(context, lyric, "artist");
        result.album = Property(context, lyric, "album");
        result.source = Property(context, lyric, "source");
        result.confidence = Number(context, lyric, "confidence", -1);
        const std::string type = Property(context, lyric, "fileType");
        result.text = Property(context, lyric, "lyricText");
        if (!type.empty() && type != "lrc") {
            const auto parser = std::find_if(environment.parsers.begin(), environment.parsers.end(),
                                             [&](const auto& p) { return p.fileType == type; });
            if (parser == environment.parsers.end()) return JS_UNDEFINED;
            if (!parser->plainText || result.text.empty()) {
                Value data(context, JS_GetPropertyStr(context, lyric, "lyricData"));
                result.text = Bytes(context, data);
            }
            result.text = Parse(*parser, std::move(result.text), duration);
        } else if (result.text.empty()) {
            Value data(context, JS_GetPropertyStr(context, lyric, "lyricData"));
            result.text = Bytes(context, data);
        }
        if (!Stopped() && !result.text.empty() && environment.found) environment.found(std::move(result));
        return JS_UNDEFINED;
    }

    JSValue XmlNode(IXMLDOMNode* node, unsigned depth) {
        if (depth > 64 || ++xmlNodes > 10000) throw std::runtime_error("XML size or nesting limit exceeded");
        Value result(context, JS_NewObject(context));
        BSTR name = nullptr, text = nullptr;
        DOMNodeType type{};
        node->get_nodeName(&name);
        node->get_text(&text);
        node->get_nodeType(&type);
        JS_SetPropertyStr(context, result, "name",
                          JS_NewStringUTF16(context, reinterpret_cast<const uint16_t*>(name ? name : L""),
                                            SysStringLen(name)));
        JS_SetPropertyStr(context, result, "text",
                          JS_NewStringUTF16(context, reinterpret_cast<const uint16_t*>(text ? text : L""),
                                            SysStringLen(text)));
        SysFreeString(name);
        SysFreeString(text);
        JS_SetPropertyStr(context, result, "type", JS_NewInt32(context, type));
        Value attrs(context, JS_NewObject(context));
        Microsoft::WRL::ComPtr<IXMLDOMNamedNodeMap> attributes;
        node->get_attributes(&attributes);
        if (attributes) {
            Microsoft::WRL::ComPtr<IXMLDOMNode> attribute;
            while (attributes->nextNode(&attribute) == S_OK && attribute) {
                attribute->get_nodeName(&name);
                attribute->get_text(&text);
                Value key(context, JS_NewStringUTF16(context, reinterpret_cast<const uint16_t*>(name),
                                                     SysStringLen(name)));
                const std::string utf8 = String(context, key);
                JS_SetPropertyStr(
                    context, attrs, utf8.c_str(),
                    JS_NewStringUTF16(context, reinterpret_cast<const uint16_t*>(text), SysStringLen(text)));
                SysFreeString(name);
                SysFreeString(text);
                attribute.Reset();
            }
        }
        JS_SetPropertyStr(context, result, "attrs", attrs.Release());
        Value children(context, JS_NewArray(context));
        Microsoft::WRL::ComPtr<IXMLDOMNodeList> list;
        node->get_childNodes(&list);
        Microsoft::WRL::ComPtr<IXMLDOMNode> child;
        uint32_t index = 0;
        while (list && list->nextNode(&child) == S_OK && child) {
            if (Stopped()) throw std::runtime_error("XML parsing cancelled");
            JS_SetPropertyUint32(context, children, index++, XmlNode(child.Get(), depth + 1));
            child.Reset();
        }
        JS_SetPropertyStr(context, result, "children", children.Release());
        return result.Release();
    }

    JSValue Xml(JSValueConst value) {
        xmlNodes = 0;
        const auto text = Wide(String(context, value));
        if (text.size() > 4 * 1024 * 1024) return JS_NULL;
        Microsoft::WRL::ComPtr<IXMLDOMDocument2> document;
        if (FAILED(CoCreateInstance(CLSID_DOMDocument60, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&document))))
            return JS_NULL;
        document->put_async(VARIANT_FALSE);
        document->put_resolveExternals(VARIANT_FALSE);
        document->put_validateOnParse(VARIANT_FALSE);
        BSTR property = SysAllocString(L"ProhibitDTD");
        VARIANT flag{};
        flag.vt = VT_BOOL;
        flag.boolVal = VARIANT_TRUE;
        document->setProperty(property, flag);
        SysFreeString(property);
        BSTR input = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        VARIANT_BOOL loaded = VARIANT_FALSE;
        document->loadXML(input, &loaded);
        SysFreeString(input);
        return loaded ? XmlNode(document.Get(), 0) : JS_NULL;
    }

    JSValue Base64(JSValueConst value, bool encode) {
        if (encode) {
            // ESLyric sources pass Unicode metadata directly to btoa, and use atob for
            // UTF-8 JSON / translated lyrics. Preserve that script contract.
            const std::string bytes = String(context, value);
            DWORD size = 0;
            CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),
                                 static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 nullptr, &size);
            std::string out(size, 0);
            if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),
                                      static_cast<DWORD>(bytes.size()),
                                      CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &size))
                return JS_NULL;
            return JS_NewStringLen(context, out.data(), size);
        }
        const std::string text = String(context, value);
        DWORD size = 0;
        if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr,
                                  &size, nullptr, nullptr))
            return JS_ThrowTypeError(context, "Invalid base64");
        std::string bytes(size, 0);
        if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                  reinterpret_cast<BYTE*>(bytes.data()), &size, nullptr, nullptr))
            return JS_NULL;
        return JS_NewStringLen(context, bytes.data(), bytes.size());
    }

    static JSValue Native(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int operation) {
        auto& self = *static_cast<Impl*>(JS_GetContextOpaque(ctx));
        const JSValue first = argc > 0 ? argv[0] : JS_UNDEFINED;
        try {
            switch (operation) {
            case 0:
                return self.Http(first, argc > 1 ? argv[1] : JS_UNDEFINED);
            case 1:
                self.Jobs();
                return JS_UNDEFINED;
            case 2: {
                const auto path = self.Resolve("", String(ctx, first));
                const auto code = ReadScript(path), name = PathName(path);
                Value result(ctx, JS_Eval(ctx, code.data(), code.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL));
                self.Check(result);
                return JS_UNDEFINED;
            }
            case 3:
                return self.Base64(first, false);
            case 4:
                return self.Base64(first, true);
            case 5: {
                const auto bytes = Bytes(ctx, first);
                return JS_NewStringLen(ctx, bytes.data(), bytes.size());
            }
            case 6:
                return Buffer(ctx, String(ctx, first));
            case 7: {
                std::string text;
                for (int i = 0; i < argc; ++i) {
                    if (i) text += ' ';
                    text += String(ctx, argv[i]);
                }
                if (self.environment.log) self.environment.log(text);
                return JS_UNDEFINED;
            }
            case 8:
                return self.Xml(first);
            case 9: {
                const auto input = Bytes(ctx, first);
                if (input.empty()) return JS_NULL;
                std::string output(4096, 0);
                int result = MZ_BUF_ERROR;
                mz_ulong size = 0;
                while (result == MZ_BUF_ERROR && output.size() <= 16 * 1024 * 1024 && !self.Stopped()) {
                    size = static_cast<mz_ulong>(output.size());
                    result = mz_uncompress(reinterpret_cast<unsigned char*>(output.data()), &size,
                                           reinterpret_cast<const unsigned char*>(input.data()),
                                           static_cast<mz_ulong>(input.size()));
                    if (result == MZ_BUF_ERROR) output.resize(output.size() * 2);
                }
                if (result != MZ_OK) return JS_NULL;
                output.resize(size);
                return Buffer(ctx, output);
            }
            case 10:
                return self.Add(first);
            case 11:
                return JS_NewBool(ctx, self.Stopped());
            case 12: {
                const std::string result =
                    self.environment.getData ? self.environment.getData(String(ctx, first)) : "";
                return JS_NewStringLen(ctx, result.data(), result.size());
            }
            case 13:
                if (self.environment.setData && argc > 1)
                    self.environment.setData(String(ctx, first), String(ctx, argv[1]));
                return JS_UNDEFINED;
            }
            return JS_UNDEFINED;
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(ctx, "%s", error.what());
        }
    }

    ScriptEnvironment environment;
    JSRuntime* runtime = nullptr;
    JSContext* context = nullptr;
    bool comInitialized = false;
    double duration = 0;
    unsigned xmlNodes = 0;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
};

ScriptHost::ScriptHost(ScriptEnvironment environment)
    : m_impl(std::make_unique<Impl>(std::move(environment))) {}
ScriptHost::~ScriptHost() = default;
ScriptConfig ScriptHost::Configure(const std::filesystem::path& path) {
    return m_impl->Configure(path);
}
void ScriptHost::Search(const ScriptConfig& script, const std::map<std::string, std::string>& meta,
                        unsigned subsong, double duration) {
    m_impl->Search(script, meta, subsong, duration);
}
std::string ScriptHost::Parse(const ScriptConfig& parser, std::string text, double duration) {
    return m_impl->Parse(parser, std::move(text), duration);
}

} // namespace glasswave::lyrics
