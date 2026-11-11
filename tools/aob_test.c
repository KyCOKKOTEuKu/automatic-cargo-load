/* Проверка поиска сигнатур на реальном exe игры без запуска игры.
   exe раскладывается в памяти по своему базовому адресу (0x140000000), как это делает загрузчик Windows,
   затем вызывается тот же resolveGameAddresses(), что и в моде. В мод не входит. */
#define AUTOLOAD_TEST
#include "KOTEuKA_CargoLoad.c"

int wmain(int argc,WCHAR **argv){
 HANDLE f=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
 DWORD size,got;BYTE *file;IMAGE_DOS_HEADER *dos;IMAGE_NT_HEADERS64 *nt;IMAGE_SECTION_HEADER *s;BYTE *image;int i,fail=0;DWORD t0;
 if(f==INVALID_HANDLE_VALUE){printf("cannot open exe\n");return 2;}
 size=GetFileSize(f,0);file=(BYTE*)VirtualAlloc(0,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 ReadFile(f,file,size,&got,0);CloseHandle(f);
 dos=(IMAGE_DOS_HEADER*)file;nt=(IMAGE_NT_HEADERS64*)(file+dos->e_lfanew);
 image=(BYTE*)VirtualAlloc((void*)nt->OptionalHeader.ImageBase,nt->OptionalHeader.SizeOfImage,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 if(!image){printf("cannot reserve image base\n");return 2;}
 memcpy(image,file,nt->OptionalHeader.SizeOfHeaders);
 s=IMAGE_FIRST_SECTION(nt);
 for(i=0;i<nt->FileHeader.NumberOfSections;i++)if(s[i].SizeOfRawData)memcpy(image+s[i].VirtualAddress,file+s[i].PointerToRawData,s[i].SizeOfRawData);
 base=image;
 t0=GetTickCount();
 if(!resolveGameAddresses())printf("RESOLVE FAILED: %s\n",gameAddrError);
 printf("time %lu ms\n",GetTickCount()-t0);
 for(i=0;i<GAME_ADDR_COUNT;i++){
  DWORD known=i==SLOT_KEYBOARD?KNOWN_SLOT_KEYBOARD_RVA:0;int j;
  for(j=0;j<sizeof(gameSignatures)/sizeof(gameSignatures[0]);j++)if(gameSignatures[j].id==i)known=gameSignatures[j].knownRva;
  printf("%-16s found=%08llX known=%08lX %s\n",gameAddrNames[i]?gameAddrNames[i]:"?",
   gameAddr[i]?(unsigned long long)(gameAddr[i]-(uintptr_t)image):0ull,known,gameAddr[i] && gameAddr[i]-(uintptr_t)image==known?"OK":"FAIL");
  if(!gameAddr[i] || gameAddr[i]-(uintptr_t)image!=known)fail++;
 }
 printf("signature()=%d\n",signature());
 printf("%s: %d mismatches\n",fail?"FAILED":"PASSED",fail);
 return fail?1:0;
}
