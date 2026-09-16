#include "ngba/runtime.hpp"
#include "ngba/file_io.hpp"
#include "ngba/version.hpp"
#include "ngba/audio.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
const auto kFrameTime = std::chrono::duration_cast<Clock::duration>(
    std::chrono::duration<double>(280896.0 / ngba::MemoryBus::kClockFrequency));

class WaveOutAudioSink : public ngba::AudioSink {
public:
    static constexpr std::size_t kBufferCount = 8;
    static constexpr std::size_t kMaxSamples = 2048;

    WaveOutAudioSink() noexcept {
        Open();
    }

    ~WaveOutAudioSink() override {
        Close();
    }

    bool Available() const noexcept { return device_ != nullptr; }

    void SetMuted(bool muted) noexcept {
        muted_ = muted;
        if (muted) {
            Reset();
        }
    }

    void Reset() noexcept {
        if (!device_) return;
        waveOutReset(device_);
        for (std::size_t i = 0; i < kBufferCount; ++i) {
            if ((headers_[i].dwFlags & WHDR_PREPARED) != 0) {
                waveOutUnprepareHeader(device_, &headers_[i], sizeof(WAVEHDR));
            }
            headers_[i].dwFlags = 0;
        }
        ring_index_ = 0;
    }

    void SubmitSamples(const std::int16_t* stereo_samples, std::size_t frame_count) override {
        if (!device_ || muted_ || frame_count == 0 || !stereo_samples) return;

        WAVEHDR& hdr = headers_[ring_index_];
        if ((hdr.dwFlags & WHDR_PREPARED) != 0) {
            while ((hdr.dwFlags & WHDR_DONE) == 0) {
                Sleep(1);
            }
            waveOutUnprepareHeader(device_, &hdr, sizeof(WAVEHDR));
        }

        const std::size_t sample_count = std::min(frame_count * 2, kMaxSamples);
        const std::size_t byte_count = sample_count * sizeof(std::int16_t);
        std::memcpy(buffers_[ring_index_].data(), stereo_samples, byte_count);

        hdr.dwBufferLength = static_cast<DWORD>(byte_count);
        hdr.dwFlags = 0;
        waveOutPrepareHeader(device_, &hdr, sizeof(WAVEHDR));
        waveOutWrite(device_, &hdr, sizeof(WAVEHDR));

        ring_index_ = (ring_index_ + 1) % kBufferCount;
    }

private:
    void Open() noexcept {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = 44100;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 4;
        wfx.nAvgBytesPerSec = 44100 * 4;

        const MMRESULT result = waveOutOpen(&device_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            device_ = nullptr;
            return;
        }

        for (std::size_t i = 0; i < kBufferCount; ++i) {
            buffers_[i].assign(kMaxSamples, 0);
            headers_[i] = WAVEHDR{};
            headers_[i].lpData = reinterpret_cast<LPSTR>(buffers_[i].data());
        }
    }

    void Close() noexcept {
        if (device_) {
            waveOutReset(device_);
            for (std::size_t i = 0; i < kBufferCount; ++i) {
                if ((headers_[i].dwFlags & WHDR_PREPARED) != 0) {
                    waveOutUnprepareHeader(device_, &headers_[i], sizeof(WAVEHDR));
                }
            }
            waveOutClose(device_);
            device_ = nullptr;
        }
    }

    HWAVEOUT device_{nullptr};
    bool muted_{false};
    std::size_t ring_index_{0};
    std::array<std::vector<std::int16_t>, kBufferCount> buffers_{};
    std::array<WAVEHDR, kBufferCount> headers_{};
};

std::wstring DirectoryOf(const std::wstring& path) {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L".\\" : path.substr(0, separator + 1);
}

std::wstring ExecutableDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) throw std::runtime_error("cannot locate ngba.exe");
    return DirectoryOf(std::wstring(path.data(), length));
}

std::string PickFile(const wchar_t* title, const wchar_t* filter, const std::wstring& directory) {
    std::vector<wchar_t> path(32768);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrInitialDir = directory.c_str();
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) return ngba::WideToUtf8(path.data());
    const DWORD error = CommDlgExtendedError();
    if (error != 0) throw std::runtime_error("cannot open file picker (code " + std::to_string(error) + ")");
    return {};
}

std::string FindBios(const std::wstring& executable_directory, const std::string& rom_path) {
    for (const auto& directory : {executable_directory, DirectoryOf(ngba::Utf8ToWide(rom_path))}) {
        const auto path = directory + L"gba_bios.bin";
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return ngba::WideToUtf8(path);
        }
    }
    return {};
}

void ShowError(HWND window, const std::string& message) {
    std::wstring text;
    try { text = ngba::Utf8ToWide(message); }
    catch (...) { text = L"NGBA encountered an error. The error text could not be decoded."; }
    if (window) SetWindowTextW(window, (L"NGBA - Error: " + text).c_str());
    MessageBoxW(window, text.c_str(), L"NGBA - Error", MB_OK | MB_ICONERROR);
}

struct Application {
    std::unique_ptr<ngba::Runtime> runtime;
    WaveOutAudioSink audio;
    ngba::Framebuffer frame;
    std::vector<std::uint32_t> pixels;
    std::string rom_path;
    std::string game_name;
    std::string message;
    std::uint16_t keys{};
    bool native{};
    bool resume_on_focus{};
    bool fast_forward{};
    unsigned fast_forward_frames{};
    Clock::time_point frame_deadline{};
    Clock::time_point render_deadline{};
    Clock::time_point fps_start_time{};
    ngba::Cycle fps_start_frame{};
    double current_fps{59.7};
    Clock::time_point last_save_flush{};

    std::string StatePath(unsigned slot) const {
        if (slot < 1 || slot > 12) throw std::invalid_argument("invalid savestate slot");
        return rom_path + (slot == 1 ? "" : ".slot" + std::to_string(slot)) + ".ngbs";
    }

    void UseStateSlot(unsigned slot, bool saving) {
        runtime->FlushBatterySave();
        const auto path = StatePath(slot);
        if (saving) {
            runtime->SaveState(path);
            message = "Saved slot " + std::to_string(slot);
        } else {
            const DWORD attributes = GetFileAttributesW(ngba::Utf8ToWide(path).c_str());
            const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                message = "Slot " + std::to_string(slot) + " is empty";
                return;
            }
            audio.Reset();
            runtime->LoadState(path);
            runtime->Bus().SetKeys(keys);
            message = "Loaded slot " + std::to_string(slot);
        }
    }

    void SetFastForward(bool enabled) {
        if (fast_forward != enabled) {
            fast_forward = enabled;
            audio.SetMuted(enabled);
            fast_forward_frames = 0;
            frame_deadline = {};
            render_deadline = {};
        }
    }

    bool Advance(HWND window, Clock::time_point now) {
        if (runtime->Bus().SaveMemoryDirty()) {
            if (last_save_flush == Clock::time_point{}) {
                last_save_flush = now;
            } else if (now - last_save_flush >= std::chrono::milliseconds(500)) {
                runtime->FlushBatterySave();
                last_save_flush = {};
            }
        }
        if (runtime->Paused()) return false;
        const auto interval = fast_forward ? (kFrameTime / 3) : kFrameTime;
        if (frame_deadline != Clock::time_point{} && now < frame_deadline) return false;
        runtime->StepFrame();
        frame_deadline = (frame_deadline == Clock::time_point{} || now - frame_deadline > interval * 4)
            ? now + interval
            : frame_deadline + interval;
        if (!fast_forward || (++fast_forward_frames >= 3 && now >= render_deadline)) {
            Refresh(window);
            fast_forward_frames = 0;
            render_deadline = now + kFrameTime;
        }
        return true;
    }

    void Refresh(HWND window) {
        runtime->Render(frame);
        pixels.resize(frame.pixels.size());
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto color = frame.pixels[index];
            pixels[index] = ((color & 255u) << 16) | (color & 0xFF00u) | ((color >> 16) & 255u);
        }
        InvalidateRect(window, nullptr, FALSE);

        const auto now = Clock::now();
        if (fps_start_time == Clock::time_point{}) {
            fps_start_time = now;
            fps_start_frame = runtime->Bus().Frames();
        } else {
            const double elapsed = std::chrono::duration<double>(now - fps_start_time).count();
            if (elapsed >= 0.5) {
                const auto frames = runtime->Bus().Frames() - fps_start_frame;
                current_fps = static_cast<double>(frames) / elapsed;
                fps_start_time = now;
                fps_start_frame = runtime->Bus().Frames();
            }
        }

        char fps_buffer[32];
        std::snprintf(fps_buffer, sizeof(fps_buffer), "%.1f FPS", runtime->Paused() ? 0.0 : current_fps);

        std::string title = std::string("NGBA v") + ngba::kVersionString + " - " + game_name + " - " +
            (runtime->Paused() ? "Paused" : fast_forward ? "Fast-forward (3x)" : "Running");
        if (!message.empty()) title += " | " + message;
        title += std::string(native ? " - Native + fallback" : " - Interpreter") +
            " - frame " + std::to_string(runtime->Bus().Frames()) +
            " (" + fps_buffer + ")" +
            " - native " + std::to_string(runtime->Cpu().TranslatedInstructions()) +
            " | Shift+F1-F12 save | F1-F12 load | Tab fast-forward | P pause | Ctrl+N step";
        SetWindowTextW(window, ngba::Utf8ToWide(title).c_str());
    }
};

int KeyBit(WPARAM key) {
    switch (key) {
    case 'Z': return 0;
    case 'X': return 1;
    case VK_BACK: return 2;
    case VK_RETURN: return 3;
    case VK_RIGHT: return 4;
    case VK_LEFT: return 5;
    case VK_UP: return 6;
    case VK_DOWN: return 7;
    case 'S': return 8;
    case 'A': return 9;
    default: return -1;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM parameter, LPARAM detail) {
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(detail)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app == nullptr) return DefWindowProcW(window, message, parameter, detail);
    if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && parameter == VK_F10 &&
        (detail & (1l << 29)) == 0) {
        message = message == WM_SYSKEYDOWN ? WM_KEYDOWN : WM_KEYUP;
    }
    if (message == WM_SYSKEYUP && parameter == VK_TAB) message = WM_KEYUP;
    try {
        switch (message) {
        case WM_KEYDOWN:
        case WM_KEYUP: {
            if (parameter == VK_TAB && message == WM_KEYUP) {
                app->SetFastForward(false);
                app->Refresh(window);
                return 0;
            }
            const int bit = KeyBit(parameter);
            if (bit >= 0) {
                if (message == WM_KEYDOWN) app->keys |= static_cast<std::uint16_t>(1u << bit);
                else app->keys &= static_cast<std::uint16_t>(~(1u << bit));
                app->runtime->Bus().SetKeys(app->keys);
                return 0;
            }
            if (message != WM_KEYDOWN || (detail & (1ll << 30)) != 0) return 0;
            if (parameter == 'P' || parameter == VK_SPACE) {
                app->runtime->SetPaused(!app->runtime->Paused());
                if (app->runtime->Paused()) {
                    app->runtime->FlushBatterySave();
                    app->audio.Reset();
                }
            } else if (parameter >= VK_F1 && parameter <= VK_F12) {
                app->UseStateSlot(static_cast<unsigned>(parameter - VK_F1 + 1), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            } else if (parameter == VK_TAB) {
                app->SetFastForward(true);
            } else if (parameter == 'N' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                app->runtime->SetPaused(true);
                app->runtime->StepFrame();
            }
            app->Refresh(window);
            return 0;
        }
        case WM_KILLFOCUS:
            app->SetFastForward(false);
            app->keys = 0;
            app->runtime->Bus().SetKeys(0);
            app->resume_on_focus = !app->runtime->Paused();
            app->runtime->SetPaused(true);
            app->audio.Reset();
            app->Refresh(window);
            return 0;
        case WM_SETFOCUS:
            if (app->resume_on_focus) app->runtime->SetPaused(false);
            app->resume_on_focus = false;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC context = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            const int width = std::max(0, std::min(static_cast<int>(client.right), static_cast<int>(client.bottom) * 3 / 2));
            const int height = width * 2 / 3;
            const int x = (client.right - width) / 2;
            const int y = (client.bottom - height) / 2;

            HDC memory_context = CreateCompatibleDC(context);
            HBITMAP memory_bitmap = CreateCompatibleBitmap(context, client.right, client.bottom);
            HGDIOBJ old_bitmap = SelectObject(memory_context, memory_bitmap);

            if (x > 0) {
                RECT left{0, 0, x, client.bottom};
                RECT right{x + width, 0, client.right, client.bottom};
                FillRect(memory_context, &left, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                FillRect(memory_context, &right, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            if (y > 0) {
                RECT top{0, 0, client.right, y};
                RECT bottom{0, y + height, client.right, client.bottom};
                FillRect(memory_context, &top, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                FillRect(memory_context, &bottom, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }

            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = 240;
            bitmap.bmiHeader.biHeight = -160;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            if (!app->pixels.empty()) {
                StretchDIBits(memory_context, x, y, width, height, 0, 0, 240, 160,
                              app->pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
            }

            BitBlt(context, 0, 0, client.right, client.bottom, memory_context, 0, 0, SRCCOPY);

            SelectObject(memory_context, old_bitmap);
            DeleteObject(memory_bitmap);
            DeleteDC(memory_context);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            app->runtime->FlushBatterySave();
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception& error) {
        app->runtime->SetPaused(true);
        app->SetFastForward(false);
        app->resume_on_focus = false;
        app->message = std::string("Error: ") + error.what();
        ShowError(window, error.what());
    }
    return DefWindowProcW(window, message, parameter, detail);
}

}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    try {
        int argument_count = 0;
        auto* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
        if (!arguments) throw std::runtime_error("cannot read command line");
        std::unique_ptr<void, decltype(&LocalFree)> argument_memory(arguments, &LocalFree);
        std::string rom;
        std::string bios;
        std::string bios_mode;
        std::string load_state;
        Application app;
        app.native = ngba::NativeBackend::Available();
        for (int index = 1; index < argument_count; ++index) {
            const auto argument = ngba::WideToUtf8(arguments[index]);
            if (argument == "--help" || argument == "-h") {
                MessageBoxW(nullptr,
                    L"Double-click ngba.exe to select a .gba ROM.\n"
                    L"Keep your gba_bios.bin beside ngba.exe or beside the ROM.\n\n"
                    L"Shift+F1 through F12: save slots 1-12\nF1 through F12: load slots 1-12\n"
                    L"Hold Tab: fast-forward (as fast as your PC allows)\n"
                    L"P / Space: pause or resume\nCtrl+N: step one frame\n"
                    L"Existing .ngbs saves are available in slot 1.\n"
                    L"Z / X: A / B\nEnter / Backspace: Start / Select\nArrows: D-pad\nA / S: L / R\n\n"
                    L"Optional command line:\nngba.exe [game.gba] [bios.bin] [--interpreter]\n"
                    L"[--bios-mode external|hle|hybrid] [--load-state FILE]",
                    (L"NGBA v" + ngba::Utf8ToWide(ngba::kVersionString) + L" - Help").c_str(), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (argument == "--interpreter") app.native = false;
            else if (argument == "--bios-mode") {
                if (++index >= argument_count) throw std::runtime_error("--bios-mode requires a value");
                bios_mode = ngba::WideToUtf8(arguments[index]);
                ngba::ParseBiosMode(bios_mode);
            }
            else if (argument == "--load-state") {
                if (++index >= argument_count) throw std::runtime_error("--load-state requires a value");
                load_state = ngba::WideToUtf8(arguments[index]);
            }
            else if (argument.find("--") == 0) throw std::runtime_error("unknown option: " + argument);
            else if (rom.empty()) rom = argument;
            else if (bios.empty()) bios = argument;
            else throw std::runtime_error("unexpected argument");
        }
        const auto executable_directory = ExecutableDirectory();
        if (rom.empty()) {
            rom = PickFile(L"NGBA - Select a Game Boy Advance ROM", L"Game Boy Advance ROM (*.gba)\0*.gba\0All files (*.*)\0*.*\0", executable_directory);
            if (rom.empty()) return 0;
        }
        if (bios.empty() && bios_mode != "hle") {
            bios = FindBios(executable_directory, rom);
            if (bios.empty()) {
                const bool requires_bios = bios_mode == "external" || bios_mode == "hybrid";
                if (!requires_bios) {
                    const int choice = MessageBoxW(nullptr,
                        L"gba_bios.bin was not found beside ngba.exe or the selected ROM.\n\n"
                        L"A 16 KiB GBA BIOS is recommended for compatibility.\n"
                        L"Yes: select your BIOS file.\n"
                        L"No: try experimental HLE (incomplete; some games will not run).\n"
                        L"Cancel: exit NGBA.",
                        L"NGBA - Select a BIOS?", MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (choice == IDCANCEL) return 0;
                    if (choice == IDNO) bios_mode = "hle";
                }
                if (bios_mode != "hle") {
                    bios = PickFile(L"NGBA - Select your 16 KiB GBA BIOS", L"BIOS files (*.bin)\0*.bin\0All files (*.*)\0*.*\0", executable_directory);
                    if (bios.empty()) return 0;
                }
            }
        }
        app.runtime.reset(new ngba::Runtime(rom, bios, app.native));
        app.runtime->SetAudioSink(&app.audio);
        if (!load_state.empty()) app.runtime->LoadState(load_state);
        if (!bios_mode.empty()) app.runtime->Cpu().SetBiosMode(ngba::ParseBiosMode(bios_mode));
        app.rom_path = rom;
        app.game_name = rom.substr(rom.find_last_of("\\/") + 1);
        WNDCLASSW type{};
        type.lpfnWndProc = WindowProcedure;
        type.hInstance = instance;
        type.lpszClassName = L"NGBA.Runtime";
        type.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&type)) throw std::runtime_error("cannot register NGBA window");
        RECT rectangle{0, 0, 720, 480};
        AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
        const std::wstring initial_title = L"NGBA v" + ngba::Utf8ToWide(ngba::kVersionString);
        HWND window = CreateWindowW(type.lpszClassName, initial_title.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, type.hInstance, &app);
        if (!window) throw std::runtime_error("cannot create NGBA window");
        app.Refresh(window);
        ShowWindow(window, show_command);
        MSG message{};
        struct TimerPeriodScope {
            TimerPeriodScope() { timeBeginPeriod(1); }
            ~TimerPeriodScope() { timeEndPeriod(1); }
        } timer_period_scope;

        bool running = true;
        while (running) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { running = false; break; }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;
            try {
                const auto now = Clock::now();
                if (!app.Advance(window, now)) {
                    if (app.runtime->Paused() || app.frame_deadline == Clock::time_point{}) {
                        Sleep(1);
                    } else {
                        const auto remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(app.frame_deadline - now).count();
                        if (remaining_us > 2000) {
                            Sleep(1);
                        } else if (remaining_us > 200) {
                            Sleep(0);
                        }
                    }
                }
            } catch (const std::exception& error) {
                app.runtime->SetPaused(true);
                app.SetFastForward(false);
                app.resume_on_focus = false;
                app.message = std::string("Error: ") + error.what();
                ShowError(window, error.what());
            }
        }
    } catch (const std::exception& error) {
        ShowError(nullptr, error.what());
        return 1;
    }
    return 0;
}
