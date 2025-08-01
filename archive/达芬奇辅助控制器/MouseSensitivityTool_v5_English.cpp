#include <windows.h>
#include <iostream>
#include <iomanip>
#include <conio.h>
#include <algorithm>
#include <cstdint>

// Global variables
int g_originalSpeed = 10;
bool g_running = true;

// Get current mouse speed
int GetMouseSpeed() {
    int speed;
    SystemParametersInfo(SPI_GETMOUSESPEED, 0, &speed, 0);
    return speed;
}

// Set mouse speed
bool SetMouseSpeed(int speed) {
    return SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<PVOID>(static_cast<uintptr_t>(speed)), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

// Display interface
void ShowInterface() {
    system("cls");
    std::cout << "==========================================\n";
    std::cout << "    Windows Mouse Speed Controller v5.0\n";
    std::cout << "==========================================\n\n";
    
    int currentSpeed = GetMouseSpeed();
    double percentage = (double)currentSpeed / 20.0 * 100.0;
    
    std::cout << "Current mouse speed: " << currentSpeed << "/20 (" << std::fixed << std::setprecision(1) << percentage << "%)\n";
    std::cout << "Original speed: " << g_originalSpeed << "/20\n\n";
    
    std::cout << "Quick Settings (Windows scale 1-20):\n";
    std::cout << "[1] Speed 1   [2] Speed 2   [3] Speed 3\n";
    std::cout << "[4] Speed 4   [5] Speed 5   [6] Speed 6\n";
    std::cout << "[7] Speed 7   [8] Speed 8   [9] Speed 9\n";
    std::cout << "[0] Speed 10 (default)\n\n";
    
    std::cout << "Fine Control:\n";
    std::cout << "[+] Increase speed    [-] Decrease speed\n";
    std::cout << "[R] Restore original  [Q] Quit\n\n";
    
    std::cout << "Current setting:\n";
    if (currentSpeed <= 2) {
        std::cout << ">> VERY SLOW (good for precision work)\n";
        std::cout << ">> This is approximately 0.1x effect you wanted!\n";
    } else if (currentSpeed <= 5) {
        std::cout << ">> SLOW (reduced sensitivity)\n";
    } else if (currentSpeed <= 10) {
        std::cout << ">> NORMAL\n";
    } else {
        std::cout << ">> FAST\n";
    }
    
    std::cout << "\nNote: Changes apply immediately to entire system!\n";
    std::cout << "Recommended: Use speed 1-2 for 0.1x effect\n";
}

int main() {
    SetConsoleTitle(TEXT("Mouse Speed Controller"));
    
    // Get original mouse speed
    g_originalSpeed = GetMouseSpeed();
    
    std::cout << "Windows Mouse Speed Controller v5.0\n";
    std::cout << "====================================\n\n";
    std::cout << "Detected original mouse speed: " << g_originalSpeed << "/20\n";
    std::cout << "This tool directly modifies Windows mouse sensitivity.\n\n";
    
    // Set to slow speed (equivalent to your desired 0.1x effect)
    std::cout << "Setting mouse to slow speed (2/20) for 0.1x effect...\n";
    if (SetMouseSpeed(2)) {
        std::cout << "Mouse speed changed successfully!\n";
        std::cout << "You should feel the mouse is much slower now.\n";
    } else {
        std::cout << "Failed to change mouse speed. Try running as administrator.\n";
    }
    
    std::cout << "\nPress any key to open control panel...\n";
    _getch();
    
    ShowInterface();
    
    // Main loop
    while (g_running) {
        if (_kbhit()) {
            char key = _getch();
            int newSpeed = GetMouseSpeed();
            
            switch (key) {
                case '1': newSpeed = 1; break;  // Slowest
                case '2': newSpeed = 2; break;  // Very slow (0.1x effect)
                case '3': newSpeed = 3; break;
                case '4': newSpeed = 4; break;
                case '5': newSpeed = 5; break;
                case '6': newSpeed = 6; break;
                case '7': newSpeed = 7; break;
                case '8': newSpeed = 8; break;
                case '9': newSpeed = 9; break;
                case '0': newSpeed = 10; break; // Default
                
                case '+':
                case '=':
                    newSpeed = std::min(20, GetMouseSpeed() + 1);
                    break;
                    
                case '-':
                    newSpeed = std::max(1, GetMouseSpeed() - 1);
                    break;
                    
                case 'r':
                case 'R':
                    newSpeed = g_originalSpeed;
                    break;
                    
                case 'q':
                case 'Q':
                case 27: // ESC
                    g_running = false;
                    continue;
                    
                default:
                    continue;
            }
            
            if (SetMouseSpeed(newSpeed)) {
                ShowInterface();
            } else {
                std::cout << "\nFailed to set mouse speed! Try running as administrator.\n";
            }
        }
        
        Sleep(50);
    }
    
    // Restore original settings
    std::cout << "\nRestoring original mouse speed (" << g_originalSpeed << ")...\n";
    if (SetMouseSpeed(g_originalSpeed)) {
        std::cout << "Original mouse speed restored.\n";
    } else {
        std::cout << "Warning: Could not restore original mouse speed.\n";
        std::cout << "You may need to manually set it back in Windows settings.\n";
    }
    
    std::cout << "Goodbye!\n";
    return 0;
}