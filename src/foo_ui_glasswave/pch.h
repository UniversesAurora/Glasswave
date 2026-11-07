#pragma once

// Windows / COM ---------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>
#include <unknwn.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>

// DirectX ---------------------------------------------------------------------
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <d2d1effects_2.h>
#include <dwrite_3.h>
#include <wincodec.h>

// Windows.UI.Composition (part of the OS, no Windows App SDK) --------------------
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.ViewManagement.h>

// C++ standard library -----------------------------------------------------------
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// foobar2000 SDK --------------------------------------------------------------
#include <SDK/foobar2000.h>
#include <SDK/ui.h>
