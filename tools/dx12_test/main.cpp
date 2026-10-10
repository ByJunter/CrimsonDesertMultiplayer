// cdmp_dx12_test.exe - a minimal D3D12 window that loads cdmp.asi, so the
// overlay hook can be exercised without the game.
//
//   cdmp_dx12_test <path\to\cdmp.asi> [seconds=8]
//
// Clears the screen to a dark blue every frame and presents with vsync. The
// mod is never unloaded (its hooks stay in DXGI's vtables until exit).

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kBuffers = 3;
constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("Usage: cdmp_dx12_test <path\\to\\cdmp.asi> [seconds]\n");
        return 2;
    }
    const int seconds = argc >= 3 ? _wtoi(argv[2]) : 8;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"cdmp_dx12_test";
    RegisterClassExW(&wc);
    RECT rc{0, 0, kWidth, kHeight};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"cdmp DX12 test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);

    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::printf("D3D12 init failed\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = kWidth;
    sd.Height = kHeight;
    sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;  // same family as many games' HDR-capable chains
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        std::printf("swap chain creation failed\n");
        return 1;
    }
    ComPtr<IDXGISwapChain3> sc;
    sc1.As(&sc);

    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
    const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> buffers[kBuffers];
    ComPtr<ID3D12CommandAllocator> allocs[kBuffers];
    UINT64 alloc_fence[kBuffers] = {};
    for (UINT i = 0; i < kBuffers; ++i) {
        sc->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtv_size;
        device->CreateRenderTargetView(buffers[i].Get(), nullptr, h);
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs[i]));
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0].Get(), nullptr,
                              IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;

    SetEnvironmentVariableW(L"CDMP_TEST_HOST", L"1");
    SetEnvironmentVariableW(L"CDMP_TEST_OVERLAY", L"1");
    if (!LoadLibraryW(argv[1])) {
        std::printf("LoadLibrary failed (%lu)\n", GetLastError());
        return 1;
    }
    std::printf("Mod loaded; rendering for %d s\n", seconds);

    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    UINT64 frames = 0;
    while (GetTickCount64() < until) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const UINT idx = sc->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < alloc_fence[idx]) {
            fence->SetEventOnCompletion(alloc_fence[idx], fence_event);
            WaitForSingleObject(fence_event, INFINITE);
        }
        allocs[idx]->Reset();
        list->Reset(allocs[idx].Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = buffers[idx].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * rtv_size;
        const float color[4] = {0.05f, 0.08f, 0.16f, 1.0f};
        list->ClearRenderTargetView(h, color, 0, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        sc->Present(1, 0);
        queue->Signal(fence.Get(), ++fence_value);
        alloc_fence[idx] = fence_value;
        ++frames;
    }
    std::printf("Presented %llu frames\n", static_cast<unsigned long long>(frames));
    queue->Signal(fence.Get(), ++fence_value);
    fence->SetEventOnCompletion(fence_value, fence_event);
    WaitForSingleObject(fence_event, 2000);
    // Exit without FreeLibrary: DXGI's vtables still point into the mod.
    ExitProcess(0);
}
