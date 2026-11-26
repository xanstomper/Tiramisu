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
void set_launcher_cursor_visible(bool visible);
// Status of the guest D3D11 pipeline query. A successful Present does
// not imply that the guest issued draws or reached a visible scene.
struct guest_draw_probe {
  unsigned state;  // 0 idle, 1 measuring, 2 draw observed, 3 failed, 4 exhausted
  unsigned samples;
  unsigned long long vertices;
  unsigned long long pixel_invocations;
  int result;
  unsigned surface_samples;
  unsigned guest_lit_pixels;
  unsigned host_lit_pixels;
  int surface_result;
  unsigned nonempty_draw_samples, guest_rtv_samples, other_rtv_samples;
  unsigned other_surface_samples, other_lit_pixels;
  unsigned other_width, other_height, other_format;
  int other_surface_result;
};
void swapchain_reset_guest_draw_probe();
guest_draw_probe swapchain_guest_draw_probe();

void swapchain_present();
void swapchain_shutdown();

} // namespace longan

#endif
