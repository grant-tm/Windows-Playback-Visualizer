// COMPILE: g++ -pthread -mwindows overlay.cpp -lgdi32 -lole32 resource.res -o overlay.exe

// Display Region
#define DISPLAY_LFT 1201
#define DISPLAY_RGT 1544
#define DISPLAY_TOP 666
#define DISPLAY_BOT 340

// Audio Capture & Rendering
#define WINDOW_SIZE 400
#define NUM_WINDOWS 20
#define COMP_WINDOW_SIZE floor((DISPLAY_RGT - DISPLAY_LFT) / NUM_WINDOWS)

#define CAPTURE_SAMPLE_SKIP 2
#define MAX_UPDATE_RATE_MS 10
#define WAVE_SCALAR 100

// Transparency Key RGB
#define KEY_R 0 
#define KEY_G 0
#define KEY_B 0

// Background Color (must be distinct from transparency key)
#define BG_R 1
#define BG_G 1
#define BG_B 1

// Foreground Color (must be distinct from transparency key)
#define FG_R 255
#define FG_G 255
#define FG_B 255

// Linegraph Color (must be distinct from transparency key)
#define LG_R 255
#define LG_G 255
#define LG_B 255

//****************************************************************************
// Headers
//****************************************************************************
// C Standard Library
#include <stdio.h>
#include <math.h>
// Containers
#include <vector>
#include <queue>
// Concurrency
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
// WinAPI
#include <windows.h>
#include <combaseapi.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>

#include "ThreadSafeQueue.h"

ThreadSafeQueue queue;

//****************************************************************************
// Audio Capture
//****************************************************************************
int startAudioCapture(){
    HRESULT hr;
    
    // Initialize COM
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "COM initialization failed!\n");
        return -1;
    }

    // Get default audio capture device
    IMMDeviceEnumerator *deviceEnumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), 
        nullptr, 
        CLSCTX_ALL, 
        __uuidof(IMMDeviceEnumerator), 
        (void**)&deviceEnumerator
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create device enumerator!\n");
        CoUninitialize();
        return -1;
    }

    // Get default audio capture device
    IMMDevice *captureDevice = nullptr;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(
        eRender, 
        eConsole, 
        &captureDevice
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get default audio endpoint!\n");
        deviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }  

    // Activate audio client
    IAudioClient *audioClient = nullptr;
    hr = captureDevice->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL, 
        nullptr, 
        (void**)&audioClient
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to activate audio client!\n");
        captureDevice->Release();
        deviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    // Get audio format
    WAVEFORMATEX *waveFormat = nullptr;
    hr = audioClient->GetMixFormat(&waveFormat);
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get mix format!\n");
        audioClient->Release();
        captureDevice->Release();
        deviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    // Initialize audio client
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 
        AUDCLNT_STREAMFLAGS_LOOPBACK, 
        0, 
        0, 
        waveFormat, 
        nullptr
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to initialize audio client!\n");
        CoTaskMemFree(waveFormat);
        audioClient->Release();
        captureDevice->Release();
        deviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    // Free the wave format data
    CoTaskMemFree(waveFormat);

    // Get the capture client
    IAudioCaptureClient *captureClient = nullptr;
    hr = audioClient->GetService(
        __uuidof(IAudioCaptureClient), 
        (void**)&captureClient
    );
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to get audio capture client!\n");
        audioClient->Release();
        captureDevice->Release();
        deviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    // Start capturing
    hr = audioClient->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to start capturing!\n");
        captureClient->Release();
        audioClient->Release();
        captureDevice->Release();
        deviceEnumerator->Release();
        CoUninitialize();
        return -1;
    }

    UINT32 packetLength = 0;
    std::vector<float> samples;
    printf("Starting audio capture\n");
    while (queue.isCapturing.load()) {
        hr = captureClient->GetNextPacketSize(&packetLength);
        if(FAILED(hr)) {
            fprintf(stderr, "Failed to get next packet size!\n");
            break;
        }
        if(packetLength > 0) {
            BYTE *data;
            UINT32 numFramesAvailable;
            DWORD flags;

            hr = captureClient->GetBuffer(&data, &numFramesAvailable, &flags, 
                nullptr, nullptr
            );
            if(FAILED(hr)) {
                fprintf(stderr, "Failed to get buffer!\n");
                break;
            }

            if(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                printf("Discontinuity detected, writing %d samples\n", numFramesAvailable);
            }

            // Process the data (e.g., write to file, stream, etc.)
            for(int i=0; i<numFramesAvailable; i++){
                samples.push_back(((float*)data)[i]);
                i += CAPTURE_SAMPLE_SKIP;
            }
            
            if(samples.size() >= WINDOW_SIZE){
                std::vector<float> packet;
                packet.insert(packet.begin(), samples.begin(), samples.begin() + WINDOW_SIZE);
                samples.erase(samples.begin(), samples.begin() + WINDOW_SIZE);
                queue.enqueue(packet);
            }

            hr = captureClient->ReleaseBuffer(numFramesAvailable);
            if(FAILED(hr)) {
                fprintf(stderr, "Failed to release buffer!\n");
                break;
            }
        }
    }

    // Stop capturing
    audioClient->Stop();

    // Cleanup
    captureClient->Release();
    audioClient->Release();
    captureDevice->Release();
    deviceEnumerator->Release();
    CoUninitialize();
    return 1;
}

//****************************************************************************
// Draw Audio Graph
//****************************************************************************

// paint the graph background and border
void paintBackground(HDC hdc, PAINTSTRUCT* ps)
{
    // select display region
    RECT rect = {DISPLAY_LFT, DISPLAY_BOT, DISPLAY_RGT, DISPLAY_TOP};
    
    // paint solid background
    HBRUSH background_brush = CreateSolidBrush(RGB(BG_R, BG_G, BG_B));
    FillRect(hdc, &rect, background_brush);
    DeleteObject(background_brush);

    // add border around background
    HBRUSH frame_brush = CreateSolidBrush(RGB(FG_R, FG_G, FG_B));
    FrameRect(hdc, &rect, frame_brush);
    DeleteObject(frame_brush);    
}

void paintGraph(HDC hdc, PAINTSTRUCT* ps, std::vector<POINT> points){
    // create a pen
    HPEN graph_pen = CreatePen(PS_SOLID, 2, RGB(LG_R, LG_G, LG_B));
    HPEN hOldPen = (HPEN)SelectObject(hdc, graph_pen);

    // adjust x values for graph (scrolling window effect)
    for(int i=0; i<points.size() / COMP_WINDOW_SIZE; i++){
        for(int j=0; j<COMP_WINDOW_SIZE; j++){
            int idx = (i * COMP_WINDOW_SIZE) + j;
            if(idx > points.size()) {break;}
            points[idx].x += DISPLAY_LFT + (i * COMP_WINDOW_SIZE);
        }
    }

    // draw the graph
    MoveToEx(hdc, DISPLAY_LFT, DISPLAY_BOT, NULL);
    Polyline(hdc, points.data(), static_cast<int>(points.size()));

    // release pen
    SelectObject(hdc, hOldPen);
    DeleteObject(graph_pen);
}

// convert float samples to POINTs
void convert_samples_to_points(std::vector<float>* samples, std::vector<POINT>* point_container){
    // center line is the middle of the display region, vertically
    long center_line = lround(0.5 * (DISPLAY_TOP - DISPLAY_BOT) + DISPLAY_BOT);
    
    // calculate y values for points (x values are variable as graph scrolls)
    for(int i=0; i<samples->size(); i++){
        POINT pt = {0, lround(WAVE_SCALAR * (*samples)[i] + center_line)};
        point_container->push_back(pt);
    }
}

// map points to x range of a single window in the display region
void map_points_to_window_range(std::vector<POINT>* points){
    // calculate the width of the whole display region
    int display_range = DISPLAY_RGT - DISPLAY_LFT;
    
    // calculate the width of a single window
    int window_range = display_range / NUM_WINDOWS;
    
    // store the number of points
    int num_points = points->size();
    
    // map x values to the range of a single window
    for(int i=0; i<num_points; i++){
        (*points)[i].x = 0 + lround(i*(float(window_range)/(num_points)));
    }
    
}

void compress_points_with_duplicate_x(std::vector<POINT>* points){
    // temporary container for compressed points
    std::vector<POINT> collapsed;

    // reused variables for calculating average y values per x value
    double y_sum = 0;
    int pt_count = 0;
    long x_cur = (*points)[0].x;

    // calculate average y values for points with the same x value
    for(auto& pt : *points){
        // if x value is the same as the current x value, add y value to sum and increment point counter
        if(pt.x == x_cur){
            y_sum += pt.y;
            ++pt_count;
        }
        // if x value is different, calculate average y value and add to compressed points
        else{
            POINT p = {x_cur, lround(y_sum / pt_count)};
            collapsed.push_back(p);
            x_cur = pt.x;
            y_sum = pt.y;
            pt_count = 1;
        }
    }

    // replace the content of points with the compressed points
    points->clear();
    points->insert(points->begin(), collapsed.begin(), collapsed.end());
}

void add_points_to_graph(std::vector<POINT>* new_points, std::vector<POINT>* graph_points){
    // add new points to graph
    graph_points->insert(graph_points->end(), new_points->begin(), new_points->end());
    
    // remove old points from graph if graph is too large
    if(graph_points->size() > (COMP_WINDOW_SIZE * NUM_WINDOWS)){
        graph_points->erase(graph_points->begin(), graph_points->begin() + COMP_WINDOW_SIZE);
    }
}

void process_samples(std::vector<float>* samples, std::vector<POINT>* points){
    std::vector<POINT> new_points;
    // calculate y values for points (x values are variable as graph scrolls)
    convert_samples_to_points(samples, &new_points);
    // calculate x value offsets for points
    map_points_to_window_range(&new_points);
    // compress points array to draw graph faster
    compress_points_with_duplicate_x(&new_points);
    // add new points to graph
    add_points_to_graph(&new_points, points);
}

std::vector<POINT> points;
void render(HWND hwnd){
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    std::vector<float> samples;
    if(queue.dequeue(samples)){
        printf("SAMPLES RECV\n ");
        process_samples(&samples, &points);
        paintBackground(hdc, &ps);
        paintGraph(hdc, &ps, points);
    }
    
    EndPaint(hwnd, &ps);
}

//****************************************************************************
// Windows Stuff
//****************************************************************************

// Window procedure declaration
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
    
    std::thread captureThread(startAudioCapture);
    
    // Register the window class
    LPCSTR CLASS_NAME = LPCSTR("OverlayWindowClass");

    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    RegisterClass(&wc);

    // Create the window
    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT, // Extended window styles
        CLASS_NAME,
        LPCSTR("Overlay Window"),
        WS_POPUP, // Make the window borderless
        0, 0, 
        GetSystemMetrics(SM_CXSCREEN), 
        GetSystemMetrics(SM_CYSCREEN), // Full-screen
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        return 0;
    }
   
    // Set the window to be transparent and click-through
    SetLayeredWindowAttributes(hwnd, RGB(KEY_R, KEY_G, KEY_B), 0, LWA_COLORKEY);

    ShowWindow(hwnd, nCmdShow);

    // Run the message loop
    MSG msg = { 0 };
    while(TRUE){
        if(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)){
            if (msg.message == WM_QUIT) //<=== **** EDITED **** 
                break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            typedef std::chrono::high_resolution_clock hiresclock;
            static auto timer = hiresclock::now();
            auto milisec = (hiresclock::now() - timer).count() / 1000000;
            if(milisec > MAX_UPDATE_RATE_MS)
            {
                timer = hiresclock::now();
                RECT rect = {DISPLAY_LFT, DISPLAY_BOT, DISPLAY_RGT, DISPLAY_TOP}; 
                InvalidateRect(hwnd, &rect, TRUE);
                UpdateWindow(hwnd);
                render(hwnd);
            }
        }
    }

    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam){
    switch (uMsg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 1;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            render(hwnd);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
