#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdarg.h>
#include <stdint.h>
#pragma comment(lib,"ws2_32.lib")
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int uiLang; /* язык интерфейса: 0 — русский, 1 — English */
#define TR(ru,en) (uiLang?(en):(ru))
extern int __cdecl _finite(double);

typedef struct { float x,y,z,w; } Vec;
typedef struct { uintptr_t p; int n,cap; } Array;
typedef struct { uintptr_t body; Vec center,axis; float pose[16]; int kind; } Item;
typedef struct { char label[40]; float mass,ix,iy,iz; int forward,up; } CargoProfile;
typedef struct { float x,y,z; } CargoPoint;
typedef struct {
 const char *bone,*label;
 float mass;
 int firstHull,hullCount,minHullCount,multipleBodies,forward,up;
} CargoAsset;
#include "cargo_catalog.inc"
typedef struct TruckProfileTag { const char *bone; float mass,bedZ; int hasBed; const char *label; } TruckProfile;
typedef struct { Item item; const TruckProfile *profile; } TruckTarget;
static const TruckProfile truckProfiles[]={
#include "truck_platforms.inc"
};
static const TruckProfile legacyTruck={"BoneBody_cdt",4500,0,1,"Voron 3327 Crane Cargo"};
static const TruckProfile genericTruck={0,0,0,0,"Шасси грузовика"};
static TruckTarget truckTargets[128];
static int truckCount;
static const TruckProfile *truckProfile;
static const char *detectedTruck="Ожидание карты";
enum { PROFILE_LIMIT=128, FIRST_ASSET_KIND=PROFILE_LIMIT+1, MAX_CARGO_HULLS=64 };
static CargoProfile profiles[PROFILE_LIMIT];
static int profileCount;
static WCHAR profilePath[MAX_PATH];
typedef void (__cdecl *QueueFn)(void*,void*);
typedef void* (__cdecl *AllocFn)(void*,size_t*,void*,void*);
static HINSTANCE self;
static BYTE *base;
static HWND window,gameWindow;
static PVOID volatile placementGameWindow;
static volatile LONG placementKeyboardSeen,placementKeysHeld,placementKeysPressed;
static Item logs[128],truck;
static int count,selected=-1,truckReady;
static uintptr_t selectedBody,world;
static char message[400]="Нажмите «Обновить», чтобы найти грузы рядом";
static float rear=3.5f,height=5.0f;
static const float scanRange=45.0f,loadRange=25.0f;
static float mapScale=3.4f;
typedef struct {
 uintptr_t body,shape;
 BYTE originalMotion;
 Vec originalPosition,originalRotation,originalCenter,originalAxis;
 Vec position,rotation,center;
 float forwardX,forwardZ,along,across,lift;
 int turnSteps;
 float sizeForward,sizeOther; /* длина и ширина груза в осях кузова при повороте 0 */
 int newLayer;                /* раскладка начала новый ярус */
 int tiltSteps;               /* наклон влево/вправо — вокруг продольной оси грузовика, шаги по 5° */
 int pitchSteps;              /* наклон вперёд/назад — вокруг поперечной оси грузовика, шаги по 5° */
 /* Составной груз: остальные тела, связанные с главным шарнирами Havok, и их исходные позы. */
 int partCount;
 uintptr_t parts[31];
 BYTE partMotion[31];
 Vec partPosition[31],partRotation[31];
 int mode;                   /* PM_LOCAL — свой груз, PM_REMOTE — хост двигает груз друга, PM_CLIENT — команды уходят хосту */
 int peer;                    /* для PM_REMOTE: номер друга в peers[] */
} Placement;
enum { PM_LOCAL, PM_REMOTE, PM_CLIENT };
static Placement placement;
/* Сеть (net.inc): клиент пересылает команды хосту, хост выполняет их у себя. */
static int netExecuting;      /* 1 — сейчас выполняется команда друга (на хосте) */
static int netClientActive(void);
static void netClientLoad(int kind,Vec cargo,Vec truckCenter,const struct TruckProfileTag *profile);
static void netClientNudge(int direction,int fine);
static void netClientFinish(int cancel);
static void netClientBedReset(void);
/* ───── Раскладка в кузове ─────
   Запоминаем, что уже уложено в текущий грузовик: края ряда поперёк кузова,
   смещение вдоль и поворот последнего груза. Следующий груз ставится вплотную
   с той стороны, что ближе к центру; если ряд заполнен — снова в центр,
   и груз ложится сверху (новый ярус). Ширина кузова задаётся пользователем. */
typedef struct { uintptr_t truckBody; int count,layer; float left,right,along; int turnSteps,tiltSteps,pitchSteps; } BedLayout;
static BedLayout bed;
static int bedLayoutOn=1;
static float bedWidth=2.4f;
static const float bedGap=0.10f;
static float widthAcross(const Placement *p){
 double a=p->turnSteps*(3.141592653589793/36.0);
 return (float)(fabs(cos(a))*p->sizeOther+fabs(sin(a))*p->sizeForward);
}
static void recordBed(void){
 float w=widthAcross(&placement),lo=placement.across-w/2,hi=placement.across+w/2;
 if(bed.truckBody!=truck.body){ZeroMemory(&bed,sizeof(bed));bed.truckBody=truck.body;}
 if(!bed.count || placement.newLayer){
  if(bed.count)bed.layer++;
  bed.left=lo;bed.right=hi;
 }else{
  if(lo<bed.left)bed.left=lo;
  if(hi>bed.right)bed.right=hi;
 }
 bed.count++;bed.along=placement.along;bed.turnSteps=placement.turnSteps;bed.tiltSteps=placement.tiltSteps;bed.pitchSteps=placement.pitchSteps;
}
/* Подбирает смещения для нового груза. Возвращает текст для подсказки. */
static const char *planBed(Placement *next){
 float w,half=bedWidth/2,right,left;
 int rightFits,leftFits;
 if(!bedLayoutOn || !bed.count || bed.truckBody!=truck.body)return 0;
 next->along=bed.along;next->turnSteps=bed.turnSteps;next->tiltSteps=bed.tiltSteps;next->pitchSteps=bed.pitchSteps;
 w=widthAcross(next);
 right=bed.right+bedGap+w/2;left=bed.left-bedGap-w/2;
 rightFits=right+w/2<=half+0.01f;leftFits=left-w/2>=-half-0.01f;
 if(rightFits && (!leftFits || fabs(right)<=fabs(left))){next->across=right;return TR("справа от уложенных","right of the loaded cargo");}
 if(leftFits){next->across=left;return TR("слева от уложенных","left of the loaded cargo");}
 next->across=0;next->newLayer=1;
 return TR("ряд заполнен — по центру, новым ярусом сверху","row is full — centred, new layer on top");
}
enum { RADAR_WIDTH=520, RADAR_HEIGHT=942, HEADER_BOTTOM=46, INFO_TOP=54, INFO_BOTTOM=136,
       MAP_LEFT=12, MAP_TOP=144, MAP_RIGHT=508, MAP_BOTTOM=484, MAP_CX=260, MAP_CY=314,
       SEL_TOP=492, SEL_BOTTOM=546, LEGEND_TOP=554, SETTINGS_TOP=602, SETTINGS_BOTTOM=638,
       AUTO_TOP=646, AUTO_BOTTOM=682, BED_TOP=690, BED_BOTTOM=726, NET_TOP=734, NET_BOTTOM=770,
       BUTTONS_TOP=778 };
/* Ползунок автообновления радара: 0 — вручную, 1..5 — каждые N секунд. */
enum { SLIDER_LEFT=262, SLIDER_STEP=44, SLIDER_MAX=5, SLIDER_CY=AUTO_TOP+13 };
static int autoRefresh,sliderDragging;
/* Кнопки настроек: подъём −/+, смещение назад −/+, ширина кузова −/+, раскладка вкл/выкл, сброс. */
enum { HEIGHT_DOWN=130, HEIGHT_UP, REAR_DOWN, REAR_UP, BED_NARROW, BED_WIDEN, BED_TOGGLE, BED_RESET, KEYS_BUTTON, LANG_RU, LANG_EN,
       NETB_OFF, NETB_HOST, NETB_CLIENT, NETB_SETUP };
typedef struct { int id,left,width,top,bottom; const WCHAR *label,*labelEn; } SettingButton;
static const SettingButton settingButtons[]={
 {HEIGHT_DOWN,196,30,SETTINGS_TOP+6,SETTINGS_BOTTOM-6,L"\x2212",L"\x2212"},
 {HEIGHT_UP,  232,30,SETTINGS_TOP+6,SETTINGS_BOTTOM-6,L"+",L"+"},
 {REAR_DOWN,  432,30,SETTINGS_TOP+6,SETTINGS_BOTTOM-6,L"\x2212",L"\x2212"},
 {REAR_UP,    468,30,SETTINGS_TOP+6,SETTINGS_BOTTOM-6,L"+",L"+"},
 {BED_NARROW, 262,30,BED_TOP+6,BED_BOTTOM-6,L"\x2212",L"\x2212"},
 {BED_WIDEN,  298,30,BED_TOP+6,BED_BOTTOM-6,L"+",L"+"},
 {BED_TOGGLE, 340,84,BED_TOP+6,BED_BOTTOM-6,L"Включена",L"Enabled"},
 {BED_RESET,  430,68,BED_TOP+6,BED_BOTTOM-6,L"Сброс",L"Reset"},
 {LANG_RU,    348,34,9,37,L"RU",L"RU"},
 {LANG_EN,    384,34,9,37,L"EN",L"EN"},
 {KEYS_BUTTON,426,82,9,37,L"Клавиши",L"Keys"},
 {NETB_OFF,   92,52,NET_TOP+6,NET_BOTTOM-6,L"Выкл",L"Off"},
 {NETB_HOST,  148,52,NET_TOP+6,NET_BOTTOM-6,L"Хост",L"Host"},
 {NETB_CLIENT,204,64,NET_TOP+6,NET_BOTTOM-6,L"Клиент",L"Client"},
 {NETB_SETUP, 438,60,NET_TOP+6,NET_BOTTOM-6,L"IP…",L"IP…"}
};
enum { SETTING_COUNT=sizeof(settingButtons)/sizeof(settingButtons[0]) };
enum { MOVE_FORWARD=105,MOVE_LEFT,MOVE_BACK,MOVE_RIGHT,TURN_LEFT,TURN_RIGHT,RAISE_CARGO,LOWER_CARGO,TILT_LEFT,TILT_RIGHT,PITCH_FORWARD,PITCH_BACK };
enum { HOTKEY_RADAR=1, PLACEMENT_TIMER=1, GAMEPAD_TIMER=2, CURSOR_TIMER=3, REFRESH_TIMER=4, NET_TIMER=5 };
typedef struct { BYTE key,scan; int direction; } PlacementKey;
static PlacementKey placementKeys[]={
 {'W',0x11,MOVE_FORWARD},{'A',0x1e,MOVE_LEFT},{'S',0x1f,MOVE_BACK},{'D',0x20,MOVE_RIGHT},
 {'Q',0x10,TURN_LEFT},{'E',0x12,TURN_RIGHT},{'R',0x13,RAISE_CARGO},{'F',0x21,LOWER_CARGO},
 {'Z',0x2c,TILT_LEFT},{'C',0x2e,TILT_RIGHT},{'T',0x14,PITCH_FORWARD},{'G',0x22,PITCH_BACK},
 {VK_RETURN,0x1c,0},{VK_ESCAPE,0x01,0}
};
enum { PLACEMENT_KEY_COUNT=sizeof(placementKeys)/sizeof(placementKeys[0]) };
static DWORD placementRepeat[PLACEMENT_KEY_COUNT];
static BYTE placementKeyDown[PLACEMENT_KEY_COUNT];
static int placementKeysReady;
typedef struct { WORD buttons; BYTE lt,rt; SHORT lx,ly,rx,ry; } PadInput;
typedef struct { DWORD packet; PadInput input; } PadSample;
typedef DWORD (WINAPI *PadGetState)(DWORD,PadSample*);
enum { PAD_UP=1,PAD_DOWN=2,PAD_LEFT=4,PAD_RIGHT=8,PAD_LB=0x100,PAD_RB=0x200,
       PAD_LT=0x400,PAD_RT=0x800,PAD_A=0x1000,PAD_X=0x4000,PAD_Y=0x8000,
       PAD_TILT_L=0x40,PAD_TILT_R=0x80, /* правый стик влево/вправо (на месте нажатий стиков) */
       PAD_PITCH_F=0x10,PAD_PITCH_B=0x20, /* правый стик вверх/вниз (на месте Start/Back) */
       PAD_OPEN=PAD_LB|PAD_RB|PAD_Y,PAD_DPAD=PAD_UP|PAD_DOWN|PAD_LEFT|PAD_RIGHT };
static const struct { WORD button; int direction; } padMoves[]={
 {PAD_UP,MOVE_FORWARD},{PAD_DOWN,MOVE_BACK},{PAD_LEFT,MOVE_LEFT},{PAD_RIGHT,MOVE_RIGHT},
 {PAD_LB,LOWER_CARGO},{PAD_RB,RAISE_CARGO},{PAD_LT,TURN_LEFT},{PAD_RT,TURN_RIGHT},
 {PAD_TILT_L,TILT_LEFT},{PAD_TILT_R,TILT_RIGHT},{PAD_PITCH_F,PITCH_FORWARD},{PAD_PITCH_B,PITCH_BACK}
};
typedef struct {
 WORD previous,repeating;
 int connected;
 DWORD nextRepeat[sizeof(padMoves)/sizeof(padMoves[0])];
} PadHistory;
static PadHistory pads[4];
static PadGetState readPad;
static PVOID volatile gamePadGetState;
static DWORD retryPadFilter;
static int padFilterReady;
static volatile LONG padChordHeld;
static const DWORD radarStyle=WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_CLIPCHILDREN;
static const int buttonIds[]={104,101,102,103,MOVE_FORWARD,MOVE_LEFT,MOVE_BACK,MOVE_RIGHT,
                              TURN_LEFT,TURN_RIGHT,RAISE_CARGO,LOWER_CARGO,TILT_LEFT,TILT_RIGHT,PITCH_FORWARD,PITCH_BACK};
enum { BUTTON_COUNT=sizeof(buttonIds)/sizeof(buttonIds[0]) };
static HFONT buttonFont;
static HCURSOR radarCursor;
static int radarCursorShowCalls;
static void revealRadarCursor(int setShape){
 CURSORINFO info={sizeof(info)};
 HWND hit;
 POINT point;
 if(GetForegroundWindow()!=window)return;
 if(!setShape){
  if(!GetCursorInfo(&info) || (info.flags&CURSOR_SHOWING) || !GetCursorPos(&point))return;
  hit=WindowFromPoint(point);
  if(hit!=window && !IsChild(window,hit))return;
 }
 SetCursor(radarCursor);
 if(GetCursorInfo(&info) && (info.flags&CURSOR_SHOWING))return;
 /* Balance our visibility changes when the radar gives focus back. */
 while(radarCursorShowCalls<32){
  radarCursorShowCalls++;
  if(ShowCursor(TRUE)>=0)break;
 }
}
static void releaseRadarCursor(void){
 while(radarCursorShowCalls>0){ShowCursor(FALSE);radarCursorShowCalls--;}
}
static RECT radarViewport(HWND h){
 RECT client,view;
 int width,height;
 GetClientRect(h,&client);
 width=client.right;
 height=MulDiv(width,RADAR_HEIGHT,RADAR_WIDTH);
 if(height>client.bottom){
  height=client.bottom;
  width=MulDiv(height,RADAR_WIDTH,RADAR_HEIGHT);
 }
 view.left=(client.right-width)/2;
 view.top=(client.bottom-height)/2;
 view.right=view.left+width;
 view.bottom=view.top+height;
 return view;
}
static HFONT radarFontW(int height,int weight){
 return CreateFontW(-height,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                    DEFAULT_PITCH,L"Segoe UI");
}
static void layoutButtons(HWND h){
 RECT view=radarViewport(h);
 int width=view.right-view.left,height=view.bottom-view.top,i;
 HFONT font;
 if(width<=0 || height<=0)return;
 font=radarFontW(MulDiv(15,height,RADAR_HEIGHT),FW_SEMIBOLD);
 for(i=0;i<BUTTON_COUNT;i++){
  HWND button=GetDlgItem(h,buttonIds[i]);
  int left=12+(i%4)*126;
  int top=BUTTONS_TOP+(i/4)*40;
  int x=MulDiv(left,width,RADAR_WIDTH);
  int right=MulDiv(left+118,width,RADAR_WIDTH);
  int y=MulDiv(top,height,RADAR_HEIGHT);
  int bottom=MulDiv(top+34,height,RADAR_HEIGHT);
  if(!button)continue;
  MoveWindow(button,view.left+x,view.top+y,right-x,bottom-y,FALSE);
  SendMessageW(button,WM_SETFONT,(WPARAM)(font?font:GetStockObject(DEFAULT_GUI_FONT)),FALSE);
 }
 for(i=0;i<SETTING_COUNT;i++){
  const SettingButton *s=&settingButtons[i];
  HWND button=GetDlgItem(h,s->id);
  int x=MulDiv(s->left,width,RADAR_WIDTH),right=MulDiv(s->left+s->width,width,RADAR_WIDTH);
  int y=MulDiv(s->top,height,RADAR_HEIGHT),bottom=MulDiv(s->bottom,height,RADAR_HEIGHT);
  if(button)MoveWindow(button,view.left+x,view.top+y,right-x,bottom-y,FALSE);
 }
 if(buttonFont)DeleteObject(buttonFont);
 buttonFont=font;
 RedrawWindow(h,0,0,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
}
static void fitClient(HWND h){
 typedef UINT (WINAPI *DpiFn)(HWND);
 typedef BOOL (WINAPI *AdjustFn)(LPRECT,DWORD,BOOL,DWORD,UINT);
 HMODULE user=GetModuleHandleW(L"user32.dll");DpiFn getDpi=(DpiFn)GetProcAddress(user,"GetDpiForWindow");AdjustFn adjust=(AdjustFn)GetProcAddress(user,"AdjustWindowRectExForDpi");
 RECT r={0,0,RADAR_WIDTH,RADAR_HEIGHT};
 if(adjust && getDpi)adjust(&r,radarStyle,FALSE,WS_EX_TOPMOST,getDpi(h));else AdjustWindowRectEx(&r,radarStyle,FALSE,WS_EX_TOPMOST);
 SetWindowPos(h,0,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);layoutButtons(h);
}
static WCHAR ini[MAX_PATH];
/* ───── Назначение клавиш (секция [Keys] в .ini) ─────
   K_RADAR — глобальная горячая клавиша окна (может быть с Ctrl/Alt/Shift).
   K_FORWARD..K_CANCEL — клавиши размещения груза; они же по порядку лежат в placementKeys[].
   K_FINE — клавиша точного шага (удерживать). */
enum { K_RADAR, K_FORWARD, K_LEFT, K_BACK, K_RIGHT, K_TURN_LEFT, K_TURN_RIGHT, K_RAISE, K_LOWER,
       K_TILT_LEFT, K_TILT_RIGHT, K_PITCH_FORWARD, K_PITCH_BACK, K_RELEASE, K_CANCEL, K_FINE, KEY_COUNT };
enum { PK_RELEASE=K_RELEASE-1, PK_CANCEL=K_CANCEL-1 };
typedef struct { const WCHAR *iniName,*title,*titleEn; UINT defaultVk; UINT vk,mods; } KeyBinding;
static KeyBinding keyBindings[KEY_COUNT]={
 {L"Radar",      L"Показать / скрыть окно",L"Show / hide window",      VK_F1},
 {L"MoveForward",L"Груз вперёд",L"Cargo forward",                 'W'},
 {L"MoveLeft",   L"Груз влево",L"Cargo left",                  'A'},
 {L"MoveBack",   L"Груз назад",L"Cargo back",                  'S'},
 {L"MoveRight",  L"Груз вправо",L"Cargo right",                 'D'},
 {L"TurnLeft",   L"Повернуть влево",L"Rotate left",             'Q'},
 {L"TurnRight",  L"Повернуть вправо",L"Rotate right",            'E'},
 {L"Raise",      L"Поднять выше",L"Raise",                'R'},
 {L"Lower",      L"Опустить ниже",L"Lower",               'F'},
 {L"TiltLeft",   L"Наклонить влево",L"Tilt left",           'Z'},
 {L"TiltRight",  L"Наклонить вправо",L"Tilt right",          'C'},
 {L"PitchForward",L"Наклонить вперёд",L"Tilt forward",       'T'},
 {L"PitchBack",  L"Наклонить назад",L"Tilt back",            'G'},
 {L"Release",    L"Отпустить груз",L"Release cargo",              VK_RETURN},
 {L"Cancel",     L"Отменить и вернуть груз",L"Cancel and return cargo",     VK_ESCAPE},
 {L"Fine",       L"Точный шаг (удерживать)",L"Fine step (hold)",     VK_SHIFT}
};
static const WCHAR *keyTitle(int i){return uiLang?keyBindings[i].titleEn:keyBindings[i].title;}
static BYTE fineScan;
static char keyHintU8[360];
/* Окно «Клавиши» (сам код окна — ниже, перед обработчиком окна радара). */
enum { KEYS_ROW_ID=200 };
static HWND keysWindow;
static HFONT keysFont;
static int keyCaptureRow=-1;
/* Имена клавиш: латиница/цифры как есть, остальное — короткие английские слова,
   чтобы их было удобно писать в .ini. */
static void keyNameW(UINT vk,WCHAR *out,int size){
 static const struct { UINT vk; const WCHAR *name; } names[]={
  {VK_RETURN,L"Enter"},{VK_ESCAPE,L"Esc"},{VK_SPACE,L"Space"},{VK_TAB,L"Tab"},{VK_BACK,L"Backspace"},
  {VK_SHIFT,L"Shift"},{VK_CONTROL,L"Ctrl"},{VK_MENU,L"Alt"},{VK_CAPITAL,L"CapsLock"},
  {VK_UP,L"Up"},{VK_DOWN,L"Down"},{VK_LEFT,L"Left"},{VK_RIGHT,L"Right"},
  {VK_INSERT,L"Insert"},{VK_DELETE,L"Delete"},{VK_HOME,L"Home"},{VK_END,L"End"},
  {VK_PRIOR,L"PageUp"},{VK_NEXT,L"PageDown"},{VK_PAUSE,L"Pause"},{VK_SCROLL,L"ScrollLock"},
  {VK_OEM_3,L"Tilde"},{VK_OEM_MINUS,L"Minus"},{VK_OEM_PLUS,L"Equals"},{VK_OEM_4,L"LBracket"},
  {VK_OEM_6,L"RBracket"},{VK_OEM_1,L"Semicolon"},{VK_OEM_7,L"Quote"},{VK_OEM_COMMA,L"Comma"},
  {VK_OEM_PERIOD,L"Period"},{VK_OEM_2,L"Slash"},{VK_OEM_5,L"Backslash"},
  {VK_MULTIPLY,L"NumMul"},{VK_ADD,L"NumAdd"},{VK_SUBTRACT,L"NumSub"},{VK_DIVIDE,L"NumDiv"},{VK_DECIMAL,L"NumDot"}
 };
 int i;
 if((vk>='A' && vk<='Z') || (vk>='0' && vk<='9')){out[0]=(WCHAR)vk;out[1]=0;return;}
 if(vk>=VK_F1 && vk<=VK_F24){_snwprintf(out,size,L"F%u",vk-VK_F1+1);out[size-1]=0;return;}
 if(vk>=VK_NUMPAD0 && vk<=VK_NUMPAD9){_snwprintf(out,size,L"Num%u",vk-VK_NUMPAD0);out[size-1]=0;return;}
 for(i=0;i<sizeof(names)/sizeof(names[0]);i++)if(names[i].vk==vk){lstrcpynW(out,names[i].name,size);return;}
 _snwprintf(out,size,L"0x%02X",vk);out[size-1]=0;
}
static void bindingTextW(int i,WCHAR *out,int size){
 WCHAR name[32];
 out[0]=0;
 if(keyBindings[i].mods&MOD_CONTROL)lstrcatW(out,L"Ctrl+");
 if(keyBindings[i].mods&MOD_ALT)lstrcatW(out,L"Alt+");
 if(keyBindings[i].mods&MOD_SHIFT)lstrcatW(out,L"Shift+");
 keyNameW(keyBindings[i].vk,name,32);
 if(lstrlenW(out)+lstrlenW(name)<size)lstrcatW(out,name);
}
static void bindingTextU8(int i,char *out,int size){
 WCHAR w[48];
 bindingTextW(i,w,48);
 if(!WideCharToMultiByte(CP_UTF8,0,w,-1,out,size,0,0))out[0]=0;
}
/* "Ctrl+Shift+F1" → vk, mods. Возвращает 0, если имя не распознано. */
static UINT parseKeyText(const WCHAR *text,UINT *mods){
 WCHAR token[32],name[32];
 const WCHAR *p=text;
 UINT vk;
 *mods=0;
 for(;;){
  int n=0;
  while(*p==L' ')p++;
  while(*p && *p!=L'+' && n<31)token[n++]=*p++;
  token[n]=0;
  while(n && token[n-1]==L' ')token[--n]=0;
  if(*p==L'+'){
   p++;
   if(!lstrcmpiW(token,L"Ctrl"))*mods|=MOD_CONTROL;
   else if(!lstrcmpiW(token,L"Alt"))*mods|=MOD_ALT;
   else if(!lstrcmpiW(token,L"Shift"))*mods|=MOD_SHIFT;
   else return 0;
   continue;
  }
  if(!token[0])return 0;
  if(token[0]==L'0' && (token[1]==L'x' || token[1]==L'X')){
   vk=(UINT)wcstoul(token+2,0,16);
   return vk>0 && vk<255?vk:0;
  }
  for(vk=1;vk<255;vk++){keyNameW(vk,name,32);if(!lstrcmpiW(name,token))return vk;}
  return 0;
 }
}
/* Код клавиши DirectInput, по которому мод читает и «глушит» клавишу в игре.
   Для Shift/Ctrl/Alt берётся левая клавиша, правая обрабатывается как пара. */
static BYTE dikFor(UINT vk){
 UINT sc;
 if(vk==VK_SHIFT)return 0x2a;
 if(vk==VK_CONTROL)return 0x1d;
 if(vk==VK_MENU)return 0x38;
 sc=MapVirtualKeyW(vk,4); /* MAPVK_VK_TO_VSC_EX */
 if(!sc || (sc&0xff)==0)return 0;
 /* Windows возвращает для стрелок и блока Insert/Home/… «цифровые» коды без признака E0
    (Up → 0x48 = Num8), поэтому расширенные клавиши помечаем явно. */
 switch(vk){
 case VK_UP:case VK_DOWN:case VK_LEFT:case VK_RIGHT:case VK_INSERT:case VK_DELETE:
 case VK_HOME:case VK_END:case VK_PRIOR:case VK_NEXT:case VK_DIVIDE:case VK_RCONTROL:case VK_RMENU:
  return (BYTE)((sc&0x7f)|0x80);
 }
 return (BYTE)((sc&0x7f)|(((sc>>8)&0xff)==0xe0?0x80:0));
}
static BYTE companionDik(BYTE dik){
 return dik==0x2a?0x36:dik==0x1d?0x9d:dik==0x38?0xb8:dik==0x1c?0x9c:0;
}
/* Проверка назначения. Пишет причину отказа в err (UTF-16). */
static int validBinding(int i,UINT vk,UINT mods,WCHAR *err,int size){
 int j;
 /* 0x01..0x06 — кнопки мыши и Ctrl+Break; Win и меню — системные. */
 if(vk<0x07 || vk==VK_LWIN || vk==VK_RWIN || vk==VK_APPS || vk>=255){
  lstrcpynW(err,TR(L"Эту клавишу назначить нельзя",L"This key cannot be assigned"),size);return 0;
 }
 if(i==K_RADAR){
  if(vk==VK_SHIFT || vk==VK_CONTROL || vk==VK_MENU){lstrcpynW(err,TR(L"Нужна обычная клавиша (можно вместе с Ctrl/Alt/Shift)",L"Needs a regular key (Ctrl/Alt/Shift allowed with it)"),size);return 0;}
 }else{
  BYTE dik=dikFor(vk);
  if(mods){lstrcpynW(err,TR(L"Для этого действия — одна клавиша без Ctrl/Alt/Shift",L"This action takes a single key without Ctrl/Alt/Shift"),size);return 0;}
  /* Таблица клавиш игры рассчитана на коды DirectInput до 0xED. */
  if(!dik || dik>0xED){lstrcpynW(err,TR(L"Игра не различает эту клавишу",L"The game cannot read this key"),size);return 0;}
 }
 for(j=0;j<KEY_COUNT;j++){
  if(j==i || keyBindings[j].vk!=vk)continue;
  if((j==K_RADAR?keyBindings[j].mods:0)!=(i==K_RADAR?mods:0))continue;
  _snwprintf(err,size,TR(L"Уже назначена: «%ls»",L"Already used by “%ls”"),keyTitle(j));err[size-1]=0;
  return 0;
 }
 return 1;
}
static void saveKeyBindings(void){
 WCHAR text[48];
 int i;
 for(i=0;i<KEY_COUNT;i++){bindingTextW(i,text,48);WritePrivateProfileStringW(L"Keys",keyBindings[i].iniName,text,ini);}
}
/* Читает [Keys]; неверные или повторяющиеся значения заменяются стандартными. */
static int keyLoadProblems;
static void loadKeyBindings(void){
 int i,round;
 WCHAR err[96];
 keyLoadProblems=0;
 /* 1) Берём всё из .ini (нераспознанное — стандартное). */
 for(i=0;i<KEY_COUNT;i++){
  WCHAR text[64];
  UINT vk=0,mods=0;
  GetPrivateProfileStringW(L"Keys",keyBindings[i].iniName,L"",text,64,ini);
  if(text[0] && !(vk=parseKeyText(text,&mods)))keyLoadProblems++;
  keyBindings[i].vk=vk?vk:keyBindings[i].defaultVk;
  keyBindings[i].mods=vk && i==K_RADAR?mods:0;
 }
 /* 2) Проверяем все вместе; неподходящее возвращаем к стандартному. */
 for(round=0;round<2;round++){
  int bad=0;
  for(i=0;i<KEY_COUNT;i++){
   UINT vk=keyBindings[i].vk,mods=keyBindings[i].mods;
   if(validBinding(i,vk,mods,err,96))continue;
   bad++;keyLoadProblems++;
   if(round==0){keyBindings[i].vk=keyBindings[i].defaultVk;keyBindings[i].mods=0;}
  }
  if(!bad)break;
  /* 3) Если и после этого есть конфликт — все клавиши по умолчанию. */
  if(round==1)for(i=0;i<KEY_COUNT;i++){keyBindings[i].vk=keyBindings[i].defaultVk;keyBindings[i].mods=0;}
 }
 saveKeyBindings();
}
static const WCHAR *const moveLabels[]={L"\x2191 Вперёд",L"\x2190 Влево",L"\x2193 Назад",L"\x2192 Вправо",
                                        L"Поворот \x2190",L"Поворот \x2192",L"Выше",L"Ниже",L"Наклон \x2190",L"Наклон \x2192",L"Наклон \x2191",L"Наклон \x2193"};
static const WCHAR *const moveLabelsEn[]={L"\x2191 Forward",L"\x2190 Left",L"\x2193 Back",L"\x2192 Right",
                                          L"Rotate \x2190",L"Rotate \x2192",L"Up",L"Down",L"Tilt \x2190",L"Tilt \x2192",L"Tilt \x2191",L"Tilt \x2193"};
/* Переносит назначения в таблицу размещения, подписи кнопок, заголовок и подсказки. */
static void applyPlacementKeys(void){
 char n[K_FINE+1][32];
 WCHAR label[64],key[48];
 int i;
 for(i=K_FORWARD;i<=K_CANCEL;i++){
  placementKeys[i-1].key=(BYTE)keyBindings[i].vk;
  placementKeys[i-1].scan=dikFor(keyBindings[i].vk);
 }
 fineScan=dikFor(keyBindings[K_FINE].vk);
 for(i=0;i<=K_FINE;i++)bindingTextU8(i,n[i],32);
 _snprintf(keyHintU8,sizeof(keyHintU8),TR("%s/%s/%s/%s — двигать · %s/%s — поворот · %s/%s/%s/%s — наклон · %s/%s — высота · %s — точно · %s — отпустить · %s — отмена","%s/%s/%s/%s — move · %s/%s — rotate · %s/%s/%s/%s — tilt · %s/%s — height · %s — fine · %s — release · %s — cancel"),
           n[K_FORWARD],n[K_LEFT],n[K_BACK],n[K_RIGHT],n[K_TURN_LEFT],n[K_TURN_RIGHT],n[K_TILT_LEFT],n[K_TILT_RIGHT],n[K_PITCH_FORWARD],n[K_PITCH_BACK],n[K_RAISE],n[K_LOWER],
           n[K_FINE],n[K_RELEASE],n[K_CANCEL]);
 keyHintU8[sizeof(keyHintU8)-1]=0;
 if(!window)return;
 for(i=0;i<12;i++){
  bindingTextW(K_FORWARD+i,key,48);
  _snwprintf(label,64,L"%ls  %ls",uiLang?moveLabelsEn[i]:moveLabels[i],key);label[63]=0;
  SetWindowTextW(GetDlgItem(window,MOVE_FORWARD+i),label);
 }
 bindingTextW(K_RADAR,key,48);
 _snwprintf(label,64,L"KOTEuKA_CargoLoad — %ls",key);label[63]=0;
 SetWindowTextW(window,label);
 InvalidateRect(window,0,TRUE);
}
static int registerRadarHotkey(void){
 UnregisterHotKey(window,HOTKEY_RADAR);
 return RegisterHotKey(window,HOTKEY_RADAR,keyBindings[K_RADAR].mods|0x4000 /* MOD_NOREPEAT */,keyBindings[K_RADAR].vk);
}
/* ───── Поиск адресов игры по сигнатурам (AOB) ─────
   Вместо жёстких смещений одной сборки игры адреса функций, таблиц виртуальных
   методов и глобальных переменных ищутся по шаблонам байтов в коде игры.
   Каждый шаблон обязан совпасть РОВНО один раз; иначе мод ничего не трогает.
   Шаблоны сохраняют смещения полей структур (они не маскируются), поэтому
   если игра поменяет раскладку структур, шаблон не совпадёт и мод отключится. */
enum { SIG_DIRECT, SIG_RIPREL };
enum { FN_QUEUE, FN_MOTION, FN_KEYBOARD, FN_CAMERA_CTOR,
       VT_SHAPE_WRAP, VT_SHAPE_HULL, VT_SHAPE_LIST, VT_SHAPE_CYL, VT_SHAPE_CAPS,
       VT_BODY, VT_CONSTRAINT, VT_CAMERA, G_WORLD, G_ALLOC, G_PAD_GETSTATE,
       SLOT_KEYBOARD, GAME_ADDR_COUNT };
typedef struct { int id; const char *name; int kind; const char *pattern; int dispOffset,endOffset; DWORD knownRva; } GameSignature;
#include "signatures.inc"
static uintptr_t gameAddr[GAME_ADDR_COUNT];
static const char *gameAddrNames[GAME_ADDR_COUNT];
static int gameAddrReady;
static char gameAddrError[200]="Адреса игры ещё не найдены";
static uintptr_t imageStart,imageEnd;
typedef struct { BYTE *start; size_t size; int exec; } ImageSection;
static ImageSection imageSections[32];
static int imageSectionCount;
static int loadImageSections(void){
 IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER*)base;
 IMAGE_NT_HEADERS64 *nt;
 IMAGE_SECTION_HEADER *s;
 int i;
 if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
 nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64)return 0;
 imageStart=(uintptr_t)base;imageEnd=imageStart+nt->OptionalHeader.SizeOfImage;
 s=IMAGE_FIRST_SECTION(nt);
 imageSectionCount=0;
 for(i=0;i<nt->FileHeader.NumberOfSections && imageSectionCount<32;i++){
  /* В памяти секция занимает VirtualSize (хвост .data без данных на диске заполнен нулями). */
  DWORD size=s[i].Misc.VirtualSize?s[i].Misc.VirtualSize:s[i].SizeOfRawData;
  if(!size)continue;
  imageSections[imageSectionCount].start=base+s[i].VirtualAddress;
  imageSections[imageSectionCount].size=size;
  imageSections[imageSectionCount].exec=(s[i].Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0;
  imageSectionCount++;
 }
 return imageSectionCount>0;
}
static int sectionKind(uintptr_t a){
 int i;
 for(i=0;i<imageSectionCount;i++){
  uintptr_t s=(uintptr_t)imageSections[i].start;
  if(a>=s && a<s+imageSections[i].size)return imageSections[i].exec?2:1;
 }
 return 0;
}
static int parsePattern(const char *text,BYTE *bytes,BYTE *mask,int limit){
 int n=0;
 while(*text){
  if(*text==' '){text++;continue;}
  if(n>=limit)return -1;
  if(text[0]=='?'){bytes[n]=0;mask[n]=0;text+=text[1]=='?'?2:1;n++;continue;}
  {char hex[3]={text[0],text[1],0};char *end;long v=strtol(hex,&end,16);if(end!=hex+2)return -1;bytes[n]=(BYTE)v;mask[n]=1;text+=2;n++;}
 }
 return n;
}
/* Ищет шаблон в исполняемых секциях. Возвращает число совпадений (не больше 2). */
static int findPattern(const BYTE *bytes,const BYTE *mask,int n,BYTE **found){
 static const BYTE common[]={0x00,0x48,0x8B,0x89,0xFF,0xCC,0x0F,0x24,0x4C,0x44,0x8D,0x83,0xC0,0xE8,0x01};
 int anchor=-1,i,hits=0,sec;
 for(i=0;i<n;i++)if(mask[i] && !memchr(common,bytes[i],sizeof(common))){anchor=i;break;}
 if(anchor<0)for(i=0;i<n;i++)if(mask[i]){anchor=i;break;}
 if(anchor<0)return 0;
 for(sec=0;sec<imageSectionCount;sec++){
  BYTE *p,*end;
  if(!imageSections[sec].exec || imageSections[sec].size<(size_t)n)continue;
  p=imageSections[sec].start+anchor;
  end=imageSections[sec].start+imageSections[sec].size-n+anchor+1;
  while(p<end){
   BYTE *start;
   p=(BYTE*)memchr(p,bytes[anchor],end-p);
   if(!p)break;
   start=p-anchor;
   for(i=0;i<n;i++)if(mask[i] && start[i]!=bytes[i])break;
   if(i==n){if(!hits)*found=start;if(++hits>1)return hits;}
   p++;
  }
 }
 return hits;
}
static int resolveGameAddresses(void){
 int i;
 BYTE bytes[128],mask[128];
 char line[160];
 ZeroMemory(gameAddr,sizeof(gameAddr));
 gameAddrReady=0;
 if(!loadImageSections()){strcpy(gameAddrError,TR("Не удалось прочитать заголовки exe игры","Could not read the game exe headers"));return 0;}
 for(i=0;i<sizeof(gameSignatures)/sizeof(gameSignatures[0]);i++){
  const GameSignature *g=&gameSignatures[i];
  BYTE *match=0;
  int n=parsePattern(g->pattern,bytes,mask,sizeof(bytes)),hits;
  uintptr_t target;
  gameAddrNames[g->id]=g->name;
  if(n<=0){sprintf(gameAddrError,TR("Испорчена сигнатура %s","Signature %s is malformed"),g->name);return 0;}
  hits=findPattern(bytes,mask,n,&match);
  if(hits!=1){
   sprintf(gameAddrError,TR("Сигнатура %s: %s","Signature %s: %s"),g->name,hits?TR("несколько совпадений","multiple matches"):TR("не найдена","not found"));
   return 0;
  }
  if(g->kind==SIG_DIRECT)target=(uintptr_t)match;
  else{int disp;memcpy(&disp,match+g->dispOffset,4);target=(uintptr_t)match+g->endOffset+disp;}
  if(target<imageStart || target>=imageEnd
     || sectionKind(target)!=(g->id<=FN_CAMERA_CTOR?2:1)){
   sprintf(gameAddrError,TR("Сигнатура %s указывает не туда","Signature %s points to the wrong place"),g->name);
   return 0;
  }
  /* Таблица виртуальных методов обязана начинаться с указателя на код игры. */
  if(g->id>=VT_SHAPE_WRAP && g->id<=VT_CAMERA){
   uintptr_t first;
   memcpy(&first,(void*)target,sizeof(first));
   if(sectionKind(first)!=2){sprintf(gameAddrError,TR("Сигнатура %s: это не таблица методов","Signature %s: not a method table"),g->name);return 0;}
  }
  gameAddr[g->id]=target;
 }
 /* Слот таблицы методов клавиатуры: ровно одна ссылка на функцию в данных. */
 {
  int sec,hits=0;
  uintptr_t value=gameAddr[FN_KEYBOARD];
  gameAddrNames[SLOT_KEYBOARD]="SLOT_KEYBOARD";
  for(sec=0;sec<imageSectionCount;sec++){
   uintptr_t *p,*end;
   if(imageSections[sec].exec)continue;
   p=(uintptr_t*)(((uintptr_t)imageSections[sec].start+7)&~(uintptr_t)7);
   end=(uintptr_t*)(imageSections[sec].start+imageSections[sec].size-7);
   for(;p<end;p++)if(*p==value){if(!hits)gameAddr[SLOT_KEYBOARD]=(uintptr_t)p;hits++;}
  }
  if(hits!=1){
   gameAddr[SLOT_KEYBOARD]=0;
   sprintf(gameAddrError,TR("Слот клавиатуры: %s","Keyboard slot: %s"),hits?TR("несколько совпадений","multiple matches"):TR("не найден","not found"));
   return 0;
  }
 }
 gameAddrReady=1;
 strcpy(gameAddrError,TR("Все адреса игры найдены по сигнатурам","All game addresses resolved by signatures"));
 (void)line;
 return 1;
}
/* Журнал рядом с DLL: какие адреса найдены. Помогает при обновлениях игры. */
static void writeAddressLog(const WCHAR *path){
 FILE *f=_wfopen(path,L"w");
 int i;
 if(!f)return;
 fprintf(f,TR("KOTEuKA_CargoLoad — поиск адресов по сигнатурам\n","KOTEuKA_CargoLoad — signature address scan\n"));
 fprintf(f,TR("Результат: %s\n","Result: %s\n"),gameAddrError);
 for(i=0;i<GAME_ADDR_COUNT;i++){
  DWORD known=0;int j;
  for(j=0;j<sizeof(gameSignatures)/sizeof(gameSignatures[0]);j++)if(gameSignatures[j].id==i)known=gameSignatures[j].knownRva;
  if(i==SLOT_KEYBOARD)known=KNOWN_SLOT_KEYBOARD_RVA;
  if(gameAddr[i])fprintf(f,"%-16s RVA 0x%08llX%s\n",gameAddrNames[i]?gameAddrNames[i]:"?",
                       (unsigned long long)(gameAddr[i]-imageStart),
                       gameAddr[i]-imageStart==known?"":TR("  (отличается от сборки 772884)","  (differs from build 772884)"));
  else fprintf(f,TR("%-16s не найден\n","%-16s not found\n"),gameAddrNames[i]?gameAddrNames[i]:"?");
 }
 fclose(f);
}
static int readmem(uintptr_t a,void *out,size_t n){SIZE_T got=0;return a && ReadProcessMemory(GetCurrentProcess(),(void*)a,out,n,&got) && got==n;}
static int array(uintptr_t a,uintptr_t *out,int maximum){Array v;int n;if(!readmem(a,&v,sizeof(v)))return 0;n=v.n;if(n<0 || n>maximum || n>(v.cap&0x3fffffff))return 0;if(!n)return 0;return readmem(v.p,out,n*sizeof(uintptr_t))?n:0;}
static float absf(float x){return x<0?-x:x;}
static float length(float x,float z){return sqrt(x*x+z*z);}
static void readProfiles(void){FILE *f;char line[256];CargoProfile p;profileCount=0;f=_wfopen(profilePath,L"r");if(!f)return;
 while(fgets(line,sizeof(line),f) && profileCount<PROFILE_LIMIT){
  if(sscanf(line,"%39s %f %f %f %f %d %d",p.label,&p.mass,&p.ix,&p.iy,&p.iz,&p.forward,&p.up)!=7)continue;
  if(!_finite(p.mass)||!_finite(p.ix)||!_finite(p.iy)||!_finite(p.iz)||p.mass<=0||p.ix<=0||p.iy<=0||p.iz<=0||p.forward<0||p.forward>2||p.up<0||p.up>2||p.forward==p.up)continue;
  profiles[profileCount++]=p;
 }fclose(f);
}
static int nearFloat(float x,float y){return absf(x-y)<=absf(y)*0.0002f+0.000000001f;}
static const TruckProfile *truckProfileFor(uintptr_t body,Vec inertia){
 static const char *fallbackBones[]={"BoneBody_cdt","BoneChassis_cdt","BoneChassies_cdt",
  "Bone_Chassis_cdt","BoneBodyRear_cdt","BoneTruck_cdt","BoneFrameFront_cdt","BoneFriction_cdt",
  "BoneCart_cdt","BoneDumper_cdt","BoneTrailer_cdt","Trailer_cdt"};
 uintptr_t name;
 char bone[64];
 int i;
 if(inertia.w<=0 || !readmem(body+0xb8,&name,8)
    || !readmem(name&~(uintptr_t)1,bone,sizeof(bone)))return 0;
 bone[sizeof(bone)-1]=0;
 if(!strcmp(bone,legacyTruck.bone) && absf(inertia.w-1.0f/4500)<1e-7f
    && absf(inertia.x-0.00004533034f)<1e-7f && absf(inertia.y-0.00004277429f)<1e-7f)
  return &legacyTruck;
 for(i=0;i<sizeof(truckProfiles)/sizeof(truckProfiles[0]);i++){
  const TruckProfile *candidate=&truckProfiles[i];
  if(!strcmp(bone,candidate->bone) && nearFloat(inertia.w,1/candidate->mass))return candidate;
 }
 if(inertia.w<=1.0f/500){
  for(i=0;i<sizeof(fallbackBones)/sizeof(fallbackBones[0]);i++){
   if(!strcmp(bone,fallbackBones[i]))return &genericTruck;
  }
 }
 return 0;
}
static int cargoHullMinimum(uintptr_t shape,CargoPoint *minimum){
 uintptr_t type;
 struct { float radius,padding;Vec half,center; } bounds;
 if(!readmem(shape,&type,8))return 0;
 if(type==gameAddr[VT_SHAPE_WRAP]){
  if(!readmem(shape+0x38,&shape,8) || !readmem(shape,&type,8))return 0;
 }
 if(type!=gameAddr[VT_SHAPE_HULL]
    || !readmem(shape+0x28,&bounds,sizeof(bounds)))return 0;
 minimum->x=bounds.center.x-bounds.half.x-bounds.radius;
 minimum->y=bounds.center.y-bounds.half.y-bounds.radius;
 minimum->z=bounds.center.z-bounds.half.z-bounds.radius;
 return _finite(minimum->x) && _finite(minimum->y) && _finite(minimum->z);
}
static int cargoHullMinima(uintptr_t body,CargoPoint *minima){
 uintptr_t shape,type,children[MAX_CARGO_HULLS][4];
 Array list;
 int i;
 if(!readmem(body+0x20,&shape,8) || !readmem(shape,&type,8))return 0;
 if(type!=gameAddr[VT_SHAPE_LIST])return cargoHullMinimum(shape,minima);
 if(!readmem(shape+0x38,&list,sizeof(list)) || list.n<1 || list.n>MAX_CARGO_HULLS
    || list.n>(list.cap&0x3fffffff)
    || !readmem(list.p,children,list.n*sizeof(children[0])))return 0;
 for(i=0;i<list.n;i++)if(!cargoHullMinimum(children[i][0],&minima[i]))return 0;
 return list.n;
}
/* Габариты груза в его собственных осях (м): объединение выпуклых оболочек формы.
   Используется раскладкой в кузове. Для форм других типов возвращает 0. */
static int hullBox(uintptr_t shape,float lo[3],float hi[3]){
 uintptr_t type;
 struct { float radius,padding;Vec half,center; } b;
 int i;
 if(!readmem(shape,&type,8))return 0;
 if(type==gameAddr[VT_SHAPE_WRAP] && (!readmem(shape+0x38,&shape,8) || !readmem(shape,&type,8)))return 0;
 if(type!=gameAddr[VT_SHAPE_HULL] || !readmem(shape+0x28,&b,sizeof(b)))return 0;
 for(i=0;i<3;i++){
  float c=((float*)&b.center)[i],h=((float*)&b.half)[i]+b.radius;
  if(!_finite(c) || !_finite(h) || h<0 || h>20)return 0;
  if(c-h<lo[i])lo[i]=c-h;
  if(c+h>hi[i])hi[i]=c+h;
 }
 return 1;
}
static int cargoSize(uintptr_t body,float size[3]){
 uintptr_t shape,type,children[MAX_CARGO_HULLS][4];
 float lo[3]={1e9f,1e9f,1e9f},hi[3]={-1e9f,-1e9f,-1e9f};
 Array list;
 int i;
 if(!readmem(body+0x20,&shape,8) || !readmem(shape,&type,8))return 0;
 if(type!=gameAddr[VT_SHAPE_LIST]){
  if(!hullBox(shape,lo,hi))return 0;
 }else{
  if(!readmem(shape+0x38,&list,sizeof(list)) || list.n<1 || list.n>MAX_CARGO_HULLS
     || list.n>(list.cap&0x3fffffff) || !readmem(list.p,children,list.n*sizeof(children[0])))return 0;
  for(i=0;i<list.n;i++)if(!hullBox(children[i][0],lo,hi))return 0;
 }
 for(i=0;i<3;i++){size[i]=hi[i]-lo[i];if(!(size[i]>0.02f && size[i]<30.0f))return 0;}
 return 1;
}
static float cargoGeometryError(const CargoAsset *asset,const CargoPoint *minima,int count){
 /* The live convex hull is inset for Havok's collision radius. Allow 35 mm
    for the residual from the original packed hull, and match each hull once. */
 const float tolerance=0.035f*0.035f;
 const CargoPoint *expected=cargoPackedHullMinima+asset->firstHull;
 uint64_t used=0;
 float error=0;
 int i,j;
 if(count<asset->minHullCount || count>asset->hullCount)return -1;
 for(i=0;i<count;i++){
  int best=-1;
  float closest=tolerance;
  for(j=0;j<asset->hullCount;j++){
   float dx,dy,dz,distance;
   if(used&((uint64_t)1<<j))continue;
   dx=minima[i].x-expected[j].x;dy=minima[i].y-expected[j].y;dz=minima[i].z-expected[j].z;
   distance=dx*dx+dy*dy+dz*dz;
   if(distance<closest){closest=distance;best=j;}
  }
  if(best<0)return -1;
  used|=(uint64_t)1<<best;
  if(closest>error)error=closest;
 }
 return error;
}
static int primitiveWaterPipe(uintptr_t body){
 uintptr_t shape,type,children[2][4];
 Array list;
 float cylinderRadius,capsuleRadius;
 Vec cylinder[2],capsule[2];
 int i;
 if(!readmem(body+0x20,&shape,8) || !readmem(shape,&type,8) || type!=gameAddr[VT_SHAPE_LIST]
    || !readmem(shape+0x38,&list,sizeof(list)) || list.n!=2 || (list.cap&0x3fffffff)<2
    || !readmem(list.p,children,sizeof(children)))return 0;
 for(i=0;i<2;i++){
  shape=children[i][0];
  if(!readmem(shape,&type,8))return 0;
  if(type==gameAddr[VT_SHAPE_WRAP] && !readmem(shape+0x38,&children[i][0],8))return 0;
 }
 /* The authored 3.8 m water pipe uses a cylinder and a capsule instead of mesh hulls. */
 return readmem(children[0][0],&type,8) && type==gameAddr[VT_SHAPE_CYL]
     && readmem(children[0][0]+0x30,&cylinderRadius,4) && nearFloat(cylinderRadius,0.5f)
     && readmem(children[0][0]+0x44,cylinder,sizeof(cylinder))
     && nearFloat(cylinder[0].x,-1.9f) && nearFloat(cylinder[1].x,1.9f)
     && nearFloat(cylinder[0].z,0.52f) && nearFloat(cylinder[1].z,0.52f)
     && readmem(children[1][0],&type,8) && type==gameAddr[VT_SHAPE_CAPS]
     && readmem(children[1][0]+0x28,&capsuleRadius,4) && nearFloat(capsuleRadius,0.52f)
     && readmem(children[1][0]+0x34,capsule,sizeof(capsule))
     && nearFloat(capsule[0].x,-1.4f) && nearFloat(capsule[1].x,1.4f)
     && nearFloat(capsule[0].z,0.52f) && nearFloat(capsule[1].z,0.52f);
}
static int cargoAssetKind(uintptr_t body,float inverseMass,const char *bone){
 CargoPoint minima[MAX_CARGO_HULLS];
 int i,n=0,best=-1;
 float bestError=0;
 for(i=0;i<sizeof(cargoAssets)/sizeof(cargoAssets[0]);i++){
  const CargoAsset *asset=&cargoAssets[i];
  float error;
  if(!nearFloat(inverseMass,1/asset->mass) || strcmp(bone,asset->bone))continue;
  if(!n && !(n=cargoHullMinima(body,minima)))return -1;
  error=cargoGeometryError(asset,minima,n);
  if(error<0)continue;
  if(best>=0 && (strcmp(asset->label,cargoAssets[best].label)
                || asset->multipleBodies!=cargoAssets[best].multipleBodies))return -1;
  if(best<0 || error<bestError){best=i;bestError=error;}
 }
 return best<0?-1:FIRST_ASSET_KIND+best;
}
static int cargoKind(uintptr_t body,Vec inertia,uintptr_t userdata){uintptr_t name;char bone[64];int i;
 /* Some catalogued props use different body tags depending on how they spawn.
    Require the asset's shape for these tags and keep legacy profiles scoped to tag 5. */
 if(userdata<5 || userdata>11)return -1;
 if(userdata==5 && absf(inertia.w-1.0f/1100)<0.0000001f && inertia.x>0 && inertia.z>0 && inertia.y>50*inertia.x && inertia.y>50*inertia.z)return 0;
 if(!readmem(body+0xb8,&name,8)||!readmem(name&~(uintptr_t)1,bone,16))return -1;
 if(userdata==5 && !memcmp(bone,"geo_body\0",9)){
  for(i=0;i<profileCount;i++){CargoProfile *p=&profiles[i];if(nearFloat(inertia.w,1/p->mass)&&nearFloat(inertia.x,p->ix)&&nearFloat(inertia.y,p->iy)&&nearFloat(inertia.z,p->iz))return i+1;}
  if(nearFloat(inertia.w,1/cargoAssets[PRIMITIVE_WATER_PIPE_ASSET].mass) && primitiveWaterPipe(body))
   return FIRST_ASSET_KIND+PRIMITIVE_WATER_PIPE_ASSET;
 }
 if(!memchr(bone,0,16)){
  if(!readmem((name&~(uintptr_t)1)+16,bone+16,sizeof(bone)-16))return -1;
  bone[sizeof(bone)-1]=0;
 }
 return cargoAssetKind(body,inertia.w,bone);
}
static const char *cargoLabel(int kind){
 if(kind>=FIRST_ASSET_KIND)return cargoAssets[kind-FIRST_ASSET_KIND].label;
 return kind==0?"Log":kind>0 && kind<=profileCount?profiles[kind-1].label:"Unknown";
}
static int cargoForward(int kind,Vec inertia){
 if(kind>=FIRST_ASSET_KIND){
  int forward=cargoAssets[kind-FIRST_ASSET_KIND].forward;
  return forward>=0?forward:inertia.x>inertia.z?0:2;
 }
 return kind?profiles[kind-1].forward:1;
}
static int pose(uintptr_t body,Item *out){BYTE b[0x230];uintptr_t vt,owner;Vec inertia;int i;
 if(!readmem(body,b,sizeof(b)))return 0;memcpy(&vt,b,8);memcpy(&owner,b+0x18,8);
 if(vt!=gameAddr[VT_BODY] || owner!=world)return 0;
 out->body=body;memcpy(out->pose,b+0x170,64);memcpy(&out->center,b+0x1c0,16);
 out->axis.x=out->pose[4];out->axis.y=out->pose[5];out->axis.z=out->pose[6];
 for(i=0;i<3;i++)if(!_finite(((float*)&out->center)[i]) || absf(((float*)&out->center)[i])>100000)return 0;
 return 1;
}
/* Версия подходит, если все адреса найдены по сигнатурам и функция очереди физики
   по-прежнему берёт мир из поля +0xD8 (байты rel32 перехода не сравниваются). */
static int signature(void){static const BYTE expected[]={0x48,0x8b,0x89,0xd8,0,0,0,0xe9};BYTE actual[8];return gameAddrReady && readmem(gameAddr[FN_QUEUE],actual,8) && !memcmp(expected,actual,8);}
static uintptr_t vehicleCamera;
static DWORD cameraSearchTime;
static int cameraSearchPending;
static int readVehicleFocus(uintptr_t camera,Vec *focus){
 uintptr_t vtable,tracker;
 BYTE followMode,flags;
 return readmem(camera,&vtable,8) && vtable==gameAddr[VT_CAMERA]
        /* Other camera modes can retain a tracker and stale truck coordinates. */
        && readmem(camera+0x28,&followMode,1) && followMode==1
        && readmem(camera+0x30,&tracker,8) && tracker
        && readmem(tracker+8,&flags,1) && !(flags&4)
        && readmem(camera+0x128,focus,12)
        && _finite(focus->x) && _finite(focus->y) && _finite(focus->z);
}
static DWORD WINAPI findVehicleCamera(void *arg){
 typedef BOOL (WINAPI *QueryWorkingSetFn)(HANDLE,void*,DWORD);
 QueryWorkingSetFn query=(QueryWorkingSetFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32QueryWorkingSet");
 DWORD capacity=8*1024*1024;
 uintptr_t *pages=0,found=0;
 uintptr_t i;
 (void)arg;
 if(!query)goto done;
 while(capacity<=64*1024*1024){
  uintptr_t *larger=(uintptr_t*)realloc(pages,capacity);
  if(!larger)goto done;
  pages=larger;
  if(query(GetCurrentProcess(),pages,capacity))break;
  if(GetLastError()!=ERROR_BAD_LENGTH)goto done;
  capacity*=2;
 }
 if(capacity>64*1024*1024 || pages[0]>=capacity/sizeof(*pages))goto done;
 /* Camera allocations start on a page. Reading only resident page headers
    avoids copying whole heaps and works across differently sized regions. */
 for(i=1;i<=pages[0];i++){
  uintptr_t entry=pages[i],page=entry&~(uintptr_t)0xfff;
  Vec focus;
  if((entry&0x100) || !readVehicleFocus(page,&focus))continue;
  if(found && found!=page){found=0;goto done;}
  found=page;
 }
done:
 free(pages);
 PostMessageW(window,WM_APP+2,(WPARAM)found,0);
 return 0;
}
static int vehicleFocus(Vec *focus){
 static const BYTE constructor[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18};
 BYTE actual[sizeof(constructor)];
 HANDLE worker;
 DWORD now=GetTickCount();
 if(readVehicleFocus(vehicleCamera,focus))return 1;
 if(cameraSearchPending || (cameraSearchTime && now-cameraSearchTime<2000))return 0;
 cameraSearchTime=now;
 if(!readmem(gameAddr[FN_CAMERA_CTOR],actual,sizeof(actual))
    || memcmp(actual,constructor,sizeof(actual)))return 0;
 worker=CreateThread(0,0,findVehicleCamera,0,0,0);
 if(worker){
  cameraSearchPending=1;
  CloseHandle(worker);
 }
 return 0;
}
static int truckGroup(int *parents,int i){
 while(parents[i]!=i)i=parents[i];
 return i;
}
static int automaticTruckIndex(void){
 Vec focus;
 float distance[128],closest=12.0f;
 int parents[128],best=-1,i,j,k,group,bed=-1;
 if(!truckCount){detectedTruck=TR("Грузовик не найден","No truck found");return -1;}
 if(!vehicleFocus(&focus)){
  detectedTruck=cameraSearchPending?TR("Поиск камеры…","Searching for camera…"):TR("Камера недоступна","Camera unavailable");
  return -1;
 }
 for(i=0;i<truckCount;i++){
  const Item *item=&truckTargets[i].item;
  float dx=item->center.x-focus.x,dy=item->center.y-focus.y,dz=item->center.z-focus.z;
  float ox=item->pose[12]-focus.x,oy=item->pose[13]-focus.y,oz=item->pose[14]-focus.z;
  float center=dx*dx+dy*dy+dz*dz,origin=ox*ox+oy*oy+oz*oz;
  distance[i]=(float)sqrt(center<origin?center:origin);
  parents[i]=i;
  if(distance[i]<closest){closest=distance[i];best=i;}
 }
 if(best<0){detectedTruck=TR("Рядом с камерой нет грузовика","No truck near the camera target");return -1;}
 /* A trailer is part of this target only if linked by a live joint. */
 for(i=0;i<truckCount;i++){
  uintptr_t links[64],body=truckTargets[i].item.body;
  int n;
  if(distance[i]>30.0f)continue;
  n=array(body+0x110,links,64);
  for(j=0;j<n;j++){
   uintptr_t data[8],other;
   if(!readmem(links[j],data,sizeof(data)) || data[0]!=gameAddr[VT_CONSTRAINT])continue;
   if(data[6]==body)other=data[7];
   else if(data[7]==body)other=data[6];
   else continue;
   for(k=0;k<truckCount;k++){
    if(distance[k]<=30.0f && truckTargets[k].item.body==other){
     parents[truckGroup(parents,k)]=truckGroup(parents,i);
     break;
    }
   }
  }
 }
 group=truckGroup(parents,best);
 for(i=0;i<truckCount;i++){
  if(truckGroup(parents,i)!=group){
   if(distance[i]<closest+0.75f){detectedTruck=TR("Неясно: грузовики слишком близко","Unclear: trucks too close together");return -1;}
  }else if(truckTargets[i].profile->hasBed && (bed<0 || distance[i]<distance[bed])){
   bed=i;
  }
 }
 detectedTruck=truckTargets[bed>=0?bed:best].profile->label;
 return bed>=0?bed:best;
}
static float cargoDistanceSquared(const Item *item){
 float dx=item->center.x-truck.center.x,dz=item->center.z-truck.center.z;
 return dx*dx+dz*dz;
}
static int compareCargoDistance(const void *left,const void *right){
 const Item *a=left,*b=right;
 float da=cargoDistanceSquared(a),db=cargoDistanceSquared(b);
 if(da<db)return -1;
 if(da>db)return 1;
 return (a->body>b->body)-(a->body<b->body);
}
static void orderCargo(void){
 int i;
 qsort(logs,count,sizeof(logs[0]),compareCargoDistance);
 selected=-1;
 for(i=0;i<count;i++)if(logs[i].body==selectedBody){selected=i;break;}
 if(selected<0 && count){selected=0;selectedBody=logs[0].body;}
 if(!count)selectedBody=0;
}
static void refresh(void){
 uintptr_t islands[4096],bodies[4096],previousWorld=world;
 int ni,i,j,k,bn,pass;
 if(placement.body && placement.mode!=PM_REMOTE)return;
 count=0;selected=-1;truckReady=0;truckCount=0;
 readProfiles();
 if(!signature()){detectedTruck=TR("Недоступно","Unavailable");strcpy(message,gameAddrReady?TR("Неподдерживаемая версия игры","Unsupported game version"):gameAddrError);return;}
 if(!readmem(gameAddr[G_WORLD],&world,8) || !world){detectedTruck=TR("Ожидание карты","Waiting for map");strcpy(message,TR("Сначала загрузите карту","Load a map first"));return;}
 if(world!=previousWorld)selectedBody=0;
 ni=array(world+0x40,islands,2048);ni+=array(world+0x50,islands+ni,2048);
 /* Find the truck first, then inspect only nearby cargo. A large map must not
    exhaust a global candidate buffer before the local objects are visited. */
 for(pass=0;pass<2;pass++){
 for(i=0;i<ni;i++){bn=array(islands[i]+0x68,bodies,4096);for(j=0;j<bn;j++){
  Vec inertia;Item item;uintptr_t userdata;if(!pose(bodies[j],&item))continue;
  if(pass && length(item.center.x-truck.center.x,item.center.z-truck.center.z)>scanRange)continue;
  if(!readmem(bodies[j]+0x220,&inertia,16) || !readmem(bodies[j]+0x10,&userdata,8))continue;
  if(!pass){
   const TruckProfile *profile=truckProfileFor(item.body,inertia);
   if(profile && truckCount<128){
    for(k=0;k<truckCount;k++)if(truckTargets[k].item.body==item.body)break;
    if(k==truckCount){
     truckTargets[truckCount].item=item;
     truckTargets[truckCount++].profile=profile;
    }
   }
   continue;
  }
  item.kind=cargoKind(bodies[j],inertia,userdata);
  if(item.kind>=0 && count<128){
   int axis=cargoForward(item.kind,inertia);item.axis.x=item.pose[axis*4];item.axis.y=item.pose[axis*4+1];item.axis.z=item.pose[axis*4+2];
   for(k=0;k<count;k++)if(logs[k].body==item.body)break;if(k==count)logs[count++]=item;
  }
 }}
 if(!pass){
  int detected=automaticTruckIndex();
  if(detected<0){
   if(cameraSearchPending)strcpy(message,TR("Определяю текущий грузовик…","Finding the current truck…"));
   else strcpy(message,truckCount?TR("Не удалось определить грузовик. Нажмите «Обновить».","Could not identify the truck. Press “Refresh”."):TR("Грузовик или прицеп не найден.","No truck or trailer found."));
   return;
  }
  truck=truckTargets[detected].item;
  truckProfile=truckTargets[detected].profile;
  truckReady=1;
 }
 }
 orderCargo();
 sprintf(message,TR("Найдено грузов: %d в радиусе %.0f м. Выберите груз на радаре.","Found %d cargo objects within %.0f m. Pick one on the radar."),count,scanRange);
}
static void cycle(int step){if(!count || placement.body)return;selected=(selected+step+count)%count;selectedBody=logs[selected].body;InvalidateRect(window,0,TRUE);}
static int cargoLoadingPose(Item *t,const TruckProfile *profile,Vec com,int forward,int up,int upSign,Vec *position,Vec *rotation,Vec *center){float fx=t->pose[8],fz=t->pose[10],n=length(fx,fz),m[3][3],cols[3][3]={{0}},q[4]={0},trace,s;int i,j,k,other;
 if(!_finite(n) || n<0.5f)return 0;fx/=n;fz/=n;
 if(forward<0||forward>2||up<0||up>2||forward==up||(upSign!=1 && upSign!=-1))return 0;
 cols[forward][0]=fx;cols[forward][2]=fz;cols[up][1]=upSign;
 other=3-forward-up;j=(other+1)%3;k=(other+2)%3;
 for(i=0;i<3;i++)cols[other][i]=cols[j][(i+1)%3]*cols[k][(i+2)%3]-cols[j][(i+2)%3]*cols[k][(i+1)%3];
 for(i=0;i<3;i++)for(j=0;j<3;j++)m[i][j]=cols[j][i];trace=m[0][0]+m[1][1]+m[2][2];
 if(trace>0){s=2*sqrt(1+trace);q[0]=(m[2][1]-m[1][2])/s;q[1]=(m[0][2]-m[2][0])/s;q[2]=(m[1][0]-m[0][1])/s;q[3]=s/4;}
 else{i=0;if(m[1][1]>m[i][i])i=1;if(m[2][2]>m[i][i])i=2;j=(i+1)%3;k=(i+2)%3;s=2*sqrt(1+m[i][i]-m[j][j]-m[k][k]);q[i]=s/4;q[j]=(m[j][i]+m[i][j])/s;q[k]=(m[k][i]+m[i][k])/s;q[3]=(m[k][j]-m[j][k])/s;}
 memcpy(rotation,q,16);
 for(i=0;i<3;i++){
  float target=t->pose[12+i]+profile->bedZ*t->pose[8+i]+(i==1?height:0);
  /* Keep the calibrated target of the original crane truck. */
  if(profile==&legacyTruck)
   target+=-t->pose[4+i]+1.665f*t->pose[8+i]-rear*(i==0?fx:i==2?fz:0);
  if(!_finite(target) || !_finite(((float*)&com)[i]))return 0;
  if(center)((float*)center)[i]=target;
  ((float*)position)[i]=target-(cols[0][i]*com.x+cols[1][i]*com.y+cols[2][i]*com.z);
 }
 position->w=0;
 return 1;
}
static int motionQueueSupported(void){
 static const BYTE expected[]={0x40,0x53,0x48,0x83,0xec,0x40,0x48,0x8b,0xd9};
 BYTE actual[sizeof(expected)];
 return signature() && readmem(gameAddr[FN_MOTION],actual,sizeof(actual))
        && !memcmp(expected,actual,sizeof(actual));
}
static int currentWorld(void){
 uintptr_t activeWorld;
 return world && motionQueueSupported()
        && readmem(gameAddr[G_WORLD],&activeWorld,8) && activeWorld==world;
}
static void submitBodyOperation(BYTE op[64],uintptr_t body){
 QueueFn queue=(QueueFn)gameAddr[FN_QUEUE];
 memcpy(op+8,&body,8);
 queue((void*)world,op);
}
static void changeMotion(uintptr_t body,BYTE motion){
 /* Havok queue operation 4 takes motion type, activation and filter mode. */
 BYTE op[64]={4};
 op[16]=motion;
 submitBodyOperation(op,body);
}
static void stopMotion(uintptr_t body){
 /* Operations 26 and 27 set linear and angular velocity respectively. */
 BYTE op[64]={26};
 submitBodyOperation(op,body);
 op[0]=27;
 submitBodyOperation(op,body);
}
static void *allocatePose(Vec p,Vec q){
 size_t size=32;
 AllocFn alloc;
 void *pair;
 if(!readmem(gameAddr[G_ALLOC],&alloc,8) || !alloc)return 0;
 pair=alloc(0,&size,0,0);
 if(pair){
  memcpy(pair,&p,16);
  memcpy((BYTE*)pair+16,&q,16);
 }
 return pair;
}
static void submitPose(uintptr_t body,void *pair){
 BYTE op[64]={25};
 memcpy(op+16,&pair,8);
 submitBodyOperation(op,body);
}
static void placementControls(void){
 /* Пока хост двигает груз друга, свои кнопки размещения у хоста неактивны. */
 int i,remote=placement.body && placement.mode==PM_REMOTE,active=placement.body && !remote;
 SetWindowTextW(GetDlgItem(window,104),remote?TR(L"Друг грузит…",L"Friend loading…"):active?TR(L"Отпустить",L"Release"):TR(L"Погрузить",L"Load"));
 EnableWindow(GetDlgItem(window,104),!remote);
 for(i=1;i<BUTTON_COUNT;i++)EnableWindow(GetDlgItem(window,buttonIds[i]),i<4?!active && !remote:active);
 for(i=0;i<BUTTON_COUNT;i++)InvalidateRect(GetDlgItem(window,buttonIds[i]),0,FALSE);
 for(i=0;i<SETTING_COUNT;i++)EnableWindow(GetDlgItem(window,settingButtons[i].id),!active);
 SetWindowTextW(GetDlgItem(window,BED_TOGGLE),bedLayoutOn?TR(L"Включена",L"Enabled"):TR(L"Выключена",L"Disabled"));
 InvalidateRect(window,0,TRUE);
}
static void saveIniInt(const WCHAR *section,const WCHAR *key,int value){
 WCHAR text[16];
 wsprintfW(text,L"%d",value);
 WritePrivateProfileStringW(section,key,text,ini);
}
/* Кнопки −/+ в строке настроек: шаг 0,5 м, пределы те же, что проверяются при чтении .ini. */
static void changeSetting(int id){
 if(placement.body)return;
 switch(id){
 case BED_NARROW:case BED_WIDEN:
  bedWidth+=id==BED_WIDEN?0.2f:-0.2f;
  if(bedWidth<1.0f)bedWidth=1.0f;
  if(bedWidth>4.0f)bedWidth=4.0f;
  saveIniInt(L"Loading",L"BedWidthCm",(int)(bedWidth*100+0.5f));
  sprintf(message,TR("Память позиции: ширина кузова %.1f м","Position memory: bed width %.1f m"),bedWidth);
  InvalidateRect(window,0,TRUE);
  return;
 case BED_TOGGLE:
  bedLayoutOn=!bedLayoutOn;
  saveIniInt(L"Loading",L"BedLayout",bedLayoutOn);
  strcpy(message,bedLayoutOn?TR("Память позиции включена: следующий груз встанет рядом с прошлым","Position memory on: next cargo goes next to the previous one")
                            :TR("Память позиции выключена: каждый груз ставится в одну точку","Position memory off: every cargo goes to the same spot"));
  placementControls();
  return;
 case BED_RESET:
  ZeroMemory(&bed,sizeof(bed));
  if(netClientActive())netClientBedReset();
  strcpy(message,TR("Память позиции сброшена: следующий груз — первый в кузове","Position memory reset: next cargo is the first in the bed"));
  InvalidateRect(window,0,TRUE);
  return;
 case HEIGHT_DOWN:if(height>1.0f+0.01f)height-=0.5f;break;
 case HEIGHT_UP:if(height<10.0f-0.01f)height+=0.5f;break;
 case REAR_DOWN:if(rear>0.0f+0.01f)rear-=0.5f;break;
 case REAR_UP:if(rear<10.0f-0.01f)rear+=0.5f;break;
 default:return;
 }
 if(height<1)height=1;if(height>10)height=10;if(rear<0)rear=0;if(rear>10)rear=10;
 saveIniInt(L"Loading",L"DropHeightCm",(int)(height*100+0.5f));
 saveIniInt(L"Loading",L"RearOffsetCm",(int)(rear*100+0.5f));
 sprintf(message,TR("Сохранено: подъём %.1f м, смещение назад %.1f м (для следующей погрузки)","Saved: lift %.1f m, rear offset %.1f m (applies to the next load)"),height,rear);
 InvalidateRect(window,0,TRUE);
}
/* Положение и размер окна запоминаются в .ini. */
/* Автообновление радара: таймер работает, пока значение > 0; сам опрос — только при открытом окне. */
static void applyAutoRefresh(void){
 KillTimer(window,REFRESH_TIMER);
 if(autoRefresh>0)SetTimer(window,REFRESH_TIMER,autoRefresh*1000,0);
}
static void setAutoRefresh(int value,int save){
 if(value<0)value=0;
 if(value>SLIDER_MAX)value=SLIDER_MAX;
 if(value==autoRefresh && !save)return;
 autoRefresh=value;
 applyAutoRefresh();
 if(save)saveIniInt(L"Radar",L"AutoRefreshSec",autoRefresh);
 InvalidateRect(window,0,FALSE);
}
/* Переводит координаты мыши в логические координаты макета окна (520 × RADAR_HEIGHT). */
static int logicalPoint(HWND h,LPARAM l,int *mx,int *my){
 RECT view=radarViewport(h);
 int width=view.right-view.left,height=view.bottom-view.top;
 if(width<=0 || height<=0)return 0;
 *mx=MulDiv((short)LOWORD(l)-view.left,RADAR_WIDTH,width);
 *my=MulDiv((short)HIWORD(l)-view.top,RADAR_HEIGHT,height);
 return 1;
}
static int sliderValueAt(int mx){
 int v=(mx-SLIDER_LEFT+SLIDER_STEP/2)/SLIDER_STEP;
 if(mx<SLIDER_LEFT)v=0;
 return v<0?0:v>SLIDER_MAX?SLIDER_MAX:v;
}
static void saveWindowPlacement(void){
 RECT r;
 if(!IsWindowVisible(window) || IsIconic(window) || !GetWindowRect(window,&r))return;
 saveIniInt(L"Window",L"X",r.left);saveIniInt(L"Window",L"Y",r.top);
 saveIniInt(L"Window",L"Width",r.right-r.left);saveIniInt(L"Window",L"Height",r.bottom-r.top);
 saveIniInt(L"Window",L"Layout",RADAR_HEIGHT);
}
static void restoreWindowPlacement(void){
 int x=GetPrivateProfileIntW(L"Window",L"X",-100000,ini),y=GetPrivateProfileIntW(L"Window",L"Y",-100000,ini);
 int w=GetPrivateProfileIntW(L"Window",L"Width",0,ini),h=GetPrivateProfileIntW(L"Window",L"Height",0,ini);
 RECT r;
 int sameLayout=GetPrivateProfileIntW(L"Window",L"Layout",0,ini)==RADAR_HEIGHT;
 if(x==-100000 || y==-100000 || w<200 || h<200)return;
 SetRect(&r,x,y,x+w,y+h);
 /* Не ставим окно туда, где сейчас нет монитора (например, отключили второй экран). */
 if(!MonitorFromRect(&r,MONITOR_DEFAULTTONULL))return;
 /* Размер берём, только если он сохранён для этого же макета окна. */
 SetWindowPos(window,0,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE|(sameLayout?0:SWP_NOSIZE));
}
static int placementDirection(DWORD key){
 int i;
 for(i=0;i<PLACEMENT_KEY_COUNT;i++)if(placementKeys[i].key==key)return placementKeys[i].direction;
 return 0;
}
typedef void (__cdecl *GameKeyboardUpdate)(void*);
static GameKeyboardUpdate originalKeyboardUpdate;
static void __cdecl filterKeyboard(void *keyboard){
 float *state=(float*)((BYTE*)keyboard+0x10);
 HWND target;
 LONG held=0,previous;
 int i;
 originalKeyboardUpdate(keyboard);
 target=(HWND)InterlockedCompareExchangePointer(&placementGameWindow,0,0);
 if(!target || GetForegroundWindow()!=target)return;
 /* The game combines immediate and buffered DirectInput events into these axes. */
 for(i=0;i<PLACEMENT_KEY_COUNT;i++){
  BYTE scan=placementKeys[i].scan,pair=companionDik(scan);
  if(!scan)continue;
  if(state[scan]>0 || (pair && state[pair]>0))held|=1L<<i;
  state[scan]=0;
  if(pair)state[pair]=0; /* правый Shift/Ctrl/Alt, Enter на цифровом блоке */
 }
 /* Клавиша точного шага — мода; игре во время размещения не передаём. */
 if(fineScan){BYTE pair=companionDik(fineScan);state[fineScan]=0;if(pair)state[pair]=0;}
 previous=InterlockedExchange(&placementKeysHeld,held);
 InterlockedOr(&placementKeysPressed,held & ~previous);
 InterlockedExchange(&placementKeyboardSeen,1);
}
static int installKeyboardFilter(void){
 static const BYTE expected[]={0x40,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xf9,0x0f,0x29,0x74,0x24,0x30,0x48,0x8b,0x89,0x80,0x11,0,0};
 BYTE actual[sizeof(expected)];
 GameKeyboardUpdate *slot=(GameKeyboardUpdate*)gameAddr[SLOT_KEYBOARD];
 GameKeyboardUpdate update=(GameKeyboardUpdate)gameAddr[FN_KEYBOARD],current;
 DWORD protection,ignored;
 if(!readmem((uintptr_t)slot,&current,sizeof(current)))return 0;
 if(current==filterKeyboard)return 1;
 if(current!=update || !readmem((uintptr_t)update,actual,sizeof(actual))
    || memcmp(expected,actual,sizeof(actual)))return 0;
 if(!VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&protection))return 0;
 originalKeyboardUpdate=update;
 current=(GameKeyboardUpdate)InterlockedCompareExchangePointer((PVOID volatile*)slot,filterKeyboard,update);
 VirtualProtect(slot,sizeof(*slot),protection,&ignored);
 return current==update;
}
static void removeStickDrift(SHORT *x,SHORT *y,float deadzone){
 float horizontal=*x,vertical=*y;
 if(horizontal*horizontal+vertical*vertical<deadzone*deadzone)*x=*y=0;
}
static DWORD WINAPI filterGamepad(DWORD index,PadSample *sample){
 PadGetState original=(PadGetState)InterlockedCompareExchangePointer(&gamePadGetState,0,0);
 DWORD result=original(index,sample);
 HWND foreground,target;
 DWORD pid=0;
 WORD buttons;
 LONG bit;
 if(result!=ERROR_SUCCESS || index>=4)return result;
 foreground=GetForegroundWindow();
 GetWindowThreadProcessId(foreground,&pid);
 if(pid!=GetCurrentProcessId())return result;
 target=(HWND)InterlockedCompareExchangePointer(&placementGameWindow,0,0);
 buttons=sample->input.buttons;
 bit=1L<<index;
 if(!(buttons&PAD_Y))InterlockedAnd(&padChordHeld,~bit);
 if((buttons&PAD_OPEN)==PAD_OPEN)InterlockedOr(&padChordHeld,bit);
 if(InterlockedCompareExchange(&padChordHeld,0,0)&bit)sample->input.buttons&=~PAD_OPEN;
 if(foreground==window){
  ZeroMemory(&sample->input,sizeof(sample->input));
  return result;
 }
 removeStickDrift(&sample->input.lx,&sample->input.ly,5000.0f);
 removeStickDrift(&sample->input.rx,&sample->input.ry,6500.0f);
 if(foreground==target){
  sample->input.buttons&=~(PAD_DPAD|PAD_LB|PAD_RB|PAD_A|PAD_X);
  sample->input.lt=sample->input.rt=0;
  sample->input.rx=sample->input.ry=0; /* правый стик — наклон груза */
 }
 return result;
}
static int installGamepadFilter(void){
 PadGetState *slot=(PadGetState*)gameAddr[G_PAD_GETSTATE],current;
 MEMORY_BASIC_INFORMATION region;
 DWORD protection,ignored;
 if(!signature() || !readmem((uintptr_t)slot,&current,sizeof(current)))return 0;
 if(current==filterGamepad)return 1;
 if(!current || !VirtualQuery(current,&region,sizeof(region)) || region.State!=MEM_COMMIT
    || (region.Protect&PAGE_GUARD) || !(region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))
    || !VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&protection))return 0;
 /* Keep the game's original target, including an input wrapper installed by the overlay. */
 InterlockedExchangePointer(&gamePadGetState,current);
 current=(PadGetState)InterlockedCompareExchangePointer((PVOID volatile*)slot,filterGamepad,current);
 VirtualProtect(slot,sizeof(*slot),protection,&ignored);
 return current==(PadGetState)InterlockedCompareExchangePointer(&gamePadGetState,0,0);
}
static void showRadar(void){
 KillTimer(window,PLACEMENT_TIMER);
 ShowWindow(window,SW_SHOW);
 SetForegroundWindow(window);
 InterlockedExchangePointer(&placementGameWindow,0);
}
static void hideRadar(void){
 DWORD pid=0;
 int radarFocused=GetForegroundWindow()==window;
 if(keysWindow)DestroyWindow(keysWindow);
 saveWindowPlacement();
 ShowWindow(window,SW_HIDE);
 GetWindowThreadProcessId(gameWindow,&pid);
 if(radarFocused && pid==GetCurrentProcessId() && IsWindowVisible(gameWindow))SetForegroundWindow(gameWindow);
}
static void focusGameForPlacement(void){
 DWORD pid=0;
 GetWindowThreadProcessId(gameWindow,&pid);
 if(pid==GetCurrentProcessId() && IsWindowVisible(gameWindow) && installKeyboardFilter()
    && SetTimer(window,PLACEMENT_TIMER,15,0)){
  placementKeysReady=0;
  InterlockedExchange(&placementKeyboardSeen,0);
  InterlockedExchange(&placementKeysHeld,0);
  InterlockedExchange(&placementKeysPressed,0);
  InterlockedExchangePointer(&placementGameWindow,gameWindow);
  ShowWindow(window,SW_HIDE);
  if(GetForegroundWindow()==gameWindow || SetForegroundWindow(gameWindow))return;
  showRadar();
 }
 strcpy(message,TR("Управление из игры недоступно — используйте кнопки радара.","In-game controls unavailable — use the radar buttons."));
}
static int placementMotion(BYTE *motion){
 Item current;
 uintptr_t shape;
 return currentWorld() && pose(placement.body,&current)
        && readmem(placement.body+0x20,&shape,8) && shape==placement.shape
        && readmem(placement.body+0x168,motion,1);
}
static int finishPlacement(int cancel){
 BYTE motion;
 if(!placement.body)return 1;
 /* Груз друга отпускает/отменяет только его команда, а не кнопки или закрытие окна хоста. */
 if(placement.mode==PM_REMOTE && !netExecuting)return 1;
 if(placement.mode==PM_CLIENT){
  netClientFinish(cancel);
  placement.body=0;placement.mode=PM_LOCAL;
  placementControls();
  return 1;
 }
 if(!placementMotion(&motion)){
  strcpy(message,TR("Груз или карта изменились. Нажмите «Обновить».","Cargo or map changed. Press “Refresh”."));
 }else{
  if(motion!=4 && motion!=placement.originalMotion){
   strcpy(message,TR("Физика груза изменилась — размещение отменено.","Cargo physics changed — placement cancelled."));
  }else{
   if(!cancel && motion!=4){
    strcpy(message,TR("Ждём физику. Нажмите «Отпустить» ещё раз.","Waiting for physics. Press release again."));
    InvalidateRect(window,0,TRUE);
    return 0;
   }
   if(cancel){
    void *pair=allocatePose(placement.originalPosition,placement.originalRotation);
    if(!pair){
     strcpy(message,TR("Отменить не удалось — нажмите «Отпустить».","Could not cancel — press “Release”."));
     InvalidateRect(window,0,TRUE);
     return 0;
    }
    submitPose(placement.body,pair);
    if(placement.mode==PM_LOCAL && selected>=0){
     logs[selected].center=placement.originalCenter;
     logs[selected].axis=placement.originalAxis;
    }
   }
   changeMotion(placement.body,placement.originalMotion);
   stopMotion(placement.body);
   {int i;
    for(i=0;i<placement.partCount;i++){
     if(cancel){void *pair=allocatePose(placement.partPosition[i],placement.partRotation[i]);if(pair)submitPose(placement.parts[i],pair);}
     changeMotion(placement.parts[i],placement.partMotion[i]);
     stopMotion(placement.parts[i]);
    }}
   if(!cancel)recordBed();
   if(cancel)strcpy(message,TR("Размещение отменено, груз возвращён","Placement cancelled, cargo returned"));
   else sprintf(message,TR("Груз отпущен — закрепите его в игре. В кузове уложено: %d","Cargo released — strap it in the game. Loaded in the bed: %d"),bed.count);
  }
 }
 {int local=placement.mode==PM_LOCAL;
  placement.body=0;placement.mode=PM_LOCAL;
  if(local)orderCargo();} /* груз друга не пересортировывает список грузов хоста */
 placementControls();
 return 1;
}
/* Кватернионы (x,y,z,w), произведение Гамильтона; поворот вектора v' = q·v·q*. */
static void quatMul(const Vec *a,const Vec *b,Vec *out){
 Vec r;
 r.x=a->w*b->x+a->x*b->w+a->y*b->z-a->z*b->y;
 r.y=a->w*b->y-a->x*b->z+a->y*b->w+a->z*b->x;
 r.z=a->w*b->z+a->x*b->y-a->y*b->x+a->z*b->w;
 r.w=a->w*b->w-a->x*b->x-a->y*b->y-a->z*b->z;
 *out=r;
}
static void quatRotate(const Vec *q,const Vec *v,Vec *out){
 Vec p={v->x,v->y,v->z,0},c={-q->x,-q->y,-q->z,q->w},t;
 quatMul(q,&p,&t);quatMul(&t,&c,out);out->w=0;
}
/* Добавочный поворот груза по трём осям грузовика, шаги по 5°:
   1) курс (turnSteps) — вокруг вертикали;
   2) наклон влево/вправо (tiltSteps) — вокруг продольной оси (forwardX,0,forwardZ);
   3) наклон вперёд/назад (pitchSteps) — вокруг поперечной оси (−forwardZ,0,forwardX),
      +шаг поднимает перед груза, −шаг опускает. */
static void placementDelta(const Placement *t,Vec *delta){
 const double k=3.141592653589793/72.0;
 double hy=t->turnSteps*k,ht=t->tiltSteps*k,hp=t->pitchSteps*k;
 Vec yaw={0,(float)sin(hy),0,(float)cos(hy)};
 Vec tilt={(float)(sin(ht)*t->forwardX),0,(float)(sin(ht)*t->forwardZ),(float)cos(ht)};
 Vec pitch={(float)(-sin(hp)*t->forwardZ),0,(float)(sin(hp)*t->forwardX),(float)cos(hp)};
 Vec rollYaw;
 quatMul(&tilt,&yaw,&rollYaw);
 quatMul(&pitch,&rollYaw,delta);
}
static void adjustedPose(const Placement *target,Vec *position,Vec *rotation,Vec *center,Vec *axis){
 /* turnSteps/tiltSteps — шаги по 5° (72 шага на полный оборот). */
 double halfAngle=target->turnSteps*(3.141592653589793/72.0);
 float s=sin(halfAngle),c=cos(halfAngle),yawSin=2*s*c,yawCos=1-2*s*s;
 Vec delta,offset,rotated;
 placementDelta(target,&delta);
 *center=target->center;
 center->x+=target->along*target->forwardX-target->across*target->forwardZ;
 center->z+=target->along*target->forwardZ+target->across*target->forwardX;
 center->y+=target->lift;
 /* Начало тела может не совпадать с центром масс — поворачиваем и это смещение (вращение вокруг центра масс). */
 offset.x=target->position.x-target->center.x;
 offset.y=target->position.y-target->center.y;
 offset.z=target->position.z-target->center.z;
 offset.w=0;
 quatRotate(&delta,&offset,&rotated);
 position->x=center->x+rotated.x;
 position->y=center->y+rotated.y;
 position->z=center->z+rotated.z;
 position->w=0;
 quatMul(&delta,&target->rotation,rotation);
 axis->x=yawCos*target->forwardX+yawSin*target->forwardZ;
 axis->y=0;
 axis->z=-yawSin*target->forwardX+yawCos*target->forwardZ;
 axis->w=0;
}
/* Составной груз: ставит остальные части так же, как сдвинулось главное тело
   (одно жёсткое перемещение D = новый поворот · исходный поворот⁻¹). */
static int submitParts(const Placement *p,Vec mainPosition,Vec mainRotation){
 Vec inverse={-p->originalRotation.x,-p->originalRotation.y,-p->originalRotation.z,p->originalRotation.w},delta;
 int i;
 if(!p->partCount)return 1;
 quatMul(&mainRotation,&inverse,&delta);
 for(i=0;i<p->partCount;i++){
  Vec offset,rotated,position,rotation;
  void *pair;
  offset.x=p->partPosition[i].x-p->originalPosition.x;
  offset.y=p->partPosition[i].y-p->originalPosition.y;
  offset.z=p->partPosition[i].z-p->originalPosition.z;
  offset.w=0;
  quatRotate(&delta,&offset,&rotated);
  position.x=mainPosition.x+rotated.x;position.y=mainPosition.y+rotated.y;position.z=mainPosition.z+rotated.z;position.w=0;
  quatMul(&delta,&p->partRotation[i],&rotation);
  if(!(pair=allocatePose(position,rotation)))return 0;
  submitPose(p->parts[i],pair);
 }
 return 1;
}
static volatile LONG padFineHeld;
/* Точный режим: зажат Shift на клавиатуре или X на геймпаде. */
static int fineStep(void){return (GetAsyncKeyState(keyBindings[K_FINE].vk)&0x8000) || InterlockedCompareExchange(&padFineHeld,0,0);}
static int wrapTurn(int t){t%=72;if(t>=36)t-=72;if(t<-36)t+=72;return t;}
/* fine — точный шаг; у хоста для груза друга берётся из команды друга. */
static void nudgePlacementEx(int direction,int fine){
 const float step=fine?0.05f:0.25f;
 const int turn=fine?1:3;
 const float horizontalLimit=25.0f;
 BYTE motion;
 Vec position,rotation,center,axis;
 Placement next;
 void *pair;
 if(!placement.body)return;
 if(placement.mode==PM_REMOTE && !netExecuting)return;
 if(placement.mode==PM_CLIENT){netClientNudge(direction,fine);return;}
 if(!placementMotion(&motion)){
  int local=placement.mode==PM_LOCAL;
  finishPlacement(0);
  if(local)showRadar();
  return;
 }
 if(motion!=4)return;
 next=placement;
 switch(direction){
 case MOVE_FORWARD:next.along+=step;break;
 case MOVE_BACK:next.along-=step;break;
 case MOVE_LEFT:next.across-=step;break;
 case MOVE_RIGHT:next.across+=step;break;
 case TURN_LEFT:next.turnSteps=wrapTurn(next.turnSteps+turn);break;
 case TURN_RIGHT:next.turnSteps=wrapTurn(next.turnSteps-turn);break;
 case RAISE_CARGO:next.lift+=step;break;
 case LOWER_CARGO:next.lift-=step;break;
 case TILT_LEFT:next.tiltSteps=wrapTurn(next.tiltSteps+turn);break;
 case TILT_RIGHT:next.tiltSteps=wrapTurn(next.tiltSteps-turn);break;
 case PITCH_FORWARD:next.pitchSteps=wrapTurn(next.pitchSteps-turn);break; /* перед груза вниз */
 case PITCH_BACK:next.pitchSteps=wrapTurn(next.pitchSteps+turn);break;    /* перед груза вверх */
 default:return;
 }
 if(absf(next.along)>horizontalLimit || absf(next.across)>horizontalLimit)return;
 if(height+next.lift<1.0f || height+next.lift>30.0f)return;
 adjustedPose(&next,&position,&rotation,&center,&axis);
 pair=allocatePose(position,rotation);
 if(!pair)return;
 submitPose(placement.body,pair);
 submitParts(&next,position,rotation);
 placement=next;
 if(placement.mode==PM_LOCAL && selected>=0){
  logs[selected].center=center;
  logs[selected].axis=axis;
 }
 {char release[32];bindingTextU8(K_RELEASE,release,32);
 sprintf(message,TR("Высота %.2f м · Поворот %d° · Наклон %d° / %d° · Сдвиг %.2f / %.2f м · шаг %s · %s — отпустить","Height %.2f m · Turn %d° · Tilt %d° / %d° · Offset %.2f / %.2f m · %s step · %s — release"),
         height+next.lift,next.turnSteps*5,next.tiltSteps*5,next.pitchSteps*5,next.along,next.across,fine?TR("точный","fine"):TR("обычный","normal"),release);}
 InvalidateRect(window,0,TRUE);
}
static void nudgePlacement(int direction){nudgePlacementEx(direction,fineStep());}
static void pollPlacementKeys(void){
 DWORD now=GetTickCount();
 LONG pressed,held;
 int i;
 if(!placementGameWindow || !placement.body)return;
 if(GetForegroundWindow()!=gameWindow){placementKeysReady=0;return;}
 pressed=InterlockedExchange(&placementKeysPressed,0);
 held=InterlockedCompareExchange(&placementKeysHeld,0,0);
 for(i=0;i<PLACEMENT_KEY_COUNT;i++){
  const PlacementKey *key=&placementKeys[i];
  int down=(held & (1L<<i))!=0;
  int tapped=(pressed & (1L<<i))!=0;
  BYTE previous=placementKeyDown[i];
  if(!placementKeysReady){
   placementKeyDown[i]=down;
   placementRepeat[i]=now+300;
   continue;
  }
  if(!InterlockedCompareExchange(&placementKeyboardSeen,0,0) && (GetAsyncKeyState(key->key)&0x8000)){
   strcpy(message,TR("Клавиатура игры недоступна — используйте кнопки радара.","Game keyboard unavailable — use the radar buttons."));
   showRadar();
   return;
  }
  if(!key->direction && !down && (previous==2 || tapped)){
   finishPlacement(i==PK_CANCEL);
   showRadar();
   return;
  }
  placementKeyDown[i]=down?(previous?previous:2):0;
  if(key->direction && (tapped || (down && (!previous || (LONG)(now-placementRepeat[i])>=0)))){
   placementRepeat[i]=now+(previous?85:300);
   nudgePlacement(key->direction);
   if(!placement.body)return;
  }
 }
 placementKeysReady=1;
}
/* Составной груз (разбитая техника и т.п.): обходим шарниры Havok от главного тела
   (тот же список связей +0x110 и VT_CONSTRAINT, по которому мод сцепляет прицеп) и
   собираем все свободные тела группы. Не берём: грузовики/прицепы (ремни ведут к ним),
   неподвижные тела, тела дальше 12 м от главного. Возвращает число найденных частей
   (без главного) или -1, если частей больше, чем помещается в таблицу. */
static int collectParts(uintptr_t mainBody,const Item *mainItem,uintptr_t truckBody,Placement *next){
 uintptr_t queue[32];
 int head=0,tail=1,i;
 queue[0]=mainBody;
 next->partCount=0;
 while(head<tail){
  uintptr_t body=queue[head++],links[64];
  int n=array(body+0x110,links,64),j;
  for(j=0;j<n;j++){
   uintptr_t data[8],other;
   Item item;
   Vec inertia;
   BYTE motion;
   float dx,dy,dz;
   if(!readmem(links[j],data,sizeof(data)) || data[0]!=gameAddr[VT_CONSTRAINT])continue;
   if(data[6]==body)other=data[7];
   else if(data[7]==body)other=data[6];
   else continue;
   if(!other || other==truckBody)continue;
   for(i=0;i<tail;i++)if(queue[i]==other)break;
   if(i<tail)continue;
   if(!pose(other,&item) || !readmem(other+0x220,&inertia,16) || !readmem(other+0x168,&motion,1))continue;
   if(truckProfileFor(other,inertia))continue;
   if(motion!=1 && motion!=2 && motion!=3 && motion!=6)continue;
   dx=item.center.x-mainItem->center.x;dy=item.center.y-mainItem->center.y;dz=item.center.z-mainItem->center.z;
   if(dx*dx+dy*dy+dz*dz>12.0f*12.0f)continue;
   if(tail>=32 || next->partCount>=(int)(sizeof(next->parts)/sizeof(next->parts[0])))return -1;
   queue[tail++]=other;
   i=next->partCount++;
   next->parts[i]=other;
   next->partMotion[i]=motion;
   memcpy(&next->partPosition[i],&item.pose[12],16);
   next->partPosition[i].w=0;
   if(!readmem(other+0x1e0,&next->partRotation[i],16))return -1;
  }
 }
 return next->partCount;
}
/* Поднимает груз cargoBody над кузовом грузовика t (профиль — truckProfile, память — bed).
   Общая часть для своей погрузки (PM_LOCAL) и для погрузки по команде друга на хосте (PM_REMOTE).
   Возвращает 1, если груз поднят; иначе пишет причину в message. */
static int beginPlacement(uintptr_t cargoBody,int kind,Vec axis,const Item *truckItem,int mode){
 Item current,t=*truckItem;
 Placement next={0};
 Vec com,inertia;
 void *pair;
 int forward,up,upSign;
 float headingLength;
 if(!pose(cargoBody,&current) || !readmem(cargoBody+0x220,&inertia,16)){strcpy(message,TR("Объект изменился. Нажмите «Обновить».","Object changed. Press “Refresh”."));return 0;}
 if(kind>=FIRST_ASSET_KIND && cargoAssets[kind-FIRST_ASSET_KIND].multipleBodies
    && collectParts(cargoBody,&current,t.body,&next)<0){
  strcpy(message,TR("Слишком много частей у груза (больше 31) — грузите краном.","The cargo has too many parts (over 31) — load it with the crane."));
  return 0;
 }
 if(!readmem(cargoBody+0x168,&next.originalMotion,1)
    || (next.originalMotion!=1 && next.originalMotion!=2 && next.originalMotion!=3 && next.originalMotion!=6)){
  strcpy(message,TR("Выберите свободно лежащий груз.","Select a loose cargo object."));
  return 0;
 }
 next.body=cargoBody;
 next.mode=mode;
 if(!readmem(cargoBody+0x20,&next.shape,8)
    || !readmem(cargoBody+0x1e0,&next.originalRotation,16)
    || !readmem(cargoBody+0x1f0,&com,16)){strcpy(message,TR("Объект изменился. Нажмите «Обновить».","Object changed. Press “Refresh”."));return 0;}
 memcpy(&next.originalPosition,&current.pose[12],16);
 next.originalPosition.w=0;
 next.originalCenter=current.center;
 next.originalAxis=axis;
 forward=cargoForward(kind,inertia);
 up=kind>=FIRST_ASSET_KIND?cargoAssets[kind-FIRST_ASSET_KIND].up:kind?profiles[kind-1].up:2;
 upSign=kind?1:-1;
 if(!cargoLoadingPose(&t,truckProfile,com,forward,up,upSign,&next.position,&next.rotation,&next.center)){strcpy(message,TR("Не удалось рассчитать место над кузовом.","Could not compute the spot above the bed."));return 0;}
 headingLength=length(t.pose[8],t.pose[10]);
 next.forwardX=t.pose[8]/headingLength;next.forwardZ=t.pose[10]/headingLength;
 {
  float size[3];
  const char *bedNote;
  Vec startPosition=next.position,startRotation=next.rotation,startCenter=next.center,startAxis;
  if(cargoSize(cargoBody,size)){next.sizeForward=size[forward];next.sizeOther=size[3-forward-up];}
  else{next.sizeForward=2.0f;next.sizeOther=1.0f;} /* форма неизвестного типа — условные габариты */
  bedNote=planBed(&next);
  if(next.along!=0 || next.across!=0 || next.turnSteps || next.tiltSteps || next.pitchSteps)adjustedPose(&next,&startPosition,&startRotation,&startCenter,&startAxis);
  pair=allocatePose(startPosition,startRotation);
  if(!pair){strcpy(message,TR("Игра не выделила память для операции","The game did not allocate memory for the operation"));return 0;}
  stopMotion(cargoBody);
  changeMotion(cargoBody,4);
  submitPose(cargoBody,pair);
  {int i;for(i=0;i<next.partCount;i++){stopMotion(next.parts[i]);changeMotion(next.parts[i],4);}}
  submitParts(&next,startPosition,startRotation);
  placement=next;
  if(mode==PM_LOCAL && selected>=0){
   logs[selected].center=startCenter;
   logs[selected].axis.x=placement.forwardX;logs[selected].axis.z=placement.forwardZ;
  }
  if(bedNote){char release[32];bindingTextU8(K_RELEASE,release,32);
   sprintf(message,TR("Память позиции: место %d, %s. Поправьте при необходимости · %s — отпустить","Position memory: slot %d, %s. Adjust if needed · %s — release"),bed.count+1,bedNote,release);}
  else lstrcpynA(message,keyHintU8,sizeof(message));
  if(next.partCount){
   char text[sizeof(message)];
   _snprintf(text,sizeof(text),TR("Составной груз: %d частей вместе · %s","Multi-part cargo: %d parts together · %s"),next.partCount+1,message);
   text[sizeof(text)-1]=0;
   lstrcpynA(message,text,sizeof(message));
  }
 }
 return 1;
}
static void loadSelected(void){
 Item current,t;
 Vec inertia,truckInertia;
 int kind,detected;
 uintptr_t userdata;
 if(placement.body && placement.mode==PM_REMOTE){
  strcpy(message,TR("Сейчас друг размещает груз через ваш мод — подождите.","A friend is placing cargo through your mod — please wait."));
  InvalidateRect(window,0,TRUE);
  return;
 }
 if(placement.body){finishPlacement(0);return;}
 if(selected<0 || !selectedBody || !truckReady)return;
 if(!currentWorld()){strcpy(message,TR("Карта сменилась или версия игры не поддерживается.","Map changed or the game version is not supported."));goto done;}
 detected=automaticTruckIndex();
 if(detected<0 || truckTargets[detected].item.body!=truck.body){
  selectedBody=0;
  refresh();
  strcpy(message,TR("Грузовик сменился. Выберите груз заново.","Truck changed. Select the cargo again."));
  goto done;
 }
 if(!pose(selectedBody,&current) || !pose(truck.body,&t)){strcpy(message,TR("Объект изменился. Нажмите «Обновить».","Object changed. Press “Refresh”."));goto done;}
 if(!readmem(truck.body+0x220,&truckInertia,16) || truckProfileFor(truck.body,truckInertia)!=truckProfile){
  strcpy(message,TR("Грузовик изменился. Нажмите «Обновить».","Truck changed. Press “Refresh”."));goto done;
 }
 if(!readmem(selectedBody+0x220,&inertia,16) || !readmem(selectedBody+0x10,&userdata,8) || cargoKind(selectedBody,inertia,userdata)!=logs[selected].kind){strcpy(message,TR("Груз изменился. Нажмите «Обновить».","Cargo changed. Press “Refresh”."));goto done;}
 if(length(current.center.x-t.center.x,current.center.z-t.center.z)>loadRange){
  sprintf(message,TR("Подъедьте ближе: груз должен быть не дальше %.0f м.","Drive closer: the cargo must be within %.0f m."),loadRange);
  goto done;
 }
 kind=logs[selected].kind;
 /* Клиент мультиплеера: физикой управляет хост, поэтому только отправляем ему запрос. */
 if(netClientActive()){netClientLoad(kind,current.center,t.center,truckProfile);goto done;}
 if(beginPlacement(selectedBody,kind,logs[selected].axis,&t,PM_LOCAL)){
  placementControls();
  focusGameForPlacement();
 }
done:InvalidateRect(window,0,TRUE);
}
static void openRadarFrom(HWND foreground){
 if(!placement.body){
  WCHAR className[64];
  if(foreground!=window && GetClassNameW(foreground,className,64)
     && lstrcmpiW(className,L"RoadcraftModWindow"))gameWindow=foreground;
  refresh();
 }
 showRadar();
}
static void toggleRadar(HWND foreground){
 if(IsWindowVisible(window)){
  if(finishPlacement(1))hideRadar();
 }else openRadarFrom(foreground);
}
static void pollGamepad(void){
 HWND foreground=GetForegroundWindow();
 DWORD now=GetTickCount(),pid=0,index;
 int radarFocused=foreground==window;
 int positioning=placement.body && placement.mode!=PM_REMOTE && (radarFocused || foreground==gameWindow);
 GetWindowThreadProcessId(foreground,&pid);
 if((LONG)(now-retryPadFilter)>=0 || !retryPadFilter){
  padFilterReady=installGamepadFilter();
  retryPadFilter=now+1000;
 }
 for(index=0;index<4;index++){
  PadHistory *pad=&pads[index];
  PadSample sample;
  WORD held,previous,pressed;
  int i;
  if(readPad(index,&sample)!=ERROR_SUCCESS){pad->connected=0;pad->repeating=0;continue;}
  held=sample.input.buttons & ~(PAD_LT|PAD_RT);
  if(sample.input.lt>30)held|=PAD_LT;
  if(sample.input.rt>30)held|=PAD_RT;
  /* Наклон — правый стик: влево/вправо и вверх (вперёд)/вниз (назад).
     Биты нажатий стиков и Start/Back моду не нужны — на их месте стик. */
  held&=~(PAD_TILT_L|PAD_TILT_R|PAD_PITCH_F|PAD_PITCH_B);
  if(sample.input.rx<-20000)held|=PAD_TILT_L;
  else if(sample.input.rx>20000)held|=PAD_TILT_R;
  if(sample.input.ry>20000)held|=PAD_PITCH_F;
  else if(sample.input.ry<-20000)held|=PAD_PITCH_B;
  previous=pad->connected?pad->previous:held;
  pressed=held & ~previous;
  pad->connected=1;
  pad->previous=held;
  if(!positioning || pid!=GetCurrentProcessId())pad->repeating=0;
  if(!padFilterReady || pid!=GetCurrentProcessId())continue;
  if((held&PAD_OPEN)==PAD_OPEN && (pressed&PAD_OPEN)){
   toggleRadar(foreground);
   return;
  }
  if(positioning){
   InterlockedExchange(&padFineHeld,(held&PAD_X)?1:0);
   if(pressed&PAD_A){
    if(finishPlacement(0))showRadar();
    return;
   }
   pad->repeating&=held;
   for(i=0;i<sizeof(padMoves)/sizeof(padMoves[0]);i++){
    WORD button=padMoves[i].button;
    if((pressed&button) || ((pad->repeating&button) && (LONG)(now-pad->nextRepeat[i])>=0)){
     pad->nextRepeat[i]=now+((pressed&button)?300:85);
     pad->repeating|=button;
     nudgePlacement(padMoves[i].direction);
     if(!placement.body)return;
    }
   }
  }else if(radarFocused){
   if(pressed&PAD_LEFT)cycle(-1);
   if(pressed&PAD_RIGHT)cycle(1);
   if(pressed&PAD_X){refresh();InvalidateRect(window,0,TRUE);}
   if(pressed&PAD_A){loadSelected();return;}
  }
 }
}
/* ───── KOTEuKA_CargoLoad: оформление окна радара ───── */
#define COL_BG      RGB(22,24,29)
#define COL_CARD    RGB(33,36,43)
#define COL_BORDER  RGB(55,60,71)
#define COL_TEXT    RGB(232,234,238)
#define COL_MUTED   RGB(148,155,168)
#define COL_ACCENT  RGB(255,153,28)
#define COL_GOOD    RGB(80,200,120)
#define COL_WARN    RGB(242,192,55)
#define COL_BAD     RGB(238,90,90)
#define COL_MAP     RGB(17,27,24)
#define COL_GRID    RGB(38,58,50)
#define COL_RING    RGB(52,82,70)
#include "net.inc"
static void mapPoint(Vec p,int *x,int *y){float fx=truck.pose[8],fz=truck.pose[10],n=length(fx,fz),dx=p.x-truck.center.x,dz=p.z-truck.center.z;if(n<.1f)n=1;fx/=n;fz/=n;*x=MAP_CX-(int)((dx*fz-dz*fx)*mapScale);*y=MAP_CY-(int)((dx*fx+dz*fz)*mapScale);}
enum MarkerShape { MARK_RECT, MARK_CIRCLE, MARK_TRIANGLE, MARK_DIAMOND };
typedef struct {
 const char *label;
 const WCHAR *name,*nameEn;
 enum MarkerShape shape;
 COLORREF color;
 int halfLength,halfWidth;
} Marker;
static const Marker markers[]={
 {"Concrete_scrap",L"Бетонный лом",L"Concrete scrap",MARK_CIRCLE,RGB(0,176,196),8,8},
 {"Metal_scrap",L"Металлолом",L"Metal scrap",MARK_TRIANGLE,RGB(236,116,44),8,8},
 {"Sellable",L"Товар",L"Sellable",MARK_DIAMOND,RGB(240,200,52),8,8},
 {"Concrete_slab",L"Бетонная плита",L"Concrete slab",MARK_RECT,RGB(74,136,244),14,8},
 {"Concrete_pile",L"Бетонная свая",L"Concrete pile",MARK_RECT,RGB(150,164,180),17,6},
 {"Iron_pipe",L"Железная труба",L"Iron pipe",MARK_RECT,RGB(64,186,100),16,4},
 {"Steel_beam",L"Стальная балка",L"Steel beam",MARK_RECT,RGB(186,96,226),16,3},
 {"Log",L"Бревно",L"Log",MARK_RECT,RGB(170,118,70),17,3},
 {"Unknown",L"Неизвестный груз",L"Unknown cargo",MARK_RECT,RGB(120,120,120),8,4}
};
static const WCHAR *markerName(const Marker *m){return uiLang?m->nameEn:m->name;}
static const Marker *cargoMarker(int kind){
 const char *label=cargoLabel(kind);
 int i;
 for(i=0;i<sizeof(markers)/sizeof(markers[0])-1;i++){
  if(!strcmp(label,markers[i].label))return &markers[i];
 }
 return &markers[i];
}
static float cargoMass(int kind){
 if(kind>=FIRST_ASSET_KIND)return cargoAssets[kind-FIRST_ASSET_KIND].mass;
 if(kind==0)return 1100;
 return kind>0 && kind<=profileCount?profiles[kind-1].mass:0;
}
/* Высота подъёма (глобальная height); в paint() это имя занято размером окна. */
static float dropHeight(void){return height;}
static void textW(HDC dc,const WCHAR *s,RECT *r,UINT flags,COLORREF color){
 SetTextColor(dc,color);
 DrawTextW(dc,s,-1,r,flags|DT_NOPREFIX);
}
static void textU8(HDC dc,const char *s,RECT *r,UINT flags,COLORREF color){
 WCHAR w[512];
 if(!MultiByteToWideChar(CP_UTF8,0,s,-1,w,512))w[0]=0;
 textW(dc,w,r,flags,color);
}
static void fillRound(HDC dc,int l,int t,int r,int b,int radius,COLORREF fill,COLORREF border){
 HPEN pen=CreatePen(PS_SOLID,1,border);
 HBRUSH brush=CreateSolidBrush(fill);
 HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,brush);
 RoundRect(dc,l,t,r,b,radius,radius);
 SelectObject(dc,oldPen);SelectObject(dc,oldBrush);
 DeleteObject(pen);DeleteObject(brush);
}
static void ring(HDC dc,int x,int y,int radius,COLORREF color,int style,int width){
 HPEN pen=CreatePen(style,width,color);
 HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
 Ellipse(dc,x-radius,y-radius,x+radius+1,y+radius+1);
 SelectObject(dc,oldPen);SelectObject(dc,oldBrush);
 DeleteObject(pen);
}
/* Рисует значок груза: (dx,dy) — направление длинной оси, scale — размер. */
static void drawShape(HDC dc,const Marker *marker,int x,int y,float dx,float dy,float scale,COLORREF outline){
 HPEN pen=CreatePen(PS_SOLID,1,outline);
 HBRUSH brush=CreateSolidBrush(marker->color);
 HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,brush);
 int a=(int)(8*scale+.5f),b=(int)(9*scale+.5f),c=(int)(10*scale+.5f),j;
 if(marker->shape==MARK_CIRCLE){
  Ellipse(dc,x-a,y-a,x+a+1,y+a+1);
 }else if(marker->shape==MARK_TRIANGLE){
  POINT triangle[3];
  triangle[0].x=x;triangle[0].y=y-b;triangle[1].x=x+b;triangle[1].y=y+a;triangle[2].x=x-b;triangle[2].y=y+a;
  Polygon(dc,triangle,3);
 }else if(marker->shape==MARK_DIAMOND){
  POINT diamond[4];
  diamond[0].x=x;diamond[0].y=y-c;diamond[1].x=x+c;diamond[1].y=y;diamond[2].x=x;diamond[2].y=y+c;diamond[3].x=x-c;diamond[3].y=y;
  Polygon(dc,diamond,4);
 }else{
  POINT corners[4];
  float hl=marker->halfLength*scale,hw=marker->halfWidth*scale;
  if(hw<1.5f)hw=1.5f;
  for(j=0;j<4;j++){
   float along=j<2?hl:-hl;
   float across=j==0 || j==3?hw:-hw;
   corners[j].x=x+(int)(along*dx-across*dy);
   corners[j].y=y+(int)(along*dy+across*dx);
  }
  Polygon(dc,corners,4);
 }
 SelectObject(dc,oldPen);SelectObject(dc,oldBrush);
 DeleteObject(pen);DeleteObject(brush);
}
static void paintCargo(HDC dc,int i){
 const Item *cargo=&logs[i];
 const Marker *marker=cargoMarker(cargo->kind);
 int x,y,inRange=cargoDistanceSquared(cargo)<=loadRange*loadRange;
 float dx=1,dy=0;
 char number[16];
 mapPoint(cargo->center,&x,&y);
 if(marker->shape==MARK_RECT){
  Vec end=cargo->center;
  int ex,ey;
  float n;
  end.x+=cargo->axis.x*3;
  end.z+=cargo->axis.z*3;
  mapPoint(end,&ex,&ey);
  dx=(float)(ex-x);dy=(float)(ey-y);n=length(dx,dy);
  if(n<1){dx=1;dy=0;}else{dx/=n;dy/=n;}
 }
 drawShape(dc,marker,x,y,dx,dy,1.0f,inRange?RGB(235,235,235):RGB(8,10,12));
 if(i==selected){
  ring(dc,x,y,19,COL_ACCENT,PS_SOLID,2);
 }
 sprintf(number,"%d",i+1);
 SetTextColor(dc,i==selected?COL_ACCENT:(inRange?COL_TEXT:COL_MUTED));
 TextOutA(dc,x+11,y-22,number,lstrlenA(number));
}
static void statusInfo(const WCHAR **text,COLORREF *color){
 if(!gameAddrReady){*text=TR(L"НЕТ АДРЕСОВ",L"NO ADDRESSES");*color=COL_BAD;}
 else if(!signature()){*text=TR(L"НЕ ТА ВЕРСИЯ",L"WRONG VERSION");*color=COL_BAD;}
 else if(placement.body && placement.mode==PM_REMOTE){*text=TR(L"ДРУГ ГРУЗИТ",L"FRIEND LOADING");*color=COL_ACCENT;}
 else if(placement.body){*text=TR(L"РАЗМЕЩЕНИЕ",L"PLACING");*color=COL_ACCENT;}
 else if(truckReady){*text=TR(L"ГОТОВ",L"READY");*color=COL_GOOD;}
 else{*text=TR(L"ОЖИДАНИЕ",L"WAITING");*color=COL_WARN;}
}
static void paint(HWND h){
 PAINTSTRUCT ps;
 HDC target=BeginPaint(h,&ps),dc;
 RECT client,view=radarViewport(h),r;
 HBITMAP bitmap;
 HGDIOBJ oldBitmap,oldFont;
 HFONT title,normal,small,bold;
 HBRUSH brush;
 const WCHAR *statusText;
 COLORREF statusColor;
 WCHAR wide[256];
 SIZE extent;
 int i,width,height;
 GetClientRect(h,&client);
 width=client.right>0?client.right:1;height=client.bottom>0?client.bottom:1;
 dc=CreateCompatibleDC(target);
 bitmap=CreateCompatibleBitmap(target,width,height);
 oldBitmap=SelectObject(dc,bitmap);
 brush=CreateSolidBrush(COL_BG);FillRect(dc,&client,brush);DeleteObject(brush);
 SetMapMode(dc,MM_ANISOTROPIC);
 SetWindowExtEx(dc,RADAR_WIDTH,RADAR_HEIGHT,0);
 SetViewportExtEx(dc,view.right-view.left,view.bottom-view.top,0);
 SetViewportOrgEx(dc,view.left,view.top,0);
 SetBkMode(dc,TRANSPARENT);
 title=radarFontW(19,FW_BOLD);normal=radarFontW(15,FW_NORMAL);
 small=radarFontW(13,FW_NORMAL);bold=radarFontW(15,FW_SEMIBOLD);
 oldFont=SelectObject(dc,title);

 /* Шапка: название и статус */
 SetRect(&r,0,0,RADAR_WIDTH,HEADER_BOTTOM);
 brush=CreateSolidBrush(COL_CARD);FillRect(dc,&r,brush);DeleteObject(brush);
 SetRect(&r,0,HEADER_BOTTOM-3,RADAR_WIDTH,HEADER_BOTTOM);
 brush=CreateSolidBrush(COL_ACCENT);FillRect(dc,&r,brush);DeleteObject(brush);
 SetRect(&r,16,0,320,HEADER_BOTTOM-3);
 textW(dc,L"KOTEuKA_CargoLoad",&r,DT_SINGLELINE|DT_VCENTER|DT_LEFT,COL_TEXT);
 statusInfo(&statusText,&statusColor);
 SelectObject(dc,small);
 GetTextExtentPoint32W(dc,statusText,lstrlenW(statusText),&extent);
 fillRound(dc,340-extent.cx-22,11,340,33,20,statusColor,statusColor);
 SetRect(&r,340-extent.cx-22,11,340,33);
 textW(dc,statusText,&r,DT_SINGLELINE|DT_VCENTER|DT_CENTER,RGB(20,20,22));

 /* Карточка состояния */
 fillRound(dc,MAP_LEFT,INFO_TOP,MAP_RIGHT,INFO_BOTTOM,12,COL_CARD,COL_BORDER);
 SetRect(&r,26,INFO_TOP+10,120,INFO_TOP+30);
 textW(dc,TR(L"ГРУЗОВИК",L"TRUCK"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 SetRect(&r,26,INFO_TOP+34,120,INFO_TOP+54);
 textW(dc,TR(L"СОСТОЯНИЕ",L"STATUS"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 SelectObject(dc,bold);
 SetRect(&r,124,INFO_TOP+10,390,INFO_TOP+30);
 textU8(dc,detectedTruck==genericTruck.label?TR("Шасси грузовика","Truck chassis"):detectedTruck,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,truckReady?COL_TEXT:COL_WARN);
 _snwprintf(wide,255,TR(L"Грузов: %d",L"Cargo: %d"),count);wide[255]=0;
 SetRect(&r,390,INFO_TOP+10,MAP_RIGHT-14,INFO_TOP+30);
 textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT,count?COL_GOOD:COL_MUTED);
 SelectObject(dc,normal);
 SetRect(&r,124,INFO_TOP+35,MAP_RIGHT-14,INFO_BOTTOM-6);
 textU8(dc,message,&r,DT_WORDBREAK|DT_END_ELLIPSIS,placement.body?COL_ACCENT:COL_TEXT);

 /* Радар */
 fillRound(dc,MAP_LEFT,MAP_TOP,MAP_RIGHT,MAP_BOTTOM,12,COL_MAP,COL_BORDER);
 SaveDC(dc);
 IntersectClipRect(dc,MAP_LEFT+2,MAP_TOP+2,MAP_RIGHT-2,MAP_BOTTOM-2);
 {
  HPEN pen=CreatePen(PS_SOLID,1,COL_GRID);
  HGDIOBJ oldPen=SelectObject(dc,pen);
  MoveToEx(dc,MAP_LEFT,MAP_CY,0);LineTo(dc,MAP_RIGHT,MAP_CY);
  MoveToEx(dc,MAP_CX,MAP_TOP,0);LineTo(dc,MAP_CX,MAP_BOTTOM);
  SelectObject(dc,oldPen);DeleteObject(pen);
 }
 SelectObject(dc,small);
 if(truckReady){
  static const int distances[]={10,25,45};
  for(i=0;i<3;i++){
   int radius=(int)(distances[i]*mapScale);
   int load=distances[i]==(int)loadRange;
   ring(dc,MAP_CX,MAP_CY,radius,load?RGB(150,96,30):COL_RING,load?PS_DASH:PS_SOLID,1);
   if(load)_snwprintf(wide,255,TR(L"%d м · зона погрузки",L"%d m · loading zone"),distances[i]);
   else _snwprintf(wide,255,TR(L"%d м",L"%d m"),distances[i]);
   wide[255]=0;
   /* Подпись — на диагонали вправо-вниз, чтобы не мешать зоне перед грузовиком. */
   SetRect(&r,MAP_CX+(int)(radius*0.71f)+4,MAP_CY+(int)(radius*0.71f)-4,MAP_CX+(int)(radius*0.71f)+200,MAP_CY+(int)(radius*0.71f)+14);
   textW(dc,wide,&r,DT_SINGLELINE,load?COL_ACCENT:COL_MUTED);
  }
  if(selected>=0 && selected<count){
   int x,y;
   HPEN pen=CreatePen(PS_DOT,1,COL_ACCENT);
   HGDIOBJ oldPen=SelectObject(dc,pen);
   mapPoint(logs[selected].center,&x,&y);
   MoveToEx(dc,MAP_CX,MAP_CY,0);LineTo(dc,x,y);
   SelectObject(dc,oldPen);DeleteObject(pen);
  }
  fillRound(dc,MAP_CX-10,MAP_CY-20,MAP_CX+11,MAP_CY+21,7,COL_ACCENT,RGB(20,20,22));
  SetRect(&r,MAP_CX-8,MAP_CY-17,MAP_CX+9,MAP_CY-9);
  brush=CreateSolidBrush(RGB(120,70,10));FillRect(dc,&r,brush);DeleteObject(brush);
  SetRect(&r,MAP_CX-40,MAP_CY-42,MAP_CX+41,MAP_CY-24);
  textW(dc,TR(L"перед",L"front"),&r,DT_SINGLELINE|DT_CENTER|DT_VCENTER,COL_ACCENT);
  SelectObject(dc,bold);
  for(i=0;i<count;i++)if(i!=selected)paintCargo(dc,i);
  if(selected>=0 && selected<count)paintCargo(dc,selected);
  SelectObject(dc,small);
 }else{
  SetRect(&r,MAP_LEFT+20,MAP_CY-30,MAP_RIGHT-20,MAP_CY+30);
  SelectObject(dc,normal);
  textW(dc,TR(L"Сядьте в грузовик и нажмите «Обновить».\nРадар появится, когда грузовик будет найден.",L"Get into a truck and press “Refresh”.\nThe radar appears once the truck is found."),&r,DT_CENTER|DT_WORDBREAK,COL_MUTED);
  SelectObject(dc,small);
 }
 SetRect(&r,MAP_LEFT+10,MAP_BOTTOM-22,MAP_RIGHT-10,MAP_BOTTOM-4);
 {WCHAR key[48];bindingTextW(K_RADAR,key,48);
  _snwprintf(wide,255,TR(L"Клик — выбрать груз · колесо мыши — масштаб · %ls — скрыть",L"Click — select cargo · mouse wheel — zoom · %ls — hide"),key);wide[255]=0;
  textW(dc,wide,&r,DT_SINGLELINE|DT_LEFT,COL_MUTED);}
 _snwprintf(wide,255,L"×%.1f",mapScale/3.4f);wide[255]=0;
 textW(dc,wide,&r,DT_SINGLELINE|DT_RIGHT,COL_MUTED);
 RestoreDC(dc,-1);

 /* Выбранный груз */
 fillRound(dc,MAP_LEFT,SEL_TOP,MAP_RIGHT,SEL_BOTTOM,12,COL_CARD,COL_BORDER);
 if(selected>=0 && selected<count){
  const Marker *marker=cargoMarker(logs[selected].kind);
  float distance=(float)sqrt(cargoDistanceSquared(&logs[selected]));
  float mass=cargoMass(logs[selected].kind);
  int inRange=distance<=loadRange;
  drawShape(dc,marker,36,(SEL_TOP+SEL_BOTTOM)/2,1,0,1.0f,RGB(235,235,235));
  SelectObject(dc,bold);
  _snwprintf(wide,255,TR(L"%ls  ·  %d из %d",L"%ls  ·  %d of %d"),markerName(marker),selected+1,count);wide[255]=0;
  SetRect(&r,64,SEL_TOP+7,MAP_RIGHT-14,SEL_TOP+27);
  textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,COL_TEXT);
  SelectObject(dc,small);
  if(placement.body){
   SetRect(&r,64,SEL_TOP+28,MAP_RIGHT-14,SEL_BOTTOM-6);
   {WCHAR release[48],fineKey[48];bindingTextW(K_RELEASE,release,48);bindingTextW(K_FINE,fineKey,48);
    _snwprintf(wide,255,TR(L"Груз над кузовом — выровняйте и нажмите %ls · %ls/X — точно",L"Cargo above the bed — line it up and press %ls · %ls/X — fine"),release,fineKey);wide[255]=0;
    textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,COL_ACCENT);}
  }else{
   if(mass>0)_snwprintf(wide,255,TR(L"Расстояние %.1f м  ·  масса ≈ %.1f т",L"Distance %.1f m  ·  mass ≈ %.1f t"),distance,mass/1000);
   else _snwprintf(wide,255,TR(L"Расстояние %.1f м",L"Distance %.1f m"),distance);
   wide[255]=0;
   SetRect(&r,64,SEL_TOP+28,330,SEL_BOTTOM-6);
   textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,COL_MUTED);
   if(inRange)lstrcpyW(wide,TR(L"● можно грузить",L"● in range"));
   else _snwprintf(wide,255,TR(L"● дальше %.0f м",L"● beyond %.0f m"),loadRange);
   wide[255]=0;
   SetRect(&r,330,SEL_TOP+28,MAP_RIGHT-14,SEL_BOTTOM-6);
   textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT,inRange?COL_GOOD:COL_BAD);
  }
 }else{
  SelectObject(dc,normal);
  SetRect(&r,26,SEL_TOP,MAP_RIGHT-14,SEL_BOTTOM);
  textW(dc,TR(L"Груз не выбран",L"No cargo selected"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 }

 /* Легенда */
 SelectObject(dc,small);
 for(i=0;i<8;i++){
  int x=MAP_LEFT+14+(i%4)*124,y=LEGEND_TOP+10+(i/4)*22;
  drawShape(dc,&markers[i],x,y,1,0,0.6f,RGB(8,10,12));
  SetRect(&r,x+14,y-9,x+122,y+9);
  textW(dc,markerName(&markers[i]),&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,COL_MUTED);
 }

 /* Настройки погрузки */
 fillRound(dc,MAP_LEFT,SETTINGS_TOP,MAP_RIGHT,SETTINGS_BOTTOM,12,COL_CARD,COL_BORDER);
 SelectObject(dc,small);
 SetRect(&r,26,SETTINGS_TOP,120,SETTINGS_BOTTOM);
 textW(dc,TR(L"Подъём",L"Lift"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 SetRect(&r,268,SETTINGS_TOP,370,SETTINGS_BOTTOM);
 textW(dc,TR(L"Смещение назад",L"Rear offset"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 SelectObject(dc,bold);
 _snwprintf(wide,255,TR(L"%.1f м",L"%.1f m"),dropHeight());wide[255]=0;
 SetRect(&r,100,SETTINGS_TOP,190,SETTINGS_BOTTOM);
 textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT,placement.body?COL_MUTED:COL_TEXT);
 _snwprintf(wide,255,TR(L"%.1f м",L"%.1f m"),rear);wide[255]=0;
 SetRect(&r,370,SETTINGS_TOP,426,SETTINGS_BOTTOM);
 textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT,placement.body?COL_MUTED:COL_TEXT);

 /* Автообновление радара: ползунок 0..5 с */
 fillRound(dc,MAP_LEFT,AUTO_TOP,MAP_RIGHT,AUTO_BOTTOM,12,COL_CARD,COL_BORDER);
 SelectObject(dc,small);
 SetRect(&r,26,AUTO_TOP,130,AUTO_BOTTOM);
 textW(dc,TR(L"Автообновление",L"Auto refresh"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 SelectObject(dc,bold);
 if(autoRefresh)_snwprintf(wide,255,TR(L"каждые %d с",L"every %d s"),autoRefresh);
 else lstrcpyW(wide,TR(L"вручную",L"manual"));
 wide[255]=0;
 SetRect(&r,140,AUTO_TOP,250,AUTO_BOTTOM);
 textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER,autoRefresh?COL_GOOD:COL_TEXT);
 {
  int knob=SLIDER_LEFT+autoRefresh*SLIDER_STEP,end=SLIDER_LEFT+SLIDER_MAX*SLIDER_STEP;
  HPEN pen;HGDIOBJ oldPen;HBRUSH fill;HGDIOBJ oldBrush;
  fillRound(dc,SLIDER_LEFT,SLIDER_CY-2,end+1,SLIDER_CY+3,4,COL_BORDER,COL_BORDER);
  if(knob>SLIDER_LEFT)fillRound(dc,SLIDER_LEFT,SLIDER_CY-2,knob+1,SLIDER_CY+3,4,COL_ACCENT,COL_ACCENT);
  SelectObject(dc,small);
  for(i=0;i<=SLIDER_MAX;i++){
   int x=SLIDER_LEFT+i*SLIDER_STEP;
   WCHAR digit[2]={(WCHAR)(L'0'+i),0};
   pen=CreatePen(PS_SOLID,1,i<=autoRefresh?COL_ACCENT:COL_MUTED);oldPen=SelectObject(dc,pen);
   MoveToEx(dc,x,SLIDER_CY+5,0);LineTo(dc,x,SLIDER_CY+9);
   SelectObject(dc,oldPen);DeleteObject(pen);
   SetRect(&r,x-8,SLIDER_CY+7,x+9,AUTO_BOTTOM);
   textW(dc,digit,&r,DT_SINGLELINE|DT_CENTER|DT_VCENTER,i==autoRefresh?COL_TEXT:COL_MUTED);
  }
  pen=CreatePen(PS_SOLID,2,RGB(20,20,22));fill=CreateSolidBrush(sliderDragging?RGB(255,190,90):COL_ACCENT);
  oldPen=SelectObject(dc,pen);oldBrush=SelectObject(dc,fill);
  Ellipse(dc,knob-8,SLIDER_CY-8,knob+9,SLIDER_CY+9);
  SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(pen);DeleteObject(fill);
 }

 /* Память позиции (раскладка в кузове) */
 fillRound(dc,MAP_LEFT,BED_TOP,MAP_RIGHT,BED_BOTTOM,12,COL_CARD,COL_BORDER);
 SelectObject(dc,small);
 SetRect(&r,26,BED_TOP,124,BED_BOTTOM);
 textW(dc,TR(L"Память позиции",L"Position memory"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 SelectObject(dc,bold);
 if(!bedLayoutOn)lstrcpyW(wide,TR(L"выкл",L"off"));
 else if(bed.count && bed.truckBody==truck.body)_snwprintf(wide,255,bed.layer?TR(L"%d шт, %d яр.",L"%d pcs · L%d"):TR(L"%d шт",L"%d pcs"),bed.count,bed.layer+1);
 else lstrcpyW(wide,TR(L"пусто",L"empty"));
 wide[255]=0;
 SetRect(&r,130,BED_TOP,212,BED_BOTTOM);
 textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,bedLayoutOn?COL_TEXT:COL_MUTED);
 SelectObject(dc,small);
 _snwprintf(wide,255,TR(L"\x2194 %.1f м",L"\x2194 %.1f m"),bedWidth);wide[255]=0;
 SetRect(&r,206,BED_TOP,258,BED_BOTTOM);
 textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT,COL_MUTED);

 /* Сеть: режим и состояние связи с другом */
 fillRound(dc,MAP_LEFT,NET_TOP,MAP_RIGHT,NET_BOTTOM,12,COL_CARD,COL_BORDER);
 SelectObject(dc,small);
 SetRect(&r,26,NET_TOP,88,NET_BOTTOM);
 textW(dc,TR(L"Сеть",L"Network"),&r,DT_SINGLELINE|DT_VCENTER,COL_MUTED);
 {COLORREF c;netStatusText(wide,256,&c);
  SetRect(&r,276,NET_TOP,432,NET_BOTTOM);
  textW(dc,wide,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,c);}

 SelectObject(dc,oldFont);
 DeleteObject(title);DeleteObject(normal);DeleteObject(small);DeleteObject(bold);
 SetMapMode(dc,MM_TEXT);
 SetViewportOrgEx(dc,0,0,0);
 BitBlt(target,0,0,width,height,dc,0,0,SRCCOPY);
 SelectObject(dc,oldBitmap);
 DeleteObject(bitmap);
 DeleteDC(dc);
 EndPaint(h,&ps);
}
static void drawButton(const DRAWITEMSTRUCT *d){
 WCHAR label[64];
 RECT r=d->rcItem;
 int inKeys=keysWindow && GetParent(d->hwndItem)==keysWindow;
 int primary=inKeys?(keyCaptureRow>=0 && (int)d->CtlID==KEYS_ROW_ID+keyCaptureRow):(d->CtlID==104 || (d->CtlID==BED_TOGGLE && bedLayoutOn) || (d->CtlID==LANG_RU && !uiLang) || (d->CtlID==LANG_EN && uiLang)
                || (d->CtlID==NETB_OFF && netMode==NET_OFF) || (d->CtlID==NETB_HOST && netMode==NET_HOST) || (d->CtlID==NETB_CLIENT && netMode==NET_CLIENT)),disabled=(d->itemState&ODS_DISABLED)!=0,pressed=(d->itemState&ODS_SELECTED)!=0;
 COLORREF fill=primary?COL_ACCENT:COL_CARD,border=primary?COL_ACCENT:COL_BORDER,color=primary?RGB(24,20,14):COL_TEXT;
 HBRUSH bg=CreateSolidBrush(COL_BG);
 HGDIOBJ oldFont=0;
 if(disabled){fill=RGB(28,30,36);border=RGB(42,45,53);color=RGB(88,93,104);}
 else if(pressed){fill=primary?RGB(214,120,8):RGB(50,55,66);}
 FillRect(d->hDC,&r,bg);DeleteObject(bg);
 fillRound(d->hDC,r.left,r.top,r.right,r.bottom,10,fill,border);
 GetWindowTextW(d->hwndItem,label,64);
 SetBkMode(d->hDC,TRANSPARENT);
 {HFONT f=inKeys?keysFont:buttonFont;if(f)oldFont=SelectObject(d->hDC,f);}
 textW(d->hDC,label,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE,color);
 if(oldFont)SelectObject(d->hDC,oldFont);
}
/* ───── Окно «Клавиши»: переназначение по нажатию ───── */
enum { KEYS_W=470, KEYS_ROW_TOP=64, KEYS_ROW_H=34, KEYS_ROW_COUNT=KEY_COUNT,
       KEYS_STATUS_TOP=KEYS_ROW_TOP+KEYS_ROW_COUNT*KEYS_ROW_H+6,
       KEYS_FOOTER_TOP=KEYS_STATUS_TOP+46, KEYS_H=KEYS_FOOTER_TOP+48,
       KEYS_DEFAULTS_ID=250, KEYS_CLOSE_ID=251 };
static float keysScale=1;
static WCHAR keysStatus[160]=L"Щёлкните по клавише действия и нажмите новую.";
static COLORREF keysStatusColor=COL_MUTED;
static int ks(int v){return (int)(v*keysScale+0.5f);}
static void keysSetStatus(const WCHAR *text,COLORREF color){
 lstrcpynW(keysStatus,text,160);keysStatusColor=color;
 if(keysWindow)InvalidateRect(keysWindow,0,FALSE);
}
static void keysRefreshButtons(void){
 int i;
 WCHAR text[48];
 if(!keysWindow)return;
 for(i=0;i<KEY_COUNT;i++){
  if(i==keyCaptureRow)lstrcpyW(text,TR(L"Нажмите…",L"Press a key…"));
  else bindingTextW(i,text,48);
  SetWindowTextW(GetDlgItem(keysWindow,KEYS_ROW_ID+i),text);
  InvalidateRect(GetDlgItem(keysWindow,KEYS_ROW_ID+i),0,FALSE);
 }
}
static void keysEndCapture(void){
 if(keyCaptureRow<0)return;
 keyCaptureRow=-1;
 registerRadarHotkey();
 keysRefreshButtons();
}
static void keysBeginCapture(int row){
 WCHAR text[160];
 if(placement.body){keysSetStatus(TR(L"Сначала отпустите или отмените груз.",L"Release or cancel the cargo first."),COL_BAD);return;}
 if(keyCaptureRow==row){keysEndCapture();keysSetStatus(TR(L"Назначение отменено.",L"Assignment cancelled."),COL_MUTED);return;}
 keyCaptureRow=row;
 /* На время выбора снимаем горячую клавишу, чтобы её можно было нажать. */
 UnregisterHotKey(window,HOTKEY_RADAR);
 _snwprintf(text,160,row==K_RADAR?TR(L"«%ls»: нажмите клавишу, можно с Ctrl/Alt/Shift",L"“%ls”: press a key, Ctrl/Alt/Shift allowed")
                                 :TR(L"«%ls»: нажмите клавишу. Повторный щелчок — отмена.",L"“%ls”: press a key. Click again to cancel."),keyTitle(row));text[159]=0;
 keysSetStatus(text,COL_ACCENT);
 keysRefreshButtons();
 SetFocus(keysWindow);
}
/* Вызывается из цикла сообщений для WM_KEYDOWN/WM_SYSKEYDOWN, пока идёт выбор клавиши. */
static void keysCapture(UINT vk){
 WCHAR err[96],text[160],name[48];
 UINT mods=0,oldVk,oldMods;
 int row=keyCaptureRow;
 if(row<0)return;
 if(row==K_RADAR){
  if(vk==VK_SHIFT || vk==VK_CONTROL || vk==VK_MENU)return; /* ждём основную клавишу */
  if(GetKeyState(VK_CONTROL)<0)mods|=MOD_CONTROL;
  if(GetKeyState(VK_MENU)<0)mods|=MOD_ALT;
  if(GetKeyState(VK_SHIFT)<0)mods|=MOD_SHIFT;
 }
 if(!validBinding(row,vk,mods,err,96)){keysSetStatus(err,COL_BAD);return;}
 oldVk=keyBindings[row].vk;oldMods=keyBindings[row].mods;
 keyBindings[row].vk=vk;keyBindings[row].mods=mods;
 keyCaptureRow=-1;
 if(!registerRadarHotkey()){
  keyBindings[row].vk=oldVk;keyBindings[row].mods=oldMods;
  registerRadarHotkey();
  keysSetStatus(row==K_RADAR?TR(L"Это сочетание уже занято другой программой — выберите другое.",L"Another program already uses this shortcut — pick a different one."):TR(L"Не удалось вернуть горячую клавишу окна.",L"Could not restore the window hotkey."),COL_BAD);
  keysRefreshButtons();
  return;
 }
 saveKeyBindings();
 applyPlacementKeys();
 bindingTextW(row,name,48);
 _snwprintf(text,160,TR(L"«%ls» — %ls. Сохранено в .ini.",L"“%ls” — %ls. Saved to .ini."),keyTitle(row),name);text[159]=0;
 keysSetStatus(text,COL_GOOD);
 keysRefreshButtons();
}
static void keysRestoreDefaults(void){
 int i;
 if(placement.body){keysSetStatus(TR(L"Сначала отпустите или отмените груз.",L"Release or cancel the cargo first."),COL_BAD);return;}
 keyCaptureRow=-1;
 for(i=0;i<KEY_COUNT;i++){keyBindings[i].vk=keyBindings[i].defaultVk;keyBindings[i].mods=0;}
 if(!registerRadarHotkey())keysSetStatus(TR(L"F1 занята другой программой — назначьте окну другую клавишу.",L"F1 is used by another program — assign the window a different key."),COL_BAD);
 else keysSetStatus(TR(L"Все клавиши возвращены по умолчанию.",L"All keys reset to defaults."),COL_GOOD);
 saveKeyBindings();
 applyPlacementKeys();
 keysRefreshButtons();
}
static void keysPaint(HWND h){
 PAINTSTRUCT ps;
 HDC target=BeginPaint(h,&ps),dc;
 RECT client,r;
 HBITMAP bitmap;
 HGDIOBJ oldBitmap,oldFont;
 HFONT title,normal,small;
 HBRUSH brush;
 int i,w,hh;
 GetClientRect(h,&client);
 w=client.right>0?client.right:1;hh=client.bottom>0?client.bottom:1;
 dc=CreateCompatibleDC(target);
 bitmap=CreateCompatibleBitmap(target,w,hh);
 oldBitmap=SelectObject(dc,bitmap);
 brush=CreateSolidBrush(COL_BG);FillRect(dc,&client,brush);DeleteObject(brush);
 SetMapMode(dc,MM_ANISOTROPIC);
 SetWindowExtEx(dc,KEYS_W,KEYS_H,0);
 SetViewportExtEx(dc,ks(KEYS_W),ks(KEYS_H),0);
 SetBkMode(dc,TRANSPARENT);
 title=radarFontW(20,FW_BOLD);normal=radarFontW(15,FW_NORMAL);small=radarFontW(13,FW_NORMAL);
 oldFont=SelectObject(dc,title);
 SetRect(&r,0,0,KEYS_W,46);
 brush=CreateSolidBrush(COL_CARD);FillRect(dc,&r,brush);DeleteObject(brush);
 SetRect(&r,0,43,KEYS_W,46);
 brush=CreateSolidBrush(COL_ACCENT);FillRect(dc,&r,brush);DeleteObject(brush);
 SetRect(&r,16,0,KEYS_W,43);
 textW(dc,TR(L"Клавиши",L"Keys"),&r,DT_SINGLELINE|DT_VCENTER,COL_TEXT);
 SelectObject(dc,small);
 SetRect(&r,120,0,KEYS_W-16,43);
 textW(dc,TR(L"геймпад: LB+RB+Y, D-pad, LT/RT, правый стик, A, X",L"gamepad: LB+RB+Y, D-pad, LT/RT, right stick, A, X"),&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT,COL_MUTED);
 SelectObject(dc,normal);
 for(i=0;i<KEYS_ROW_COUNT;i++){
  int top=KEYS_ROW_TOP+i*KEYS_ROW_H;
  if(i==K_FORWARD || i==K_RELEASE){
   HPEN pen=CreatePen(PS_SOLID,1,COL_BORDER);HGDIOBJ op=SelectObject(dc,pen);
   MoveToEx(dc,16,top-3,0);LineTo(dc,KEYS_W-16,top-3);
   SelectObject(dc,op);DeleteObject(pen);
  }
  SetRect(&r,20,top,236,top+KEYS_ROW_H-4);
  textW(dc,keyTitle(i),&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,i==keyCaptureRow?COL_ACCENT:COL_TEXT);
 }
 SelectObject(dc,small);
 fillRound(dc,12,KEYS_STATUS_TOP,KEYS_W-12,KEYS_STATUS_TOP+38,10,COL_CARD,COL_BORDER);
 SetRect(&r,24,KEYS_STATUS_TOP,KEYS_W-24,KEYS_STATUS_TOP+38);
 textW(dc,keysStatus,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,keysStatusColor);
 SelectObject(dc,oldFont);
 DeleteObject(title);DeleteObject(normal);DeleteObject(small);
 SetMapMode(dc,MM_TEXT);
 BitBlt(target,0,0,w,hh,dc,0,0,SRCCOPY);
 SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
 EndPaint(h,&ps);
}
static LRESULT CALLBACK keysProc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_PAINT){keysPaint(h);return 0;}
 if(m==WM_ERASEBKGND)return 1;
 if(m==WM_DRAWITEM){drawButton((const DRAWITEMSTRUCT*)l);return TRUE;}
 if(m==WM_COMMAND){
  int id=LOWORD(w);
  if(id>=KEYS_ROW_ID && id<KEYS_ROW_ID+KEY_COUNT)keysBeginCapture(id-KEYS_ROW_ID);
  else if(id==KEYS_DEFAULTS_ID)keysRestoreDefaults();
  else if(id==KEYS_CLOSE_ID)DestroyWindow(h);
  return 0;
 }
 if(m==WM_ACTIVATE && LOWORD(w)==WA_INACTIVE && keyCaptureRow>=0){
  keysEndCapture();keysSetStatus(TR(L"Назначение отменено.",L"Assignment cancelled."),COL_MUTED);
 }
 if(m==WM_SETCURSOR && LOWORD(l)==HTCLIENT){SetCursor(radarCursor);return TRUE;}
 if(m==WM_CLOSE){DestroyWindow(h);return 0;}
 if(m==WM_DESTROY){
  keysEndCapture();
  keysWindow=0;
  if(keysFont){DeleteObject(keysFont);keysFont=0;}
  return 0;
 }
 return DefWindowProcW(h,m,w,l);
}
static void openKeysWindow(void){
 static int registered;
 typedef UINT (WINAPI *DpiFn)(HWND);
 DpiFn getDpi=(DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow");
 RECT r,owner;
 int i;
 if(keysWindow){ShowWindow(keysWindow,SW_SHOW);SetForegroundWindow(keysWindow);return;}
 if(!registered){
  WNDCLASSW wc;
  ZeroMemory(&wc,sizeof(wc));
  wc.lpfnWndProc=keysProc;wc.hInstance=self;wc.lpszClassName=L"KOTEuKA_CargoLoadKeys";
  wc.hCursor=radarCursor;wc.hbrBackground=CreateSolidBrush(COL_BG);
  registered=RegisterClassW(&wc)!=0;
 }
 keysScale=getDpi?getDpi(window)/96.0f:1.0f;
 if(keysScale<1)keysScale=1;
 SetRect(&r,0,0,ks(KEYS_W),ks(KEYS_H));
 AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_TOPMOST);
 GetWindowRect(window,&owner);
 keysWindow=CreateWindowExW(WS_EX_TOPMOST,L"KOTEuKA_CargoLoadKeys",TR(L"KOTEuKA_CargoLoad — клавиши",L"KOTEuKA_CargoLoad — keys"),WS_CAPTION|WS_SYSMENU,
                            owner.left+40,owner.top+60,r.right-r.left,r.bottom-r.top,window,0,self,0);
 if(!keysWindow)return;
 keysFont=radarFontW(ks(14),FW_SEMIBOLD);
 for(i=0;i<KEY_COUNT;i++){
  int top=KEYS_ROW_TOP+i*KEYS_ROW_H;
  HWND b=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,ks(244),ks(top),ks(210),ks(KEYS_ROW_H-6),keysWindow,(HMENU)(uintptr_t)(KEYS_ROW_ID+i),self,0);
  (void)b;
 }
 CreateWindowW(L"BUTTON",TR(L"По умолчанию",L"Defaults"),WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,ks(12),ks(KEYS_FOOTER_TOP),ks(160),ks(34),keysWindow,(HMENU)KEYS_DEFAULTS_ID,self,0);
 CreateWindowW(L"BUTTON",TR(L"Закрыть",L"Close"),WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,ks(KEYS_W-12-120),ks(KEYS_FOOTER_TOP),ks(120),ks(34),keysWindow,(HMENU)KEYS_CLOSE_ID,self,0);
 keysSetStatus(keyLoadProblems?TR(L"Часть значений в .ini не распознана — для них взяты стандартные.",L"Some .ini values were not recognised — defaults used for them."):TR(L"Щёлкните по клавише действия и нажмите новую.",L"Click an action's key, then press the new one."),keyLoadProblems?COL_WARN:COL_MUTED);
 keysRefreshButtons();
 ShowWindow(keysWindow,SW_SHOW);
 SetForegroundWindow(keysWindow);
}
#include "net_ui.inc"
/* Язык интерфейса: [UI] Language=ru|en; без записи — по языку Windows. */
static void loadLanguage(void){
 WCHAR v[8];
 GetPrivateProfileStringW(L"UI",L"Language",L"",v,8,ini);
 if(!lstrcmpiW(v,L"en"))uiLang=1;
 else if(!lstrcmpiW(v,L"ru"))uiLang=0;
 else uiLang=PRIMARYLANGID(GetUserDefaultUILanguage())!=LANG_RUSSIAN;
}
/* Переписывает все подписи окна радара на текущем языке. */
static void applyLanguageTexts(void){
 int i;
 SetWindowTextW(GetDlgItem(window,101),TR(L"Обновить",L"Refresh"));
 SetWindowTextW(GetDlgItem(window,102),TR(L"« Предыдущий",L"« Previous"));
 SetWindowTextW(GetDlgItem(window,103),TR(L"Следующий »",L"Next »"));
 for(i=0;i<SETTING_COUNT;i++)SetWindowTextW(GetDlgItem(window,settingButtons[i].id),uiLang?settingButtons[i].labelEn:settingButtons[i].label);
 placementControls();
 applyPlacementKeys();
 for(i=0;i<BUTTON_COUNT;i++)InvalidateRect(GetDlgItem(window,buttonIds[i]),0,FALSE);
 for(i=0;i<SETTING_COUNT;i++)InvalidateRect(GetDlgItem(window,settingButtons[i].id),0,FALSE);
}
static void setLanguage(int english){
 int reopenKeys=keysWindow!=0;
 if(english==uiLang)return;
 uiLang=english;
 WritePrivateProfileStringW(L"UI",L"Language",uiLang?L"en":L"ru",ini);
 if(reopenKeys)DestroyWindow(keysWindow);
 if(netWindow)DestroyWindow(netWindow);
 applyLanguageTexts();
 if(!placement.body && gameAddrReady)refresh();
 strcpy(message,TR("Язык интерфейса: русский","Interface language: English"));
 InvalidateRect(window,0,TRUE);
 if(reopenKeys)openKeysWindow();
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){int i;
 if(m==WM_PAINT){paint(h);return 0;}
 if(m==WM_ERASEBKGND)return 1;
 if(m==WM_DRAWITEM){drawButton((const DRAWITEMSTRUCT*)l);return TRUE;}
 if(m==WM_ACTIVATE){
  if(LOWORD(w)==WA_INACTIVE){KillTimer(h,CURSOR_TIMER);releaseRadarCursor();}
  else if(!HIWORD(w)){ClipCursor(0);SetTimer(h,CURSOR_TIMER,100,0);revealRadarCursor(0);}
 }
 if(m==WM_SETCURSOR && LOWORD(l)==HTCLIENT && GetForegroundWindow()==h){revealRadarCursor(1);return TRUE;}
 if(m==WM_SIZE){if(w!=SIZE_MINIMIZED)layoutButtons(h);return 0;}
 if(m==WM_EXITSIZEMOVE){saveWindowPlacement();return 0;}
 if(m==WM_GETMINMAXINFO){
  MINMAXINFO *sizes=(MINMAXINFO*)l;
  sizes->ptMinTrackSize.x=380;
  sizes->ptMinTrackSize.y=560;
  return 0;
 }
 if(m==0x02e0){ /* WM_DPICHANGED */
  RECT *r=(RECT*)l;
  SetWindowPos(h,0,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
  return 0;
 }
 if(m==0x020a){mapScale*=((short)HIWORD(w)>0)?1.25f:0.8f;if(mapScale<3.4f)mapScale=3.4f;if(mapScale>35)mapScale=35;InvalidateRect(h,0,TRUE);return 0;}
 if(m==WM_APP+2){
  cameraSearchPending=0;
  vehicleCamera=(uintptr_t)w;
  if(vehicleCamera)refresh();
  else if(truckCount && !truckReady){
   detectedTruck=TR("Камера недоступна","Camera unavailable");
   strcpy(message,TR("Не удалось определить грузовик. Нажмите «Обновить».","Could not identify the truck. Press “Refresh”."));
  }
  InvalidateRect(h,0,TRUE);
  return 0;
 }
 if(m==WM_COMMAND){
  int id=LOWORD(w);
  switch(id){
  case 101:refresh();InvalidateRect(h,0,TRUE);break;
  case 102:cycle(-1);break;
  case 103:cycle(1);break;
  case 104:loadSelected();break;
  case MOVE_FORWARD:case MOVE_LEFT:case MOVE_BACK:case MOVE_RIGHT:
  case TURN_LEFT:case TURN_RIGHT:case RAISE_CARGO:case LOWER_CARGO:case TILT_LEFT:case TILT_RIGHT:case PITCH_FORWARD:case PITCH_BACK:
   nudgePlacement(id);break;
  case HEIGHT_DOWN:case HEIGHT_UP:case REAR_DOWN:case REAR_UP:
  case KEYS_BUTTON:openKeysWindow();break;
  case LANG_RU:case LANG_EN:setLanguage(id==LANG_EN);break;
  case NETB_OFF:netSetMode(NET_OFF);break;
  case NETB_HOST:netSetMode(NET_HOST);break;
  case NETB_CLIENT:netSetMode(NET_CLIENT);break;
  case NETB_SETUP:openNetWindow();break;
  case BED_NARROW:case BED_WIDEN:case BED_TOGGLE:case BED_RESET:
   changeSetting(id);break;
  }
  return 0;
 }
 /* Ползунок автообновления: клик или перетаскивание по строке ползунка. */
 if(m==WM_LBUTTONDOWN){
  int mx,my;
  if(logicalPoint(h,l,&mx,&my) && my>=AUTO_TOP && my<AUTO_BOTTOM
     && mx>=SLIDER_LEFT-16 && mx<=SLIDER_LEFT+SLIDER_MAX*SLIDER_STEP+16){
   sliderDragging=1;
   SetCapture(h);
   setAutoRefresh(sliderValueAt(mx),0);
   return 0;
  }
 }
 if(m==WM_MOUSEMOVE && sliderDragging){
  int mx,my;
  if(logicalPoint(h,l,&mx,&my))setAutoRefresh(sliderValueAt(mx),0);
  return 0;
 }
 if((m==WM_LBUTTONUP || m==WM_CAPTURECHANGED) && sliderDragging){
  sliderDragging=0;
  if(m==WM_LBUTTONUP)ReleaseCapture();
  setAutoRefresh(autoRefresh,1);
  if(autoRefresh && !placement.body){refresh();InvalidateRect(h,0,TRUE);}
  return 0;
 }
 if(m==WM_TIMER && w==REFRESH_TIMER){
  /* Не трогаем радар, пока окно скрыто, груз в воздухе или идёт поиск камеры. */
  if(IsWindowVisible(h) && !IsIconic(h) && !placement.body && !sliderDragging && !cameraSearchPending){
   refresh();
   InvalidateRect(h,0,TRUE);
  }
  return 0;
 }
 if(m==WM_LBUTTONDOWN && truckReady && !placement.body){
  RECT view=radarViewport(h);
  int width=view.right-view.left,height=view.bottom-view.top,mx,my,best=-1;
  float distance=400;
  if(width<=0 || height<=0)return 0;
  mx=MulDiv((short)LOWORD(l)-view.left,RADAR_WIDTH,width);
  my=MulDiv((short)HIWORD(l)-view.top,RADAR_HEIGHT,height);
  if(mx<MAP_LEFT || mx>=MAP_RIGHT || my<MAP_TOP || my>=MAP_BOTTOM)return 0;
  for(i=0;i<count;i++){int x,y;float d;mapPoint(logs[i].center,&x,&y);d=(x-mx)*(x-mx)+(y-my)*(y-my);if(d<distance){distance=d;best=i;}}
  if(best>=0){selected=best;selectedBody=logs[best].body;InvalidateRect(h,0,TRUE);}return 0;
 }
 if(m==WM_TIMER && w==PLACEMENT_TIMER){pollPlacementKeys();return 0;}
 if(m==WM_TIMER && w==GAMEPAD_TIMER){pollGamepad();return 0;}
 if(m==WM_TIMER && w==CURSOR_TIMER){revealRadarCursor(0);return 0;}
 if(m==WM_TIMER && w==NET_TIMER){netTick();return 0;}
 if(m==WM_HOTKEY && w==HOTKEY_RADAR){
  DWORD pid=0;
  HWND foreground=GetForegroundWindow();
  GetWindowThreadProcessId(foreground,&pid);
  if(pid!=GetCurrentProcessId())return 0;
  toggleRadar(foreground);
  return 0;
 }
 if(m==WM_CLOSE){if(finishPlacement(1))hideRadar();return 0;}
 return DefWindowProcW(h,m,w,l);
}
static DWORD WINAPI run(void *arg){
 WNDCLASSW wc;MSG msg;DWORD n;int i;
 HMODULE xinput;
 typedef HANDLE (WINAPI *DpiContextFn)(HANDLE);
 DpiContextFn setDpiContext=(DpiContextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext");
 static const WCHAR *labels[]={L"Погрузить",L"Обновить",L"« Предыдущий",L"Следующий »",
                              L"\x2191 Вперёд  W",L"\x2190 Влево  A",L"\x2193 Назад  S",L"\x2192 Вправо  D",
                              L"Поворот \x2190  Q",L"Поворот \x2192  E",L"Выше  R",L"Ниже  F",L"Наклон \x2190  Z",L"Наклон \x2192  C",L"Наклон \x2191  T",L"Наклон \x2193  G"};
 if(setDpiContext)setDpiContext((HANDLE)(intptr_t)-4);
 base=(BYTE*)GetModuleHandleW(0);n=GetModuleFileNameW(self,ini,MAX_PATH);if(!n || n>=MAX_PATH)return 1;while(n && ini[n-1]!=L'\\')n--;ini[n]=0;lstrcpyW(profilePath,ini);lstrcatW(profilePath,L"KOTEuKA_CargoLoad.profiles");lstrcatW(ini,L"KOTEuKA_CargoLoad.ini");
 loadLanguage();
 detectedTruck=TR("Ожидание карты","Waiting for map");
 strcpy(message,TR("Нажмите «Обновить», чтобы найти грузы рядом","Press “Refresh” to find nearby cargo"));
 {WCHAR logPath[MAX_PATH];lstrcpyW(logPath,profilePath);lstrcpyW(logPath+lstrlenW(logPath)-8,L"log");
  resolveGameAddresses();writeAddressLog(logPath);
  if(!gameAddrReady)strcpy(message,gameAddrError);}
 rear=GetPrivateProfileIntW(L"Loading",L"RearOffsetCm",350,ini)/100.0f;height=GetPrivateProfileIntW(L"Loading",L"DropHeightCm",500,ini)/100.0f;
 if(rear<0 || rear>10 || height<1 || height>10)return 1;
 loadKeyBindings();
 bedLayoutOn=GetPrivateProfileIntW(L"Loading",L"BedLayout",1,ini)!=0;
 bedWidth=GetPrivateProfileIntW(L"Loading",L"BedWidthCm",240,ini)/100.0f;
 if(bedWidth<1.0f || bedWidth>4.0f)bedWidth=2.4f;
 autoRefresh=GetPrivateProfileIntW(L"Radar",L"AutoRefreshSec",0,ini);
 if(autoRefresh<0 || autoRefresh>SLIDER_MAX)autoRefresh=0;
 radarCursor=LoadCursorW(0,MAKEINTRESOURCEW(32512));
 ZeroMemory(&wc,sizeof(wc));wc.lpfnWndProc=proc;wc.hInstance=self;wc.lpszClassName=L"KOTEuKA_CargoLoadWindow";wc.hCursor=radarCursor;wc.hbrBackground=CreateSolidBrush(COL_BG);RegisterClassW(&wc);
 window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"KOTEuKA_CargoLoad — F1",radarStyle,CW_USEDEFAULT,CW_USEDEFAULT,505,535,0,0,self,0);
 if(!window)return 1;
 for(i=0;i<BUTTON_COUNT;i++)CreateWindowW(L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,1,1,window,(HMENU)(uintptr_t)buttonIds[i],self,0);
 for(i=0;i<SETTING_COUNT;i++)CreateWindowW(L"BUTTON",uiLang?settingButtons[i].labelEn:settingButtons[i].label,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,1,1,window,(HMENU)(uintptr_t)settingButtons[i].id,self,0);
 fitClient(window);
 restoreWindowPlacement();
 placementControls();
 applyAutoRefresh();
 if(!registerRadarHotkey()){
  /* Сохранённое сочетание занято другой программой — пробуем F1, чтобы окно можно было открыть. */
  UINT oldVk=keyBindings[K_RADAR].vk,oldMods=keyBindings[K_RADAR].mods;
  WCHAR err[96];
  keyBindings[K_RADAR].vk=0;
  if(validBinding(K_RADAR,VK_F1,0,err,96)){keyBindings[K_RADAR].vk=VK_F1;keyBindings[K_RADAR].mods=0;}
  else{keyBindings[K_RADAR].vk=oldVk;keyBindings[K_RADAR].mods=oldMods;}
  if(registerRadarHotkey() && (oldVk!=VK_F1 || oldMods))strcpy(message,TR("Сохранённая клавиша окна занята другой программой — временно F1","The saved window key is used by another program — F1 for now"));
  else{keyBindings[K_RADAR].vk=oldVk;keyBindings[K_RADAR].mods=oldMods;strcpy(message,TR("Горячая клавиша окна занята другой программой — откройте окно геймпадом (LB+RB+Y)","The window hotkey is used by another program — open it with the gamepad (LB+RB+Y)"));}
 }
 applyLanguageTexts();
 {WCHAR logPath[MAX_PATH];lstrcpyW(logPath,profilePath);lstrcpyW(logPath+lstrlenW(logPath)-8,L"log");netStart(logPath);}
 xinput=LoadLibraryExW(L"XInput1_4.dll",0,0x800); /* LOAD_LIBRARY_SEARCH_SYSTEM32 */
 if(xinput)readPad=(PadGetState)GetProcAddress(xinput,"XInputGetState");
 if(readPad)SetTimer(window,GAMEPAD_TIMER,15,0);
 while(GetMessageW(&msg,0,0,0)>0){
  if(keyCaptureRow>=0 && keysWindow && (msg.hwnd==keysWindow || IsChild(keysWindow,msg.hwnd))){
   if(msg.message==WM_KEYDOWN || msg.message==WM_SYSKEYDOWN){keysCapture((UINT)msg.wParam);continue;}
   if(msg.message==WM_KEYUP || msg.message==WM_SYSKEYUP || msg.message==WM_CHAR || msg.message==WM_SYSCHAR)continue;
  }
  if(placement.body && placement.mode!=PM_REMOTE && msg.message==WM_KEYDOWN && (msg.hwnd==window || IsChild(window,msg.hwnd))){
   int direction=placementDirection((DWORD)msg.wParam);
   if(direction){nudgePlacement(direction);continue;}
   if(msg.wParam==placementKeys[PK_CANCEL].key || msg.wParam==placementKeys[PK_RELEASE].key){
    if(!(msg.lParam & (1L<<30)))finishPlacement(msg.wParam==placementKeys[PK_CANCEL].key);
    continue;
   }
  }
  TranslateMessage(&msg);
  DispatchMessageW(&msg);
 }
 releaseRadarCursor();
 InterlockedExchangePointer(&placementGameWindow,0);
 return 0;
}
#ifdef AUTOLOAD_TEST
__declspec(dllexport) void testAdjustedPose(float *basePosition,float *baseRotation,float *baseCenter,
                                           float *heading,float *offsets,int turnSteps,
                                           float *outPosition,float *outRotation,float *outCenter){
 Placement target={0};
 Vec position,rotation,center,axis;
 memcpy(&target.position,basePosition,16);
 memcpy(&target.rotation,baseRotation,16);
 memcpy(&target.center,baseCenter,16);
 target.forwardX=heading[0];target.forwardZ=heading[1];
 target.along=offsets[0];target.across=offsets[1];target.lift=offsets[2];
 target.turnSteps=turnSteps;
 adjustedPose(&target,&position,&rotation,&center,&axis);
 memcpy(outPosition,&position,16);memcpy(outRotation,&rotation,16);memcpy(outCenter,&center,16);
}
__declspec(dllexport) int testLoadingPose(float *matrix,float *center,float *position,float *rotation){Item t;Vec com,p,q;memcpy(t.pose,matrix,64);memcpy(&com,center,16);if(!cargoLoadingPose(&t,&legacyTruck,com,1,2,-1,&p,&q,0))return 0;memcpy(position,&p,16);memcpy(rotation,&q,16);return 1;}
__declspec(dllexport) int testCargoPose(float *matrix,float *center,int forward,int up,float *position,float *rotation){Item t;Vec com,p,q;memcpy(t.pose,matrix,64);memcpy(&com,center,16);if(!cargoLoadingPose(&t,&legacyTruck,com,forward,up,1,&p,&q,0))return 0;memcpy(position,&p,16);memcpy(rotation,&q,16);return 1;}
__declspec(dllexport) const char *testTruckTarget(uintptr_t body,float *inertia,float *matrix,float *outCenter){
 const TruckProfile *profile;
 Item t;
 Vec v,com={0},p,q,center={0};
 memcpy(&v,inertia,16);memcpy(t.pose,matrix,64);
 profile=truckProfileFor(body,v);
 if(!profile || !cargoLoadingPose(&t,profile,com,2,1,1,&p,&q,&center))return 0;
 memcpy(outCenter,&center,16);
 return profile->label;
}
__declspec(dllexport) void testReadProfiles(WCHAR *path){lstrcpynW(profilePath,path,MAX_PATH);readProfiles();}
__declspec(dllexport) int testCargoKind(uintptr_t body,float *v,uintptr_t userdata){Vec inertia;memcpy(&inertia,v,16);return cargoKind(body,inertia,userdata);}
#endif
/* version.dll из папки игры подхватывают и другие программы оттуда (Roadcraft.exe, crash_reporter.exe,
   редактор). Мод работает только в самой игре: иначе лишние окна, горячие клавиши, сеть и журнал. */
static int isGameProcess(void){
 WCHAR path[MAX_PATH];DWORD n=GetModuleFileNameW(0,path,MAX_PATH);const WCHAR *name=path;DWORD i;
 if(!n || n>=MAX_PATH)return 0;
 for(i=0;i<n;i++)if(path[i]==L'\\' || path[i]==L'/')name=path+i+1;
 return !lstrcmpiW(name,L"Roadcraft - Retail.exe");
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID reserved){if(reason==DLL_PROCESS_ATTACH){self=h;DisableThreadLibraryCalls(h);
#ifndef AUTOLOAD_TEST
 if(isGameProcess()){HANDLE t=CreateThread(0,0,run,0,0,0);if(t)CloseHandle(t);}
#endif
 }return TRUE;}

