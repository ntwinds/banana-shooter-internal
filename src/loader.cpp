#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    explicit operator bool()const{return value&&value!=INVALID_HANDLE_VALUE;}
};
std::uintptr_t module_base(DWORD pid,const wchar_t* module,std::wstring* path=nullptr) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if(!snapshot)return 0;
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Module32FirstW(snapshot.value,&entry))do{
        if(!_wcsicmp(entry.szModule,module)){if(path)*path=entry.szExePath;return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);}
    }while(Module32NextW(snapshot.value,&entry));
    return 0;
}
DWORD select_process() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(!snapshot)return 0;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);DWORD selected{};
    if(Process32FirstW(snapshot.value,&entry))do{
        if(!_wcsicmp(entry.szExeFile,L"Banana Shooter.exe")){
            if(selected){std::puts("Multiple Banana Shooter processes: supply an explicit PID.");return 0;}
            selected=entry.th32ProcessID;
        }
    }while(Process32NextW(snapshot.value,&entry));
    return selected;
}
int wmain(int argc,wchar_t** argv) {
    const bool stop_mode=argc>=2&&!_wcsicmp(argv[1],L"--stop");
    if(argc>3){std::puts("Usage: BananaLoader.exe [BananaInternalUI.dll] [PID]");return 1;}
    std::wstring dll;
    if(argc>=2&&!stop_mode){wchar_t absolute[32768]{};const DWORD n=GetFullPathNameW(argv[1],32768,absolute,nullptr);if(!n||n>=32768)return 1;dll=absolute;}
    else{wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);dll=exe;dll=dll.substr(0,dll.find_last_of(L"\\/")+1)+L"BananaInternalUI.dll";}
    const DWORD attributes=GetFileAttributesW(dll.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY)){std::puts("DLL file not found.");return 1;}
    DWORD pid{};
    if(argc==3){wchar_t* end{};const unsigned long value=std::wcstoul(argv[2],&end,10);if(!value||!end||*end){std::puts("Invalid PID.");return 1;}pid=static_cast<DWORD>(value);}
    else pid=select_process();
    if(!pid){std::puts("Start Banana Shooter first.");return 1;}
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_CREATE_THREAD|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid));
    if(!process){std::printf("OpenProcess failed (%lu).\n",GetLastError());return 1;}
    wchar_t executable[32768]{};DWORD length=32768;
    if(!QueryFullProcessImageNameW(process.value,0,executable,&length))return 1;
    const wchar_t* name=std::wcsrchr(executable,L'\\');name=name?name+1:executable;
    if(_wcsicmp(name,L"Banana Shooter.exe")){std::puts("PID is not Banana Shooter.exe.");return 1;}
    USHORT process_machine{},native_machine{};
    if(!IsWow64Process2(process.value,&process_machine,&native_machine)||process_machine!=IMAGE_FILE_MACHINE_UNKNOWN||native_machine!=IMAGE_FILE_MACHINE_AMD64){std::puts("Target must be Windows AMD64.");return 1;}
    if(!module_base(pid,L"GameAssembly.dll")){std::puts("GameAssembly.dll has not loaded yet.");return 1;}
    const auto split=dll.find_last_of(L"\\/");
    const std::wstring dll_name=dll.substr(split==std::wstring::npos?0:split+1);
    if(stop_mode){
        std::wstring loaded_path;
        const auto remote_module=module_base(pid,dll_name.c_str(),&loaded_path);
        if(!remote_module){std::puts("Updated DLL is not loaded.");return 1;}
        HMODULE image=LoadLibraryExW(loaded_path.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
        if(!image){std::puts("Cannot map DLL export table.");return 1;}
        const auto entry=GetProcAddress(image,"RequestStop");
        const auto rva=entry?reinterpret_cast<std::uintptr_t>(entry)-reinterpret_cast<std::uintptr_t>(image):0;
        FreeLibrary(image);
        if(!rva){std::puts("Stop export unavailable.");return 1;}
        Handle thread(CreateRemoteThread(process.value,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_module+rva),nullptr,0,nullptr));
        if(!thread||WaitForSingleObject(thread.value,5000)!=WAIT_OBJECT_0){std::puts("Stop request failed.");return 1;}
        std::puts("Deactivation requested. Overlay closes; game callbacks restore cursor state.");return 0;
    }
    if(module_base(pid,L"BananaInternal.dll")||module_base(pid,dll_name.c_str())){std::puts("A previous Banana Internal build is already loaded. Restart the game before loading the ImGui update.");return 1;}
    const auto local_load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    HMODULE owning_module{};
    if(!local_load||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(local_load),&owning_module))return 1;
    wchar_t owner_path[32768]{};GetModuleFileNameW(owning_module,owner_path,32768);
    const wchar_t* owner_name=std::wcsrchr(owner_path,L'\\');owner_name=owner_name?owner_name+1:owner_path;
    const auto remote_base=module_base(pid,owner_name);
    if(!remote_base){std::puts("Cannot resolve target LoadLibraryW module.");return 1;}
    const auto load_rva=reinterpret_cast<std::uintptr_t>(local_load)-reinterpret_cast<std::uintptr_t>(owning_module);
    const auto remote_load=reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_base+load_rva);
    const SIZE_T size=(dll.size()+1)*sizeof(wchar_t);
    void* remote=VirtualAllocEx(process.value,nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!remote){std::puts("Path allocation failed.");return 1;}
    SIZE_T written{};
    if(!WriteProcessMemory(process.value,remote,dll.c_str(),size,&written)||written!=size){VirtualFreeEx(process.value,remote,0,MEM_RELEASE);std::puts("Path write failed.");return 1;}
    Handle thread(CreateRemoteThread(process.value,nullptr,0,remote_load,remote,0,nullptr));
    if(!thread){VirtualFreeEx(process.value,remote,0,MEM_RELEASE);std::printf("LoadLibrary thread failed (%lu).\n",GetLastError());return 1;}
    const DWORD wait=WaitForSingleObject(thread.value,15000);
    if(wait!=WAIT_OBJECT_0){std::puts("LoadLibrary is still running or wait failed. Path allocation retained; do not retry blindly.");return 1;}
    VirtualFreeEx(process.value,remote,0,MEM_RELEASE);
    if(!module_base(pid,dll_name.c_str())){std::puts("DLL did not load. Check target permissions and DLL dependencies.");return 1;}
    std::printf("DLL loaded into PID %lu. Look for the ImGui menu; Insert toggles it. Check BananaInternalUI.log on initialization failure.\n",pid);
    return 0;
}
