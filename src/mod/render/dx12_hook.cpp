#include "mod/render/dx12_hook.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <safetyhook.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "mod/log.h"

using Microsoft::WRL::ComPtr;

namespace cdmp::render {
namespace {

// vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, ...).
constexpr int kSlotPresent = 8;
constexpr int kSlotResizeBuffers = 13;
constexpr int kSlotPresent1 = 22;
constexpr int kSlotResizeBuffers1 = 39;
constexpr int kSlotExecuteCommandLists = 10;  // ID3D12CommandQueue

constexpr UINT kSrvDescriptors = 256;
// If the swap chain we draw on stops presenting while another one keeps going
// (the game recreated it), switch after this many foreign presents.
constexpr int kSwitchAfterForeignPresents = 120;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
                                                     DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);

PresentFn o_present = nullptr;
Present1Fn o_present1 = nullptr;
ResizeBuffersFn o_resize = nullptr;
ResizeBuffers1Fn o_resize1 = nullptr;
ExecuteCommandListsFn o_execute = nullptr;
void** g_queue_vtable = nullptr;
std::atomic<bool> g_execute_hooked{false};
thread_local ID3D12CommandQueue* t_last_direct_queue = nullptr;
thread_local bool t_in_present = false;

DrawFn g_draw = nullptr;
CRITICAL_SECTION g_render_cs;
std::atomic<bool> g_disabled{false};
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_frames{0};

std::mutex g_status_mutex;
Status g_status;

void note(const std::string& queue_source, const std::string& error) {
    std::lock_guard lock(g_status_mutex);
    if (!queue_source.empty()) g_status.queue_source = queue_source;
    if (!error.empty()) g_status.last_error = error;
}

void disable(const std::string& why) {
    if (!g_disabled.exchange(true)) {
        log::error("Overlay disabled: {}", why);
        note({}, why);
    }
}

// Inline-hooks the function a vtable slot points to. Patching the vtable slot
// itself is not enough in this game: Streamline / DLSS frame generation call
// DXGI's Present directly, never through the vtable (verified in game: 0 calls
// through a patched slot). Hooking the code catches every caller.
template <class Fn>
bool hook_function(void* target, void* detour, SafetyHookInline& hook, Fn& original,
                   const char* name) {
    uint8_t head[5] = {};
    std::memcpy(head, target, sizeof head);
    hook = safetyhook::create_inline(target, detour, SafetyHookInline::StartDisabled);
    if (!hook) {
        log::error("Overlay: cannot hook {} at 0x{:X}", name, reinterpret_cast<uintptr_t>(target));
        return false;
    }
    original = hook.original<Fn>();  // publish the trampoline before enabling
    if (!hook.enable()) {
        log::error("Overlay: cannot enable hook on {}", name);
        return false;
    }
    log::info("Overlay: hooked {} at 0x{:X} (first bytes {:02X} {:02X} {:02X} {:02X} {:02X})", name,
              reinterpret_cast<uintptr_t>(target), head[0], head[1], head[2], head[3], head[4]);
    return true;
}

SafetyHookInline g_present_hook;
SafetyHookInline g_present1_hook;
SafetyHookInline g_resize_hook;
SafetyHookInline g_resize1_hook;
SafetyHookInline g_execute_hook;

void STDMETHODCALLTYPE hk_execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) t_last_direct_queue = q;
    o_execute(q, n, lists);
}

// ------------------------------------------------------------------ renderer

struct Renderer {
    IDXGISwapChain* owner = nullptr;  // identity only; never AddRef'd
    int foreign_presents = 0;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    UINT rtv_size = 0;
    UINT srv_size = 0;
    std::vector<bool> srv_used;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<UINT64> allocator_fence;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    UINT buffer_count = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool imgui_ready = false;
    LARGE_INTEGER last_frame{};
};

Renderer g_r;
ImGuiContext* g_imgui = nullptr;

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
               D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    for (UINT i = 0; i < kSrvDescriptors; ++i) {
        if (g_r.srv_used[i]) continue;
        g_r.srv_used[i] = true;
        cpu->ptr = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * g_r.srv_size;
        gpu->ptr = g_r.srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + i * g_r.srv_size;
        return;
    }
    // Out of descriptors: hand out slot 0 rather than garbage (glyphs may glitch).
    *cpu = g_r.srv_heap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_r.srv_heap->GetGPUDescriptorHandleForHeapStart();
}

void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
              D3D12_GPU_DESCRIPTOR_HANDLE) {
    const UINT i = static_cast<UINT>(
        (cpu.ptr - g_r.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr) / g_r.srv_size);
    if (i < kSrvDescriptors) g_r.srv_used[i] = false;
}

void wait_gpu_idle() {
    if (!g_r.fence || !g_r.queue) return;
    const UINT64 v = ++g_r.fence_value;
    if (FAILED(g_r.queue->Signal(g_r.fence.Get(), v))) return;
    if (g_r.fence->GetCompletedValue() < v) {
        g_r.fence->SetEventOnCompletion(v, g_r.fence_event);
        WaitForSingleObject(g_r.fence_event, 2000);
    }
}

void shutdown_renderer() {
    wait_gpu_idle();
    if (g_r.imgui_ready) ImGui_ImplDX12_Shutdown();
    if (g_r.fence_event) CloseHandle(g_r.fence_event);
    g_r = Renderer{};
}

ComPtr<ID3D12CommandQueue> find_queue(IDXGISwapChain* sc, std::string& source) {
    ComPtr<ID3D12CommandQueue> queue;
    // For D3D12 swap chains, GetDevice() hands back the queue the chain was
    // created with, which is exactly the queue we must submit on.
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&queue)))) {
        source = "swapchain->GetDevice";
        return queue;
    }
    // Fallback: the last direct queue that executed on this (present) thread.
    if (!g_execute_hooked.exchange(true)) {
        log::info("Overlay: GetDevice gave no queue; watching ExecuteCommandLists instead");
        hook_function(g_queue_vtable[kSlotExecuteCommandLists],
                      reinterpret_cast<void*>(&hk_execute), g_execute_hook, o_execute,
                      "ExecuteCommandLists");
    }
    if (t_last_direct_queue) {
        queue = t_last_direct_queue;
        source = "ExecuteCommandLists (present thread)";
    }
    return queue;
}

bool init_renderer(IDXGISwapChain* sc, const DXGI_SWAP_CHAIN_DESC& desc) {
    std::string source;
    ComPtr<ID3D12CommandQueue> queue = find_queue(sc, source);
    if (!queue) return false;  // try again next frame
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        disable("present queue is not a direct queue");
        return false;
    }

    Renderer r;
    r.owner = sc;
    r.queue = queue;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&r.device)))) return disable("no device"), false;
    r.buffer_count = desc.BufferCount;
    r.format = desc.BufferDesc.Format;

    D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, r.buffer_count,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvDescriptors,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(r.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&r.rtv_heap))) ||
        FAILED(r.device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&r.srv_heap)))) {
        return disable("descriptor heap creation failed"), false;
    }
    r.rtv_size = r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    r.srv_size = r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    r.srv_used.assign(kSrvDescriptors, false);

    r.allocators.resize(r.buffer_count);
    r.allocator_fence.assign(r.buffer_count, 0);
    for (auto& a : r.allocators) {
        if (FAILED(r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&a)))) {
            return disable("command allocator creation failed"), false;
        }
    }
    if (FAILED(r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           r.allocators[0].Get(), nullptr,
                                           IID_PPV_ARGS(&r.list))) ||
        FAILED(r.list->Close()) ||
        FAILED(r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)))) {
        return disable("command list / fence creation failed"), false;
    }
    r.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    QueryPerformanceCounter(&r.last_frame);
    g_r = std::move(r);

    if (!g_imgui) {
        IMGUI_CHECKVERSION();
        g_imgui = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // never write imgui.ini into the game folder
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();
    }

    ImGui_ImplDX12_InitInfo info;
    info.Device = g_r.device.Get();
    info.CommandQueue = g_r.queue.Get();
    info.NumFramesInFlight = static_cast<int>(g_r.buffer_count);
    info.RTVFormat = g_r.format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_r.srv_heap.Get();
    info.SrvDescriptorAllocFn = srv_alloc;
    info.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&info)) return disable("ImGui DX12 init failed"), false;
    g_r.imgui_ready = true;

    note(source, {});
    log::info("Overlay ready: {} buffers, format {}, {}x{}, queue via {}", desc.BufferCount,
              static_cast<int>(desc.BufferDesc.Format), desc.BufferDesc.Width,
              desc.BufferDesc.Height, source);
    return true;
}

void draw_frame(IDXGISwapChain* sc, const DXGI_SWAP_CHAIN_DESC& desc) {
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return disable("no IDXGISwapChain3");
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    if (idx >= g_r.buffer_count) return;

    // Make sure the GPU is done with this buffer's allocator from last time.
    if (g_r.fence->GetCompletedValue() < g_r.allocator_fence[idx]) {
        g_r.fence->SetEventOnCompletion(g_r.allocator_fence[idx], g_r.fence_event);
        if (WaitForSingleObject(g_r.fence_event, 1000) != WAIT_OBJECT_0) return;  // skip frame
    }

    ComPtr<ID3D12Resource> back_buffer;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&back_buffer)))) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_r.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(idx) * g_r.rtv_size;
    g_r.device->CreateRenderTargetView(back_buffer.Get(), nullptr, rtv);

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = static_cast<float>(now.QuadPart - g_r.last_frame.QuadPart) / freq.QuadPart;
    g_r.last_frame = now;

    const float w = static_cast<float>(desc.BufferDesc.Width);
    const float h = static_cast<float>(desc.BufferDesc.Height);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime = dt > 0.0f && dt < 1.0f ? dt : 1.0f / 60.0f;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (g_draw) g_draw(w, h);
    ImGui::Render();

    auto& alloc = g_r.allocators[idx];
    if (FAILED(alloc->Reset()) || FAILED(g_r.list->Reset(alloc.Get(), nullptr))) return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = back_buffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_r.list->ResourceBarrier(1, &barrier);
    g_r.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g_r.srv_heap.Get()};
    g_r.list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_r.list.Get());
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_r.list->ResourceBarrier(1, &barrier);
    if (FAILED(g_r.list->Close())) return disable("command list Close failed");

    ID3D12CommandList* lists[] = {g_r.list.Get()};
    g_r.queue->ExecuteCommandLists(1, lists);
    g_r.queue->Signal(g_r.fence.Get(), ++g_r.fence_value);
    g_r.allocator_fence[idx] = g_r.fence_value;
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

void on_present(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return;
    // Ignore tiny helper swap chains; the game's is at least a few hundred px.
    if (desc.BufferDesc.Width < 320 || desc.BufferDesc.Height < 240) return;

    if (g_r.owner && sc != g_r.owner) {
        if (++g_r.foreign_presents < kSwitchAfterForeignPresents) return;
        log::info("Overlay: switching to a new swap chain");
        shutdown_renderer();
    }
    if (g_r.owner == sc) g_r.foreign_presents = 0;

    // Rebuild if the chain changed shape (buffer count / format).
    if (g_r.owner == sc &&
        (desc.BufferCount != g_r.buffer_count || desc.BufferDesc.Format != g_r.format)) {
        log::info("Overlay: swap chain changed ({} buffers, format {}), rebuilding",
                  desc.BufferCount, static_cast<int>(desc.BufferDesc.Format));
        shutdown_renderer();
    }
    if (!g_r.owner && !init_renderer(sc, desc)) return;
    draw_frame(sc, desc);
}

// SEH wrapper: no C++ objects with destructors allowed in this function.
void guarded_present(IDXGISwapChain* sc) {
    EnterCriticalSection(&g_render_cs);
    __try {
        on_present(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_disabled = true;
        log::write(log::Level::Error, "Overlay disabled: exception inside the render hook");
    }
    LeaveCriticalSection(&g_render_cs);
}

void before_resize(IDXGISwapChain* sc) {
    EnterCriticalSection(&g_render_cs);
    __try {
        // We hold no back-buffer references, so only make sure our commands on
        // the old buffers have finished.
        if (sc == g_r.owner) wait_gpu_idle();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_disabled = true;
    }
    LeaveCriticalSection(&g_render_cs);
}

// ------------------------------------------------------------------- hooks

void present_common(IDXGISwapChain* sc, UINT flags) {
    g_presents.fetch_add(1, std::memory_order_relaxed);
    if (g_disabled || (flags & DXGI_PRESENT_TEST) || t_in_present) return;
    t_in_present = true;  // Present may call Present1 internally; draw once
    guarded_present(sc);
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    present_common(sc, flags);
    HRESULT hr = o_present(sc, sync, flags);
    t_in_present = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* params) {
    present_common(sc, flags);
    HRESULT hr = o_present1(sc, sync, flags, params);
    t_in_present = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                    DXGI_FORMAT fmt, UINT flags) {
    before_resize(sc);
    return o_resize(sc, count, w, h, fmt, flags);
}

HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h,
                                     DXGI_FORMAT fmt, UINT flags, const UINT* masks,
                                     IUnknown* const* queues) {
    before_resize(sc);
    return o_resize1(sc, count, w, h, fmt, flags, masks, queues);
}

// Builds a throwaway device + swap chain just to learn the vtable addresses
// of DXGI's swap chain class and D3D12's command queue class.
bool find_vtables(void**& swapchain_vtable, void**& queue_vtable) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"cdmp_dummy_window";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                nullptr, nullptr, inst, nullptr);
    bool ok = false;
    if (hwnd) {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IDXGISwapChain1> swapchain;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                ComPtr<IDXGIAdapter> warp;
                if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
                    D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            }
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (device && SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = 64;
            sd.Height = 64;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr,
                                                          nullptr, &swapchain))) {
                swapchain_vtable = *reinterpret_cast<void***>(swapchain.Get());
                queue_vtable = *reinterpret_cast<void***>(queue.Get());
                ok = true;
            }
        }
        DestroyWindow(hwnd);
    }
    UnregisterClassW(wc.lpszClassName, inst);
    return ok;
}

}  // namespace

bool install(DrawFn draw) {
    InitializeCriticalSection(&g_render_cs);
    g_draw = draw;

    void** sc_vtable = nullptr;
    if (!find_vtables(sc_vtable, g_queue_vtable)) {
        log::error("Overlay: could not create the dummy D3D12 swap chain");
        note({}, "dummy swap chain failed");
        return false;
    }
    // Resize hooks first: once Present is hooked we may start holding GPU
    // work that a resize has to wait for.
    const bool ok =
        hook_function(sc_vtable[kSlotResizeBuffers], reinterpret_cast<void*>(&hk_resize),
                      g_resize_hook, o_resize, "ResizeBuffers") &&
        hook_function(sc_vtable[kSlotResizeBuffers1], reinterpret_cast<void*>(&hk_resize1),
                      g_resize1_hook, o_resize1, "ResizeBuffers1") &&
        hook_function(sc_vtable[kSlotPresent], reinterpret_cast<void*>(&hk_present),
                      g_present_hook, o_present, "Present") &&
        hook_function(sc_vtable[kSlotPresent1], reinterpret_cast<void*>(&hk_present1),
                      g_present1_hook, o_present1, "Present1");
    if (!ok) {
        // Leave whatever did hook in place: each hook is complete on its own,
        // and an unhooked Present simply means nothing gets drawn.
        note({}, "inline hook failed");
        return false;
    }
    {
        std::lock_guard lock(g_status_mutex);
        g_status.installed = true;
    }
    log::info("Overlay hooks installed (swap chain vtable 0x{:X})",
              reinterpret_cast<uintptr_t>(sc_vtable));
    return true;
}

Status status() {
    std::lock_guard lock(g_status_mutex);
    Status s = g_status;
    s.disabled = g_disabled;
    s.presents = g_presents.load(std::memory_order_relaxed);
    s.frames_drawn = g_frames.load(std::memory_order_relaxed);
    return s;
}

}  // namespace cdmp::render
