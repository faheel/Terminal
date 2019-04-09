//
//    Copyright (C) Microsoft.  All rights reserved.
//
// WARNING!!!
// This is a fork of conpty.h
// It has some small modifications to help debug conhost-backed pseudoconsoles
//      within the context of Universal Applications. Notably:
// * SetHandleInformation and HANDLE_FLAG_INHERIT are not present in
//      WINAPI_PARTITION_APP, so we're just leaving the handles inheritable for
//      now. This is definitely a bug, but the ConhostConnection isn't meant to
//      be shipping code. Conhosts created by this version of CreateConPty will
//      only go away when the app is closed, not when the pipes are broken.
//      Fortunately, because the universal app is containered, they'll be
//      cleaned up when the app is terminated. IF YOU USE THIS HEADER OUTSIDE OF
//      A UNIVERSAL APP, THE CHILD CONHOST.EXE PROCESSES WILL NOT BE TERMINATED.
// * Whoever includes this will also need to define STARTF_USESTDHANDLES:
//   ```
//   #ifndef STARTF_USESTDHANDLES
//   #define STARTF_USESTDHANDLES       0x00000100
//   #endif
//   ```

#include <windows.h>
#include <string>
#include <sstream>
#include <strsafe.h>
#include <memory>
#pragma once

const unsigned int PTY_SIGNAL_RESIZE_WINDOW = 8u;

HRESULT CreateConPty(const std::wstring& cmdline,       // _In_
                     const unsigned short w,            // _In_
                     const unsigned short h,            // _In_
                     HANDLE* const hInput,              // _Out_
                     HANDLE* const hOutput,             // _Out_
                     HANDLE* const hSignal,             // _Out_
                     PROCESS_INFORMATION* const piPty); // _Out_

bool SignalResizeWindow(const HANDLE hSignal,
                        const unsigned short w,
                        const unsigned short h);


// Function Description:
// - Creates a headless conhost in "pty mode" and launches the given commandline
//      attached to the conhost. Gives back handles to three different pipes:
//   * hInput: The caller can write input to the conhost, encoded in utf-8, on
//      this pipe. For keys that don't have character representations, the
//      caller should use the `TERM=xterm` VT sequences for encoding the input.
//   * hOutput: The caller should read from this pipe. The headless conhost will
//      "render" it's state to a stream of utf-8 encoded text with VT sequences.
//   * hSignal: The caller can use this to resize the size of the underlying PTY
//      using the SignalResizeWindow function.
// Arguments:
// - cmdline: The commandline to launch as a console process attached to the pty
//      that's created.
// - w: The initial width of the pty, in characters
// - h: The initial height of the pty, in characters
// - hInput: A handle to the pipe for writing input to the pty.
// - hOutput: A handle to the pipe for reading the output of the pty.
// - hSignal: A handle to the pipe for writing signal messages to the pty.
// - piPty: The PROCESS_INFORMATION of the pty process. NOTE: This is *not* the
//      PROCESS_INFORMATION of the process that's created as a result the cmdline.
// Return Value:
// - S_OK if we succeeded, or an appropriate HRESULT for failing format the
//      commandline or failing to launch the conhost
__declspec(noinline) inline
HRESULT CreateConPty(const std::wstring& cmdline,
                     const unsigned short w,
                     const unsigned short h,
                     HANDLE* const hInput,
                     HANDLE* const hOutput,
                     HANDLE* const hSignal,
                     PROCESS_INFORMATION* const piPty)
{
    // Create some anon pipes so we can pass handles down and into the console.
    // IMPORTANT NOTE:
    // We're creating the pipe here with un-inheritable handles, then marking
    //      the conhost sides of the pipes as inheritable. We do this because if
    //      the entire pipe is marked as inheritable, when we pass the handles
    //      to CreateProcess, at some point the entire pipe object is copied to
    //      the conhost process, which includes the terminal side of the pipes
    //      (_inPipe and _outPipe). This means that if we die, there's still
    //      outstanding handles to our side of the pipes, and those handles are
    //      in conhost, despite conhost being unable to reference those handles
    //      and close them.
    // CRITICAL: Close our side of the handles. Otherwise you'll get the same
    //      problem if you close conhost, but not us (the terminal).
    HANDLE outPipeConhostSide;
    HANDLE inPipeConhostSide;
    HANDLE signalPipeConhostSide;

    SECURITY_ATTRIBUTES sa;
    sa = {0};
    sa.nLength = sizeof(sa);
    //sa.bInheritHandle = FALSE;
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    CreatePipe(&inPipeConhostSide, hInput, &sa, 0);
    CreatePipe(hOutput, &outPipeConhostSide, &sa, 0);

    // Mark inheritable for signal handle when creating. It'll have the same value on the other side.
    sa.bInheritHandle = TRUE;
    CreatePipe(&signalPipeConhostSide, hSignal, &sa, 0);

    //SetHandleInformation(inPipeConhostSide, HANDLE_FLAG_INHERIT, 1);
    //SetHandleInformation(outPipeConhostSide, HANDLE_FLAG_INHERIT, 1);

    std::wstring conhostCmdline = L"conhost.exe";
    conhostCmdline += L" --headless";
    std::wstringstream ss;
    if (w != 0 && h != 0)
    {
        ss << L" --width " << (unsigned long)w;
        ss << L" --height " << (unsigned long)h;
    }

    ss << L" --signal 0x" << std::hex << HandleToUlong(signalPipeConhostSide);
    conhostCmdline += ss.str();
    conhostCmdline += L" -- ";
    conhostCmdline += cmdline;

    STARTUPINFO si = {0};
    si.cb = sizeof(STARTUPINFOW);
    si.hStdInput = inPipeConhostSide;
    si.hStdOutput = outPipeConhostSide;
    si.hStdError = outPipeConhostSide;
    si.dwFlags |= STARTF_USESTDHANDLES;

    std::unique_ptr<wchar_t[]> mutableCommandline = std::make_unique<wchar_t[]>(conhostCmdline.length() + 1);
    if (mutableCommandline == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = StringCchCopy(mutableCommandline.get(), conhostCmdline.length()+1, conhostCmdline.c_str());
    if (!SUCCEEDED(hr))
    {
        return hr;
    }

    bool fSuccess = !!CreateProcessW(
        nullptr,
        mutableCommandline.get(),
        nullptr,    // lpProcessAttributes
        nullptr,    // lpThreadAttributes
        true,       // bInheritHandles
        0,          // dwCreationFlags
        nullptr,    // lpEnvironment
        nullptr,    // lpCurrentDirectory
        &si,        // lpStartupInfo
        piPty       // lpProcessInformation
    );

    CloseHandle(inPipeConhostSide);
    CloseHandle(outPipeConhostSide);
    CloseHandle(signalPipeConhostSide);

    return fSuccess ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

// Function Description:
// - Resizes the pty that's connected to hSignal.
// Arguments:
// - hSignal: A signal pipe as returned by CreateConPty.
// - w: The new width of the pty, in characters
// - h: The new height of the pty, in characters
// Return Value:
// - true if the resize succeeded, else false.
__declspec(noinline) inline
bool SignalResizeWindow(HANDLE hSignal, const unsigned short w, const unsigned short h)
{
    unsigned short signalPacket[3];
    signalPacket[0] = PTY_SIGNAL_RESIZE_WINDOW;
    signalPacket[1] = w;
    signalPacket[2] = h;

    return !!WriteFile(hSignal, signalPacket, sizeof(signalPacket), nullptr, nullptr);
}

