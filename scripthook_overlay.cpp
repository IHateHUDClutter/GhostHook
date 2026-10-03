/* D3D11 renderer for the existing GhostHook shared menu.
 * The menu model and input/callback behavior remain in scripthook_menu.c. */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <stdint.h>

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#include "third_party/minhook/include/MinHook.h"
#include "scripthook_menu_overlay.h"
#include "resource.h"

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *ResizeFn)(IDXGISwapChain *, UINT, UINT,
                                               UINT, DXGI_FORMAT, UINT);

static PresentFn g_present;
static ResizeFn g_resize;
static IDXGISwapChain *g_swap;
static ID3D11Device *g_device;
static ID3D11DeviceContext *g_context;
static ID3D11RenderTargetView *g_target;
static ID3D11ShaderResourceView *g_logoView;
static unsigned char *g_logoPixels;
static UINT g_logoWidth;
static UINT g_logoHeight;
static ImFont *g_font;
static int g_loadedFontChoice = -1;
static int g_loadedFontSize = -1;
static HWND g_window;
static LARGE_INTEGER g_clockFreq;
static LARGE_INTEGER g_lastFrame;
static CRITICAL_SECTION g_renderLock;
static volatile LONG g_lockReady;
static volatile LONG g_rendererReady;

static const wchar_t *g_fontFiles[6] = {
    L"segoeui.ttf", L"arial.ttf", L"tahoma.ttf",
    L"verdana.ttf", L"trebuc.ttf", L"georgia.ttf"
};

static void ReleaseLogoView(void) {
    if (g_logoView) {
        g_logoView->Release();
        g_logoView = NULL;
    }
}

static void ReleaseLogoPixels(void) {
    if (g_logoPixels) {
        HeapFree(GetProcessHeap(), 0, g_logoPixels);
        g_logoPixels = NULL;
    }
}

static int LoadLogoPixels(void) {
    HMODULE module = NULL;
    HRSRC resource;
    HGLOBAL loaded;
    const void *bytes;
    DWORD byteCount;
    HRESULT initialized;
    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    unsigned char *pixels = NULL;
    UINT width = 0, height = 0;
    int ok = 0;

    if (g_logoPixels) return 1;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(uintptr_t)&LoadLogoPixels, &module))
        return 0;
    resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_GHOSTHOOK_LOGO),
                             MAKEINTRESOURCEW(10));
    if (!resource) return 0;
    loaded = LoadResource(module, resource);
    bytes = loaded ? LockResource(loaded) : NULL;
    byteCount = SizeofResource(module, resource);
    if (!bytes || !byteCount) return 0;

    initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory((BYTE *)bytes, byteCount)) ||
        FAILED(factory->CreateDecoderFromStream(
            stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, NULL, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) ||
        !width || !height || width > 8192 || height > 8192)
        goto done;

    pixels = (unsigned char *)HeapAlloc(
        GetProcessHeap(), 0, (SIZE_T)width * height * 4);
    if (!pixels || FAILED(converter->CopyPixels(
            NULL, width * 4, width * height * 4, pixels)))
        goto done;

    g_logoPixels = pixels;
    g_logoWidth = width;
    g_logoHeight = height;
    pixels = NULL;
    ok = 1;

done:
    if (pixels) HeapFree(GetProcessHeap(), 0, pixels);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return ok;
}

static int CreateLogoView(void) {
    D3D11_TEXTURE2D_DESC desc = {};
    D3D11_SUBRESOURCE_DATA data = {};
    ID3D11Texture2D *texture = NULL;
    HRESULT hr;

    if ((!g_logoPixels && !LoadLogoPixels()) ||
        !g_logoWidth || !g_logoHeight)
        return 0;
    ReleaseLogoView();
    desc.Width = g_logoWidth;
    desc.Height = g_logoHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    data.pSysMem = g_logoPixels;
    data.SysMemPitch = g_logoWidth * 4;
    hr = g_device->CreateTexture2D(&desc, &data, &texture);
    if (FAILED(hr) || !texture) return 0;
    hr = g_device->CreateShaderResourceView(texture, NULL, &g_logoView);
    texture->Release();
    if (SUCCEEDED(hr) && g_logoView) {
        ReleaseLogoPixels();
        return 1;
    }
    return 0;
}

static void *ReadFontFile(int choice, int *byteCount) {
    wchar_t path[MAX_PATH];
    static const wchar_t fonts[] = L"\\Fonts\\";
    DWORD length, read = 0;
    LARGE_INTEGER size;
    HANDLE file;
    void *data;

    *byteCount = 0;
    if (choice < 0 || choice >= 6) choice = 0;
    length = GetWindowsDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH ||
        length + (DWORD)(sizeof(fonts) / sizeof(fonts[0])) +
            lstrlenW(g_fontFiles[choice]) >= MAX_PATH)
        return NULL;
    lstrcatW(path, fonts);
    lstrcatW(path, g_fontFiles[choice]);
    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return NULL;
    }
    data = IM_ALLOC((size_t)size.QuadPart);
    if (!data || !ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL) ||
        read != (DWORD)size.QuadPart) {
        if (data) IM_FREE(data);
        CloseHandle(file);
        return NULL;
    }
    CloseHandle(file);
    *byteCount = (int)size.QuadPart;
    return data;
}

static int RebuildFontIfNeeded(int force) {
    ImGuiIO &io = ImGui::GetIO();
    int choice = ShMenuOverlayFontChoice();
    int size = ShMenuOverlayFontSize();
    int byteCount = 0;
    void *data;

    if (!force && choice == g_loadedFontChoice && size == g_loadedFontSize)
        return 1;
    ImGui_ImplDX11_InvalidateDeviceObjects();
    io.Fonts->Clear();
    data = ReadFontFile(choice, &byteCount);
    g_font = data
        ? io.Fonts->AddFontFromMemoryTTF(data, byteCount, (float)size)
        : NULL;
    if (!g_font) {
        ImFontConfig config;
        config.SizePixels = (float)size;
        g_font = io.Fonts->AddFontDefault(&config);
    }
    io.FontDefault = g_font;
    g_loadedFontChoice = choice;
    g_loadedFontSize = size;
    return ImGui_ImplDX11_CreateDeviceObjects() ? 1 : 0;
}

static void ReleaseTarget(void) {
    if (g_target) {
        g_target->Release();
        g_target = NULL;
    }
}

static int CreateTarget(IDXGISwapChain *swap) {
    ID3D11Texture2D *back = NULL;
    HRESULT hr;

    ReleaseTarget();
    hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
    if (FAILED(hr) || !back) return 0;
    hr = g_device->CreateRenderTargetView(back, NULL, &g_target);
    back->Release();
    return SUCCEEDED(hr) && g_target;
}

static int IsGameSwapChain(IDXGISwapChain *swap, DXGI_SWAP_CHAIN_DESC *desc) {
    DWORD pid = 0;
    RECT client;

    if (FAILED(swap->GetDesc(desc)) || !desc->OutputWindow) return 0;
    GetWindowThreadProcessId(desc->OutputWindow, &pid);
    if (pid != GetCurrentProcessId()) return 0;
    if (!GetClientRect(desc->OutputWindow, &client)) return 0;
    return (client.right - client.left) >= 640 &&
           (client.bottom - client.top) >= 360;
}

static int InitializeRenderer(IDXGISwapChain *swap) {
    DXGI_SWAP_CHAIN_DESC desc;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ImGuiIO *io;

    if (g_swap || !IsGameSwapChain(swap, &desc)) return 0;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void **)&device)) ||
        !device)
        return 0;
    device->GetImmediateContext(&context);
    if (!context) {
        device->Release();
        return 0;
    }

    g_swap = swap;
    g_swap->AddRef();
    g_device = device;
    g_context = context;
    g_window = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    io = &ImGui::GetIO();
    io->IniFilename = NULL;
    io->LogFilename = NULL;
    io->BackendPlatformName = "GhostHook keyboard polling";
    ImGui::StyleColorsDark();

    if (!ImGui_ImplDX11_Init(g_device, g_context) || !CreateTarget(swap) ||
        !RebuildFontIfNeeded(1)) {
        ReleaseTarget();
        ReleaseLogoView();
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_context->Release();
        g_device->Release();
        g_swap->Release();
        g_context = NULL;
        g_device = NULL;
        g_swap = NULL;
        g_window = NULL;
        return 0;
    }
    CreateLogoView();

    QueryPerformanceFrequency(&g_clockFreq);
    QueryPerformanceCounter(&g_lastFrame);
    InterlockedExchange(&g_rendererReady, 1);
    ShMenuOverlaySetReady(1);
    return 1;
}

static ImU32 Colour(unsigned rgb, unsigned alpha = 255) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha);
}

static int ViewportRows(int rows, float available, float fixed, float rowHeight) {
    int fit = (int)((available - fixed) / rowHeight);
    if (fit < 1) fit = 1;
    return rows < fit ? rows : fit;
}

static int ViewportFirst(int selected, int rows) {
    return selected >= rows ? selected - rows + 1 : 0;
}

static void DrawMenu(const ShMenuOverlayView *view) {
    ImDrawList *draw;
    ImFont *font = g_font ? g_font : ImGui::GetFont();
    float scale = ShMenuOverlayScale();
    float x = 20.0f * scale;
    float y = 20.0f * scale;
    float width = 520.0f * scale;
    float pad = 20.0f * scale;
    float baseSize = (float)ShMenuOverlayFontSize();
    float logoWidth = 300.0f * scale;
    float logoHeight = g_logoWidth
        ? logoWidth * (float)g_logoHeight / (float)g_logoWidth : 0.0f;
    float header = (logoHeight + 20.0f * scale);
    float rowHeight = (baseSize + 16.0f) * scale;
    float titleSize = baseSize * (4.0f / 3.0f) * scale;
    float bodySize = baseSize * scale;
    float smallSize = baseSize * (5.0f / 6.0f) * scale;
    ImU32 textColour = Colour(ShMenuOverlayTextColour());
    ImU32 accentColour = Colour(ShMenuOverlayAccentColour(), 225);
    float cursor = y + pad;
    float contentTop;
    int i;

    float fixed = pad + header + rowHeight + pad;
    if (view->hint[0]) fixed += rowHeight;
    if (view->footer[0]) fixed += rowHeight;
    if (view->status[0]) fixed += rowHeight;
    int rows = ViewportRows(view->rows, ImGui::GetIO().DisplaySize.y - y - pad,
                            fixed, rowHeight);
    int first = ViewportFirst(view->sel, rows);
    float height = fixed + rowHeight * (float)rows;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##GhostHookOverlay", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height),
                        IM_COL32(5, 7, 10,
                                 ShMenuOverlayBackgroundAlpha()),
                        5.0f * scale);
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + header),
                        IM_COL32(18, 24, 31, 245), 5.0f * scale);
    if (g_logoView) {
        ImGui::SetCursorScreenPos(ImVec2(x + pad, cursor));
        ImGui::Image((ImTextureID)g_logoView,
                     ImVec2(logoWidth, logoHeight));
    }
    cursor += header;
    draw->AddText(font, titleSize, ImVec2(x + pad, cursor),
                  textColour, view->title);
    cursor += rowHeight;
    if (view->hint[0]) {
        draw->AddText(font, smallSize, ImVec2(x + pad, cursor),
                      Colour(0x9AA4B0), view->hint);
        cursor += rowHeight;
    }
    contentTop = cursor;

    for (i = 0; i < rows; ++i) {
        const ShMenuOverlayRow *row = &view->row[first + i];
        float top = contentTop + rowHeight * (float)i;
        ImU32 colour = row->selected ? Colour(0xFFFFFF) : textColour;
        if (row->selected)
            draw->AddRectFilled(ImVec2(x + 8.0f * scale, top),
                                ImVec2(x + width - 8.0f * scale,
                                       top + rowHeight - 3.0f * scale),
                                accentColour, 3.0f * scale);
        draw->AddText(font, bodySize,
                      ImVec2(x + pad + 8.0f * scale, top + 5.0f * scale),
                      colour, row->name);
        if (row->value[0]) {
            ImVec2 size = font->CalcTextSizeA(bodySize, FLT_MAX, 0.0f,
                                               row->value);
            draw->AddText(font, bodySize,
                          ImVec2(x + width - pad - size.x,
                                 top + 5.0f * scale), colour, row->value);
        }
    }

    cursor = contentTop + rowHeight * (float)rows;
    if (view->footer[0]) {
        draw->AddText(font, smallSize, ImVec2(x + pad, cursor + 5.0f * scale),
                      Colour(0x8C8C8C), view->footer);
        cursor += rowHeight;
    }
    if (view->status[0])
        draw->AddText(font, smallSize, ImVec2(x + pad, cursor + 5.0f * scale),
                      Colour(0xA0E6A0), view->status);
    ImGui::End();
}

static void RenderFrame(IDXGISwapChain *swap) {
    DXGI_SWAP_CHAIN_DESC desc;
    ShMenuOverlayView view;
    ImGuiIO *io;
    LARGE_INTEGER now;
    ID3D11RenderTargetView *oldTarget = NULL;
    ID3D11DepthStencilView *oldDepth = NULL;

    EnterCriticalSection(&g_renderLock);
    if (!g_rendererReady) InitializeRenderer(swap);
    if (!g_rendererReady || swap != g_swap || !g_target) {
        LeaveCriticalSection(&g_renderLock);
        return;
    }
    if (FAILED(swap->GetDesc(&desc))) {
        LeaveCriticalSection(&g_renderLock);
        return;
    }

    io = &ImGui::GetIO();
    io->DisplaySize = ImVec2((float)desc.BufferDesc.Width,
                             (float)desc.BufferDesc.Height);
    if (io->DisplaySize.x <= 0.0f || io->DisplaySize.y <= 0.0f) {
        RECT client;
        GetClientRect(g_window, &client);
        io->DisplaySize = ImVec2((float)(client.right - client.left),
                                 (float)(client.bottom - client.top));
    }
    QueryPerformanceCounter(&now);
    io->DeltaTime = g_clockFreq.QuadPart > 0
        ? (float)((double)(now.QuadPart - g_lastFrame.QuadPart) /
                  (double)g_clockFreq.QuadPart)
        : (1.0f / 60.0f);
    if (io->DeltaTime <= 0.0f) io->DeltaTime = 1.0f / 60.0f;
    g_lastFrame = now;

    if (!RebuildFontIfNeeded(0)) {
        LeaveCriticalSection(&g_renderLock);
        return;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (ShMenuOverlayCapture(&view)) DrawMenu(&view);
    ImGui::Render();

    g_context->OMGetRenderTargets(1, &oldTarget, &oldDepth);
    g_context->OMSetRenderTargets(1, &g_target, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &oldTarget, oldDepth);
    if (oldTarget) oldTarget->Release();
    if (oldDepth) oldDepth->Release();
    LeaveCriticalSection(&g_renderLock);
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain *swap, UINT sync,
                                              UINT flags) {
    if (g_lockReady) RenderFrame(swap);
    return g_present(swap, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain *swap, UINT count,
                                             UINT width, UINT height,
                                             DXGI_FORMAT format, UINT flags) {
    HRESULT hr;
    int ours = 0;

    if (g_lockReady) {
        EnterCriticalSection(&g_renderLock);
        ours = g_rendererReady && swap == g_swap;
        if (ours) {
            ShMenuOverlaySetReady(0);
            InterlockedExchange(&g_rendererReady, 0);
            ImGui_ImplDX11_InvalidateDeviceObjects();
            ReleaseTarget();
        }
        LeaveCriticalSection(&g_renderLock);
    }

    hr = g_resize(swap, count, width, height, format, flags);

    if (ours && SUCCEEDED(hr)) {
        EnterCriticalSection(&g_renderLock);
        if (CreateTarget(swap) && ImGui_ImplDX11_CreateDeviceObjects()) {
            InterlockedExchange(&g_rendererReady, 1);
            ShMenuOverlaySetReady(1);
        }
        LeaveCriticalSection(&g_renderLock);
    }
    return hr;
}

static DWORD WINAPI OverlayInstallThread(void *) {
    static const wchar_t className[] = L"GhostHookOverlayProbe";
    WNDCLASSEXW wc = {};
    HWND window = NULL;
    DXGI_SWAP_CHAIN_DESC desc = {};
    IDXGISwapChain *swap = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    void **vtable;
    MH_STATUS status;
    int presentCreated = 0;
    int resizeCreated = 0;

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = className;
    LoadLogoPixels();
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;
    window = CreateWindowExW(0, className, L"", WS_OVERLAPPED,
                             0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (!window) return 0;

    desc.BufferCount = 1;
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;

    if (FAILED(D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &desc, &swap, &device, NULL, &context)) ||
        !swap)
        goto done;

    vtable = *(void ***)swap;
    status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) goto done;
    status = MH_CreateHook(vtable[8], (LPVOID)HookPresent,
                           (LPVOID *)&g_present);
    if (status != MH_OK) goto done;
    presentCreated = 1;
    status = MH_CreateHook(vtable[13], (LPVOID)HookResize,
                           (LPVOID *)&g_resize);
    if (status != MH_OK) goto done;
    resizeCreated = 1;
    if (MH_QueueEnableHook(vtable[8]) != MH_OK ||
        MH_QueueEnableHook(vtable[13]) != MH_OK ||
        MH_ApplyQueued() != MH_OK)
        goto done;

    InitializeCriticalSection(&g_renderLock);
    InterlockedExchange(&g_lockReady, 1);
    presentCreated = 0;
    resizeCreated = 0;

done:
    if (resizeCreated) MH_RemoveHook(vtable[13]);
    if (presentCreated) MH_RemoveHook(vtable[8]);
    if (context) context->Release();
    if (device) device->Release();
    if (swap) swap->Release();
    if (window) DestroyWindow(window);
    UnregisterClassW(className, wc.hInstance);
    return 0;
}

extern "C" void ShOverlayStart(void) {
    static volatile LONG started;
    HANDLE thread;

    if (InterlockedExchange(&started, 1)) return;
    thread = CreateThread(NULL, 0, OverlayInstallThread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}
