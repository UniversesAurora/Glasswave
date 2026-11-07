#pragma once
// Original Windows 7 artwork, read at run time from files the user provides (ADR 0007).
//
// Glasswave ships no Microsoft bitmaps. When the user copies Win7's wmploc.dll,
// aero.msstyles, ehres.dll (and later uDWM.dll) into `<profile>\glasswave\`, the UI
// draws their PNG resources; otherwise each control falls back to procedural painting.
// The files are mapped as image resources only (LOAD_LIBRARY_AS_IMAGE_RESOURCE): no
// code from them is ever executed.
//
// One bitmap pixel is drawn as one DIP: the artwork keeps WMP 12's 96-DPI design
// proportions and is resampled (high-quality cubic) at other scales.
// Main thread only.

namespace glasswave::win7 {

// AeroAtlas is the DWM image atlas inside aero.msstyles (STREAM resource 971).
enum class Module { Wmploc, Aero, AeroAtlas, Ehres, Count };

class Resources {
public:
    static Resources& Get();
    static void Shutdown();  // before the D2D device goes away

    // Resources are named like FindResource names them: "#4153" for numeric IDs, or a
    // string name such as "ALBUM_0_PNG". The int overloads take numeric IDs.
    bool Has(Module module) const { return Handle(module) != nullptr; }
    bool Has(Module module, const std::wstring& name);
    bool Has(Module module, int id) { return Has(module, L"#" + std::to_wstring(id)); }
    // Size of a resource bitmap in pixels (0x0 when missing).
    D2D1_SIZE_U Size(Module module, const std::wstring& name);
    D2D1_SIZE_U Size(Module module, int id) { return Size(module, L"#" + std::to_wstring(id)); }
    // Device bitmap for `dc`'s device; nullptr when the file or resource is missing.
    ID2D1Bitmap* Bitmap(ID2D1DeviceContext* dc, Module module, const std::wstring& name);
    ID2D1Bitmap* Bitmap(ID2D1DeviceContext* dc, Module module, int id) {
        return Bitmap(dc, module, L"#" + std::to_wstring(id));
    }
    // A copy of one rectangle of a resource as its own bitmap, so filtering never reaches
    // the neighbouring pixels of an atlas (cached).
    ID2D1Bitmap* Piece(ID2D1DeviceContext* dc, Module module, const std::wstring& name, const D2D1_RECT_U& src);

    const std::wstring& Folder() const { return m_folder; }

    ~Resources();
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;

private:
    Resources();
    HMODULE Handle(Module module) const { return m_modules[static_cast<size_t>(module)]; }
    IWICBitmapSource* Decode(Module module, const std::wstring& name);
    winrt::com_ptr<IWICBitmapSource> LoadHd(Module module, const std::wstring& name, IWICBitmapSource* base);

    std::wstring m_folder;
    std::array<HMODULE, static_cast<size_t>(Module::Count)> m_modules{};
    std::map<std::pair<int, std::wstring>, winrt::com_ptr<IWICBitmap>> m_decoded;  // nullptr = known missing
    std::map<std::pair<int, std::wstring>, winrt::com_ptr<ID2D1Bitmap>> m_device;
    std::map<std::wstring, winrt::com_ptr<ID2D1Bitmap>> m_pieces;
};

// Draw a whole resource with its top-left corner at `at` (DIP). False if missing.
bool Draw(ID2D1DeviceContext* dc, Module module, int id, D2D1_POINT_2F at, float opacity = 1.0f);
// Draw the `src` part (bitmap pixels) of a resource at `at` (DIP).
bool DrawPart(ID2D1DeviceContext* dc, Module module, int id, D2D1_RECT_F src, D2D1_POINT_2F at,
              float opacity = 1.0f);
// Draw `src` (whole bitmap pixels) of a resource as a nine-grid into `dst` (DIP). `margins`
// are the fixed borders in bitmap pixels (left, top, right, bottom). Like Win7's DWM, the
// borders are drawn one bitmap pixel per device pixel at every DPI (hairlines stay
// hairlines); only the middle stretches. Pieces landing on whole device pixels are copied
// without filtering.
bool DrawNineGrid(ID2D1DeviceContext* dc, Module module, const std::wstring& name, D2D1_RECT_F src,
                  D2D1_RECT_F margins, D2D1_RECT_F dst, float opacity = 1.0f);
inline bool DrawNineGrid(ID2D1DeviceContext* dc, Module module, int id, D2D1_RECT_F src, D2D1_RECT_F margins,
                         D2D1_RECT_F dst, float opacity = 1.0f) {
    return DrawNineGrid(dc, module, L"#" + std::to_wstring(id), src, margins, dst, opacity);
}
// Draw a whole resource scaled into `dst` (DIP).
bool DrawStretched(ID2D1DeviceContext* dc, Module module, const std::wstring& name, D2D1_RECT_F dst,
                   float opacity = 1.0f);

}  // namespace glasswave::win7
