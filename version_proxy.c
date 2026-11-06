#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Independent loader: no dependency on, or calls into, dinput8.dll. */
static HMODULE own;
static volatile LONG initialized;
static HMODULE real;
static BOOL initialize(void){
 WCHAR path[MAX_PATH];UINT n=GetSystemDirectoryW(path,MAX_PATH);
 if(!n || n+13>=MAX_PATH)return FALSE;
 lstrcatW(path,L"\\version.dll");real=LoadLibraryW(path);return real!=NULL;
}
static FARPROC resolve(const char *name){
 if(InterlockedCompareExchange(&initialized,1,0)==0){
  InterlockedExchange(&initialized,initialize()?2:3);
 }else while(InterlockedCompareExchange(&initialized,1,1)==1)Sleep(0);
 if(initialized!=2)return 0;
 return GetProcAddress(real,name);
}
__declspec(dllexport) BOOL WINAPI GetFileVersionInfoA(LPCSTR p,DWORD h,DWORD n,LPVOID out){
 typedef BOOL(WINAPI *Fn)(LPCSTR,DWORD,DWORD,LPVOID);
 Fn f=(Fn)resolve("GetFileVersionInfoA");return f?f(p,h,n,out):FALSE;
}
__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR p,LPDWORD h){
 typedef DWORD(WINAPI *Fn)(LPCSTR,LPDWORD);
 Fn f=(Fn)resolve("GetFileVersionInfoSizeA");return f?f(p,h):0;
}
__declspec(dllexport) BOOL WINAPI GetFileVersionInfoExW(DWORD flags,LPCWSTR p,DWORD h,DWORD n,LPVOID out){
 typedef BOOL(WINAPI *Fn)(DWORD,LPCWSTR,DWORD,DWORD,LPVOID);
 Fn f=(Fn)resolve("GetFileVersionInfoExW");return f?f(flags,p,h,n,out):FALSE;
}
__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags,LPCWSTR p,LPDWORD h){
 typedef DWORD(WINAPI *Fn)(DWORD,LPCWSTR,LPDWORD);
 Fn f=(Fn)resolve("GetFileVersionInfoSizeExW");return f?f(flags,p,h):0;
}
__declspec(dllexport) BOOL WINAPI GetFileVersionInfoExA(DWORD flags,LPCSTR p,DWORD h,DWORD n,LPVOID out){
 typedef BOOL(WINAPI *Fn)(DWORD,LPCSTR,DWORD,DWORD,LPVOID);
 Fn f=(Fn)resolve("GetFileVersionInfoExA");return f?f(flags,p,h,n,out):FALSE;
}
__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeExA(DWORD flags,LPCSTR p,LPDWORD h){
 typedef DWORD(WINAPI *Fn)(DWORD,LPCSTR,LPDWORD);
 Fn f=(Fn)resolve("GetFileVersionInfoSizeExA");return f?f(flags,p,h):0;
}
__declspec(dllexport) BOOL WINAPI GetFileVersionInfoW(LPCWSTR p,DWORD h,DWORD n,LPVOID out){
 typedef BOOL(WINAPI *Fn)(LPCWSTR,DWORD,DWORD,LPVOID);
 Fn f=(Fn)resolve("GetFileVersionInfoW");return f?f(p,h,n,out):FALSE;
}
__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR p,LPDWORD h){
 typedef DWORD(WINAPI *Fn)(LPCWSTR,LPDWORD);
 Fn f=(Fn)resolve("GetFileVersionInfoSizeW");return f?f(p,h):0;
}
__declspec(dllexport) BOOL WINAPI VerQueryValueA(LPVOID p,LPSTR s,LPVOID *out,PUINT n){
 typedef BOOL(WINAPI *Fn)(LPCVOID,LPCSTR,LPVOID*,PUINT);
 Fn f=(Fn)resolve("VerQueryValueA");return f?f(p,s,out,n):FALSE;
}
__declspec(dllexport) BOOL WINAPI VerQueryValueW(LPVOID p,LPWSTR s,LPVOID *out,PUINT n){
 typedef BOOL(WINAPI *Fn)(LPCVOID,LPCWSTR,LPVOID*,PUINT);
 Fn f=(Fn)resolve("VerQueryValueW");return f?f(p,s,out,n):FALSE;
}
static DWORD WINAPI loadAutoload(void *arg){
 WCHAR path[MAX_PATH];DWORD n=GetModuleFileNameW(own,path,MAX_PATH);DWORD i=n;
 if(!n || n>=MAX_PATH)return 1;
 while(i && path[i-1]!=L'\\')i--;
 if(!i || i+24>=MAX_PATH)return 1;
 path[i]=0;lstrcatW(path,L"RoadCraftAutoload.dll");
 return LoadLibraryW(path)?0:1;
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID reserved){
 if(reason==DLL_PROCESS_ATTACH){HANDLE t;own=h;DisableThreadLibraryCalls(h);
#ifndef PROXY_TEST
 t=CreateThread(0,0,loadAutoload,0,0,0);if(t)CloseHandle(t);
#endif
 }return TRUE;
}


