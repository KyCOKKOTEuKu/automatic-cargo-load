/* Только для проверки внешнего вида окна без игры. В мод не входит. */
#define AUTOLOAD_TEST
#include "KOTEuKA_CargoLoad.c"
#include <stdarg.h>

static void pw(const WCHAR *fmt,...){WCHAR w[512];char u[1024];va_list a;va_start(a,fmt);_vsnwprintf(w,511,fmt,a);va_end(a);w[511]=0;WideCharToMultiByte(CP_UTF8,0,w,-1,u,1024,0,0);fputs(u,stdout);}
static int assetIndex(const char *label){int i;for(i=0;i<sizeof(cargoAssets)/sizeof(cargoAssets[0]);i++)if(!strcmp(cargoAssets[i].label,label))return FIRST_ASSET_KIND+i;return -1;}
static void save(HWND h,const WCHAR *path){
 RECT r;HDC wdc,mdc;HBITMAP bmp;BITMAPFILEHEADER fh={0};BITMAPINFOHEADER ih={sizeof(ih)};HANDLE f;DWORD n,size;void *bits;
 GetWindowRect(h,&r);
 wdc=GetDC(0);mdc=CreateCompatibleDC(wdc);
 ih.biWidth=r.right-r.left;ih.biHeight=-(r.bottom-r.top);ih.biPlanes=1;ih.biBitCount=32;
 bmp=CreateDIBSection(wdc,(BITMAPINFO*)&ih,DIB_RGB_COLORS,&bits,0,0);
 SelectObject(mdc,bmp);
 PrintWindow(h,mdc,2);
 size=ih.biWidth*(r.bottom-r.top)*4;
 fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+size;
 f=CreateFileW(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);
 WriteFile(f,&fh,sizeof(fh),&n,0);WriteFile(f,&ih,sizeof(ih),&n,0);WriteFile(f,bits,size,&n,0);CloseHandle(f);
 DeleteDC(mdc);ReleaseDC(0,wdc);DeleteObject(bmp);
}
static void pump(int ms){DWORD end=GetTickCount()+ms;MSG m;while(GetTickCount()<end){while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}}
int wmain(int argc,WCHAR **argv){
 static const WCHAR *labels[]={L"Погрузить",L"Обновить",L"« Предыдущий",L"Следующий »",
  L"\x2191 Вперёд  W",L"\x2190 Влево  A",L"\x2193 Назад  S",L"\x2192 Вправо  D",
  L"Поворот \x2190  Q",L"Поворот \x2192  E",L"Выше  R",L"Ниже  F",L"Наклон \x2190  Z",L"Наклон \x2192  C",L"Наклон \x2191  T",L"Наклон \x2193  G"};
 static BYTE fakeBase[0x2cd40c0+16];
 static const BYTE sig[]={0x48,0x8b,0x89,0xd8,0,0,0,0xe9,0xd4,0xd4,3,0};
 static const struct { const char *label; float x,z,ax,az; } demo[]={
  {"Concrete_slab",-6,12,1,0},{"Log",9,8,0.7f,0.7f},{"Metal_scrap",-14,-6,0,0},{"Sellable",20,-10,0,0},
  {"Iron_pipe",-22,18,0,1},{"Steel_beam",30,25,1,0.2f},{"Concrete_scrap",4,-30,0,0},{"Concrete_pile",-35,-20,1,0}};
 WNDCLASSW wc={0};int i;
 typedef HANDLE (WINAPI *DpiContextFn)(HANDLE);
 DpiContextFn setDpiContext=(DpiContextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext");
 if(setDpiContext)setDpiContext((HANDLE)(intptr_t)-4);
 if(argc<5 || !argv[4][0])return 3; /* путь к тестовому .ini обязателен, иначе Windows пишет в win.ini */
 lstrcpynW(ini,argv[4],MAX_PATH);
 memcpy(fakeBase+0x2cd40c0,sig,sizeof(sig));base=fakeBase;gameAddrReady=1;gameAddr[FN_QUEUE]=(uintptr_t)(fakeBase+0x2cd40c0);self=GetModuleHandleW(0);
 radarCursor=LoadCursorW(0,MAKEINTRESOURCEW(32512));
 wc.lpfnWndProc=proc;wc.hInstance=self;wc.lpszClassName=L"KOTEuKA_Preview";wc.hCursor=radarCursor;wc.hbrBackground=CreateSolidBrush(COL_BG);RegisterClassW(&wc);
 window=CreateWindowExW(0,wc.lpszClassName,L"KOTEuKA_CargoLoad — F1",radarStyle,40,40,505,535,0,0,self,0);
 for(i=0;i<BUTTON_COUNT;i++)CreateWindowW(L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,1,1,window,(HMENU)(uintptr_t)buttonIds[i],self,0);
 for(i=0;i<SETTING_COUNT;i++)CreateWindowW(L"BUTTON",settingButtons[i].label,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,1,1,window,(HMENU)(uintptr_t)settingButtons[i].id,self,0);
 fitClient(window);
 /* ── Тесты клавиш ── */
 {
  /* Исходная таблица + наклон Z/C/T/G (DIK 0x2C/0x2E/0x14/0x22) перед Enter/Esc. */
  static const BYTE originalScan[14]={0x11,0x1e,0x1f,0x20,0x10,0x12,0x13,0x21,0x2c,0x2e,0x14,0x22,0x1c,0x01};
  static const BYTE originalVk[14]={'W','A','S','D','Q','E','R','F','Z','C','T','G',VK_RETURN,VK_ESCAPE};
  int k,bad=0,roundTrip=0;
  UINT vk,mods;
  WCHAR name[32],err[96];
  loadKeyBindings();applyPlacementKeys();
  for(k=0;k<14;k++)if(placementKeys[k].scan!=originalScan[k] || placementKeys[k].key!=originalVk[k]){bad++;printf("key %d: vk=%02X scan=%02X\n",k,placementKeys[k].key,placementKeys[k].scan);}
  printf("defaults = original table: %s\n",bad?"FAILED":"PASSED");
  printf("fineScan=%02X (ожидается 2A), DIK: Up=%02X Left=%02X Num5=%02X NumDiv=%02X F1=%02X Space=%02X\n",
         fineScan,dikFor(VK_UP),dikFor(VK_LEFT),dikFor(VK_NUMPAD5),dikFor(VK_DIVIDE),dikFor(VK_F1),dikFor(VK_SPACE));
  if(fineScan!=0x2a || dikFor(VK_UP)!=0xC8 || dikFor(VK_LEFT)!=0xCB || dikFor(VK_NUMPAD5)!=0x4C || dikFor(VK_DIVIDE)!=0xB5 || dikFor(VK_SPACE)!=0x39)bad++;
  for(vk=7;vk<255;vk++){UINT back;keyNameW(vk,name,32);back=parseKeyText(name,&mods);if(back!=vk){roundTrip++;if(roundTrip<5)pw(L"roundtrip %02X '%ls' -> %02X\n",vk,name,back);}}
  printf("name<->vk roundtrip mismatches: %d\n",roundTrip);
  vk=parseKeyText(L"Ctrl+Shift+F5",&mods);printf("parse Ctrl+Shift+F5 -> vk=%02X mods=%u (ожидается 74, 6)\n",vk,mods);
  if(vk!=VK_F5 || mods!=(MOD_CONTROL|MOD_SHIFT))bad++;
  if(validBinding(K_LEFT,'W',0,err,96)){bad++;printf("конфликт не пойман\n");}else pw(L"конфликт W: %ls\n",err);
  if(validBinding(K_FORWARD,VK_LWIN,0,err,96))bad++;
  if(validBinding(K_RAISE,'T',MOD_CONTROL,err,96))bad++;
  if(!validBinding(K_RADAR,'W',MOD_CONTROL,err,96)){bad++;printf("Ctrl+W для окна должно быть можно\n");}
  /* .ini: перестановка W<->A, мусор, дубликат */
  WritePrivateProfileStringW(L"Keys",L"MoveForward",L"A",ini);
  WritePrivateProfileStringW(L"Keys",L"MoveLeft",L"W",ini);
  WritePrivateProfileStringW(L"Keys",L"Raise",L"Foo",ini);
  WritePrivateProfileStringW(L"Keys",L"Lower",L"Q",ini);
  WritePrivateProfileStringW(L"Keys",L"Radar",L"Ctrl+F2",ini);
  loadKeyBindings();applyPlacementKeys();
  {WCHAR a[48],b[48],c[48],e[48],f[48],g[48];bindingTextW(K_FORWARD,a,48);bindingTextW(K_LEFT,b,48);bindingTextW(K_RAISE,c,48);bindingTextW(K_LOWER,e,48);bindingTextW(K_TURN_LEFT,f,48);bindingTextW(K_RADAR,g,48);
  pw(L"после .ini: Вперёд=%ls Влево=%ls Выше=%ls Ниже=%ls ПоворотВлево=%ls Окно=%ls, проблем=%d\n",a,b,c,e,f,g,keyLoadProblems);
   if(lstrcmpW(a,L"A")||lstrcmpW(b,L"W")||lstrcmpW(c,L"R")||lstrcmpW(e,L"F")||lstrcmpW(f,L"Q")||lstrcmpW(g,L"Ctrl+F2"))bad++;}
  printf("hint: %s\n",keyHintU8);
  /* Захват в окне «Клавиши» */
  openKeysWindow();pump(200);
  keysBeginCapture(K_RELEASE);keysCapture(VK_SPACE);
  {WCHAR a[48];bindingTextW(K_RELEASE,a,48);pw(L"захват Отпустить -> %ls (ожидается Space), ini=",a);
   GetPrivateProfileStringW(L"Keys",L"Release",L"",a,48,ini);pw(L"%ls\n",a);if(lstrcmpW(a,L"Space"))bad++;}
  keysBeginCapture(K_CANCEL);keysCapture('A');
 pw(L"захват Отмена=A (занято): статус «%ls», строка ещё ждёт=%d\n",keysStatus,keyCaptureRow==K_CANCEL);
  if(keyCaptureRow!=K_CANCEL)bad++;
  keysCapture(VK_BACK);
  {WCHAR a[48];bindingTextW(K_CANCEL,a,48);pw(L"затем Backspace -> %ls\n",a);if(lstrcmpW(a,L"Backspace"))bad++;}
  keysBeginCapture(K_RADAR);pump(300);
  if(argc>5)save(keysWindow,argv[5]);
  keysEndCapture();DestroyWindow(keysWindow);pump(100);
  printf("hotkey re-registered after capture: %d\n",registerRadarHotkey());
  printf("keys tests: %s\n",bad?"FAILED":"PASSED");
 }
 /* Состояние 1: грузовик не найден */
 detectedTruck="Ожидание карты";strcpy(message,"Сначала загрузите карту");
 /* Строка «Сеть» на снимке: режим «Хост», один друг с модом. */
 netMode=NET_HOST;peers[0].used=1;peers[0].transport=NT_STEAM;peers[0].isClient=1;peers[0].lastSeen=GetTickCount();lstrcpyA(peers[0].name,"Friend");
 placementControls();ShowWindow(window,SW_SHOW);pump(400);save(window,argv[1]);
 /* Состояние 2: найдено 8 грузов */
 memset(&truck,0,sizeof(truck));truck.pose[10]=1;truckReady=1;detectedTruck="Voron 3327 Cargo";
 for(i=0;i<8;i++){memset(&logs[i],0,sizeof(Item));logs[i].body=i+1;logs[i].kind=strcmp(demo[i].label,"Log")?assetIndex(demo[i].label):0;
  logs[i].center.x=demo[i].x;logs[i].center.z=demo[i].z;logs[i].axis.x=demo[i].ax;logs[i].axis.z=demo[i].az;}
 count=8;selected=0;autoRefresh=2;
 sprintf(message,"Найдено грузов: %d в радиусе %.0f м. Выберите груз на радаре.",count,scanRange);
 InvalidateRect(window,0,TRUE);pump(400);save(window,argv[2]);
 /* Состояние 3: размещение груза */
 placement.body=1;placement.turnSteps=-3;placement.lift=0.5f;placementControls();
 sprintf(message,"Высота %.2f м · Поворот %d° · Сдвиг %.2f / %.2f м · шаг %s · Enter — отпустить",5.5f,-15,0.30f,-0.05f,"точный");
 InvalidateRect(window,0,TRUE);pump(400);save(window,argv[3]);
 /* Проверка ползунка: клик по отметке «4» в клиентских координатах окна. */
 {
  RECT view=radarViewport(window);
  int px=view.left+MulDiv(SLIDER_LEFT+4*SLIDER_STEP,view.right-view.left,RADAR_WIDTH);
  int py=view.top+MulDiv(SLIDER_CY,view.bottom-view.top,RADAR_HEIGHT);
  placement.body=0;placementControls();
  SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px,py));
  SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(px,py));
  printf("slider click -> autoRefresh=%d (ожидается 4), ini=%d\n",autoRefresh,GetPrivateProfileIntW(L"Radar",L"AutoRefreshSec",-1,ini));
  px=view.left+MulDiv(SLIDER_LEFT-10,view.right-view.left,RADAR_WIDTH);
  SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(px,py));
  SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(px,py));
  printf("slider left  -> autoRefresh=%d (ожидается 0)\n",autoRefresh);
  /* Таймер: при 1 с радар должен сам обновиться (в тесте карты нет, сообщение сменится). */
  setAutoRefresh(1,0);strcpy(message,"маркер");
  pump(1500);
  printf("timer refresh -> message changed=%d, truckReady=%d\n",strcmp(message,"маркер")!=0,truckReady);
 }
 /* Логика раскладки: 5 брёвен шириной 0,6 м в кузов 2,4 м.
    Ожидается: 0, +0.70, -0.70, 0 (новый ярус), +0.70. */
 {
  static const float expected[]={0,0.70f,-0.70f,0,0.70f};
  static const int expectedLayer[]={0,0,0,1,1};
  int k,bad=0;
  setAutoRefresh(0,0);
  ZeroMemory(&bed,sizeof(bed));truck.body=777;bedWidth=2.4f;bedLayoutOn=1;
  for(k=0;k<5;k++){
   Placement next={0};
   const char *note;
   next.sizeForward=4.0f;next.sizeOther=0.6f;
   note=planBed(&next);
   placement=next;
   printf("груз %d: across=%+.2f newLayer=%d (%s)\n",k+1,next.across,next.newLayer,note?note:"первый");
   if(fabs(next.across-expected[k])>0.001f)bad++;
   recordBed();
   if(bed.layer!=expectedLayer[k])bad++;
  }
  /* Поворот 90°: ширина поперёк становится длиной (4 м) — в кузов 2,4 м не влезает рядом. */
  {Placement next={0};next.sizeForward=4.0f;next.sizeOther=0.6f;next.turnSteps=18;
   printf("ширина при повороте 90°: %.2f (ожидается 4.00)\n",widthAcross(&next));
   if(fabs(widthAcross(&next)-4.0f)>0.01f)bad++;}
  placement.body=0;
  printf("bed layout: %s\n",bad?"FAILED":"PASSED");
  /* Снимок окна с непустой раскладкой */
  truck.body=777;truckReady=1;count=8;selected=0;detectedTruck="Voron 3327 Cargo";autoRefresh=2;
  sprintf(message,"Груз отпущен — закрепите его в игре. В кузове уложено: %d",bed.count);
  InvalidateRect(window,0,TRUE);pump(300);save(window,argv[2]);
 }
 /* Английский интерфейс */
 uiLang=1;applyLanguageTexts();
 truck.body=777;truckReady=1;count=8;selected=0;detectedTruck="Voron 3327 Cargo";placement.body=0;placementControls();
 sprintf(message,"Found %d cargo objects within %.0f m. Pick one on the radar.",count,scanRange);
 InvalidateRect(window,0,TRUE);pump(300);
 if(argc>6)save(window,argv[6]);
 openKeysWindow();pump(300);
 if(argc>7)save(keysWindow,argv[7]);
 DestroyWindow(keysWindow);pump(100);
 {WCHAR t[96];GetWindowTextW(window,t,96);pw(L"EN: заголовок «%ls», кнопка 101 = ",t);GetWindowTextW(GetDlgItem(window,101),t,96);pw(L"«%ls», погрузка = ",t);GetWindowTextW(GetDlgItem(window,104),t,96);pw(L"«%ls»\n",t);}
 pw(L"EN hint: ");printf("%s\n",keyHintU8);
 uiLang=0;applyLanguageTexts();
 {WCHAR t[96];GetWindowTextW(GetDlgItem(window,101),t,96);pw(L"RU снова: кнопка 101 = «%ls»\n",t);}
 return 0;
}
