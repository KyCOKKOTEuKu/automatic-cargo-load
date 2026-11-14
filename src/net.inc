/* ───── KOTEuKA_CargoLoad: мультиплеер ─────
   Факт: в сетевой игре физикой грузов управляет хост, клиент только показывает присланные
   хостом положения. Поэтому мод клиента ничего не двигает сам: он отправляет команды
   (погрузить / сдвинуть / отпустить / отменить) моду хоста, а тот выполняет их тем же кодом,
   что и в одиночной игре. Мод нужен у обоих игроков; у хоста — режим «Хост».

   Связь: 1) Steam P2P (ISteamNetworkingMessages, через ретрансляторы Valve) — с друзьями Steam,
   которые сейчас в той же игре; 2) TCP по IP (локальная сеть, VPN, проброс порта) — запасной путь.
   Сообщение — структура NetMsg фиксированного размера. */

enum { NET_OFF, NET_HOST, NET_CLIENT };
enum { NM_HELLO=1, NM_HOST, NM_LOAD, NM_NUDGE, NM_RELEASE, NM_CANCEL, NM_BEDRESET, NM_RESULT, NM_BYE };
enum { RS_STARTED=1, RS_FAILED, RS_MOVED, RS_DONE, RS_BUSY, RS_DENIED };
enum { NT_STEAM=1, NT_TCP };
enum { NET_PEERS=8, NET_ALIVE_MS=15000, NET_ANNOUNCE_MS=3000, NET_CONNECT_MS=5000,
       NET_PENDING_MS=6000, NET_REMOTE_IDLE_MS=45000, NET_DEFAULT_PORT=41871 };
#define NET_MAGIC "KCL1"
#define NET_CHANNEL 4932428
#define NETF_BED    1   /* NM_LOAD: память позиции включена */
#define NETF_FINE   1   /* NM_NUDGE: точный шаг */
#define NETF_POSE   2   /* NM_RESULT: в cargo[] высота/сдвиги, в value — поворот */
#define NETF_LOCAL  8   /* NM_RESULT: хост не смог сдвинуть груз — клиенту поднять его у себя */
enum { NET_VERIFY_MS=450 }; /* через сколько проверять, что груз действительно сдвинулся */
enum { NET_CLIENT_VERIFY_MS=900 }; /* клиенту — с запасом на пинг и сглаживание игры */
#define NET_MOVED_M 0.5f           /* подъём над кузовом — метры; меньше 0,5 м = «не сдвинулся» */
#pragma pack(push,1)
typedef struct {
 char magic[4];
 BYTE type,lang,status,flags;
 int kind,profile,value,reserved;
 float cargo[3],truck[3],height,bedWidth;
 char text[200];                 /* UTF-8: имя игрока или сообщение */
} NetMsg;
#pragma pack(pop)
typedef struct {
 int used,transport,isHost,isClient,connecting;
 uint64_t steamId;
 SOCKET sock;
 int rxLen;
 BYTE rx[sizeof(NetMsg)];
 char name[64];
 DWORD lastSeen;
 /* Для хоста: настройки и память позиции этого друга. */
 int lang,bedOn;
 float height,bedWidth;
 BedLayout bed;
 Item truck;
 const TruckProfile *profile;
} NetPeer;
static NetPeer peers[NET_PEERS];
static int netMode,netSteamOn=1,netTcpPort=NET_DEFAULT_PORT,wsaState;
static WCHAR netHostAddress[128];
static SOCKET netListen=INVALID_SOCKET;
static DWORD netNextAnnounce,netNextConnect,netNextSteamTry;
static int clientPending,clientHost=-1;
static uintptr_t clientPendingBody;
static int clientPendingKind;
static Vec clientPendingAxis;
static DWORD clientLocalVerify;  /* клиент поднял груз сам — время проверки, что он сдвинулся */
static DWORD clientRemoteVerify; /* хост поднял груз клиента — время проверки в мире клиента */
static Vec clientStartCenter;
static DWORD clientIgnoreDone;   /* до этого времени RS_DONE от хоста — ответ на нашу отмену */
static DWORD hostVerify;         /* хост поднял груз друга — время проверки, что он сдвинулся */
static DWORD clientPendingTime;
static int hostRetry,hostRetryCount;
static DWORD hostRetryTime;
static char netProblem[120];      /* последняя ошибка связи — показывается в строке «Сеть» */
static WCHAR netLogPath[MAX_PATH];

static void netLog(const char *format,...){
 FILE *f;va_list args;SYSTEMTIME t;
 if(!netLogPath[0] || !(f=_wfopen(netLogPath,L"a")))return;
 GetLocalTime(&t);
 fprintf(f,"[%02d:%02d:%02d.%03d] net: ",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
 va_start(args,format);vfprintf(f,format,args);va_end(args);
 fputc('\n',f);fclose(f);
}
static int peerAlive(const NetPeer *p){
 return p->used && !p->connecting && GetTickCount()-p->lastSeen<NET_ALIVE_MS;
}
static int truckProfileIndex(const TruckProfile *p){
 int n=sizeof(truckProfiles)/sizeof(truckProfiles[0]);
 if(p==&legacyTruck)return -1;
 if(p==&genericTruck)return -2;
 if(p>=truckProfiles && p<truckProfiles+n)return (int)(p-truckProfiles);
 return -3;
}
static const TruckProfile *truckProfileAt(int i){
 int n=sizeof(truckProfiles)/sizeof(truckProfiles[0]);
 if(i==-1)return &legacyTruck;
 if(i==-2)return &genericTruck;
 return i>=0 && i<n?&truckProfiles[i]:0;
}
static int finiteVec(const float *v,int n){int i;for(i=0;i<n;i++)if(!_finite(v[i]) || v[i]>1e7f || v[i]<-1e7f)return 0;return 1;}

/* ── Steam (функции берутся из steam_api64.dll, которую уже загрузила игра) ── */
typedef struct { int type,size; uint64_t id; BYTE rest[120]; } SteamIdent;
typedef struct { void *data; int size; unsigned conn; SteamIdent peer; } SteamMsgHead;
typedef struct { uint64_t game; unsigned ip; unsigned short port,query; uint64_t lobby; } FriendGame;
static struct {
 int ready;
 void *msgs,*friends,*user,*utils;
 int (*send)(void*,const SteamIdent*,const void*,unsigned,int,int);
 int (*recv)(void*,int,SteamMsgHead**,int);
 BYTE (*accept)(void*,const SteamIdent*);
 void (*release)(SteamMsgHead*);
 void (*setId)(SteamIdent*,uint64_t);
 uint64_t (*getId)(const SteamIdent*);
 int (*friendCount)(void*,int);
 uint64_t (*friendAt)(void*,int,int);
 BYTE (*friendGame)(void*,uint64_t,FriendGame*);
 const char *(*friendName)(void*,uint64_t);
 const char *(*myName)(void*);
 unsigned (*appId)(void*);
 uint64_t (*mySteamId)(void*);
 unsigned app;
 uint64_t me;
} steam;
static FARPROC steamProc(const char *name){
 HMODULE m=GetModuleHandleW(L"steam_api64.dll");
 FARPROC f=m?GetProcAddress(m,name):0;
 if(!f && (m=GetModuleHandleW(L"steam_api64_o.dll")))f=GetProcAddress(m,name);
 return f;
}
static int steamLoad(void){
 typedef void *(*Accessor)(void);
 Accessor msgs,friends,user,utils;
 DWORD now=GetTickCount();
 if(steam.ready)return 1;
 if(netNextSteamTry && (LONG)(now-netNextSteamTry)<0)return 0;
 netNextSteamTry=now+5000;
 msgs=(Accessor)steamProc("SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
 friends=(Accessor)steamProc("SteamAPI_SteamFriends_v017");
 user=(Accessor)steamProc("SteamAPI_SteamUser_v023");
 utils=(Accessor)steamProc("SteamAPI_SteamUtils_v010");
 *(FARPROC*)&steam.send=steamProc("SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
 *(FARPROC*)&steam.recv=steamProc("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
 *(FARPROC*)&steam.accept=steamProc("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
 *(FARPROC*)&steam.release=steamProc("SteamAPI_SteamNetworkingMessage_t_Release");
 *(FARPROC*)&steam.setId=steamProc("SteamAPI_SteamNetworkingIdentity_SetSteamID64");
 *(FARPROC*)&steam.getId=steamProc("SteamAPI_SteamNetworkingIdentity_GetSteamID64");
 *(FARPROC*)&steam.friendCount=steamProc("SteamAPI_ISteamFriends_GetFriendCount");
 *(FARPROC*)&steam.friendAt=steamProc("SteamAPI_ISteamFriends_GetFriendByIndex");
 *(FARPROC*)&steam.friendGame=steamProc("SteamAPI_ISteamFriends_GetFriendGamePlayed");
 *(FARPROC*)&steam.friendName=steamProc("SteamAPI_ISteamFriends_GetFriendPersonaName");
 *(FARPROC*)&steam.myName=steamProc("SteamAPI_ISteamFriends_GetPersonaName");
 *(FARPROC*)&steam.appId=steamProc("SteamAPI_ISteamUtils_GetAppID");
 *(FARPROC*)&steam.mySteamId=steamProc("SteamAPI_ISteamUser_GetSteamID");
 if(!msgs || !friends || !user || !utils || !steam.send || !steam.recv || !steam.accept || !steam.release
    || !steam.setId || !steam.getId || !steam.friendCount || !steam.friendAt || !steam.friendGame
    || !steam.friendName || !steam.myName || !steam.appId || !steam.mySteamId){
  strcpy(netProblem,TR("Steam API не найден","Steam API not found"));
  return 0;
 }
 steam.msgs=msgs();steam.friends=friends();steam.user=user();steam.utils=utils();
 if(!steam.msgs || !steam.friends || !steam.user || !steam.utils){
  strcpy(netProblem,TR("Steam ещё не готов","Steam not ready yet"));
  return 0;
 }
 steam.app=steam.appId(steam.utils);
 steam.me=steam.mySteamId(steam.user);
 steam.ready=1;
 netProblem[0]=0;
 netLog("Steam ready, app %u",steam.app);
 return 1;
}
static void myName(char *out,int size){
 const char *n=steam.ready?steam.myName(steam.friends):0;
 lstrcpynA(out,n && *n?n:"Player",size);
}

/* ── Таблица друзей ── */
static NetPeer *peerBySteam(uint64_t id,int create){
 int i,freeSlot=-1;
 for(i=0;i<NET_PEERS;i++){
  if(peers[i].used && peers[i].transport==NT_STEAM && peers[i].steamId==id)return &peers[i];
  if(!peers[i].used && freeSlot<0)freeSlot=i;
 }
 if(!create || freeSlot<0)return 0;
 ZeroMemory(&peers[freeSlot],sizeof(NetPeer));
 peers[freeSlot].used=1;peers[freeSlot].transport=NT_STEAM;peers[freeSlot].steamId=id;
 peers[freeSlot].sock=INVALID_SOCKET;
 return &peers[freeSlot];
}
static NetPeer *peerNewTcp(SOCKET s,int connecting){
 int i;
 BOOL noDelay=TRUE;
 setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(const char*)&noDelay,sizeof(noDelay)); /* без буферизации Nagle — меньше задержка */
 for(i=0;i<NET_PEERS;i++)if(!peers[i].used){
  ZeroMemory(&peers[i],sizeof(NetPeer));
  peers[i].used=1;peers[i].transport=NT_TCP;peers[i].sock=s;peers[i].connecting=connecting;
  peers[i].lastSeen=GetTickCount();
  lstrcpyA(peers[i].name,"TCP");
  return &peers[i];
 }
 return 0;
}
static void peerDrop(NetPeer *p,const char *why);

static void netSend(NetPeer *p,NetMsg *m){
 memcpy(m->magic,NET_MAGIC,4);
 m->lang=(BYTE)uiLang;
 if(p->transport==NT_STEAM && steam.ready){
  SteamIdent id;ZeroMemory(&id,sizeof(id));
  steam.setId(&id,p->steamId);
  steam.send(steam.msgs,&id,m,sizeof(NetMsg),1|8|32,NET_CHANNEL); /* NoNagle | Reliable | AutoRestartBrokenSession */
 }else if(p->transport==NT_TCP && p->sock!=INVALID_SOCKET && !p->connecting){
  int sent=send(p->sock,(const char*)m,sizeof(NetMsg),0);
  if(sent!=sizeof(NetMsg) && WSAGetLastError()!=WSAEWOULDBLOCK)peerDrop(p,"send failed");
 }
}
static void netSendSimple(NetPeer *p,int type,const char *text){
 NetMsg m;ZeroMemory(&m,sizeof(m));
 m.type=(BYTE)type;
 if(text)lstrcpynA(m.text,text,sizeof(m.text));
 netSend(p,&m);
}
static void netReply(NetPeer *p,int status,const char *text){
 NetMsg m;ZeroMemory(&m,sizeof(m));
 m.type=NM_RESULT;m.status=(BYTE)status;
 if(text)lstrcpynA(m.text,text,sizeof(m.text));
 netSend(p,&m);
}
static void netAnnounce(NetPeer *p){
 char name[64];
 myName(name,64);
 netSendSimple(p,netMode==NET_HOST?NM_HOST:NM_HELLO,name);
}

/* ── Хост: команды друга выполняются в его контексте (грузовик, настройки, память, язык) ── */
typedef struct { Item truck; const TruckProfile *profile; BedLayout bed; float height,bedWidth; int bedOn,lang; char message[400]; } NetCtx;
static void ctxEnter(NetPeer *p,NetCtx *s){
 s->truck=truck;s->profile=truckProfile;s->bed=bed;s->height=height;s->bedWidth=bedWidth;
 s->bedOn=bedLayoutOn;s->lang=uiLang;memcpy(s->message,message,sizeof(message));
 truck=p->truck;truckProfile=p->profile;bed=p->bed;height=p->height;bedWidth=p->bedWidth;
 bedLayoutOn=p->bedOn;uiLang=p->lang;message[0]=0;
 netExecuting=1;
}
static void ctxLeave(NetPeer *p,NetCtx *s,char *out,int size){
 p->truck=truck;p->profile=truckProfile;p->bed=bed;
 if(out)lstrcpynA(out,message,size);
 truck=s->truck;truckProfile=s->profile;bed=s->bed;height=s->height;bedWidth=s->bedWidth;
 bedLayoutOn=s->bedOn;uiLang=s->lang;memcpy(message,s->message,sizeof(message));
 netExecuting=0;
 placementControls();   /* подписи кнопок — снова на языке хоста */
}
static void hostNote(NetPeer *p,const char *text){
 if(placement.body && placement.mode!=PM_REMOTE)return; /* хост сам грузит — не перебиваем его подсказку */
 _snprintf(message,sizeof(message),"%s: %s",p->name[0]?p->name:"?",text);
 message[sizeof(message)-1]=0;
 InvalidateRect(window,0,TRUE);
}
/* Ищет у хоста грузовик и груз, которые друг видит в точках m->truck и m->cargo. */
static int netFindTargets(const NetMsg *m,Item *cargoOut,Item *truckOut,const TruckProfile **profileOut,float *cargoMiss,float *truckMiss){
 static uintptr_t islands[4096],bodies[4096];
 uintptr_t activeWorld;
 const TruckProfile *wanted=truckProfileAt(m->profile);
 int ni,i,j,bn,foundCargo=0,foundTruck=0;
 float bestCargo=2.5f,bestTruck=6.0f;
 *cargoMiss=*truckMiss=-1;
 if(!wanted || !signature() || !readmem(gameAddr[G_WORLD],&activeWorld,8) || !activeWorld)return 0;
 if(activeWorld!=world){world=activeWorld;selectedBody=0;selected=-1;count=0;truckReady=0;}
 ni=array(world+0x40,islands,2048);ni+=array(world+0x50,islands+ni,2048);
 for(i=0;i<ni;i++){bn=array(islands[i]+0x68,bodies,4096);for(j=0;j<bn;j++){
  Vec inertia;Item item;uintptr_t userdata;const TruckProfile *profile;float dx,dy,dz,d;
  if(!pose(bodies[j],&item))continue;
  if(!readmem(bodies[j]+0x220,&inertia,16) || !readmem(bodies[j]+0x10,&userdata,8))continue;
  profile=truckProfileFor(item.body,inertia);
  if(profile){
   if(profile!=wanted)continue;
   dx=item.center.x-m->truck[0];dy=item.center.y-m->truck[1];dz=item.center.z-m->truck[2];
   d=(float)sqrt(dx*dx+dy*dy+dz*dz);
   if(*truckMiss<0 || d<*truckMiss)*truckMiss=d;
   if(d<bestTruck){bestTruck=d;*truckOut=item;*profileOut=profile;foundTruck=1;}
   continue;
  }
  dx=item.center.x-m->cargo[0];dy=item.center.y-m->cargo[1];dz=item.center.z-m->cargo[2];
  d=(float)sqrt(dx*dx+dy*dy+dz*dz);
  if(d>30.0f)continue;               /* дальние тела не классифицируем — это дорого */
  item.kind=cargoKind(bodies[j],inertia,userdata);
  if(item.kind!=m->kind)continue;
  if(*cargoMiss<0 || d<*cargoMiss)*cargoMiss=d;
  if(d<bestCargo){
   int axis=cargoForward(item.kind,inertia);
   item.axis.x=item.pose[axis*4];item.axis.y=item.pose[axis*4+1];item.axis.z=item.pose[axis*4+2];item.axis.w=0;
   bestCargo=d;*cargoOut=item;foundCargo=1;
  }
 }}
 return foundCargo && foundTruck;
}
static void hostLoad(NetPeer *p,const NetMsg *m){
 Item cargoItem,truckItem;
 const TruckProfile *profile=0;
 float cargoMiss,truckMiss;
 NetCtx ctx;
 char out[200],text[200];
 int ok,hostLang=uiLang;
 text[0]=0;
 uiLang=p->lang;   /* отказы — на языке друга */
 if(placement.body)
  lstrcpynA(text,TR("Хост сейчас размещает другой груз — подождите.","The host is placing other cargo — please wait."),sizeof(text));
 else if(!finiteVec(m->cargo,3) || !finiteVec(m->truck,3) || !_finite(m->height) || !_finite(m->bedWidth))
  lstrcpynA(text,"bad request",sizeof(text));
 uiLang=hostLang;
 if(text[0]){netReply(p,placement.body?RS_BUSY:RS_FAILED,text);return;}
 p->height=m->height>=1.0f && m->height<=10.0f?m->height:5.0f;
 p->bedWidth=m->bedWidth>=1.0f && m->bedWidth<=4.0f?m->bedWidth:2.4f;
 p->bedOn=(m->flags&NETF_BED)!=0;
 if(!netFindTargets(m,&cargoItem,&truckItem,&profile,&cargoMiss,&truckMiss)){
  netLog("LOAD from %s: not found (kind %d, profile %d, cargo miss %.2f m, truck miss %.2f m, cargo at %.1f %.1f %.1f, truck at %.1f %.1f %.1f)",
         p->name,m->kind,m->profile,cargoMiss,truckMiss,m->cargo[0],m->cargo[1],m->cargo[2],m->truck[0],m->truck[1],m->truck[2]);
  uiLang=p->lang;
  if(truckMiss<0)
   lstrcpynA(text,TR("Хост не видит у себя ваш грузовик. Нажмите «Обновить» и повторите.","The host cannot see your truck. Press “Refresh” and retry."),sizeof(text));
  else if(truckMiss>=6.0f)
   _snprintf(text,sizeof(text),TR("У хоста ваш грузовик в %.1f м от вашей точки — остановитесь и повторите.","On the host your truck is %.1f m away from your spot — stop and retry."),truckMiss);
  else if(cargoMiss<0)
   lstrcpynA(text,TR("Хост не нашёл рядом груз этого типа. Нажмите «Обновить».","The host found no cargo of this type nearby. Press “Refresh”."),sizeof(text));
  else
   _snprintf(text,sizeof(text),TR("У хоста этот груз в %.1f м от вашей точки. Нажмите «Обновить» и повторите.","On the host this cargo is %.1f m from your spot. Press “Refresh” and retry."),cargoMiss);
  text[sizeof(text)-1]=0;
  uiLang=hostLang;
  netReply(p,RS_FAILED,text);
  return;
 }
 if(length(cargoItem.center.x-truckItem.center.x,cargoItem.center.z-truckItem.center.z)>loadRange+5.0f){
  uiLang=p->lang;lstrcpynA(text,TR("У хоста груз слишком далеко от грузовика.","On the host the cargo is too far from the truck."),sizeof(text));uiLang=hostLang;
  netReply(p,RS_FAILED,text);
  return;
 }
 p->truck=truckItem;p->profile=profile;
 ctxEnter(p,&ctx);
 ok=beginPlacement(cargoItem.body,cargoItem.kind,cargoItem.axis,&truckItem,PM_REMOTE);
 if(ok){placement.peer=(int)(p-peers);hostRetry=0;hostVerify=GetTickCount()+NET_VERIFY_MS;if(!hostVerify)hostVerify=1;}
 ctxLeave(p,&ctx,out,sizeof(out));
 {BYTE motion=0;readmem(cargoItem.body+0x168,&motion,1);
  netLog("LOAD from %s: %s (body %p, motion %d, parts %d, cargo miss %.2f m, truck miss %.2f m)",p->name,ok?"started":"failed",
         (void*)cargoItem.body,ok?placement.originalMotion:motion,ok?placement.partCount:0,cargoMiss,truckMiss);}
 netReply(p,ok?RS_STARTED:RS_FAILED,out);
 if(ok)hostNote(p,TR("грузит груз через ваш мод","is loading cargo through your mod"));
 InvalidateRect(window,0,TRUE);
}
static int remoteOf(const NetPeer *p){
 return placement.body && placement.mode==PM_REMOTE && placement.peer==(int)(p-peers);
}
static void hostNudge(NetPeer *p,const NetMsg *m){
 NetCtx ctx;NetMsg r;
 if(!remoteOf(p)){netReply(p,RS_DONE,"");return;}
 if(m->value<MOVE_FORWARD || m->value>PITCH_BACK)return;
 ctxEnter(p,&ctx);
 nudgePlacementEx(m->value,(m->flags&NETF_FINE)!=0);
 ZeroMemory(&r,sizeof(r));
 r.type=NM_RESULT;
 if(placement.body){
  r.status=RS_MOVED;r.flags=NETF_POSE|((m->flags&NETF_FINE)?NETF_FINE<<2:0);
  r.cargo[0]=height+placement.lift;r.cargo[1]=placement.along;r.cargo[2]=placement.across;r.value=placement.turnSteps*5;r.reserved=placement.tiltSteps*5;r.kind=placement.pitchSteps*5;
 }else r.status=RS_DONE;
 ctxLeave(p,&ctx,r.text,sizeof(r.text));
 netSend(p,&r);
}
static void hostFinish(NetPeer *p,int cancel){
 NetCtx ctx;char out[200];int done;
 if(!remoteOf(p)){netReply(p,RS_DONE,"");return;}
 ctxEnter(p,&ctx);
 done=finishPlacement(cancel);
 ctxLeave(p,&ctx,out,sizeof(out));
 netLog("%s from %s: %s",cancel?"CANCEL":"RELEASE",p->name,done?"done":"waiting for physics");
 if(!done){hostRetry=1;hostRetryCount=0;hostRetryTime=GetTickCount()+150;return;}
 netReply(p,RS_DONE,out);
 if(cancel)hostNote(p,TR("отменил погрузку","cancelled loading"));
 else hostNote(p,TR("погрузил груз","loaded cargo"));
}
/* Отпускание ждёт, пока физика применит тип движения; через 3 с — отмена, чтобы груз не завис. */
static void hostRetryTick(void){
 NetPeer *p;NetCtx ctx;char out[200];int done;
 if(!hostRetry || (LONG)(GetTickCount()-hostRetryTime)<0)return;
 if(!placement.body || placement.mode!=PM_REMOTE){hostRetry=0;return;}
 p=&peers[placement.peer];
 ctxEnter(p,&ctx);
 done=finishPlacement(0);
 if(!done && ++hostRetryCount>=20)done=finishPlacement(1);
 ctxLeave(p,&ctx,out,sizeof(out));
 if(!done){hostRetryTime=GetTickCount()+150;return;}
 hostRetry=0;
 netReply(p,RS_DONE,out);
 hostNote(p,TR("погрузил груз","loaded cargo"));
}
/* Друг пропал посреди размещения — возвращаем груз на место. */
static void hostWatchdog(void){
 NetPeer *p;NetCtx ctx;
 if(!placement.body || placement.mode!=PM_REMOTE)return;
 p=&peers[placement.peer];
 if(p->used && GetTickCount()-p->lastSeen<NET_REMOTE_IDLE_MS)return;
 ctxEnter(p,&ctx);
 finishPlacement(1);
 ctxLeave(p,&ctx,0,0);
 hostRetry=0;
 netLog("remote placement cancelled: %s is gone",p->name);
 hostNote(p,TR("пропала связь — груз возвращён на место","connection lost — cargo returned"));
}

/* Проверка, что груз действительно оказался там, куда его поставил мод.
   Факт из игры (пользователь): груз в кузове машины клиента хост не поднимает, а лежащий рядом — поднимает.
   Гипотеза: машина клиента и груз в ней считаются у клиента (в exe: net_type_client_authority,
   NetPredictedClientTransform), и позы от хоста игра клиента перезаписывает. */
static int placementLanded(float *toTarget,float *fromStart){
 Vec position,rotation,center,axis;
 Item now;
 float dx,dy,dz;
 if(!placement.body || !pose(placement.body,&now))return 0;
 adjustedPose(&placement,&position,&rotation,&center,&axis);
 dx=now.center.x-center.x;dy=now.center.y-center.y;dz=now.center.z-center.z;
 *toTarget=(float)sqrt(dx*dx+dy*dy+dz*dz);
 dx=now.center.x-placement.originalCenter.x;dy=now.center.y-placement.originalCenter.y;dz=now.center.z-placement.originalCenter.z;
 *fromStart=(float)sqrt(dx*dx+dy*dy+dz*dz);
 /* Сдвинулся, если ближе к цели, чем к исходному месту (или уже почти у цели). */
 return *toTarget<1.0f || *toTarget<*fromStart;
}
/* Хост поднял груз друга, но через NET_VERIFY_MS груз не сдвинулся — им управляет игра друга.
   Возвращаем как было и просим мод друга поднять груз у себя. */
static void hostVerifyTick(void){
 NetPeer *p;NetCtx ctx;float toTarget,fromStart;char text[200];int moved,saved;
 if(!hostVerify || (LONG)(GetTickCount()-hostVerify)<0)return;
 hostVerify=0;
 if(!placement.body || placement.mode!=PM_REMOTE)return;
 p=&peers[placement.peer];
 moved=placementLanded(&toTarget,&fromStart);
 netLog("verify %s: %s (to target %.2f m, from start %.2f m)",p->name,moved?"moved":"NOT moved",toTarget,fromStart);
 if(moved)return;
 ctxEnter(p,&ctx);
 finishPlacement(1);
 ctxLeave(p,&ctx,0,0);
 hostRetry=0;
 saved=uiLang;uiLang=p->lang;
 lstrcpynA(text,TR("Этим грузом управляет ваша игра (он в вашей машине) — поднимаю у вас…","Your game controls this cargo (it is in your vehicle) — lifting it on your side…"),sizeof(text));
 uiLang=saved;
 {NetMsg r;ZeroMemory(&r,sizeof(r));r.type=NM_RESULT;r.status=RS_FAILED;r.flags=NETF_LOCAL;lstrcpynA(r.text,text,sizeof(r.text));netSend(p,&r);}
 hostNote(p,TR("груз в машине друга — его поднимает игра друга","cargo is in the friend's vehicle — the friend's game lifts it"));
}
/* ── Клиент ── */
static NetPeer *netHostPeer(void){
 int i;
 if(clientHost>=0 && peers[clientHost].isHost && peerAlive(&peers[clientHost]))return &peers[clientHost];
 clientHost=-1;
 for(i=0;i<NET_PEERS;i++)if(peers[i].isHost && peerAlive(&peers[i])){
  clientHost=i;
  netLog("host selected: %s (%s)",peers[i].name,peers[i].transport==NT_STEAM?"Steam":"TCP");
  return &peers[i];
 }
 return 0;
}
static int netClientActive(void){return netMode==NET_CLIENT;}
static void clientEndPlacement(int show){
 if(placement.mode!=PM_CLIENT || !placement.body)return;
 placement.body=0;placement.mode=PM_LOCAL;
 placementControls();
 if(show)showRadar();
}
static void netClientLoad(int kind,Vec cargo,Vec truckCenter,const TruckProfile *profile){
 NetPeer *h=netHostPeer();
 NetMsg m;
 if(!h){strcpy(message,TR("Хост не найден. У хоста должен стоять этот мод в режиме «Хост».","Host not found. The host must run this mod in “Host” mode."));return;}
 if(clientPending && GetTickCount()-clientPendingTime<NET_PENDING_MS){strcpy(message,TR("Ждём ответа хоста…","Waiting for the host…"));return;}
 ZeroMemory(&m,sizeof(m));
 m.type=NM_LOAD;m.kind=kind;m.profile=truckProfileIndex(profile);
 m.cargo[0]=cargo.x;m.cargo[1]=cargo.y;m.cargo[2]=cargo.z;
 m.truck[0]=truckCenter.x;m.truck[1]=truckCenter.y;m.truck[2]=truckCenter.z;
 m.height=height;m.bedWidth=bedWidth;m.flags=bedLayoutOn?NETF_BED:0;
 netSend(h,&m);
 clientPending=1;clientPendingBody=selectedBody;clientPendingTime=GetTickCount();
 clientPendingKind=kind;clientPendingAxis=selected>=0?logs[selected].axis:cargo;
 netLog("LOAD sent to %s: kind %d at %.1f %.1f %.1f",h->name,kind,cargo.x,cargo.y,cargo.z);
 _snprintf(message,sizeof(message),TR("Запрос отправлен хосту (%s)…","Request sent to the host (%s)…"),h->name);
 message[sizeof(message)-1]=0;
}
static void netClientNudge(int direction,int fine){
 NetPeer *h=netHostPeer();
 NetMsg m;
 if(!h){strcpy(message,TR("Связь с хостом потеряна.","Lost connection to the host."));clientEndPlacement(1);return;}
 ZeroMemory(&m,sizeof(m));
 m.type=NM_NUDGE;m.value=direction;m.flags=fine?NETF_FINE:0;
 netSend(h,&m);
}
static void netClientFinish(int cancel){
 NetPeer *h=netHostPeer();
 if(h)netSendSimple(h,cancel?NM_CANCEL:NM_RELEASE,0);
 strcpy(message,h?(cancel?TR("Отмена отправлена хосту…","Cancel sent to the host…"):TR("Команда «Отпустить» отправлена хосту…","Release sent to the host…"))
                 :TR("Связь с хостом потеряна.","Lost connection to the host."));
}
static void netClientBedReset(void){
 NetPeer *h=netHostPeer();
 if(h)netSendSimple(h,NM_BEDRESET,0);
}
/* Подъём груза модом клиента, как в одиночной игре: груз, которым управляет игра клиента
   (лежит в его машине или касался её), хост сдвинуть не может. */
static void clientLiftLocally(const char *why){
 Item t;
 netLog("%s — lifting locally (body %p)",why,(void*)clientPendingBody);
 if(clientPendingBody && truckReady && pose(truck.body,&t)
    && beginPlacement(clientPendingBody,clientPendingKind,clientPendingAxis,&t,PM_LOCAL)){
  clientLocalVerify=GetTickCount()+NET_VERIFY_MS;if(!clientLocalVerify)clientLocalVerify=1;
  placementControls();
  focusGameForPlacement();
 }else{
  if(!clientPendingBody || !truckReady || !pose(truck.body,&t))
   strcpy(message,TR("Не удалось поднять груз у себя — нажмите «Обновить» и повторите.","Could not lift the cargo on your side — press “Refresh” and retry."));
  netLog("local lift refused: %s",message);
  placementControls();
  showRadar();
 }
}
static void clientResult(NetPeer *p,const NetMsg *m){
 if(clientHost>=0 && p!=&peers[clientHost])return;
 if(m->status!=RS_MOVED)netLog("result from %s: status %d, flags %d, %s",p->name,m->status,m->flags,m->text);
 switch(m->status){
 case RS_STARTED:
  if(!clientPending)return;
  clientPending=0;
  ZeroMemory(&placement,sizeof(placement));
  placement.body=clientPendingBody?clientPendingBody:1;
  placement.mode=PM_CLIENT;
  /* Проверка в СВОЁМ мире: хост может «видеть» груз сдвинутым, а игра клиента держит его на месте. */
  {Item s;clientRemoteVerify=0;
   if(clientPendingBody && pose(clientPendingBody,&s)){clientStartCenter=s.center;clientRemoteVerify=GetTickCount()+NET_CLIENT_VERIFY_MS;if(!clientRemoteVerify)clientRemoteVerify=1;}}
  lstrcpynA(message,keyHintU8,sizeof(message));
  placementControls();
  focusGameForPlacement();
  break;
 case RS_MOVED:
  if(m->flags&NETF_POSE){
   char release[32];bindingTextU8(K_RELEASE,release,32);
   _snprintf(message,sizeof(message),TR("Высота %.2f м · Поворот %d° · Наклон %d° / %d° · Сдвиг %.2f / %.2f м · шаг %s · %s — отпустить","Height %.2f m · Turn %d° · Tilt %d° / %d° · Offset %.2f / %.2f m · %s step · %s — release"),
             m->cargo[0],m->value,m->reserved,m->kind,m->cargo[1],m->cargo[2],(m->flags&(NETF_FINE<<2))?TR("точный","fine"):TR("обычный","normal"),release);
   message[sizeof(message)-1]=0;
  }else if(m->text[0])lstrcpynA(message,m->text,sizeof(message));
  break;
 case RS_DONE:
  /* ответ хоста на нашу отмену после подъёма у себя — сообщение о местном подъёме не затираем */
  if(clientIgnoreDone && (LONG)(GetTickCount()-clientIgnoreDone)<0 && placement.mode!=PM_CLIENT){clientIgnoreDone=0;break;}
  if(m->text[0])lstrcpynA(message,m->text,sizeof(message));
  clientEndPlacement(1);
  break;
 case RS_FAILED:
  if(m->flags&NETF_LOCAL){
   /* Хост не смог сдвинуть груз (он в машине клиента) — поднимаем у себя, как в одиночной игре. */
   clientPending=0;clientRemoteVerify=0;
   if(placement.body && placement.mode==PM_LOCAL)break; /* уже подняли сами по своей проверке */
   if(placement.mode==PM_CLIENT){placement.body=0;placement.mode=PM_LOCAL;}
   clientLiftLocally("host could not move the cargo");
   break;
  }
  /* обычный отказ — ниже */
 default: /* RS_FAILED, RS_BUSY, RS_DENIED */
  clientPending=0;
  lstrcpynA(message,m->text[0]?m->text:TR("Хост отказал.","The host refused."),sizeof(message));
  clientEndPlacement(1);
  break;
 }
 InvalidateRect(window,0,TRUE);
}

/* ── Разбор входящих ── */
static void netHandle(NetPeer *p,NetMsg *m){
 if(memcmp(m->magic,NET_MAGIC,4))return;
 m->text[sizeof(m->text)-1]=0;
 p->lastSeen=GetTickCount();
 p->lang=m->lang?1:0;
 if((m->type==NM_HELLO || m->type==NM_HOST) && m->text[0] && p->transport==NT_TCP)lstrcpynA(p->name,m->text,sizeof(p->name));
 switch(m->type){
 case NM_HELLO:
  if(!p->isClient)netLog("client seen: %s",p->name);
  p->isClient=1;
  if(netMode==NET_HOST)netAnnounce(p);
  break;
 case NM_HOST:
  if(!p->isHost)netLog("host seen: %s",p->name);
  p->isHost=1;
  break;
 case NM_LOAD:case NM_NUDGE:case NM_RELEASE:case NM_CANCEL:case NM_BEDRESET:
  if(netMode!=NET_HOST){
   char text[200];int saved=uiLang;
   uiLang=p->lang;lstrcpynA(text,TR("У этого игрока мод не в режиме «Хост».","This player's mod is not in “Host” mode."),sizeof(text));uiLang=saved;
   netReply(p,RS_DENIED,text);
   break;
  }
  if(m->type==NM_LOAD)hostLoad(p,m);
  else if(m->type==NM_NUDGE)hostNudge(p,m);
  else if(m->type==NM_BEDRESET){ZeroMemory(&p->bed,sizeof(p->bed));netLog("bed reset by %s",p->name);}
  else hostFinish(p,m->type==NM_CANCEL);
  break;
 case NM_RESULT:
  if(netMode==NET_CLIENT)clientResult(p,m);
  break;
 case NM_BYE:
  p->isHost=p->isClient=0;
  break;
 }
}

/* ── TCP ── */
static int wsaReady(void){
 if(!wsaState){WSADATA d;wsaState=WSAStartup(MAKEWORD(2,2),&d)==0?1:-1;}
 return wsaState==1;
}
static void peerDrop(NetPeer *p,const char *why){
 if(!p->used)return;
 netLog("drop %s (%s)",p->name,why);
 if(p->transport==NT_TCP && p->sock!=INVALID_SOCKET)closesocket(p->sock);
 p->sock=INVALID_SOCKET;
 p->used=0;p->isHost=p->isClient=0;
 /* Друг отключился, пока хост держал его груз, — сразу возвращаем груз на место,
    чтобы освободившийся слот не достался другому игроку вместе с грузом. */
 if(placement.body && placement.mode==PM_REMOTE && placement.peer==(int)(p-peers))hostWatchdog();
}
static void netCloseTcp(void){
 int i;
 if(netListen!=INVALID_SOCKET){closesocket(netListen);netListen=INVALID_SOCKET;}
 for(i=0;i<NET_PEERS;i++)if(peers[i].used && peers[i].transport==NT_TCP)peerDrop(&peers[i],"closed");
}
static void netOpenListen(void){
 struct sockaddr_in a;u_long nb=1;
 if(netListen!=INVALID_SOCKET || netMode!=NET_HOST || netTcpPort<=0 || !wsaReady())return;
 netListen=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
 if(netListen==INVALID_SOCKET)return;
 ZeroMemory(&a,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((u_short)netTcpPort);a.sin_addr.s_addr=htonl(INADDR_ANY);
 if(bind(netListen,(struct sockaddr*)&a,sizeof(a)) || listen(netListen,4) || ioctlsocket(netListen,FIONBIO,&nb)){
  _snprintf(netProblem,sizeof(netProblem),TR("TCP-порт %d занят","TCP port %d is busy"),netTcpPort);netProblem[sizeof(netProblem)-1]=0;
  netLog("listen on %d failed: %d",netTcpPort,WSAGetLastError());
  closesocket(netListen);netListen=INVALID_SOCKET;
  return;
 }
 netLog("listening on TCP %d",netTcpPort);
}
static void netTryConnect(void){
 char host[128],*colon;
 char port[16];
 struct addrinfo hints,*res=0;
 SOCKET s;u_long nb=1;int i;
 if(netMode!=NET_CLIENT || !netHostAddress[0] || !wsaReady())return;
 for(i=0;i<NET_PEERS;i++)if(peers[i].used && peers[i].transport==NT_TCP)return; /* уже есть или подключаемся */
 if(netNextConnect && (LONG)(GetTickCount()-netNextConnect)<0)return;
 netNextConnect=GetTickCount()+NET_CONNECT_MS;
 WideCharToMultiByte(CP_UTF8,0,netHostAddress,-1,host,sizeof(host),0,0);
 _snprintf(port,sizeof(port),"%d",netTcpPort>0?netTcpPort:NET_DEFAULT_PORT);port[15]=0;
 colon=strrchr(host,':');
 if(colon && !strchr(colon+1,']') && strchr(host,':')==colon){*colon=0;lstrcpynA(port,colon+1,sizeof(port));}
 ZeroMemory(&hints,sizeof(hints));hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;hints.ai_protocol=IPPROTO_TCP;
 if(getaddrinfo(host,port,&hints,&res) || !res){
  _snprintf(netProblem,sizeof(netProblem),TR("Адрес «%s» не распознан","Address “%s” not recognised"),host);netProblem[sizeof(netProblem)-1]=0;
  return;
 }
 s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
 if(s!=INVALID_SOCKET && !ioctlsocket(s,FIONBIO,&nb)){
  int r=connect(s,res->ai_addr,(int)res->ai_addrlen);
  if(r==0 || WSAGetLastError()==WSAEWOULDBLOCK){
   NetPeer *p=peerNewTcp(s,1);
   if(p){lstrcpynA(p->name,host,sizeof(p->name));p->lastSeen=GetTickCount();s=INVALID_SOCKET;netLog("connecting to %s:%s",host,port);}
  }
 }
 if(s!=INVALID_SOCKET)closesocket(s);
 freeaddrinfo(res);
}
static void netTcpTick(void){
 int i;
 if(netListen!=INVALID_SOCKET){
  for(;;){
   struct sockaddr_in a;int len=sizeof(a);u_long nb=1;
   SOCKET s=accept(netListen,(struct sockaddr*)&a,&len);
   NetPeer *p;
   if(s==INVALID_SOCKET)break;
   ioctlsocket(s,FIONBIO,&nb);
   p=peerNewTcp(s,0);
   if(!p){closesocket(s);break;}
   _snprintf(p->name,sizeof(p->name),"%u.%u.%u.%u",a.sin_addr.S_un.S_un_b.s_b1,a.sin_addr.S_un.S_un_b.s_b2,a.sin_addr.S_un.S_un_b.s_b3,a.sin_addr.S_un.S_un_b.s_b4);
   p->name[sizeof(p->name)-1]=0;
   netLog("TCP client connected: %s",p->name);
  }
 }
 netTryConnect();
 for(i=0;i<NET_PEERS;i++){
  NetPeer *p=&peers[i];
  if(!p->used || p->transport!=NT_TCP)continue;
  if(p->connecting){
   fd_set w,e;struct timeval zero={0,0};
   FD_ZERO(&w);FD_ZERO(&e);FD_SET(p->sock,&w);FD_SET(p->sock,&e);
   if(select(0,0,&w,&e,&zero)>0){
    if(FD_ISSET(p->sock,&e)){
     _snprintf(netProblem,sizeof(netProblem),TR("Хост %s не отвечает по TCP","Host %s does not answer on TCP"),p->name);netProblem[sizeof(netProblem)-1]=0;
     peerDrop(p,"connect failed");continue;
    }
    if(FD_ISSET(p->sock,&w)){p->connecting=0;p->lastSeen=GetTickCount();netProblem[0]=0;netLog("TCP connected to %s",p->name);netAnnounce(p);}
   }else if(GetTickCount()-p->lastSeen>NET_CONNECT_MS){peerDrop(p,"connect timeout");continue;}
   continue;
  }
  for(;;){
   int got=recv(p->sock,(char*)p->rx+p->rxLen,sizeof(NetMsg)-p->rxLen,0);
   if(got==0){peerDrop(p,"closed by peer");break;}
   if(got<0){if(WSAGetLastError()!=WSAEWOULDBLOCK)peerDrop(p,"recv failed");break;}
   p->rxLen+=got;
   if(p->rxLen==sizeof(NetMsg)){NetMsg m;memcpy(&m,p->rx,sizeof(m));p->rxLen=0;netHandle(p,&m);if(!p->used)break;}
  }
 }
}

/* ── Steam ── */
static void netSteamTick(void){
 SteamMsgHead *list[16];
 int n,i;
 DWORD now=GetTickCount();
 if(!netSteamOn || !steamLoad())return;
 if(!netNextAnnounce || (LONG)(now-netNextAnnounce)>=0){
  int total=steam.friendCount(steam.friends,4); /* k_EFriendFlagImmediate */
  netNextAnnounce=now+NET_ANNOUNCE_MS;
  for(i=0;i<total && i<1000;i++){
   uint64_t id=steam.friendAt(steam.friends,i,4);
   FriendGame g;
   NetPeer *p;
   SteamIdent ident;
   ZeroMemory(&g,sizeof(g));
   if(!steam.friendGame(steam.friends,id,&g) || (unsigned)(g.game&0xffffff)!=steam.app)continue;
   if(!(p=peerBySteam(id,1)))continue;
   {const char *name=steam.friendName(steam.friends,id);lstrcpynA(p->name,name&&*name?name:"Steam",sizeof(p->name));}
   ZeroMemory(&ident,sizeof(ident));steam.setId(&ident,id);
   steam.accept(steam.msgs,&ident);
   netAnnounce(p);
  }
 }
 n=steam.recv(steam.msgs,NET_CHANNEL,list,16);
 for(i=0;i<n;i++){
  if(list[i]->size==sizeof(NetMsg)){
   NetMsg m;NetPeer *p;
   memcpy(&m,list[i]->data,sizeof(m));
   p=peerBySteam(steam.getId(&list[i]->peer),1);
   if(p){
    if(!p->name[0]){const char *name=steam.friendName(steam.friends,p->steamId);lstrcpynA(p->name,name&&*name?name:"Steam",sizeof(p->name));}
    netHandle(p,&m);
   }
  }
  steam.release(list[i]);
 }
}

/* ── Общий цикл (таймер NET_TIMER, 50 мс) ── */
static void netTick(void){
 DWORD now=GetTickCount();
 int i;
 static DWORD nextTcpAnnounce;
 if(netMode==NET_OFF)return;
 netSteamTick();
 /* TCP-друзьям напоминаем о себе, чтобы связь не считалась пропавшей. */
 if(!nextTcpAnnounce || (LONG)(now-nextTcpAnnounce)>=0){
  nextTcpAnnounce=now+NET_ANNOUNCE_MS;
  for(i=0;i<NET_PEERS;i++)if(peers[i].used && peers[i].transport==NT_TCP && !peers[i].connecting)netAnnounce(&peers[i]);
 }
 netTcpTick();
 for(i=0;i<NET_PEERS;i++)if(peers[i].used && peers[i].transport==NT_TCP && !peers[i].connecting && now-peers[i].lastSeen>NET_ALIVE_MS*2)peerDrop(&peers[i],"timeout");
 if(netMode==NET_HOST){hostRetryTick();hostWatchdog();hostVerifyTick();}
 if(netMode==NET_CLIENT){
  /* Клиент поднял груз у себя: проверяем, что игра его действительно сдвинула. */
  if(clientLocalVerify && (LONG)(now-clientLocalVerify)>=0){
   float toTarget,fromStart;
   clientLocalVerify=0;
   if(placement.body && placement.mode==PM_LOCAL && placement.body==clientPendingBody){
    int moved=placementLanded(&toTarget,&fromStart);
    netLog("local verify: %s (to target %.2f m, from start %.2f m)",moved?"moved":"NOT moved",toTarget,fromStart);
    if(!moved){
     finishPlacement(1);
     strcpy(message,TR("Груз не сдвинулся ни у хоста, ни у вас. Пришлите KOTEuKA_CargoLoad.log обоих игроков.","The cargo did not move on the host or on your side. Please send KOTEuKA_CargoLoad.log from both players."));
     showRadar();
     InvalidateRect(window,0,TRUE);
    }
   }
  }
  /* Хост сказал «поднял», но в мире клиента груз стоит: им управляет игра клиента (груз касался
     его машины). Отменяем у хоста и поднимаем сами. */
  if(clientRemoteVerify && (LONG)(now-clientRemoteVerify)>=0){
   Item s;
   clientRemoteVerify=0;
   if(placement.body && placement.mode==PM_CLIENT && placement.body==clientPendingBody && pose(clientPendingBody,&s)){
    float dx=s.center.x-clientStartCenter.x,dy=s.center.y-clientStartCenter.y,dz=s.center.z-clientStartCenter.z;
    float d=(float)sqrt(dx*dx+dy*dy+dz*dz);
    netLog("client verify: %s (moved %.2f m in this game)",d>=NET_MOVED_M?"moved":"NOT moved",d);
    if(d<NET_MOVED_M){
     NetPeer *h=netHostPeer();
     if(h)netSendSimple(h,NM_CANCEL,0);
     placement.body=0;placement.mode=PM_LOCAL;
     clientIgnoreDone=now+3000;if(!clientIgnoreDone)clientIgnoreDone=1;
     clientLiftLocally("cargo did not move in this game");
     InvalidateRect(window,0,TRUE);
    }
   }
  }
  if(clientPending && now-clientPendingTime>NET_PENDING_MS){
   clientPending=0;
   strcpy(message,TR("Хост не ответил. Проверьте, что у него режим «Хост».","The host did not answer. Check that it is in “Host” mode."));
   InvalidateRect(window,0,TRUE);
  }
  if(placement.body && placement.mode==PM_CLIENT && !netHostPeer()){
   strcpy(message,TR("Связь с хостом потеряна — размещение прервано.","Lost connection to the host — placement stopped."));
   clientEndPlacement(1);
   InvalidateRect(window,0,TRUE);
  }
 }
 {static DWORD lastPaint;if(now-lastPaint>1000){lastPaint=now;InvalidateRect(window,0,FALSE);}}
}
static void netSetMode(int mode){
 int i;
 static const WCHAR *names[]={L"off",L"host",L"client"};
 if(mode==netMode)return;
 if(placement.body){strcpy(message,TR("Сначала отпустите или отмените груз.","Release or cancel the cargo first."));InvalidateRect(window,0,TRUE);return;}
 for(i=0;i<NET_PEERS;i++)if(peers[i].used && peerAlive(&peers[i]))netSendSimple(&peers[i],NM_BYE,0);
 netCloseTcp();
 for(i=0;i<NET_PEERS;i++){peers[i].isHost=peers[i].isClient=0;}
 clientPending=0;clientHost=-1;hostRetry=0;netProblem[0]=0;netNextAnnounce=0;netNextConnect=0;
 netMode=mode;
 WritePrivateProfileStringW(L"Net",L"Mode",names[mode],ini);
 netLog("mode %ls",names[mode]);
 if(mode==NET_HOST)netOpenListen();
 if(mode==NET_OFF)strcpy(message,TR("Сеть выключена: мод работает только в вашей игре.","Network off: the mod only works in your own game."));
 else if(mode==NET_HOST)strcpy(message,TR("Режим «Хост»: друзья с этим модом смогут грузить в вашей игре.","Host mode: friends with this mod can load cargo in your game."));
 else strcpy(message,TR("Режим «Клиент»: команды погрузки уходят хосту. Ищу хоста…","Client mode: loading commands go to the host. Looking for the host…"));
 for(i=0;i<SETTING_COUNT;i++)InvalidateRect(GetDlgItem(window,settingButtons[i].id),0,FALSE);
 InvalidateRect(window,0,TRUE);
}
static void netLoadSettings(void){
 WCHAR v[16];
 GetPrivateProfileStringW(L"Net",L"Mode",L"off",v,16,ini);
 netMode=!lstrcmpiW(v,L"host")?NET_HOST:!lstrcmpiW(v,L"client")?NET_CLIENT:NET_OFF;
 netSteamOn=GetPrivateProfileIntW(L"Net",L"Steam",1,ini)!=0;
 netTcpPort=GetPrivateProfileIntW(L"Net",L"Port",NET_DEFAULT_PORT,ini);
 if(netTcpPort<0 || netTcpPort>65535)netTcpPort=NET_DEFAULT_PORT;
 GetPrivateProfileStringW(L"Net",L"HostAddress",L"",netHostAddress,128,ini);
}
static void netStart(const WCHAR *logPath){
 lstrcpynW(netLogPath,logPath,MAX_PATH);
 netLoadSettings();
 netLog("start: mode %d, steam %d, port %d",netMode,netSteamOn,netTcpPort);
 if(netMode==NET_HOST)netOpenListen();
 /* 10 мс (на деле ~15 мс — шаг таймера Windows): прежние 50 мс давали до ~0,1 с задержки на каждое нажатие. */
 SetTimer(window,NET_TIMER,10,0);
}
static void netStatusText(WCHAR *out,int size,COLORREF *color){
 WCHAR name[64];
 int i,friends=0;
 *color=COL_MUTED;
 if(netMode==NET_OFF){lstrcpynW(out,TR(L"выкл — только своя игра",L"off — own game only"),size);return;}
 if(netProblem[0] && !(netMode==NET_CLIENT && netHostPeer())){
  MultiByteToWideChar(CP_UTF8,0,netProblem,-1,out,size);out[size-1]=0;*color=COL_WARN;return;
 }
 if(netMode==NET_HOST){
  for(i=0;i<NET_PEERS;i++)if(peers[i].isClient && peerAlive(&peers[i]))friends++;
  if(placement.body && placement.mode==PM_REMOTE){
   MultiByteToWideChar(CP_UTF8,0,peers[placement.peer].name,-1,name,64);name[63]=0;
   _snwprintf(out,size,TR(L"грузит: %ls",L"loading: %ls"),name);*color=COL_ACCENT;
  }else{
   _snwprintf(out,size,TR(L"друзей с модом: %d",L"friends with mod: %d"),friends);*color=friends?COL_GOOD:COL_TEXT;
  }
 }else{
  NetPeer *h=netHostPeer();
  if(h){MultiByteToWideChar(CP_UTF8,0,h->name,-1,name,64);name[63]=0;_snwprintf(out,size,TR(L"хост: %ls",L"host: %ls"),name);*color=COL_GOOD;}
  else{lstrcpynW(out,TR(L"ищу хоста…",L"looking for host…"),size);*color=COL_WARN;}
 }
 out[size-1]=0;
}
