#include "webcam_enum.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <objbase.h>
  #include <initguid.h>
  #include <dshow.h>
  #include <string>
#endif

namespace {

#ifdef _WIN32
std::string NarrowFromWide(const wchar_t *w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
#endif

} // namespace

std::vector<HrWebcamDevice> HrEnumerateWebcams() {
    std::vector<HrWebcamDevice> out;

#ifdef _WIN32
    // This can be called from the UI thread at any point after the app's
    // already up, when COM may or may not already be initialized here
    // (wxWidgets typically does its own OleInitialize on the main thread).
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool should_uninit = (hr_co == S_OK || hr_co == S_FALSE);

    ICreateDevEnum *dev_enum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ICreateDevEnum, reinterpret_cast<void **>(&dev_enum));
    if (SUCCEEDED(hr) && dev_enum) {
        IEnumMoniker *enum_moniker = nullptr;
        hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0);
        // S_OK with a non-null enumerator means devices exist; S_FALSE (or a
        // null enumerator) means the category is empty -- not an error, just
        // "no cameras attached".
        if (hr == S_OK && enum_moniker) {
            IMoniker *moniker = nullptr;
            int idx = 0;
            while (enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
                IPropertyBag *props = nullptr;
                if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                                      reinterpret_cast<void **>(&props))) && props) {
                    VARIANT var;
                    VariantInit(&var);
                    std::string name;
                    if (SUCCEEDED(props->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR) {
                        name = NarrowFromWide(var.bstrVal);
                    }
                    VariantClear(&var);
                    if (name.empty()) name = "Camera " + std::to_string(idx);

                    HrWebcamDevice dev;
                    dev.index = idx;
                    dev.name = name;
                    out.push_back(dev);

                    props->Release();
                }
                moniker->Release();
                ++idx;
            }
            enum_moniker->Release();
        }
        dev_enum->Release();
    }

    if (should_uninit) CoUninitialize();
#endif

    return out;
}
