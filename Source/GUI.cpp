// Dear ImGui: standalone example application for Windows API + DirectX 12

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#include "GUI.h"
#include "Client.h"
#include "fmod.hpp"
#include "fmod_errors.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include <d3d12.h>
#include <dxgi1_5.h>
#include <tchar.h>

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

// Config for example app
static const int APP_NUM_FRAMES_IN_FLIGHT = 2;
static const int APP_NUM_BACK_BUFFERS     = 2;
static const int APP_SRV_HEAP_SIZE        = 64;

ImFont* main_font;
ImFont* message_font;

struct FrameContext {
        ID3D12CommandAllocator* CommandAllocator;
        UINT64                  FenceValue;
};

// Simple free list based allocator
struct ExampleDescriptorHeapAllocator {
        ID3D12DescriptorHeap*       Heap     = nullptr;
        D3D12_DESCRIPTOR_HEAP_TYPE  HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
        D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
        D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
        UINT                        HeapHandleIncrement;
        ImVector<int>               FreeIndices;

        void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap) {
                IM_ASSERT(Heap == nullptr && FreeIndices.empty());
                Heap                            = heap;
                D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
                HeapType                        = desc.Type;
                HeapStartCpu                    = Heap->GetCPUDescriptorHandleForHeapStart();
                HeapStartGpu                    = Heap->GetGPUDescriptorHandleForHeapStart();
                HeapHandleIncrement             = device->GetDescriptorHandleIncrementSize(HeapType);
                FreeIndices.reserve((int)desc.NumDescriptors);
                for (int n = desc.NumDescriptors; n > 0; n--)
                        FreeIndices.push_back(n - 1);
        }

        void Destroy() {
                Heap = nullptr;
                FreeIndices.clear();
        }

        void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle) {
                IM_ASSERT(FreeIndices.Size > 0);
                int idx = FreeIndices.back();
                FreeIndices.pop_back();
                out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
                out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
        }

        void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle) {
                int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
                int gpu_idx = (int)((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
                IM_ASSERT(cpu_idx == gpu_idx);
                FreeIndices.push_back(cpu_idx);
        }
};

// Data
static FrameContext g_frameContext[APP_NUM_FRAMES_IN_FLIGHT] = {};
static UINT         g_frameIndex                             = 0;

static ID3D12Device*                  g_pd3dDevice      = nullptr;
static ID3D12DescriptorHeap*          g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap*          g_pd3dSrvDescHeap = nullptr;
static ExampleDescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;
static ID3D12CommandQueue*            g_pd3dCommandQueue                                 = nullptr;
static ID3D12GraphicsCommandList*     g_pd3dCommandList                                  = nullptr;
static ID3D12Fence*                   g_fence                                            = nullptr;
static HANDLE                         g_fenceEvent                                       = nullptr;
static UINT64                         g_fenceLastSignaledValue                           = 0;
static IDXGISwapChain3*               g_pSwapChain                                       = nullptr;
static bool                           g_SwapChainTearingSupport                          = false;
static bool                           g_SwapChainOccluded                                = false;
static HANDLE                         g_hSwapChainWaitableObject                         = nullptr;
static ID3D12Resource*                g_mainRenderTargetResource[APP_NUM_BACK_BUFFERS]   = {};
static D3D12_CPU_DESCRIPTOR_HANDLE    g_mainRenderTargetDescriptor[APP_NUM_BACK_BUFFERS] = {};

// Forward declarations of helper functions
bool           CreateDeviceD3D(HWND hWnd);
void           CleanupDeviceD3D();
void           CreateRenderTarget();
void           CleanupRenderTarget();
void           WaitForPendingOperations();
FrameContext*  WaitForNextFrameContext();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ChatAppGUI(FMOD::System* sound_system, FMOD::Sound* notification_sound, FMOD::Sound* private_sound);

FMOD::System* InitFMOD() {
        FMOD::System* sound_system;
        FMOD::System_Create(&sound_system);
        sound_system->init(512, FMOD_INIT_NORMAL, NULL);

        return sound_system;
}

void DeinitFMOD(FMOD::System* sound_system) {
        sound_system->close();
        sound_system->release();
}

int GUI() {
        // Make process DPI aware and obtain main monitor scale
        ImGui_ImplWin32_EnableDpiAwareness();
        float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

        // Create application window
        WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
        ::RegisterClassExW(&wc);
        HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Chat App", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1'280 * main_scale),
                                    (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

        // Initialize Direct3D
        if (!CreateDeviceD3D(hwnd)) {
                CleanupDeviceD3D();
                ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
                return 1;
        }

        // Show the window
        ::ShowWindow(hwnd, SW_SHOWDEFAULT);
        ::UpdateWindow(hwnd);

        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Gamepad Controls
        io.ConfigInputTextEnterKeepActive = true;

        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        // ImGui::StyleColorsLight();

        // Setup scaling
        ImGuiStyle& style = ImGui::GetStyle();
        // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting
                                         // Style + calling this again)
        style.ScaleAllSizes(main_scale);
                                         // Set initial font scale. (using io.ConfigDpiScaleFonts=true makes this unnecessary. We leave both here for documentation purpose)
        style.FontScaleDpi =
                main_scale;

        style.ChildBorderSize = 1.0f;

        style.WindowRounding = 6.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 6.0f;

        style.PopupRounding = 6.0f;
        style.PopupBorderSize = 0.0f;

        style.ItemSpacing = ImVec2{12, 0};

        style.SelectableTextAlign = ImVec2{0.5f, 0.5f};

        style.SeparatorTextBorderSize = 1.0f;
        style.SeparatorTextAlign = {0.05f, 0.5f};
        style.SeparatorTextPadding = {0, 0};
        style.ItemSpacing = {8, 1};
        style.WindowPadding = {12, 12};

        // ===== Colours =====
        style.Colors[ImGuiCol_ChildBg] = ImVec4{0.25f, 0.25f, 0.3f, 1.0f};
        style.Colors[ImGuiCol_PopupBg] = ImVec4{0.4f, 0.4f, 0.4f, 0.8f};
        style.Colors[ImGuiCol_Border] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};

        style.Colors[ImGuiCol_FrameBg] = ImVec4{0.5f, 0.5f, 1.0f, 0.5f};
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4{0.5f, 0.5f, 1.0f, 1.0f};
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4{0.5f, 0.5f, 1.0f, 0.75f};

        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4{0.4f, 0.4f, 0.4f, 1.0f};
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4{0.5f, 0.5f, 0.5f, 1.0f};
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4{0.55f, 0.55f, 0.55f, 1.0f};

        style.Colors[ImGuiCol_Button] = ImVec4{0.55f, 0.55f, 1.0f, 0.8f};
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4{0.55f, 0.55f, 1.0f, 0.9f};
        style.Colors[ImGuiCol_ButtonActive] = ImVec4{0.55f, 0.55f, 1.0f, 1.0f};

        style.Colors[ImGuiCol_Header] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4{1.0f, 1.0f, 1.0f, 0.1f};
        style.Colors[ImGuiCol_HeaderActive] = ImVec4{1.0f, 1.0f, 1.0f, 0.2f};

        style.Colors[ImGuiCol_Separator] = ImVec4{1.0f, 1.0f, 1.0f, 0.5f};

        // Setup Platform/Renderer backends
        ImGui_ImplWin32_Init(hwnd);

        ImGui_ImplDX12_InitInfo init_info = {};
        init_info.Device                  = g_pd3dDevice;
        init_info.CommandQueue            = g_pd3dCommandQueue;
        init_info.NumFramesInFlight       = APP_NUM_FRAMES_IN_FLIGHT;
        init_info.RTVFormat               = DXGI_FORMAT_R8G8B8A8_UNORM;
        init_info.DSVFormat               = DXGI_FORMAT_UNKNOWN;
        // Allocating SRV descriptors (for textures) is up to the application, so we provide callbacks.
        // (current version of the backend will only allocate one descriptor, future versions will need to allocate more)
        init_info.SrvDescriptorHeap       = g_pd3dSrvDescHeap;
        init_info.SrvDescriptorAllocFn    = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) {
                return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle);
        };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
                return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle);
        };
        ImGui_ImplDX12_Init(&init_info);

        style.FontSizeBase = 20.0f;
        main_font          = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");

        // Our state
        bool   show_demo_window    = true;
        bool   show_another_window = false;
        ImVec4 clear_color         = ImVec4(0.15f, 0.15f, 0.10f, 1.00f);

        // Sound
        FMOD::System* sound_system = InitFMOD();
        FMOD::Sound*  sound        = NULL;
        sound_system->createSound("Assets/sound.mp3", FMOD_LOOP_OFF, NULL, &sound);

        // Private Sound
        FMOD::Sound*  private_sound        = NULL;
        sound_system->createSound("Assets/privatesound.mp3", FMOD_LOOP_OFF, NULL, &private_sound);

        // Main loop
        bool done = false;
        while (!done) {
                // Poll and handle messages (inputs, window resize, etc.)
                // See the WndProc() function below for our to dispatch events to the Win32 backend.
                MSG msg;
                while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                        ::TranslateMessage(&msg);
                        ::DispatchMessage(&msg);
                        if (msg.message == WM_QUIT) done = true;
                }
                if (done) break;

                // Handle window screen locked
                if ((g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd)) {
                        ::Sleep(10);
                        continue;
                }
                g_SwapChainOccluded = false;

                // Start the Dear ImGui frame
                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();


                // ================ IMGUI DEMO ======================
                // Useful for styling
                // ImGui::ShowDemoWindow(&show_demo_window);

                // ================== MY UI =========================
                ChatAppGUI(sound_system, sound, private_sound);

                // Rendering
                ImGui::Render();

                FrameContext* frameCtx      = WaitForNextFrameContext();
                UINT          backBufferIdx = g_pSwapChain->GetCurrentBackBufferIndex();
                frameCtx->CommandAllocator->Reset();

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource   = g_mainRenderTargetResource[backBufferIdx];
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                g_pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr);
                g_pd3dCommandList->ResourceBarrier(1, &barrier);

                // Render Dear ImGui graphics
                const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w,
                                                          clear_color.w };
                g_pd3dCommandList->ClearRenderTargetView(g_mainRenderTargetDescriptor[backBufferIdx], clear_color_with_alpha, 0, nullptr);
                g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
                g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                g_pd3dCommandList->ResourceBarrier(1, &barrier);
                g_pd3dCommandList->Close();

                g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pd3dCommandList);
                g_pd3dCommandQueue->Signal(g_fence, ++g_fenceLastSignaledValue);
                frameCtx->FenceValue = g_fenceLastSignaledValue;

                // Present
                HRESULT hr          = g_pSwapChain->Present(1, 0); // Present with vsync
                // HRESULT hr = g_pSwapChain->Present(0, g_SwapChainTearingSupport ? DXGI_PRESENT_ALLOW_TEARING : 0); // Present without vsync
                g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
                g_frameIndex++;
        }

        WaitForPendingOperations();

        // Cleanup
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

        sound->release();
        private_sound->release();
        DeinitFMOD(sound_system);

        return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd) {
        // Setup swap chain
        // This is a basic setup. Optimally could handle fullscreen mode differently. See #8979 for suggestions.
        DXGI_SWAP_CHAIN_DESC1 sd;
        {
                ZeroMemory(&sd, sizeof(sd));
                sd.BufferCount        = APP_NUM_BACK_BUFFERS;
                sd.Width              = 0;
                sd.Height             = 0;
                sd.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.Flags              = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.SampleDesc.Count   = 1;
                sd.SampleDesc.Quality = 0;
                sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;
                sd.Scaling            = DXGI_SCALING_STRETCH;
                sd.Stereo             = FALSE;
        }

        // [DEBUG] Enable debug interface
#ifdef DX12_ENABLE_DEBUG_LAYER
        ID3D12Debug* pdx12Debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug)))) pdx12Debug->EnableDebugLayer();
#endif

        // Create device
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        if (D3D12CreateDevice(nullptr, featureLevel, IID_PPV_ARGS(&g_pd3dDevice)) != S_OK) return false;

        // [DEBUG] Setup debug interface to break on any warnings/errors
#ifdef DX12_ENABLE_DEBUG_LAYER
        if (pdx12Debug != nullptr) {
                ID3D12InfoQueue* pInfoQueue = nullptr;
                g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue));
                pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
                pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
                pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
                pInfoQueue->Release();
                pdx12Debug->Release();
        }
#endif

        {
                D3D12_DESCRIPTOR_HEAP_DESC desc = {};
                desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                desc.NumDescriptors             = APP_NUM_BACK_BUFFERS;
                desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                desc.NodeMask                   = 1;
                if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK) return false;

                SIZE_T                      rtvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle         = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++) {
                        g_mainRenderTargetDescriptor[i] = rtvHandle;
                        rtvHandle.ptr += rtvDescriptorSize;
                }
        }

        {
                D3D12_DESCRIPTOR_HEAP_DESC desc = {};
                desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                desc.NumDescriptors             = APP_SRV_HEAP_SIZE;
                desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK) return false;
                g_pd3dSrvDescHeapAlloc.Create(g_pd3dDevice, g_pd3dSrvDescHeap);
        }

        {
                D3D12_COMMAND_QUEUE_DESC desc = {};
                desc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
                desc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
                desc.NodeMask                 = 1;
                if (g_pd3dDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_pd3dCommandQueue)) != S_OK) return false;
        }

        for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
                if (g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContext[i].CommandAllocator)) != S_OK)
                        return false;

        if (g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContext[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_pd3dCommandList)) !=
                    S_OK ||
            g_pd3dCommandList->Close() != S_OK)
                return false;

        if (g_pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK) return false;

        g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (g_fenceEvent == nullptr) return false;

        {
                IDXGIFactory5*   dxgiFactory = nullptr;
                IDXGISwapChain1* swapChain1  = nullptr;
                if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) != S_OK) return false;

                BOOL allow_tearing = FALSE;
                dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
                g_SwapChainTearingSupport = (allow_tearing == TRUE);
                if (g_SwapChainTearingSupport) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

                if (dxgiFactory->CreateSwapChainForHwnd(g_pd3dCommandQueue, hWnd, &sd, nullptr, nullptr, &swapChain1) != S_OK) return false;
                if (swapChain1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain)) != S_OK) return false;
                if (g_SwapChainTearingSupport) dxgiFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

                swapChain1->Release();
                dxgiFactory->Release();
                g_pSwapChain->SetMaximumFrameLatency(APP_NUM_BACK_BUFFERS);
                g_hSwapChainWaitableObject = g_pSwapChain->GetFrameLatencyWaitableObject();
        }

        CreateRenderTarget();
        return true;
}

void CleanupDeviceD3D() {
        CleanupRenderTarget();
        if (g_pSwapChain) {
                g_pSwapChain->SetFullscreenState(false, nullptr);
                g_pSwapChain->Release();
                g_pSwapChain = nullptr;
        }
        if (g_hSwapChainWaitableObject != nullptr) {
                CloseHandle(g_hSwapChainWaitableObject);
        }
        for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
                if (g_frameContext[i].CommandAllocator) {
                        g_frameContext[i].CommandAllocator->Release();
                        g_frameContext[i].CommandAllocator = nullptr;
                }
        if (g_pd3dCommandQueue) {
                g_pd3dCommandQueue->Release();
                g_pd3dCommandQueue = nullptr;
        }
        if (g_pd3dCommandList) {
                g_pd3dCommandList->Release();
                g_pd3dCommandList = nullptr;
        }
        if (g_pd3dRtvDescHeap) {
                g_pd3dRtvDescHeap->Release();
                g_pd3dRtvDescHeap = nullptr;
        }
        if (g_pd3dSrvDescHeap) {
                g_pd3dSrvDescHeap->Release();
                g_pd3dSrvDescHeap = nullptr;
        }
        if (g_fence) {
                g_fence->Release();
                g_fence = nullptr;
        }
        if (g_fenceEvent) {
                CloseHandle(g_fenceEvent);
                g_fenceEvent = nullptr;
        }
        if (g_pd3dDevice) {
                g_pd3dDevice->Release();
                g_pd3dDevice = nullptr;
        }

#ifdef DX12_ENABLE_DEBUG_LAYER
        IDXGIDebug1* pDebug = nullptr;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug)))) {
                pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
                pDebug->Release();
        }
#endif
}

void CreateRenderTarget() {
        for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++) {
                ID3D12Resource* pBackBuffer = nullptr;
                g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
                g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, g_mainRenderTargetDescriptor[i]);
                g_mainRenderTargetResource[i] = pBackBuffer;
        }
}

void CleanupRenderTarget() {
        WaitForPendingOperations();

        for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
                if (g_mainRenderTargetResource[i]) {
                        g_mainRenderTargetResource[i]->Release();
                        g_mainRenderTargetResource[i] = nullptr;
                }
}

void WaitForPendingOperations() {
        g_pd3dCommandQueue->Signal(g_fence, ++g_fenceLastSignaledValue);

        g_fence->SetEventOnCompletion(g_fenceLastSignaledValue, g_fenceEvent);
        ::WaitForSingleObject(g_fenceEvent, INFINITE);
}

FrameContext* WaitForNextFrameContext() {
        FrameContext* frame_context = &g_frameContext[g_frameIndex % APP_NUM_FRAMES_IN_FLIGHT];
        if (g_fence->GetCompletedValue() < frame_context->FenceValue) {
                g_fence->SetEventOnCompletion(frame_context->FenceValue, g_fenceEvent);
                HANDLE waitableObjects[] = { g_hSwapChainWaitableObject, g_fenceEvent };
                ::WaitForMultipleObjects(2, waitableObjects, TRUE, INFINITE);
        } else ::WaitForSingleObject(g_hSwapChainWaitableObject, INFINITE);

        return frame_context;
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

        switch (msg) {
        case WM_SIZE:
                if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                        CleanupRenderTarget();
                        DXGI_SWAP_CHAIN_DESC1 desc = {};
                        g_pSwapChain->GetDesc1(&desc);
                        HRESULT result = g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), desc.Format, desc.Flags);
                        IM_ASSERT(SUCCEEDED(result) && "Failed to resize swapchain.");
                        CreateRenderTarget();
                }
                return 0;
        case WM_SYSCOMMAND:
                if ((wParam & 0xff'f0) == SC_KEYMENU) // Disable ALT application menu
                        return 0;
                break;
        case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;
        }
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ===================== MY GUI =====================

#include "Message.h"
#include "imgui_internal.h"
#include <print>

static float message_scroll_position = -1.0f;

float GetTextHeight(const char* text, float wrap_width) {
        ImVec2 single_line_height = ImGui::CalcTextSize("Hello");
        ImVec2 message_text_size  = ImGui::CalcTextSize(text, NULL, false, wrap_width);

        return single_line_height.y + (single_line_height.y) * (message_text_size.y / single_line_height.y);
}

void ChatAppGUI(FMOD::System* sound_system, FMOD::Sound* notification_sound, FMOD::Sound* private_sound) {
        // TODO: Check Server is still running, if not throw an error modal with a refresh button to allow checking.

        // Need to negate so we can use as a ptr to open popup.
        static bool                                 not_logged_in     = true;
        static bool                                 failed_to_connect = true;
        static char                                 user_name[64]     = {};
        static Client                               user_client{};
        static ChannelID                            current_channel_id = ChannelIDGlobal;
        static char                                 input_buffer[512]{};  // TODO: Store somewhere else.
        static u32                                  last_message_count{}; // Used for checking if theres new messages
        static bool                                 last_was_at_bottom{};
        static std::unordered_map<UserID, float[3]> user_colours{};
        static std::unordered_map<ChannelID, u32>   last_read_message{};
        static std::unordered_map<ChannelID, u32>   last_notified_message{};

        std::srand((u32)std::time(nullptr));

        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        // ===== LOG IN =====

        if (not_logged_in and !ImGui::IsPopupOpen("LoginPopup")) {
                ImGui::OpenPopup("LoginPopup");
        }

        ImGuiWindowFlags login_window_flags{};
        login_window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
        login_window_flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;

        ImGui::SetNextWindowSize(viewport->Size);

        if (ImGui::BeginPopupModal("LoginPopup", &not_logged_in, login_window_flags)) {
                ImGui::Text("Username:");
                ImGui::InputText("##Username:", user_name, 64);

                if (ImGui::IsKeyPressed(ImGuiKey_Enter) or ImGui::Button("Enter")) {
                        not_logged_in = false;

                        if (user_client.Init() != ReturnCode::Success) {
                                std::println("Server might be down!");

                        } else {
                                user_client.SendUserName(std::string(user_name));

                                failed_to_connect = false;
                        }
                }

                ImGui::EndPopup();
                return;
        }


        if (!failed_to_connect) {
                // ===== Check Server Is Available =====
                if (user_client.Ping() != ReturnCode::Success) failed_to_connect = true;
        }

        if (failed_to_connect) {
                if (!ImGui::IsPopupOpen("FailedConnectingPopup")) ImGui::OpenPopup("FailedConnectingPopup");
        }

        // ===== Reconnect to Server =====

        ImGui::SetNextWindowSize(viewport->Size);

        if (ImGui::BeginPopupModal("FailedConnectingPopup", &failed_to_connect, login_window_flags)) {
                ImGui::Text("Failed to connect to server. It Might be down.");

                if (ImGui::Button("Retry")) {
                        if (user_client.Reconnect() != ReturnCode::Success) {
                                std::println("Server might be down!");
                        } else {
                                // Reset the channels, we can keep global messages but private channels are broken with server change.
                                user_client.channel_count = 1;
                                Channel channel = user_client.channels[ChannelIDGlobal]; // Copy global
                                user_client.channels.clear();
                                user_client.channels[ChannelIDGlobal] = channel;
                                user_client.channels[ChannelIDGlobal].user_count = 0;

                                current_channel_id = ChannelIDGlobal;

                                user_client.SendUserName(std::string(user_name));
                                failed_to_connect = false;
                        }
                }

                ImGui::EndPopup();
                return;
        }

        // ===== Notify if new messages =====
        for (u32 channel_idx = 0; channel_idx < user_client.channel_count; channel_idx++) {
                ChannelID channel_id = user_client.chat_channels[channel_idx];
                Channel& channel = user_client.channels[channel_id];

                if (channel.message_count > last_notified_message[channel_id]) {
                        if (channel_id == current_channel_id and last_read_message[current_channel_id] == channel.message_count) {
                                // ===== Dont Send Notification If We Are Looking At It =====
                        } else {
                                if (channel_id == ChannelIDGlobal) sound_system->playSound(notification_sound, NULL, false, nullptr);
                                else sound_system->playSound(private_sound, NULL, false, nullptr);
                        }
                }

                last_notified_message[channel_id] = channel.message_count;
        }

        // ===== Chat App =====

        ImGuiWindowFlags window_flags{};

        window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
        window_flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
        window_flags |= ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(viewport->Size);

        ImGui::Begin("AppBase", nullptr, window_flags);
        {
                ImGuiChildFlags child_flags{};

                child_flags |= ImGuiChildFlags_Borders;

                // ===== CHANNELS =====

                ImGui::BeginGroup();

                ImVec2 chat_channel_size{ min(ImGui::GetContentRegionAvail().x * 0.3f, 400), ImGui::GetContentRegionAvail().y };

                ImGui::BeginChild("ChatChannels", chat_channel_size, child_flags);
                {
                        ImGui::Text("Channels");
                        ImGui::Separator();

                        // ===== TODO =====
                        // On changeing channel, send request for how many messages in that channel.
                        // If we have cached the same number we should be ok, maybe want to check most recent?
                        // Otherwise we can reserve how many there should be and then request for the messages that we dont have.
                        // if we add message deletion this becomes a bit more difficult, unless we store some empty message for deleted messages.
                        // Can have like a server user which can print server messages.

                        for (u32 i = 0; i < user_client.channel_count; i++) {
                                ChannelID chat_channel_id   = user_client.chat_channels[i];
                                ImVec2    channel_text_size = ImGui::CalcTextSize(user_client.channels[chat_channel_id].name.c_str());

                                ImGui::PushID(i);

                                float channel_height = GetTextHeight(user_client.channels[chat_channel_id].name.c_str(), ImGui::GetContentRegionAvail().x);

                                ImGui::BeginChild("##channels", ImVec2{ ImGui::GetContentRegionAvail().x, channel_height }, child_flags);
                                if (ImGui::BeginPopupContextWindow()) {
                                        ImGuiStyle* style = &ImGui::GetStyle();

                                        ImGuiSelectableFlags selectable_flag = 0;

                                        // ===== Dont Allow Leaving Global =====
                                        if (chat_channel_id == ChannelIDGlobal) {
                                                ImGui::PushStyleColor(ImGuiCol_Text, style->Colors[ImGuiCol_TextDisabled]);
                                                selectable_flag |= ImGuiSelectableFlags_Disabled;
                                        }

                                        if (ImGui::Selectable("Leave", false, selectable_flag)) {
                                                // ===== Leave Channel =====
                                                user_client.LeaveChannel(chat_channel_id);

                                                // ===== Switch To Global If This Was Active ======
                                                if (chat_channel_id == current_channel_id) current_channel_id = ChannelIDGlobal;
                                        }

                                        if (chat_channel_id == ChannelIDGlobal) ImGui::PopStyleColor();

                                        ImGui::EndPopup();
                                }

                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.3f, 1.0f));

                                std::string channel_text = user_client.channels[chat_channel_id].name;

                                if (last_read_message[chat_channel_id] != user_client.channels[chat_channel_id].message_count) {
                                        channel_text += "*";
                                }

                                if (ImGui::Selectable(channel_text.c_str(), current_channel_id == chat_channel_id)) {
                                        current_channel_id = chat_channel_id;
                                }

                                ImGui::PopStyleColor();

                                ImGui::EndChild();

                                ImGui::PopID();
                        }
                }
                ImGui::EndChild();

                ImGui::EndGroup();
                ImGui::SameLine();
                ImGui::BeginGroup();

                // ===== MESSAGES =====

                ImVec2 chat_message_size = { ImGui::GetContentRegionAvail().x - chat_channel_size.x, ImGui::GetContentRegionAvail().y };

                ImGui::BeginChild("ChatLog", chat_message_size, child_flags);
                {

                        // ===== MESSAGES =====

                        ImGui::Text(user_client.channels[current_channel_id].name.c_str());
                        ImGui::Separator();

                        ImVec2 messages_size = ImGui::GetContentRegionAvail();
                        messages_size.y -= 100;

                        // ImGui::SetNextWindowScroll(ImVec2{0, message_scroll_position});

                        ImGui::BeginChild("Messages", messages_size, child_flags);
                        {
                                // ===== Get Messages =====
                                user_client.ProcessMessages();

                                Message* messages      = user_client.channels[current_channel_id].messages;
                                u32      message_count = user_client.channels[current_channel_id].message_count;

                                for (u32 i = 0; i < message_count; i++) {
                                        // ===== Server Messages =====
                                        if (messages[i].sender == 0) {
                                                ImGui::SeparatorText(messages[i].content);
                                                continue;
                                        }

                                        // ===== Actual Messages ======
                                        User& user = user_client.users[messages[i].sender];

                                        ImVec2 user_text_size     = ImGui::CalcTextSize(user.user_name.c_str());
                                        ImVec2 single_line_height = ImGui::CalcTextSize("Hello");
                                        ImVec2 message_text_size =
                                                ImGui::CalcTextSize(messages[i].content, NULL, false, ImGui::GetContentRegionAvail().x - user_text_size.x - 40);

                                        ImGui::PushID(i);

                                        float message_height = GetTextHeight(messages[i].content, ImGui::GetContentRegionAvail().x - user_text_size.x - 40);

                                        ImGui::BeginChild("##messages", ImVec2{ ImGui::GetContentRegionAvail().x, message_height }, child_flags);

                                        if (!user_colours.contains(user.id)) {
                                                std::srand((u32)std::time(nullptr));
                                                user_colours[user.id][0] = 0.5f + (float(std::rand() % 1'000) / 2'000);
                                                user_colours[user.id][1] = 0.5f + (float(std::rand() % 1'000) / 2'000);
                                                user_colours[user.id][2] = 0.5f + (float(std::rand() % 1'000) / 2'000);
                                        }

                                        float* c = user_colours[user.id];

                                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c[0], c[1], c[2], 1.0f));
                                        ImGui::TextWrapped(user.user_name.c_str());
                                        ImGui::PopStyleColor();

                                        ImGui::SameLine();

                                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 1.0f, 0.8f, 1.0f));
                                        ImGui::TextWrapped(messages[i].content);
                                        ImGui::PopStyleColor();

                                        ImGui::EndChild();

                                        ImGui::PopID();
                                }

                                // ===== SCROLLING BEHAVIOUR =====
                                // Need to do this weird check as setting scroll position on the first frame doesnt work.
                                // So we just skip a frame.
                                if (message_scroll_position == -1.0f) {
                                        message_scroll_position = -2.0f;
                                } else if (message_scroll_position == -2.0f) {
                                        message_scroll_position = ImGui::GetScrollMaxY();
                                } else {
                                        message_scroll_position = ImGui::GetScrollY();
                                        if (message_scroll_position == ImGui::GetScrollMaxY()) last_was_at_bottom = true;
                                        else last_was_at_bottom = false;
                                }

                                // TODO: if (new_message and ImGui::GetScrollMaxY - scroll_positions < 50.0f) message_scroll_position = ImGui::GetScrollMaxY();

                                // If theres new messages and were at the bottom of the chat window, scroll with the message.
                                if (last_message_count != message_count and last_was_at_bottom) {
                                        // Add some offset, for some reason FLT_MAX doesnt work, but this allows multi line messages to also be scrolled.
                                        message_scroll_position = ImGui::GetScrollMaxY() + 10000.0f;
                                }

                                ImGui::SetScrollY(message_scroll_position);

                                last_message_count = message_count;

                                if (message_scroll_position >= ImGui::GetScrollMaxY()) {
                                        // ===== Set Messages Read as Up-To-Date =====
                                        last_read_message[current_channel_id] = message_count;
                                }
                        }
                        ImGui::EndChild();

                        // ===== MESSAGE INPUT =====

                        ImGui::BeginGroup();
                        {
                                // ===== Send Input =====
                                if (ImGui::InputTextMultiline(
                                            "###Message", input_buffer, 512, ImVec2{ ImGui::GetContentRegionAvail().x - 150, ImGui::GetContentRegionAvail().y },
                                            ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue)) {
                                        if (ImGui::IsKeyPressed(ImGuiKey_Enter) and ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                                                u32 end               = (u32)strlen(input_buffer);
                                                input_buffer[end]     = '\n';
                                                input_buffer[end + 1] = 0;
                                        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                                                if (strlen(input_buffer) != 0) user_client.SendMessage(current_channel_id, input_buffer);

                                                std::memset(input_buffer, 0, 512);
                                                if (ImGuiInputTextState * state{ ImGui::GetInputTextState(ImGui::GetItemID()) })
                                                        state->ReloadUserBufAndSelectAll();
                                        }
                                        ImGui::SetKeyboardFocusHere(-1);
                                }

                                // ===== Send Button =====

                                ImGui::SameLine();
                                if (ImGui::Button("SEND", ImGui::GetContentRegionAvail())) {
                                        if (strlen(input_buffer) != 0) user_client.SendMessage(current_channel_id, input_buffer);

                                        std::memset(input_buffer, 0, 512);
                                        if (ImGuiInputTextState * state{ ImGui::GetInputTextState(ImGui::GetItemID()) }) state->ReloadUserBufAndSelectAll();
                                }
                        }
                        ImGui::EndGroup();
                }
                ImGui::EndChild();
                ImGui::EndGroup();
                ImGui::SameLine();

                // ===== MEMBERS =====

                ImGui::BeginGroup();

                ImVec2 members_channel_size{ ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y };

                ImGui::BeginChild("MembersPanel", members_channel_size, child_flags);
                {
                        ImGui::Text("Members");
                        ImGui::Separator();

                        Channel& channel = user_client.channels[current_channel_id];

                        for (u32 i = 0; i < channel.user_count; i++) {
                                UserID user_id = channel.users[i];
                                User&  user    = user_client.users[user_id];

                                if (user.user_name.empty()) {
                                        // ===== Request Name =====
                                        Message message{};

                                        message.channel = ChannelIDServer;

                                        // ===== Set Type =====
                                        ServerMessageType message_type = MessageUserNameRequest;
                                        memcpy(&message.content[0], &message_type, sizeof(ServerMessageType));
                                        message.content_length += sizeof(ServerMessageType);

                                        // ===== Set ID to request =====
                                        memcpy(&message.content[sizeof(ServerMessageType)], &user_id, sizeof(UserID));
                                        message.content_length += sizeof(UserID);

                                        // ===== Send Message =====
                                        int send_flags = 0;
                                        send(user_client.client_socket, (char*)&message, sizeof(Message), send_flags);

                                        // ===== Set to temp name so that we dont request multiple times.
                                        user.user_name = "Looking Up...";

                                        continue;
                                }
                                float channel_height = GetTextHeight(user.user_name.c_str(), ImGui::GetContentRegionAvail().x);

                                ImGui::PushID(i);

                                ImGui::BeginChild("##users", ImVec2{ ImGui::GetContentRegionAvail().x, channel_height }, child_flags);
                                if (ImGui::BeginPopupContextWindow()) {
                                        ImGuiStyle* style = &ImGui::GetStyle();

                                        ImGuiSelectableFlags selectable_flag = 0;

                                        // ===== Dont allow Private Messaging yourself =====
                                        if (user_id == user_client.id) {
                                                ImGui::PushStyleColor(ImGuiCol_Text, style->Colors[ImGuiCol_TextDisabled]);
                                                selectable_flag |= ImGuiSelectableFlags_Disabled;
                                        }

                                        if (ImGui::Selectable("Private Message", false, selectable_flag)) {
                                                // ===== Check if Already Exists =====
                                                bool channel_found = false;
                                                for (u32 channel_idx = 0; channel_idx < user_client.channel_count; channel_idx++) {
                                                        ChannelID channel_id = user_client.chat_channels[channel_idx];
                                                        // ===== Dont check global channel (Could check channels with ID >= ChannelIDUser) =====
                                                        if (channel_id == ChannelIDGlobal) continue;
                                                        Channel& channel = user_client.channels[channel_id];

                                                        if (channel.user_count != 2) continue;

                                                        if ((channel.users[0] == user_client.id or channel.users[1] == user_client.id) and
                                                            (channel.users[0] == user_id or channel.users[1] == user_id)) {
                                                                // ===== This is the private message channel between these two users =====
                                                                // NOTE: If we add group chats this is no longer true.
                                                                current_channel_id = channel_id;
                                                                channel_found      = true;
                                                        }
                                                }

                                                // ===== Create Channel =====
                                                if (!channel_found) user_client.CreatePrivateMessageChannel(user_id);
                                        }

                                        if (user_id == user_client.id) ImGui::PopStyleColor();

                                        ImGui::EndPopup();
                                }

                                if (!user_colours.contains(user_id)) {
                                        user_colours[user_id][0] = 0.5f + (float(std::rand() % 1'000) / 2'000);
                                        user_colours[user_id][1] = 0.5f + (float(std::rand() % 1'000) / 2'000);
                                        user_colours[user_id][2] = 0.5f + (float(std::rand() % 1'000) / 2'000);
                                }

                                float* c = user_colours[user_id];

                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c[0], c[1], c[2], 1.0f));
                                ImGui::Text(user.user_name.c_str());
                                ImGui::PopStyleColor();

                                ImGui::EndChild();

                                ImGui::PopID();
                        }
                }
                ImGui::EndChild();

                ImGui::EndGroup();
        }
        ImGui::End();
}