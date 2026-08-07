#include "hr_mic_enum.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <propkeydef.h>
  #include <functiondiscoverykeys_devpkey.h>
#endif

namespace {

#ifdef _WIN32
// NOTE: deliberately *not* reusing the symbol name PKEY_Device_FriendlyName
// that hr_audio.cpp defines for itself (that .cpp's copy has external
// linkage, and a second definition of the same name in this translation
// unit would be a duplicate-symbol link error) -- this is the same GUID,
// just under a name local to this file.
static const PROPERTYKEY kPkeyFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd,
      { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

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

std::vector<HrMicDevice> HrEnumerateMics() {
    std::vector<HrMicDevice> out;

#ifdef _WIN32
    // Same reasoning as HrEnumerateWebcams() in hr_webcam_enum.cpp: this may
    // run on a thread that already has COM initialized (by wx or otherwise),
    // so only release what we actually ended up owning ourselves.
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool should_uninit = (hr_co == S_OK || hr_co == S_FALSE);

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void **>(&enumerator));
    if (SUCCEEDED(hr) && enumerator) {
        IMMDeviceCollection *col = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT count = 0;
            col->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice *dev = nullptr;
                if (FAILED(col->Item(i, &dev)) || !dev) continue;

                HrMicDevice mic;

                LPWSTR id_str = nullptr;
                if (SUCCEEDED(dev->GetId(&id_str)) && id_str) {
                    mic.id = NarrowFromWide(id_str);
                    CoTaskMemFree(id_str);
                }

                IPropertyStore *props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                    PROPVARIANT var;
                    PropVariantInit(&var);
                    if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &var)) && var.vt == VT_LPWSTR) {
                        mic.name = NarrowFromWide(var.pwszVal);
                    }
                    PropVariantClear(&var);
                    props->Release();
                }
                if (mic.name.empty()) mic.name = "Microphone " + std::to_string(i);

                if (!mic.id.empty()) out.push_back(mic);
                dev->Release();
            }
            col->Release();
        }
        enumerator->Release();
    }

    if (should_uninit) CoUninitialize();
#endif

    return out;
}
