#include "../src/windows.cpp"

#include <array>
#include <cassert>

namespace {

const std::string kRomPath = u8"ngba-controls-test-\u00E9-\u6E38\u620F.gba";

void Keyboard(bool shift = false, bool control = false) {
    std::array<BYTE, 256> state{};
    state[VK_SHIFT] = state[VK_LSHIFT] = shift ? 0x80 : 0;
    state[VK_CONTROL] = state[VK_LCONTROL] = control ? 0x80 : 0;
    assert(SetKeyboardState(state.data()));
}

void Press(HWND window, WPARAM key, bool shift = false, bool control = false) {
    Keyboard(shift, control);
    const UINT down = key == VK_F10 ? WM_SYSKEYDOWN : WM_KEYDOWN;
    const UINT up = key == VK_F10 ? WM_SYSKEYUP : WM_KEYUP;
    SendMessageW(window, down, key, 0);
    SendMessageW(window, up, key, 1l << 31);
    Keyboard();
}

std::wstring Title(HWND window) {
    std::array<wchar_t, 1024> title{};
    GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    return title.data();
}

void TestSlots(Application& app, HWND window) {
    app.runtime->SetPaused(true);
    assert(app.StatePath(1) == kRomPath + ".ngbs");
    std::array<ngba::Cycle, 12> saved_frames{};
    std::array<std::vector<std::uint8_t>, 12> saved_bytes;
    for (unsigned slot = 1; slot <= 12; ++slot) {
        if (slot != 1) assert(app.StatePath(slot) == kRomPath + ".slot" + std::to_string(slot) + ".ngbs");
        Press(window, 'N', false, true);
        assert(app.runtime->Paused());
        app.runtime->Bus().Write32(0x02000000, slot);
        Press(window, VK_F1 + slot - 1, true);
        assert(app.message == "Saved slot " + std::to_string(slot));
        saved_frames[slot - 1] = app.runtime->Bus().Frames();
        saved_bytes[slot - 1] = ngba::ReadBinaryFile(app.StatePath(slot), 2u * 1024u * 1024u);
        assert(GetFileAttributesW(ngba::Utf8ToWide(app.StatePath(slot) + ".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
        app.runtime->Bus().Write32(0x02000000, 99);
        Keyboard(true);
        SendMessageW(window, slot == 10 ? WM_SYSKEYDOWN : WM_KEYDOWN, VK_F1 + slot - 1, 1l << 30);
        Keyboard();
        assert(ngba::ReadBinaryFile(app.StatePath(slot), 2u * 1024u * 1024u) == saved_bytes[slot - 1]);
    }
    for (unsigned slot = 1; slot <= 12; ++slot) {
        Press(window, VK_F1 + slot - 1);
        assert(app.message == "Loaded slot " + std::to_string(slot));
        assert(app.runtime->Paused());
        assert(app.runtime->Bus().Frames() == saved_frames[slot - 1]);
        assert(app.runtime->Bus().Read32(0x02000000) == slot);
        assert(ngba::ReadBinaryFile(app.StatePath(slot), 2u * 1024u * 1024u) == saved_bytes[slot - 1]);
    }
    Press(window, 'N');
    assert(app.runtime->Bus().Frames() == saved_frames.back());
    Press(window, 'N', false, true);
    Press(window, VK_F12, true);
    const auto overwritten = app.runtime->Bus().Frames();
    Press(window, VK_F1);
    Press(window, VK_F12);
    assert(app.runtime->Bus().Frames() == overwritten);
    assert(ngba::ReadBinaryFile(app.StatePath(1), 2u * 1024u * 1024u) == saved_bytes[0]);
    assert(DeleteFileW(ngba::Utf8ToWide(app.StatePath(12)).c_str()));
    Press(window, VK_F12);
    assert(app.message == "Slot 12 is empty");
    assert(app.runtime->Bus().Frames() == overwritten);
    app.runtime->SetPaused(false);
    Press(window, VK_F1);
    assert(!app.runtime->Paused());
    Press(window, VK_F12);
    assert(app.message == "Slot 12 is empty" && !app.runtime->Paused());
    const auto before_error = app.runtime->Bus().Frames();
    ngba::WriteBinaryFile(app.StatePath(12), {1, 2, 3});
    bool rejected = false;
    try { app.UseStateSlot(12, false); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected && app.runtime->Bus().Frames() == before_error);
    Press(window, VK_F12, true);
}

void TestKeysAndFocus(Application& app, HWND window) {
    SendMessageW(window, WM_KEYDOWN, VK_BACK, 0);
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    assert(app.keys == 12);
    assert((app.runtime->Bus().Read16(0x04000130) & 12) == 0);
    Press(window, VK_F1);
    assert((app.runtime->Bus().Read16(0x04000130) & 12) == 0);
    SendMessageW(window, WM_KEYUP, VK_BACK, 0);
    SendMessageW(window, WM_KEYUP, VK_RETURN, 0);
    assert(app.keys == 0);
    assert((app.runtime->Bus().Read16(0x04000130) & 12) == 12);
    SendMessageW(window, WM_KEYDOWN, VK_TAB, 0);
    assert(app.fast_forward);
    assert(Title(window).find(L"Fast-forward") != std::wstring::npos);
    SendMessageW(window, WM_KEYDOWN, VK_TAB, 1l << 30);
    assert(app.fast_forward);
    SendMessageW(window, WM_KEYUP, VK_TAB, 0);
    assert(!app.fast_forward);
    SendMessageW(window, WM_KEYDOWN, VK_TAB, 0);
    SendMessageW(window, WM_SYSKEYUP, VK_TAB, 1l << 29);
    assert(!app.fast_forward);
    app.runtime->SetPaused(false);
    SendMessageW(window, WM_KEYDOWN, 'Z', 0);
    SendMessageW(window, WM_KEYDOWN, VK_TAB, 0);
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    assert(!app.fast_forward && app.keys == 0 && app.runtime->Paused());
    assert((app.runtime->Bus().Read16(0x04000130) & 0x3FF) == 0x3FF);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    assert(!app.runtime->Paused() && !app.fast_forward);
    Press(window, 'P');
    assert(app.runtime->Paused());
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    assert(app.runtime->Paused());
    Press(window, VK_SPACE);
    assert(!app.runtime->Paused());
}

void TestFramePacing(Application& app, HWND window) {
    const auto now = Clock::now();
    app.SetFastForward(false);
    app.runtime->SetPaused(false);
    auto frame = app.runtime->Bus().Frames();
    assert(app.Advance(window, now));
    assert(app.runtime->Bus().Frames() == ++frame);
    assert(!app.Advance(window, now));
    assert(!app.Advance(window, now + kFrameTime - Clock::duration(1)));
    assert(app.Advance(window, now + kFrameTime));
    assert(app.runtime->Bus().Frames() == ++frame);
    SendMessageW(window, WM_KEYDOWN, VK_TAB, 0);
    const auto ff_interval = kFrameTime / 3;
    assert(app.Advance(window, now));
    assert(app.runtime->Bus().Frames() == ++frame);
    assert(!app.Advance(window, now));
    assert(app.Advance(window, now + ff_interval));
    assert(app.runtime->Bus().Frames() == ++frame);
    assert(app.Advance(window, now + ff_interval * 2));
    assert(app.runtime->Bus().Frames() == ++frame);
    assert(app.Advance(window, now + ff_interval * 3));
    assert(app.runtime->Bus().Frames() == ++frame);
    const auto title = Title(window);
    assert(title.find(L"FPS") != std::wstring::npos);
    assert(app.Advance(window, now + ff_interval * 4));
    assert(app.runtime->Bus().Frames() == ++frame);
    assert(app.Advance(window, now + ff_interval * 5));
    assert(app.Advance(window, now + ff_interval * 6));
    assert(Title(window) != title);
    app.runtime->SetPaused(true);
    frame = app.runtime->Bus().Frames();
    assert(!app.Advance(window, now + kFrameTime * 2));
    assert(app.runtime->Bus().Frames() == frame);
    SendMessageW(window, WM_KEYUP, VK_TAB, 0);
    app.runtime->SetPaused(false);
    assert(app.Advance(window, now + ff_interval * 7));
    assert(!app.Advance(window, now + ff_interval * 7));
    Press(window, VK_F11, true);
    app.SetFastForward(false);
    for (unsigned index = 1; index <= 12; ++index) assert(app.Advance(window, now + ff_interval * 7 + kFrameTime * index));
    Press(window, VK_F12, true);
    const auto normal = ngba::ReadBinaryFile(app.StatePath(12), 2u * 1024u * 1024u);
    Press(window, VK_F11);
    app.SetFastForward(true);
    for (unsigned index = 1; index <= 12; ++index) assert(app.Advance(window, now + ff_interval * 7 + (kFrameTime / 3) * index));
    Press(window, VK_F12, true);
    assert(ngba::ReadBinaryFile(app.StatePath(12), 2u * 1024u * 1024u) == normal);
}

}

int main() {
    std::array<BYTE, 256> original_keyboard{};
    assert(GetKeyboardState(original_keyboard.data()));
    std::vector<std::uint8_t> rom(512, 0);
    rom[0] = 0x32; rom[3] = 0xEA;
    rom[0xB2] = 0x96;
    rom[0xD0] = 1; rom[0xD2] = 0xA0; rom[0xD3] = 0xE3;
    rom[0xD4] = 0xFD; rom[0xD5] = 0xFF; rom[0xD6] = 0xFF; rom[0xD7] = 0xEA;
    ngba::WriteBinaryFile(kRomPath, rom);
    Application app;
    app.rom_path = kRomPath;
    app.game_name = "Controls test";
    app.native = ngba::NativeBackend::Available();
    app.runtime.reset(new ngba::Runtime(kRomPath, "", app.native));
    app.runtime->SetAudioSink(&app.audio);
    WNDCLASSW type{};
    type.lpfnWndProc = WindowProcedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"NGBA.ControlsTest";
    assert(RegisterClassW(&type));
    HWND window = CreateWindowW(type.lpszClassName, L"Controls test", WS_OVERLAPPEDWINDOW,
        0, 0, 720, 480, nullptr, nullptr, type.hInstance, &app);
    assert(window);
    TestSlots(app, window);
    TestKeysAndFocus(app, window);
    TestFramePacing(app, window);
    DestroyWindow(window);
    for (unsigned slot = 1; slot <= 12; ++slot) DeleteFileW(ngba::Utf8ToWide(app.StatePath(slot)).c_str());
    DeleteFileW(ngba::Utf8ToWide(kRomPath).c_str());
    assert(SetKeyboardState(original_keyboard.data()));
    return 0;
}
