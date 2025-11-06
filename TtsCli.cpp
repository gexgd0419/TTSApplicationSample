#pragma comment(lib, "Ws2_32.lib")
#define _WINSOCKAPI_    // Stops windows.h from including winsock.h
#include "TtsCli.h"
#include "TtsApplication.h" // for SpeakText()
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <sstream>
#include <atlbase.h>
#include <sapi.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <iostream>
#include <chrono>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <sstream>

// allows wcout << L"…" to print UTF-8

// Keep your utf8_to_wstring helper (or use this one)
static std::wstring utf8_to_wstring(const std::string &s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wstr(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &wstr[0], len);
    return wstr;
}


// utility to clamp rate/volume
static LONG ClampSapiRate(float rate) {
    LONG r = static_cast<LONG>((rate - 1.0f) * 10.0f);
    if (r < -10) r = -10;
    if (r > 10) r = 10;
    return r;
}
static int ClampVolume(int vol) {
    if (vol < 0) return 0;
    if (vol > 100) return 100;
    return vol;
}

int RunCli(int argc, char *argv[])
{
    _setmode(_fileno(stdout), _O_U8TEXT); 

    // preliminary parse for server-mode options: --server, --voice, --rate, --volume
    bool serverMode = false;
    std::wstring srvVoice = L"Default";
    float srvRate = 1.0f;
    int srvVolume = 100;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--server" || arg == "--daemon")
        {
            serverMode = true;
        }
        else if (arg == "--voice" && i + 1 < argc)
        {
            ++i;
            srvVoice = utf8_to_wstring(argv[i]);
        }
        else if (arg == "--rate" && i + 1 < argc)
        {
            srvRate = std::stof(argv[++i]);
        }
        else if (arg == "--volume" && i + 1 < argc)
        {
            srvVolume = std::stoi(argv[++i]);
        }
    }

    // If started in server mode, run a persistent process that reads stdin
    // and speaks queued lines. Usage: TtsApplication --server [--voice name] [--rate x] [--volume n]
if (serverMode)
    {
        // make console wide output visible
        _setmode(_fileno(stdout), _O_U8TEXT);
        std::wcout << L"[Server] Starting TCP server mode...\n";

        // Client connection wrapper so we can send replies back to the client
        struct Client {
            SOCKET sock;
            std::mutex sendMutex;
            std::atomic<bool> alive{ true };
            Client(SOCKET s) : sock(s) {}
            ~Client() {
                if (sock != INVALID_SOCKET) {
                    closesocket(sock);
                }
            }
            bool sendUtf8(const std::string &msg) {
                std::lock_guard<std::mutex> lk(sendMutex);
                if (!alive.load() || sock == INVALID_SOCKET) return false;
                const char *buf = msg.data();
                int left = (int)msg.size();
                while (left > 0) {
                    int sent = ::send(sock, buf, left, 0);
                    if (sent == SOCKET_ERROR) return false;
                    left -= sent;
                    buf += sent;
                }
                return true;
            }
        };

        // Message stored in queue: text + the originating client (may be nullptr)
        struct Message {
            std::wstring text;
            std::shared_ptr<Client> client;
        };

        // thread-safe queue
        std::queue<Message> q;
        std::mutex qmutex;
        std::condition_variable qcv;
        std::atomic<bool> running{ true };

        // Copies of settings
        std::wstring voiceSetting = srvVoice;
        float rateSetting = srvRate;
        int volumeSetting = srvVolume;

        // Helper: convert wide string to UTF-8
        auto wstring_to_utf8 = [](const std::wstring &ws) -> std::string {
            if (ws.empty()) return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
            if (len <= 0) return {};
            std::string s(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, nullptr, nullptr);
            return s;
        };

        // --- Voice worker (STA) ---
        std::thread voiceThread([&q, &qmutex, &qcv, &running, voiceSetting, rateSetting, volumeSetting]() {
            // set up COM in STA for SAPI
            HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            if (FAILED(hr)) {
                std::wcerr << L"[Server] CoInitializeEx failed: " << std::hex << hr << L"\n";
                return;
            }

            CComPtr<ISpVoice> cpVoice;
            hr = cpVoice.CoCreateInstance(CLSID_SpVoice);
            if (FAILED(hr)) {
                std::wcerr << L"[Server] CoCreateInstance failed: " << std::hex << hr << L"\n";
                CoUninitialize();
                return;
            }

            // select voice if requested
            if (!voiceSetting.empty() && voiceSetting != L"Default") {
                CComPtr<IEnumSpObjectTokens> cpEnum;
                if (SUCCEEDED(SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum)) && cpEnum) {
                    while (true) {
                        CComPtr<ISpObjectToken> cpToken;
                        if (cpEnum->Next(1, &cpToken, NULL) != S_OK) break;
                        CSpDynamicString desc;
                        SpGetDescription(cpToken, &desc);
                        if (desc && wcsstr(desc, voiceSetting.c_str())) {
                            cpVoice->SetVoice(cpToken);
                            break;
                        }
                    }
                }
            }

            cpVoice->SetVolume(ClampVolume(volumeSetting));
            cpVoice->SetRate(ClampSapiRate(rateSetting));

            while (running.load()) {
                std::unique_lock<std::mutex> lk(qmutex);
                qcv.wait(lk, [&]() { return !q.empty() || !running.load(); });
                if (!running.load() && q.empty()) break;
                if (q.empty()) continue;
                Message msg = std::move(q.front());
                q.pop();
                std::wstring text = std::move(msg.text);
                lk.unlock();

                // Speak synchronously on this dedicated STA worker thread.
                // Using SPF_DEFAULT (synchronous) avoids complexity with async
                // state transitions and ensures the ISpVoice remains valid
                // for the duration of the utterance.
                try {
                    hr = cpVoice->Speak(text.c_str(), SPF_DEFAULT, NULL);
                    if (FAILED(hr)) {
                        std::wcerr << L"[Server] Speak failed: 0x" << std::hex << hr
                                   << L" for text snippet: " << text.substr(0, 200) << L"\n";
                    }
                }
                catch (const std::exception &ex) {
                    std::wcerr << L"[Server] Exception while speaking: " << ex.what() << L"\n";
                }
                catch (...) {
                    std::wcerr << L"[Server] Unknown exception while speaking.\n";
                }
                // After speaking, attempt to notify originating client (best-effort)
                try {
                    if (msg.client) {
                        msg.client->sendUtf8(std::string("PLAYED\n"));
                    }
                } catch (...) {}
            }

            cpVoice.Release();
            CoUninitialize();
        });

        // --- TCP listener ---
        // default port (change as needed)
        const char *bindPortStr = "5150";
        // if you parsed --port, use that instead; example: bindPortStr = portArg.c_str();

        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::wcerr << L"[Server] WSAStartup failed\n";
            running.store(false);
            qcv.notify_one();
            if (voiceThread.joinable()) voiceThread.join();
            return 1;
        }

        addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        int rc = getaddrinfo(NULL, bindPortStr, &hints, &res);
        if (rc != 0 || !res) {
            std::wcerr << L"[Server] getaddrinfo failed: " << rc << L"\n";
            WSACleanup();
            running.store(false);
            qcv.notify_one();
            if (voiceThread.joinable()) voiceThread.join();
            return 1;
        }

        SOCKET listenSock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (listenSock == INVALID_SOCKET) {
            std::wcerr << L"[Server] socket() failed\n";
            freeaddrinfo(res);
            WSACleanup();
            running.store(false);
            qcv.notify_one();
            if (voiceThread.joinable()) voiceThread.join();
            return 1;
        }

        // Allow immediate reuse of the port
        int opt = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        if (bind(listenSock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
            std::wcerr << L"[Server] bind() failed, err=" << WSAGetLastError() << L"\n";
            closesocket(listenSock);
            freeaddrinfo(res);
            WSACleanup();
            running.store(false);
            qcv.notify_one();
            if (voiceThread.joinable()) voiceThread.join();
            return 1;
        }

        freeaddrinfo(res);

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            std::wcerr << L"[Server] listen() failed, err=" << WSAGetLastError() << L"\n";
            closesocket(listenSock);
            WSACleanup();
            running.store(false);
            qcv.notify_one();
            if (voiceThread.joinable()) voiceThread.join();
            return 1;
        }

        std::wcout << L"[Server] Listening on port " << bindPortStr << L". Connect with: nc 127.0.0.1 " << bindPortStr << L" or telnet\n";

        // accept loop runs in current thread: accepts and reads lines from each connection
        // each client may send multiple newline-terminated messages
        while (running.load()) {
            sockaddr_in clientAddr;
            int addrlen = sizeof(clientAddr);
            SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrlen);
            if (clientSock == INVALID_SOCKET) {
                int err = WSAGetLastError();
                if (!running.load()) break;
                std::wcerr << L"[Server] accept() failed: " << err << L"\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            // Keep the client socket open and create a Client object we can
            // use to reply with ACKs or unsent text later.
            auto client = std::make_shared<Client>(clientSock);

            std::thread([client, &q, &qmutex, &qcv, &running]() {
                std::string buffer;
                const int BUFSZ = 4096;
                char tmp[BUFSZ];
                SOCKET sock = client->sock;
                while (running.load() && client->alive.load()) {
                    int n = recv(sock, tmp, BUFSZ, 0);
                    if (n == 0 || n == SOCKET_ERROR) break;
                    buffer.append(tmp, tmp + n);

                    // extract lines
                    size_t pos;
                    while ((pos = buffer.find('\n')) != std::string::npos) {
                        std::string line = buffer.substr(0, pos);
                        // remove possible '\r'
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        buffer.erase(0, pos + 1);

                        std::wstring w = utf8_to_wstring(line);
                        if (!w.empty()) {
                            Message m;
                            m.text = std::move(w);
                            m.client = client;
                            {
                                std::lock_guard<std::mutex> lk(qmutex);
                                q.push(std::move(m));
                            }
                            qcv.notify_one();
                            // send immediate enqueue ack (best-effort)
                            try { client->sendUtf8(std::string("ENQUEUED\n")); } catch (...) {}
                        }
                    }
                }
                // mark client dead; Client destructor will close the socket
                client->alive.store(false);
            }).detach();
        }

        // shutdown
        closesocket(listenSock);
        WSACleanup();

        // stop accepting/producing and notify worker
        running.store(false);

        // Take remaining queued messages (unspoken) and attempt to send them
        // back to originating clients as UNSPOKEN notifications.
        std::queue<Message> remaining;
        {
            std::lock_guard<std::mutex> lk(qmutex);
            std::swap(remaining, q);
        }
        while (!remaining.empty()) {
            Message &m = remaining.front();
            if (m.client) {
                try {
                    std::string uns = std::string("UNSPOKEN:") + wstring_to_utf8(m.text) + "\n";
                    m.client->sendUtf8(uns);
                } catch (...) {
                    // ignore send errors
                }
            }
            remaining.pop();
        }

        qcv.notify_one();
        if (voiceThread.joinable()) voiceThread.join();

        std::wcout << L"[Server] Shutdown complete.\n";
        return 0;
    } // end serverMode

    if (argc < 2)
        return 1; // nothing to say
    std::wcout << L"Normal mode started.\n";
    // Default values
    std::wstring voice = L"Default";
    float rate = 1.0f;
    int volume = 100;
    std::wstring outputFile; // empty means play directly
    std::wstring text;

    // Parse flags
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--voice" && i + 1 < argc)
        {
            ++i;
            voice = utf8_to_wstring(argv[i]);
        }
        else if (arg == "--rate" && i + 1 < argc)
            rate = std::stof(argv[++i]);
        else if (arg == "--volume" && i + 1 < argc)
            volume = std::stoi(argv[++i]);
        else if (arg == "--output" && i + 1 < argc)
        {
            ++i;
            outputFile = utf8_to_wstring(argv[i]);
        }
        else
        {
            // Append to text
            std::wstring wstr = utf8_to_wstring(argv[i]);
            if (!text.empty())
                text += L" ";
            text += wstr;
        }
    }
    try
    {
        if (!outputFile.empty())
        {
            // Save to .wav
            HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            if (FAILED(hr))
                throw std::runtime_error("Failed to initialize COM");

            CComPtr<ISpVoice> cpVoice;
            hr = cpVoice.CoCreateInstance(CLSID_SpVoice);
            if (FAILED(hr))
            {
                CoUninitialize();
                throw std::runtime_error("Failed to create SAPI voice instance");
            }

            // Set voice
            if (!voice.empty() && voice != L"Default")
            {
                CComPtr<IEnumSpObjectTokens> cpEnum;
                hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum);
                if (SUCCEEDED(hr) && cpEnum)
                {
                    while (true)
                    {
                        CComPtr<ISpObjectToken> cpToken;
                        if (cpEnum->Next(1, &cpToken, NULL) != S_OK)
                            break;

                        CSpDynamicString desc;
                        SpGetDescription(cpToken, &desc);
                        if (desc && wcsstr(desc, voice.c_str()))
                        {
                            cpVoice->SetVoice(cpToken);
                            break;
                        }
                    }
                }
            }

            cpVoice->SetVolume(volume);
            cpVoice->SetRate(static_cast<LONG>((rate - 1.0f) * 10));

            // Set output to file
            CComPtr<ISpStream> cpStream;
            CSpStreamFormat format;
            hr = SPBindToFile(outputFile.c_str(), SPFM_CREATE_ALWAYS, &cpStream, &format.FormatId(), format.WaveFormatExPtr());
            if (FAILED(hr))
            {
                cpVoice.Release();
                CoUninitialize();
                throw std::runtime_error("Failed to open output file for writing");
            }

            cpVoice->SetOutput(cpStream, TRUE);
            cpVoice->Speak(text.c_str(), SPF_DEFAULT, NULL);
            cpVoice->SetOutput(NULL, TRUE);

            cpStream.Release();
            cpVoice.Release();
            CoUninitialize();
        }
        else
        {
            // Play directly
            SpeakText(text, voice, rate, volume, L"");
        }
    }
    catch (const std::exception &e)
    {
        MessageBoxA(NULL, e.what(), "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    return 0;
}
