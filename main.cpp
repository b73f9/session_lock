#include <tchar.h>
#include <windows.h>
#include <wtsapi32.h>
#include <Shobjidl.h>

bool registerHotkeys(HWND hwnd) {
    BOOL success = true;
    success &= RegisterHotKey(hwnd, 101, MOD_ALT, VK_OEM_3);
    success &= RegisterHotKey(hwnd, 102, MOD_CONTROL, VK_OEM_3);
    success &= RegisterHotKey(hwnd, 103, 0, VK_F1);
    return success;
}

void unregisterHotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, 101);
    UnregisterHotKey(hwnd, 102);
    UnregisterHotKey(hwnd, 103);
}

IDesktopWallpaper *getDesktopWallpaper() {
    // This is here to make sure the wallpaper pointer is always valid
    // We need to do this as it doesn't stay that way across explorer restarts
    // There's a race condition after calling this, but for all practical uses it doesnt matter.
    static IDesktopWallpaper *pDesktopWallpaper = nullptr;
    DESKTOP_SLIDESHOW_STATE slideshowState;
    if(pDesktopWallpaper == nullptr || pDesktopWallpaper->GetStatus(&slideshowState) != S_OK) {
        if(pDesktopWallpaper != nullptr) {
            pDesktopWallpaper->Release();
        }
        HRESULT resCreateInstance = CoCreateInstance(__uuidof(DesktopWallpaper),
                                                     nullptr,
                                                     CLSCTX_ALL,
                                                     IID_PPV_ARGS(&pDesktopWallpaper));
        if(FAILED(resCreateInstance)) {
            MessageBox(GetForegroundWindow(), "Cannot initialize the wallpaper interface!", "Error!", 0);
            return nullptr;
        }
    }

    return pDesktopWallpaper;
}

LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM);

HWND initializeWindow(HINSTANCE hThisInstance){
    WNDCLASSEX wincl;

    wincl.cbSize = sizeof(WNDCLASSEX);
    wincl.style = 0;
    wincl.lpfnWndProc = WindowProcedure;
    wincl.cbClsExtra = 0;
    wincl.cbWndExtra = 0;
    wincl.hInstance = hThisInstance;
    wincl.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wincl.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wincl.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wincl.lpszMenuName  = NULL;
    wincl.lpszClassName = "LidLock";
    wincl.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    
    if(!RegisterClassEx(&wincl)) {
        MessageBox(GetForegroundWindow(), "RegisterClassEx failed!", "Error", 0);
        return nullptr;
    }
    
    HWND window = CreateWindowEx(0, _T("LidLock"), _T(""), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 0, 0, HWND_MESSAGE, nullptr, hThisInstance, nullptr);
    if(window == nullptr){
        MessageBox(GetForegroundWindow(), "CreateWindowEx failed!", "Error", 0);
        return nullptr;
    }

    return window;
}

int WINAPI WinMain(HINSTANCE hThisInstance, HINSTANCE, LPSTR, int) {
    // Elevate process priority
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    
    // Initialize things
    HWND window = initializeWindow(hThisInstance);
    if(window == nullptr){
        return -1;
    }

    if(!registerHotkeys(window)) {
        MessageBox(GetForegroundWindow(), "Can't register one or more of the hotkeys", "Error", 0);
        return -2;
    }
    
    if(!RegisterPowerSettingNotification(window, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_WINDOW_HANDLE)) {
        MessageBox(GetForegroundWindow(), "Cannot register for lid state events!", "Error!", 0);
        return -3;
    }
    
    if(!WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION)) {
        MessageBox(GetForegroundWindow(), "Cannot register for session (un)lock events!", "Error!", 0);
        return -4;
    }
    
    if(FAILED(CoInitialize(nullptr))){
        MessageBox(GetForegroundWindow(), "COM library initialization failed!", "Error!", 0);
        return -5;
    }
    
    // Message loop
    MSG messages;
    while(GetMessage(&messages, nullptr, 0, 0)) {
        TranslateMessage(&messages);
        DispatchMessage(&messages);
    }
    
    // Cleanup
    unregisterHotkeys(window);
    
    IDesktopWallpaper* wallpaper = getDesktopWallpaper();
    if(wallpaper != nullptr){
        wallpaper->Release();
    }
    
    return messages.wParam;
}

void handleHotkeys(LPARAM lParam){
    // Alt + ~ -> next wallpaper in the slideshow
    if(LOWORD(lParam) == MOD_ALT && HIWORD(lParam) == VK_OEM_3) {
        IDesktopWallpaper *wallpaper = getDesktopWallpaper();
        if(wallpaper != nullptr){
            wallpaper->AdvanceSlideshow(nullptr, DSD_FORWARD);
        }
    }

    // Ctrl + ~ -> switch to (or from) a single color wallpaper
    if(LOWORD(lParam) == MOD_CONTROL && HIWORD(lParam) == VK_OEM_3) {
        IDesktopWallpaper *wallpaper = getDesktopWallpaper();
        if(wallpaper != nullptr){
            if(wallpaper->Enable(true) == S_FALSE) {
                wallpaper->Enable(false);
            }
        }
    }

    // Make the F1 key act as the sound (un)mute button
    if(LOWORD(lParam) == 0 && HIWORD(lParam) == VK_F1) {
        keybd_event(VK_VOLUME_MUTE, 0, 0, 0);
        keybd_event(VK_VOLUME_MUTE, 0, KEYEVENTF_KEYUP, 0);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool session_locked = false;
    switch(message) {
        // Detect lid state changes
        case WM_POWERBROADCAST:
            if(wParam == PBT_POWERSETTINGCHANGE) {
                POWERBROADCAST_SETTING *pbs = (POWERBROADCAST_SETTING *) lParam;
                // Lid close event
                if(pbs->PowerSetting == GUID_LIDSWITCH_STATE_CHANGE && *pbs->Data == 0) {
                    // Only lock the system if it isn't already locked
                    if(session_locked == false && !LockWorkStation()) {
                        MessageBox(GetForegroundWindow(), 
                                   "There was an error while locking the workstation!", 
                                   "Error!", 0);
                    }
                }
            }
            break;

        // Detect session lock/unlock events
        case WM_WTSSESSION_CHANGE:
            if(wParam == WTS_SESSION_LOCK) {
                session_locked = true;
            }
            if(wParam == WTS_SESSION_UNLOCK) {
                session_locked = false;
            }
            break;

        // Detect commands from the user
        case WM_HOTKEY:
            handleHotkeys(lParam);
            break;

        default:
            return DefWindowProc(window, message, wParam, lParam);
    }

    return 0;
}
