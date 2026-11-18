// tiramisu/src/host/swapchain.h
#ifndef TIRAMISU_SWAPCHAIN_H
#define TIRAMISU_SWAPCHAIN_H

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.UI.Core.h>

namespace longan {

struct swapchain_result {
  HRESULT device_hr;
  HRESULT sc_hr;
  unsigned width, height;
  unsigned feature_level;  // achieved D3D_FEATURE_LEVEL
};

// create a flip-model D3D11 swapchain over the CoreWindow. FLIP_DISCARD +
// BGRA: the only composition-friendly model on Xbox (no exclusive fullscreen).
swapchain_result swapchain_create(winrt::Windows::UI::Core::CoreWindow const& w);

void swapchain_render_frame(float t);   // t in [0,1) animates the backdrop
void* swapchain_dxgi();                 // IDXGISwapChain1* for the UI layer
void* swapchain_d3d();                  // ID3D11Device1* for the UI layer
void swapchain_bind_d3d9();             // bind the D3D9->D3D11 drawing backend
void swapchain_restore_d3d11();         // rebind D3D11 CoreWindow swapchain if guest used D3D12
void swapchain_restore_launcher_format();
void draw_pointer(bool launcher);
void set_pointer_cursor_mode(bool kbm_active);
void swapchain_present();
void swapchain_shutdown();

} // namespace longan

#endif
