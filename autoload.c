#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
typedef struct { const char *bone; float mass,bedZ; int hasBed; const char *label; } TruckProfile;
typedef struct { Item item; const TruckProfile *profile; } TruckTarget;
static const TruckProfile truckProfiles[]={
#include "truck_platforms.inc"
};
static const TruckProfile legacyTruck={"BoneBody_cdt",4500,0,1,"Voron 3327 Crane Cargo"};
static const TruckProfile genericTruck={0,0,0,0,"Truck chassis"};
static TruckTarget truckTargets[128];
static int truckCount;
static const TruckProfile *truckProfile;
static const char *detectedTruck="Waiting for map";
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
static char message[180]="Refresh to find nearby cargo";
static float rear=3.5f,height=5.0f,range=25.0f;
static float mapScale=10.0f;
typedef struct {
 uintptr_t body,shape;
 BYTE originalMotion;
 Vec originalPosition,originalRotation,originalCenter,originalAxis;
 Vec position,rotation,center;
 float forwardX,forwardZ,along,across,lift;
 int turnSteps;
} Placement;
static Placement placement;
enum { RADAR_WIDTH=490, RADAR_HEIGHT=588, MAP_TOP=62 };
enum { MOVE_FORWARD=105,MOVE_LEFT,MOVE_BACK,MOVE_RIGHT,TURN_LEFT,TURN_RIGHT,RAISE_CARGO,LOWER_CARGO };
enum { HOTKEY_RADAR=1, PLACEMENT_TIMER=1, GAMEPAD_TIMER=2, CURSOR_TIMER=3 };
typedef struct { BYTE key,scan; int direction; } PlacementKey;
static const PlacementKey placementKeys[]={
 {'W',0x11,MOVE_FORWARD},{'A',0x1e,MOVE_LEFT},{'S',0x1f,MOVE_BACK},{'D',0x20,MOVE_RIGHT},
 {'Q',0x10,TURN_LEFT},{'E',0x12,TURN_RIGHT},{'R',0x13,RAISE_CARGO},{'F',0x21,LOWER_CARGO},
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
       PAD_OPEN=PAD_LB|PAD_RB|PAD_Y,PAD_DPAD=PAD_UP|PAD_DOWN|PAD_LEFT|PAD_RIGHT };
static const struct { WORD button; int direction; } padMoves[]={
 {PAD_UP,MOVE_FORWARD},{PAD_DOWN,MOVE_BACK},{PAD_LEFT,MOVE_LEFT},{PAD_RIGHT,MOVE_RIGHT},
 {PAD_LB,LOWER_CARGO},{PAD_RB,RAISE_CARGO},{PAD_LT,TURN_LEFT},{PAD_RT,TURN_RIGHT}
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
                              TURN_LEFT,TURN_RIGHT,RAISE_CARGO,LOWER_CARGO};
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
static HFONT radarFont(int height){
 return CreateFontW(-height,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,
                    DEFAULT_PITCH,L"Segoe UI");
}
static void layoutButtons(HWND h){
 RECT view=radarViewport(h);
 int width=view.right-view.left,height=view.bottom-view.top,i;
 HFONT font;
 if(width<=0 || height<=0)return;
 font=radarFont(MulDiv(16,height,RADAR_HEIGHT));
 for(i=0;i<BUTTON_COUNT;i++){
  HWND button=GetDlgItem(h,buttonIds[i]);
  int left=12+(i%4)*119;
  int top=454+(i/4)*43;
  int x=MulDiv(left,width,RADAR_WIDTH);
  int right=MulDiv(left+107,width,RADAR_WIDTH);
  int y=MulDiv(top,height,RADAR_HEIGHT);
  int bottom=MulDiv(top+34,height,RADAR_HEIGHT);
  if(!button)continue;
  MoveWindow(button,view.left+x,view.top+y,right-x,bottom-y,FALSE);
  SendMessageW(button,WM_SETFONT,(WPARAM)(font?font:GetStockObject(DEFAULT_GUI_FONT)),FALSE);
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
 if(type==(uintptr_t)base+0x3ea8b38){
  if(!readmem(shape+0x38,&shape,8) || !readmem(shape,&type,8))return 0;
 }
 if(type!=(uintptr_t)base+0x3ea9970
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
 if(type!=(uintptr_t)base+0x3ea5808)return cargoHullMinimum(shape,minima);
 if(!readmem(shape+0x38,&list,sizeof(list)) || list.n<1 || list.n>MAX_CARGO_HULLS
    || list.n>(list.cap&0x3fffffff)
    || !readmem(list.p,children,list.n*sizeof(children[0])))return 0;
 for(i=0;i<list.n;i++)if(!cargoHullMinimum(children[i][0],&minima[i]))return 0;
 return list.n;
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
 if(userdata!=5)return -1;
 if(absf(inertia.w-1.0f/1100)<0.0000001f && inertia.x>0 && inertia.z>0 && inertia.y>50*inertia.x && inertia.y>50*inertia.z)return 0;
 if(!readmem(body+0xb8,&name,8)||!readmem(name&~(uintptr_t)1,bone,16))return -1;
 if(!memcmp(bone,"geo_body\0",9)){
  for(i=0;i<profileCount;i++){CargoProfile *p=&profiles[i];if(nearFloat(inertia.w,1/p->mass)&&nearFloat(inertia.x,p->ix)&&nearFloat(inertia.y,p->iy)&&nearFloat(inertia.z,p->iz))return i+1;}
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
 if(vt!=(uintptr_t)base+0x3eaf290 || owner!=world)return 0;
 out->body=body;memcpy(out->pose,b+0x170,64);memcpy(&out->center,b+0x1c0,16);
 out->axis.x=out->pose[4];out->axis.y=out->pose[5];out->axis.z=out->pose[6];
 for(i=0;i<3;i++)if(!_finite(((float*)&out->center)[i]) || absf(((float*)&out->center)[i])>100000)return 0;
 return 1;
}
static int signature(void){static BYTE expected[]={0x48,0x8b,0x89,0xd8,0,0,0,0xe9,0xd4,0xd4,3,0};BYTE actual[12];return readmem((uintptr_t)base+0x2cd40c0,actual,12) && !memcmp(expected,actual,12);}
static uintptr_t vehicleCamera;
static DWORD cameraSearchTime;
static int cameraSearchPending;
static int readVehicleFocus(uintptr_t camera,Vec *focus){
 uintptr_t vtable,tracker;
 BYTE flags;
 return readmem(camera,&vtable,8) && vtable==(uintptr_t)base+0x38c66e0
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
 static const uintptr_t alignments[]={0x10000,0x8000,0x1000};
 int pass;
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
 for(pass=0;pass<3;pass++){
  uintptr_t i;
  for(i=1;i<=pages[0];i++){
   uintptr_t entry=pages[i],page=entry&~(uintptr_t)0xfff,vtable;
   Vec focus;
   if((entry&0x100) || (page&(alignments[pass]-1))
      || (pass && !(page&(alignments[pass-1]-1))))continue;
   if(!readmem(page,&vtable,8) || vtable!=(uintptr_t)base+0x38c66e0
      || !readVehicleFocus(page,&focus))continue;
   if(found && found!=page){found=0;goto done;}
   found=page;
  }
  if(found)break;
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
 if(!readmem((uintptr_t)base+0x9e5d80,actual,sizeof(actual))
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
 if(!truckCount){detectedTruck="No truck found";return -1;}
 if(!vehicleFocus(&focus)){
  detectedTruck=cameraSearchPending?"Searching for camera...":"Camera unavailable";
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
 if(best<0){detectedTruck="No truck near the camera target";return -1;}
 /* A trailer is part of this target only if linked by a live joint. */
 for(i=0;i<truckCount;i++){
  uintptr_t links[64],body=truckTargets[i].item.body;
  int n;
  if(distance[i]>30.0f)continue;
  n=array(body+0x110,links,64);
  for(j=0;j<n;j++){
   uintptr_t data[8],other;
   if(!readmem(links[j],data,sizeof(data)) || data[0]!=(uintptr_t)base+0x3eae7d8)continue;
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
   if(distance[i]<closest+0.75f){detectedTruck="Ambiguous (trucks too close)";return -1;}
  }else if(truckTargets[i].profile->hasBed && (bed<0 || distance[i]<distance[bed])){
   bed=i;
  }
 }
 detectedTruck=truckTargets[bed>=0?bed:best].profile->label;
 return bed>=0?bed:best;
}
static void refresh(void){
 uintptr_t islands[4096],bodies[4096],previousWorld=world;
 int ni,i,j,k,bn,pass;
 if(placement.body)return;
 count=0;selected=-1;truckReady=0;truckCount=0;
 readProfiles();
 if(!signature()){detectedTruck="Unavailable";strcpy(message,"Unsupported game build");return;}
 if(!readmem((uintptr_t)base+0x4828a70,&world,8) || !world){detectedTruck="Waiting for map";strcpy(message,"Load a map first");return;}
 if(world!=previousWorld)selectedBody=0;
 ni=array(world+0x40,islands,2048);ni+=array(world+0x50,islands+ni,2048);
 /* Find the truck first, then inspect only nearby cargo. A large map must not
    exhaust a global candidate buffer before the local objects are visited. */
 for(pass=0;pass<2;pass++){
 for(i=0;i<ni;i++){bn=array(islands[i]+0x68,bodies,4096);for(j=0;j<bn;j++){
  Vec inertia;Item item;uintptr_t userdata;if(!pose(bodies[j],&item))continue;
  if(pass && length(item.center.x-truck.center.x,item.center.z-truck.center.z)>range)continue;
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
   if(cameraSearchPending)strcpy(message,"Finding the current truck...");
   else strcpy(message,truckCount?"Current truck unclear. Refresh to retry.":"No truck or trailer found.");
   return;
  }
  truck=truckTargets[detected].item;
  truckProfile=truckTargets[detected].profile;
  truckReady=1;
 }
 }
 /* Stable IDs during refresh: selection must not silently jump to another log. */
 selected=-1;for(i=0;i<count;i++)if(logs[i].body==selectedBody)selected=i;
 if(selected<0 && count){selected=0;selectedBody=logs[0].body;}
 if(!count)selectedBody=0;
 sprintf(message,"%d cargo objects nearby. Click to select.",count);
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
 return signature() && readmem((uintptr_t)base+0x2cd8810,actual,sizeof(actual))
        && !memcmp(expected,actual,sizeof(actual));
}
static int currentWorld(void){
 uintptr_t activeWorld;
 return world && motionQueueSupported()
        && readmem((uintptr_t)base+0x4828a70,&activeWorld,8) && activeWorld==world;
}
static void submitBodyOperation(BYTE op[64],uintptr_t body){
 QueueFn queue=(QueueFn)(base+0x2cd40c0);
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
 if(!readmem((uintptr_t)base+0x59d9a20,&alloc,8) || !alloc)return 0;
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
 int i,active=placement.body!=0;
 SetWindowTextW(GetDlgItem(window,104),active?L"Drop":L"Load");
 for(i=1;i<BUTTON_COUNT;i++)EnableWindow(GetDlgItem(window,buttonIds[i]),i<4?!active:active);
 InvalidateRect(window,0,TRUE);
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
  BYTE scan=placementKeys[i].scan;
  if(state[scan]>0 || (scan==0x1c && state[0x9c]>0))held|=1L<<i;
  state[scan]=0;
 }
 state[0x9c]=0; /* Numpad Enter. */
 previous=InterlockedExchange(&placementKeysHeld,held);
 InterlockedOr(&placementKeysPressed,held & ~previous);
 InterlockedExchange(&placementKeyboardSeen,1);
}
static int installKeyboardFilter(void){
 static const BYTE expected[]={0x40,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xf9,0x0f,0x29,0x74,0x24,0x30,0x48,0x8b,0x89,0x80,0x11,0,0};
 BYTE actual[sizeof(expected)];
 GameKeyboardUpdate *slot=(GameKeyboardUpdate*)(base+0x398f8f8);
 GameKeyboardUpdate update=(GameKeyboardUpdate)(base+0x1daac50),current;
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
static void removeStickDrift(SHORT *x,SHORT *y){
 const float deadzone=5000.0f;
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
 removeStickDrift(&sample->input.lx,&sample->input.ly);
 removeStickDrift(&sample->input.rx,&sample->input.ry);
 if(foreground==target){
  sample->input.buttons&=~(PAD_DPAD|PAD_LB|PAD_RB|PAD_A);
  sample->input.lt=sample->input.rt=0;
 }
 return result;
}
static int installGamepadFilter(void){
 PadGetState *slot=(PadGetState*)(base+0x58114c8),current;
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
 strcpy(message,"In-game controls unavailable. Use the radar buttons.");
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
 if(!placementMotion(&motion)){
  strcpy(message,"Cargo or map changed. Refresh first.");
 }else{
  if(motion!=4 && motion!=placement.originalMotion){
   strcpy(message,"Cargo motion changed. Placement cancelled.");
  }else{
   if(!cancel && motion!=4){
    strcpy(message,"Waiting for physics. Try Enter or Drop again.");
    InvalidateRect(window,0,TRUE);
    return 0;
   }
   if(cancel){
    void *pair=allocatePose(placement.originalPosition,placement.originalRotation);
    if(!pair){
     strcpy(message,"Could not cancel. Use Drop to release.");
     InvalidateRect(window,0,TRUE);
     return 0;
    }
    submitPose(placement.body,pair);
    logs[selected].center=placement.originalCenter;
    logs[selected].axis=placement.originalAxis;
   }
   changeMotion(placement.body,placement.originalMotion);
   stopMotion(placement.body);
   strcpy(message,cancel?"Placement cancelled":"Cargo released");
  }
 }
 placement.body=0;
 placementControls();
 return 1;
}
static void adjustedPose(const Placement *target,Vec *position,Vec *rotation,Vec *center,Vec *axis){
 double halfAngle=target->turnSteps*(3.141592653589793/24.0);
 float s=sin(halfAngle),c=cos(halfAngle),yawSin=2*s*c,yawCos=1-2*s*s;
 float ox=target->position.x-target->center.x;
 float oz=target->position.z-target->center.z;
 Vec original=target->rotation;
 *center=target->center;
 center->x+=target->along*target->forwardX-target->across*target->forwardZ;
 center->z+=target->along*target->forwardZ+target->across*target->forwardX;
 center->y+=target->lift;
 /* The body origin may differ from the center of mass. Rotate that offset too. */
 position->x=center->x+yawCos*ox+yawSin*oz;
 position->y=target->position.y+target->lift;
 position->z=center->z-yawSin*ox+yawCos*oz;
 position->w=0;
 rotation->x=c*original.x+s*original.z;
 rotation->y=c*original.y+s*original.w;
 rotation->z=c*original.z-s*original.x;
 rotation->w=c*original.w-s*original.y;
 axis->x=yawCos*target->forwardX+yawSin*target->forwardZ;
 axis->y=0;
 axis->z=-yawSin*target->forwardX+yawCos*target->forwardZ;
 axis->w=0;
}
static void nudgePlacement(int direction){
 const float step=0.25f;
 const float horizontalLimit=25.0f;
 BYTE motion;
 Vec position,rotation,center,axis;
 Placement next;
 void *pair;
 if(!placement.body)return;
 if(!placementMotion(&motion)){
  finishPlacement(0);
  showRadar();
  return;
 }
 if(motion!=4)return;
 next=placement;
 switch(direction){
 case MOVE_FORWARD:next.along+=step;break;
 case MOVE_BACK:next.along-=step;break;
 case MOVE_LEFT:next.across-=step;break;
 case MOVE_RIGHT:next.across+=step;break;
 case TURN_LEFT:next.turnSteps=(next.turnSteps+37)%24-12;break;
 case TURN_RIGHT:next.turnSteps=(next.turnSteps+35)%24-12;break;
 case RAISE_CARGO:next.lift+=step;break;
 case LOWER_CARGO:next.lift-=step;break;
 default:return;
 }
 if(absf(next.along)>horizontalLimit || absf(next.across)>horizontalLimit)return;
 if(height+next.lift<1.0f || height+next.lift>30.0f)return;
 adjustedPose(&next,&position,&rotation,&center,&axis);
 pair=allocatePose(position,rotation);
 if(!pair)return;
 submitPose(placement.body,pair);
 placement=next;
 logs[selected].center=center;
 logs[selected].axis=axis;
 sprintf(message,"Height %.2f m | Turn %d deg | Enter to drop",
         height+next.lift,next.turnSteps*15);
 InvalidateRect(window,0,TRUE);
}
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
   strcpy(message,"Game keyboard unavailable. Use the radar buttons.");
   showRadar();
   return;
  }
  if(!key->direction && !down && (previous==2 || tapped)){
   finishPlacement(key->key==VK_ESCAPE);
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
static void loadSelected(void){
 Item current,t;
 Placement next={0};
 Vec com,inertia,truckInertia;
 void *pair;
 int kind,forward,up,upSign,detected;
 float headingLength;
 uintptr_t userdata;
 if(placement.body){finishPlacement(0);return;}
 if(selected<0 || !selectedBody || !truckReady)return;
 if(!currentWorld()){strcpy(message,"Map changed or unsupported game build.");goto done;}
 detected=automaticTruckIndex();
 if(detected<0 || truckTargets[detected].item.body!=truck.body){
  selectedBody=0;
  refresh();
  strcpy(message,"Truck changed or unclear. Select cargo again.");
  goto done;
 }
 if(!pose(selectedBody,&current) || !pose(truck.body,&t)){strcpy(message,"Object changed. Refresh before loading.");goto done;}
 if(!readmem(truck.body+0x220,&truckInertia,16) || truckProfileFor(truck.body,truckInertia)!=truckProfile){
  strcpy(message,"Target truck changed. Refresh first.");goto done;
 }
 if(!readmem(selectedBody+0x220,&inertia,16) || !readmem(selectedBody+0x10,&userdata,8) || cargoKind(selectedBody,inertia,userdata)!=logs[selected].kind){strcpy(message,"Cargo identity changed. Refresh first.");goto done;}
 if(length(current.center.x-t.center.x,current.center.z-t.center.z)>range){strcpy(message,"Selected cargo moved outside range");goto done;}
 kind=logs[selected].kind;
 if(kind>=FIRST_ASSET_KIND && cargoAssets[kind-FIRST_ASSET_KIND].multipleBodies){
  strcpy(message,"Cargo has multiple physics bodies. Load it with the crane.");
  goto done;
 }
 if(!readmem(selectedBody+0x168,&next.originalMotion,1)
    || (next.originalMotion!=1 && next.originalMotion!=2 && next.originalMotion!=3 && next.originalMotion!=6)){
  strcpy(message,"Select a loose, movable cargo object.");
  goto done;
 }
 next.body=selectedBody;
 if(!readmem(selectedBody+0x20,&next.shape,8)
    || !readmem(selectedBody+0x1e0,&next.originalRotation,16)
    || !readmem(selectedBody+0x1f0,&com,16))return;
 memcpy(&next.originalPosition,&current.pose[12],16);
 next.originalPosition.w=0;
 next.originalCenter=current.center;
 next.originalAxis=logs[selected].axis;
 forward=cargoForward(kind,inertia);
 up=kind>=FIRST_ASSET_KIND?cargoAssets[kind-FIRST_ASSET_KIND].up:kind?profiles[kind-1].up:2;
 upSign=kind?1:-1;
 if(!cargoLoadingPose(&t,truckProfile,com,forward,up,upSign,&next.position,&next.rotation,&next.center))return;
 headingLength=length(t.pose[8],t.pose[10]);
 next.forwardX=t.pose[8]/headingLength;next.forwardZ=t.pose[10]/headingLength;
 pair=allocatePose(next.position,next.rotation);
 if(!pair){strcpy(message,"Engine allocation failed");goto done;}
 stopMotion(selectedBody);
 changeMotion(selectedBody,4);
 submitPose(selectedBody,pair);
 placement=next;
 logs[selected].center=placement.center;
 logs[selected].axis.x=placement.forwardX;logs[selected].axis.z=placement.forwardZ;
 strcpy(message,"WASD: move | Q/E: turn | R/F: height");
 placementControls();
 focusGameForPlacement();
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
 int positioning=placement.body && (radarFocused || foreground==gameWindow);
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
static void mapPoint(Vec p,int *x,int *y){float fx=truck.pose[8],fz=truck.pose[10],n=length(fx,fz),dx=p.x-truck.center.x,dz=p.z-truck.center.z;if(n<.1f)n=1;fx/=n;fz/=n;*x=240-(int)((dx*fz-dz*fx)*mapScale);*y=235-(int)((dx*fx+dz*fz)*mapScale);}
enum MarkerShape { MARK_RECT, MARK_CIRCLE, MARK_TRIANGLE, MARK_DIAMOND };
typedef struct {
 const char *label;
 enum MarkerShape shape;
 COLORREF color;
 int halfLength,halfWidth;
} Marker;
static const Marker markers[]={
 {"Concrete_scrap",MARK_CIRCLE,RGB(0,146,166),8,8},
 {"Metal_scrap",MARK_TRIANGLE,RGB(219,102,35),8,8},
 {"Sellable",MARK_DIAMOND,RGB(224,183,38),8,8},
 {"Concrete_slab",MARK_RECT,RGB(46,108,223),14,8},
 {"Concrete_pile",MARK_RECT,RGB(105,121,138),17,6},
 {"Iron_pipe",MARK_RECT,RGB(37,151,72),16,4},
 {"Steel_beam",MARK_RECT,RGB(158,64,196),16,2},
 {"Log",MARK_RECT,RGB(126,84,48),17,3},
 {"Unknown",MARK_RECT,RGB(90,90,90),8,4}
};
static const Marker *cargoMarker(int kind){
 const char *label=cargoLabel(kind);
 int i;
 for(i=0;i<sizeof(markers)/sizeof(markers[0])-1;i++){
  if(!strcmp(label,markers[i].label))return &markers[i];
 }
 return &markers[i];
}
static void paintCargo(HDC dc,int i){
 const Item *cargo=&logs[i];
 const Marker *marker=cargoMarker(cargo->kind);
 int x,y;
 char number[16];
 HPEN pen=CreatePen(PS_SOLID,1,RGB(30,30,30));
 HBRUSH brush=CreateSolidBrush(marker->color);
 HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,brush);
 mapPoint(cargo->center,&x,&y);
 if(marker->shape==MARK_CIRCLE){
  Ellipse(dc,x-8,y-8,x+9,y+9);
 }else if(marker->shape==MARK_TRIANGLE){
  POINT triangle[]={{x,y-9},{x+9,y+8},{x-9,y+8}};
  Polygon(dc,triangle,3);
 }else if(marker->shape==MARK_DIAMOND){
  POINT diamond[]={{x,y-10},{x+10,y},{x,y+10},{x-10,y}};
  Polygon(dc,diamond,4);
 }else{
  Vec end=cargo->center;
  int ex,ey,j;
  POINT corners[4];
  float dx,dy,n;
  end.x+=cargo->axis.x*3;
  end.z+=cargo->axis.z*3;
  mapPoint(end,&ex,&ey);
  dx=ex-x;dy=ey-y;n=length(dx,dy);
  if(n<1){dx=1;dy=0;}else{dx/=n;dy/=n;}
  for(j=0;j<4;j++){
   int along=j<2?marker->halfLength:-marker->halfLength;
   int across=j==0 || j==3?marker->halfWidth:-marker->halfWidth;
   corners[j].x=x+(int)(along*dx-across*dy);
   corners[j].y=y+(int)(along*dy+across*dx);
  }
  Polygon(dc,corners,4);
 }
 SelectObject(dc,oldPen);SelectObject(dc,oldBrush);
 DeleteObject(pen);DeleteObject(brush);
 if(i==selected){
  HGDIOBJ fill=SelectObject(dc,GetStockObject(NULL_BRUSH));
  Rectangle(dc,x-19,y-19,x+20,y+20);
  SelectObject(dc,fill);
 }
 sprintf(number,"%d",i+1);
 TextOutA(dc,x+10,y-20,number,lstrlenA(number));
}
static void paint(HWND h){
 PAINTSTRUCT ps;
 HDC dc=BeginPaint(h,&ps);
 RECT view=radarViewport(h);
 HFONT font=radarFont(16);
 HGDIOBJ oldFont=font?SelectObject(dc,font):0;
 int i;
 char text[80];
 SetMapMode(dc,MM_ANISOTROPIC);
 SetWindowExtEx(dc,RADAR_WIDTH,RADAR_HEIGHT,0);
 SetViewportExtEx(dc,view.right-view.left,view.bottom-view.top,0);
 SetViewportOrgEx(dc,view.left,view.top,0);
 SetBkMode(dc,TRANSPARENT);
 TextOutA(dc,14,12,message,lstrlenA(message));
 {
  RECT detectionLine={14,37,476,59};
  sprintf(text,"Detected truck: %.62s",detectedTruck);
  DrawTextA(dc,text,-1,&detectionLine,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
 }
 SaveDC(dc);
 IntersectClipRect(dc,8,MAP_TOP,482,410);
 if(truckReady){
  Rectangle(dc,230,215,250,255);
  TextOutA(dc,215,195,"Front",5);
  for(i=0;i<count;i++)if(i!=selected)paintCargo(dc,i);
  if(selected>=0 && selected<count)paintCargo(dc,selected);
 }
 RestoreDC(dc,-1);
 if(selected>=0 && selected<count){
  sprintf(text,"%d/%d: %s | Wheel: zoom",selected+1,count,cargoLabel(logs[selected].kind));
  for(i=0;text[i];i++)if(text[i]=='_')text[i]=' ';
  TextOutA(dc,14,424,text,lstrlenA(text));
 }
 if(font){SelectObject(dc,oldFont);DeleteObject(font);}
 EndPaint(h,&ps);
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){int i;
 if(m==WM_PAINT){paint(h);return 0;}
 if(m==WM_ACTIVATE){
  if(LOWORD(w)==WA_INACTIVE){KillTimer(h,CURSOR_TIMER);releaseRadarCursor();}
  else if(!HIWORD(w)){ClipCursor(0);SetTimer(h,CURSOR_TIMER,100,0);revealRadarCursor(0);}
 }
 if(m==WM_SETCURSOR && LOWORD(l)==HTCLIENT && GetForegroundWindow()==h){revealRadarCursor(1);return TRUE;}
 if(m==WM_SIZE){if(w!=SIZE_MINIMIZED)layoutButtons(h);return 0;}
 if(m==WM_GETMINMAXINFO){
  MINMAXINFO *sizes=(MINMAXINFO*)l;
  sizes->ptMinTrackSize.x=360;
  sizes->ptMinTrackSize.y=390;
  return 0;
 }
 if(m==0x02e0){ /* WM_DPICHANGED */
  RECT *r=(RECT*)l;
  SetWindowPos(h,0,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
  return 0;
 }
 if(m==0x020a){mapScale*=((short)HIWORD(w)>0)?1.25f:0.8f;if(mapScale<4)mapScale=4;if(mapScale>35)mapScale=35;InvalidateRect(h,0,TRUE);return 0;}
 if(m==WM_APP+2){
  cameraSearchPending=0;
  vehicleCamera=(uintptr_t)w;
  if(vehicleCamera)refresh();
  else if(truckCount && !truckReady){
   detectedTruck="Camera unavailable";
   strcpy(message,"Current truck unclear. Refresh to retry.");
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
  case TURN_LEFT:case TURN_RIGHT:case RAISE_CARGO:case LOWER_CARGO:
   nudgePlacement(id);break;
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
  if(mx<8 || mx>=482 || my<MAP_TOP || my>=410)return 0;
  for(i=0;i<count;i++){int x,y;float d;mapPoint(logs[i].center,&x,&y);d=(x-mx)*(x-mx)+(y-my)*(y-my);if(d<distance){distance=d;best=i;}}
  if(best>=0){selected=best;selectedBody=logs[best].body;InvalidateRect(h,0,TRUE);}return 0;
 }
 if(m==WM_TIMER && w==PLACEMENT_TIMER){pollPlacementKeys();return 0;}
 if(m==WM_TIMER && w==GAMEPAD_TIMER){pollGamepad();return 0;}
 if(m==WM_TIMER && w==CURSOR_TIMER){revealRadarCursor(0);return 0;}
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
 static const WCHAR *labels[]={L"Load",L"Refresh",L"Previous",L"Next",
                              L"\x2191 W",L"\x2190 A",L"\x2193 S",L"\x2192 D",
                              L"Turn left Q",L"Turn right E",L"Raise R",L"Lower F"};
 if(setDpiContext)setDpiContext((HANDLE)(intptr_t)-4);
 base=(BYTE*)GetModuleHandleW(0);n=GetModuleFileNameW(self,ini,MAX_PATH);if(!n || n>=MAX_PATH)return 1;while(n && ini[n-1]!=L'\\')n--;ini[n]=0;lstrcpyW(profilePath,ini);lstrcatW(profilePath,L"RoadCraftAutoload.profiles");lstrcatW(ini,L"RoadCraftAutoload.ini");
 rear=GetPrivateProfileIntW(L"Loading",L"RearOffsetCm",350,ini)/100.0f;height=GetPrivateProfileIntW(L"Loading",L"DropHeightCm",500,ini)/100.0f;
 if(rear<0 || rear>10 || height<1 || height>10)return 1;
 radarCursor=LoadCursorW(0,MAKEINTRESOURCEW(32512));
 ZeroMemory(&wc,sizeof(wc));wc.lpfnWndProc=proc;wc.hInstance=self;wc.lpszClassName=L"RoadCraftAutoloadWindow";wc.hCursor=radarCursor;wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
 window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"RoadCraft Autoload - F9",radarStyle,CW_USEDEFAULT,CW_USEDEFAULT,505,535,0,0,self,0);
 if(!window)return 1;
 for(i=0;i<BUTTON_COUNT;i++)CreateWindowW(L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,1,1,window,(HMENU)(uintptr_t)buttonIds[i],self,0);
 fitClient(window);
 placementControls();
 RegisterHotKey(window,HOTKEY_RADAR,0x4000,VK_F9);
 xinput=LoadLibraryExW(L"XInput1_4.dll",0,0x800); /* LOAD_LIBRARY_SEARCH_SYSTEM32 */
 if(xinput)readPad=(PadGetState)GetProcAddress(xinput,"XInputGetState");
 if(readPad)SetTimer(window,GAMEPAD_TIMER,15,0);
 while(GetMessageW(&msg,0,0,0)>0){
  if(placement.body && msg.message==WM_KEYDOWN && (msg.hwnd==window || IsChild(window,msg.hwnd))){
   int direction=placementDirection((DWORD)msg.wParam);
   if(direction){nudgePlacement(direction);continue;}
   if(msg.wParam==VK_ESCAPE || msg.wParam==VK_RETURN){
    if(!(msg.lParam & (1L<<30)))finishPlacement(msg.wParam==VK_ESCAPE);
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
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID reserved){if(reason==DLL_PROCESS_ATTACH){self=h;DisableThreadLibraryCalls(h);
#ifndef AUTOLOAD_TEST
 {HANDLE t=CreateThread(0,0,run,0,0,0);if(t)CloseHandle(t);}
#endif
 }return TRUE;}

