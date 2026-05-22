/*
 * monitor_enum.cpp — HomRec 2.0
 * Enumerates all active DXGI/D3D11 monitors at call time.
 * Intentionally has NO persistent state — safe to call from any thread
 * at any time, even while the capture pipeline is running.
 */

#include "monitor_enum.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace homrec {

// -- String helpers ------------------------------------------------------------

static std::string wcs_to_utf8(const wchar_t* wcs) {
    if (!wcs || !wcs[0]) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, wcs, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 0) return {};
    std::string out(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wcs, -1, out.data(), sz, nullptr, nullptr);
    return out;
}

// -- enumerate_monitors() -----------------------------------------------------

std::vector<MonitorInfo> enumerate_monitors() {
    std::vector<MonitorInfo> result;

    // -- Create a temporary D3D11 device (no window, no swap chain) --------
    ID3D11Device* device = nullptr;
    D3D_FEATURE_LEVEL fl{};
    D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_11_1,
                                   D3D_FEATURE_LEVEL_11_0,
                                   D3D_FEATURE_LEVEL_10_1 };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, wanted, ARRAYSIZE(wanted), D3D11_SDK_VERSION,
        &device, &fl, nullptr);
    if (FAILED(hr)) return result;

    // -- Walk adapters → outputs -------------------------------------------
    IDXGIDevice*  dxgi_dev  = nullptr;
    IDXGIAdapter* adapter   = nullptr;

    device->QueryInterface(__uuidof(IDXGIDevice),
                           reinterpret_cast<void**>(&dxgi_dev));
    if (dxgi_dev) {
        dxgi_dev->GetParent(__uuidof(IDXGIAdapter),
                            reinterpret_cast<void**>(&adapter));
        dxgi_dev->Release();
    }

    int monitor_index = 0;
    if (adapter) {
        IDXGIOutput* output = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                MonitorInfo mi;
                mi.index       = monitor_index++;
                mi.device_name = wcs_to_utf8(desc.DeviceName);
                mi.width       = static_cast<UINT32>(
                    desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
                mi.height      = static_cast<UINT32>(
                    desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
                mi.desktop_rect = desc.DesktopCoordinates;

                // Friendly name = "Display N" or device name
                mi.friendly_name = "Display " + std::to_string(mi.index + 1)
                                 + "  (" + std::to_string(mi.width)
                                 + "×" + std::to_string(mi.height) + ")";

                // Primary monitor has its top-left at (0,0)
                mi.is_primary = (desc.DesktopCoordinates.left == 0 &&
                                 desc.DesktopCoordinates.top  == 0);

                // Refresh rate via DXGI mode enumeration (first mode)
                IDXGIOutput1* out1 = nullptr;
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1),
                                reinterpret_cast<void**>(&out1)))) {
                    UINT mode_count = 0;
                    out1->GetDisplayModeList1(DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                                              &mode_count, nullptr);
                    if (mode_count > 0) {
                        std::vector<DXGI_MODE_DESC1> modes(mode_count);
                        out1->GetDisplayModeList1(DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                                                  &mode_count, modes.data());
                        // Pick the mode matching our resolution with the highest refresh
                        UINT best_hz = 0;
                        for (auto& m : modes) {
                            if (m.Width == mi.width && m.Height == mi.height) {
                                UINT hz = m.RefreshRate.Numerator
                                        / std::max(1u, m.RefreshRate.Denominator);
                                if (hz > best_hz) best_hz = hz;
                            }
                        }
                        mi.refresh_hz = best_hz;
                    }
                    out1->Release();
                }

                result.push_back(mi);
            }
            output->Release();
        }
        adapter->Release();
    }

    device->Release();
    return result;
}

} // namespace homrec
