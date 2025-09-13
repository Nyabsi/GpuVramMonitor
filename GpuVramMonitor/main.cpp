#include <windows.h>
#include <pdh.h>
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <dxgi1_6.h>
#include <PdhMsg.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

struct ProcessGpuInfo {
    DWORD pid;
    std::wstring processName;
    uint64_t luid1;
    uint64_t luid2;
    int physicalGpu;
    size_t dedicatedBytes;
    size_t sharedBytes;
    std::wstring gpuName;

    ProcessGpuInfo() : pid(0), luid1(0), luid2(0), physicalGpu(0),
        dedicatedBytes(0), sharedBytes(0), gpuName(L"") {
    }
};

class GpuVramMonitor {
private:
    PDH_HQUERY hQuery;
    PDH_HCOUNTER hCounterDedicated;
    PDH_HCOUNTER hCounterShared;
    std::map<DWORD, ProcessGpuInfo> processMap;
    uint64_t maxDedicatedVRAMBytes = 0;
    uint64_t maxSharedVRAMBytes = 0;
    std::wstring primaryGpuName;

public:
    GpuVramMonitor() : hQuery(NULL), hCounterDedicated(NULL), hCounterShared(NULL) {}

    ~GpuVramMonitor() {
        if (hQuery) {
            PdhCloseQuery(hQuery);
        }
    }

    bool Initialize() {
        if (PdhOpenQuery(NULL, 0, &hQuery) != ERROR_SUCCESS) return false;

        if (PdhAddCounter(hQuery,
            L"\\GPU Process Memory(*)\\Dedicated Usage",
            0, &hCounterDedicated) != ERROR_SUCCESS) return false;

        if (PdhAddCounter(hQuery,
            L"\\GPU Process Memory(*)\\Shared Usage",
            0, &hCounterShared) != ERROR_SUCCESS) return false;

        PdhCollectQueryData(hQuery);
        FetchPrimaryGpuInfo();
        return true;
    }

    bool ParseCounterName(const std::wstring& name, ProcessGpuInfo& info) {
        std::wistringstream stream(name);
        std::wstring token;
        std::vector<std::wstring> tokens;

        while (std::getline(stream, token, L'_')) {
            tokens.push_back(token);
        }

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == L"pid" && i + 1 < tokens.size()) {
                info.pid = std::stoul(tokens[i + 1]);
                i++;
            }
            else if (tokens[i] == L"luid" && i + 2 < tokens.size()) {
                info.luid1 = std::stoull(tokens[i + 1], nullptr, 16);
                info.luid2 = std::stoull(tokens[i + 2], nullptr, 16);
                i += 2;
            }
            else if (tokens[i] == L"phys" && i + 1 < tokens.size()) {
                info.physicalGpu = std::stoi(tokens[i + 1]);
                i++;
            }
        }

        return info.pid != 0;
    }

    std::wstring GetProcessName(DWORD pid) {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) return L"";

        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
            CloseHandle(hProcess);
            std::wstring fullPath(path);
            size_t pos = fullPath.find_last_of(L"\\/");
            return (pos == std::wstring::npos) ? fullPath : fullPath.substr(pos + 1);
        }
        CloseHandle(hProcess);
        return L"";
    }

    std::map<std::pair<uint64_t, uint64_t>, std::wstring> EnumerateGpuLUIDs() {
        std::map<std::pair<uint64_t, uint64_t>, std::wstring> luidToName;
        IDXGIFactory6* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&factory))) return luidToName;

        IDXGIAdapter1* adapter = nullptr;
        UINT i = 0;
        while (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                LUID luid = desc.AdapterLuid;
                luidToName[{ (uint64_t)luid.HighPart, (uint64_t)(uint32_t)luid.LowPart }] = std::wstring(desc.Description);
            }
            adapter->Release();
            ++i;
        }
        factory->Release();
        return luidToName;
    }

    void FetchPrimaryGpuInfo() {
        IDXGIFactory6* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&factory))) {
            maxDedicatedVRAMBytes = 0;
            maxSharedVRAMBytes = 0;
            primaryGpuName = L"Unknown";
            return;
        }

        IDXGIAdapter1* adapter = nullptr;

        UINT i = 0;
        while (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC1 descr{};
            if (SUCCEEDED(adapter->GetDesc1(&descr))) {
                // make sure it's not "Windows Basic Virtual Adapter"
                if (!(descr.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && descr.DedicatedVideoMemory > 0) {
                    maxDedicatedVRAMBytes = descr.DedicatedVideoMemory;
                    maxSharedVRAMBytes = descr.SharedSystemMemory;
                    primaryGpuName = std::wstring(descr.Description);
                }
            }
            adapter->Release();
            ++i;
        }
        factory->Release();
    }

    bool Update() {
        processMap.clear();
        if (PdhCollectQueryData(hQuery) != ERROR_SUCCESS) return false;

        DWORD bufferSize = 0, itemCount = 0;
        PDH_FMT_COUNTERVALUE_ITEM* items = nullptr;

        if (PdhGetFormattedCounterArray(hCounterDedicated, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr) != PDH_MORE_DATA)
            return false;

        items = (PDH_FMT_COUNTERVALUE_ITEM*)malloc(bufferSize);
        if (PdhGetFormattedCounterArray(hCounterDedicated, PDH_FMT_LARGE, &bufferSize, &itemCount, items) != ERROR_SUCCESS) {
            free(items);
            return false;
        }

        for (DWORD i = 0; i < itemCount; ++i) {
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                ProcessGpuInfo info;
                if (ParseCounterName(items[i].szName, info)) {
                    info.dedicatedBytes = items[i].FmtValue.largeValue;
                    processMap[info.pid] = info;
                }
            }
        }
        free(items);

        bufferSize = 0;
        if (PdhGetFormattedCounterArray(hCounterShared, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr) != PDH_MORE_DATA)
            return false;

        items = (PDH_FMT_COUNTERVALUE_ITEM*)malloc(bufferSize);
        if (PdhGetFormattedCounterArray(hCounterShared, PDH_FMT_LARGE, &bufferSize, &itemCount, items) != ERROR_SUCCESS) {
            free(items);
            return false;
        }

        for (DWORD i = 0; i < itemCount; ++i) {
            if (items != NULL && items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                ProcessGpuInfo info;
                if (ParseCounterName(items[i].szName, info)) {
                    if (processMap.find(info.pid) != processMap.end()) {
                        processMap[info.pid].sharedBytes = items[i].FmtValue.largeValue;
                    }
                }
            }
        }

        free(items);

        auto gpuMap = EnumerateGpuLUIDs();
        for (auto it = processMap.begin(); it != processMap.end(); ) { 
            it->second.processName = GetProcessName(it->first); 
            auto key = std::make_pair(it->second.luid1, it->second.luid2);
            it->second.gpuName = (gpuMap.count(key)) ? gpuMap[key] : L"Unknown GPU";
            // If the process name is empty but allocates VRAM it's an system process
            // it's removed because Task Manager removes these processes as well.
            if (it->second.processName.empty()) { 
                it = processMap.erase(it); 
            } else { 
                ++it; 
            } 
        }

        return true;
    }

    void DisplayResults() {
        if (processMap.empty()) {
            std::cout << "No GPU memory usage data available.\n";
            return;
        }

        std::vector<ProcessGpuInfo> processes;
        for (const auto& p : processMap) {
            if (p.second.dedicatedBytes > 0 || p.second.sharedBytes > 0)
                processes.push_back(p.second);
        }

        std::sort(processes.begin(), processes.end(), [](auto& a, auto& b) {
            return (a.dedicatedBytes + a.sharedBytes) > (b.dedicatedBytes + b.sharedBytes);
            });

        size_t totalDedicated = 0, totalShared = 0;
        for (const auto& p : processes) {
            totalDedicated += p.dedicatedBytes;
            totalShared += p.sharedBytes;
        }

        std::cout << std::string(120, '=') << "\n";

        std::cout << std::left << std::setw(8) << "PID"
            << std::setw(30) << "Process Name"
            << std::setw(30) << "GPU Name"
            << std::right
            << std::setw(15) << "Dedicated (MB)"
            << std::setw(15) << "Shared (MB)"
            << std::setw(15) << "Total (MB)" << "\n";
        std::cout << std::string(120, '-') << "\n";

        for (const auto& p : processes) {
            double dedicatedMB = p.dedicatedBytes / 1'000'000.0;
            double sharedMB = p.sharedBytes / 1'000'000.0;
            double totalMB = dedicatedMB + sharedMB;

            std::wcout << std::left << std::setw(8) << p.pid
                << std::setw(30) << p.processName
                << std::setw(30) << p.gpuName;

            std::cout << std::right << std::fixed << std::setprecision(1)
                << std::setw(15) << dedicatedMB
                << std::setw(15) << sharedMB
                << std::setw(15) << totalMB
                << "\n";
        }

        std::cout << std::string(120, '=') << "\n";

        double totalDedicatedGB = totalDedicated / 1'000'000'000.0;
        double totalSharedGB = totalShared / 1'000'000'000.0;
        double totalGB = totalDedicatedGB + totalSharedGB;

        double maxDedicatedGB = maxDedicatedVRAMBytes / 1024.0 / 1024.0 / 1024.0;
        double maxSharedGB = maxSharedVRAMBytes / 1024.0 / 1024.0 / 1024.0;

        std::cout << std::left << std::setw(68) << "TOTAL"
            << std::right << std::fixed << std::setprecision(2)
            << std::setw(15) << totalDedicatedGB << " GB"
            << std::setw(12) << totalSharedGB << " GB"
            << std::setw(12) << totalGB << " GB\n";

        std::cout << std::string(120, '-') << "\n";

        std::wcout << L"Primary GPU: " << primaryGpuName << L"\n";
        std::cout << "Max VRAM Budget: " << std::fixed << std::setprecision(2)
            << maxDedicatedGB << " GB (Dedicated) + "
            << maxSharedGB << " GB (Shared)\n";

        std::cout << "Total VRAM Usage: " << std::fixed << std::setprecision(2)
            << totalDedicatedGB << " GB (Dedicated) + "
            << totalSharedGB << " GB (Shared) = "
            << totalGB << " GB\n";

        std::cout << std::string(120, '=') << "\n";
    }
};

int main() {
    GpuVramMonitor monitor;

    if (!monitor.Initialize()) {
        std::cerr << "Failed to initialize GPU monitoring\n";
        return 1;
    }

    while (true) {
        system("cls");
        if (monitor.Update()) {
            monitor.DisplayResults();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    return 0;
}
