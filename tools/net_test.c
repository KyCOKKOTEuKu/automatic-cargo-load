/* Проверка сетевого протокола без игры: два процесса — хост и клиент — по TCP на 127.0.0.1.
   Игровых адресов нет, поэтому хост честно отвечает «не вижу грузовик»; проверяется доставка
   команд, ответы, язык ответа и поведение при разрыве связи. В мод не входит.
   Запуск: net_test.exe host <ini> <log>   и   net_test.exe client <ini> <log> <result.txt> */
#define AUTOLOAD_TEST
#include "KOTEuKA_CargoLoad.c"

static FILE *out;
static void pump(int ms){DWORD end=GetTickCount()+ms;MSG m;while((LONG)(GetTickCount()-end)<0){while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}}
static int waitFor(int (*cond)(void),int ms){DWORD end=GetTickCount()+ms;while((LONG)(GetTickCount()-end)<0){pump(20);if(cond())return 1;}return 0;}
static int hostFound(void){return netHostPeer()!=0;}
static int answered(void){return !clientPending;}
static char lastMessage[400];
static int messageChanged(void){return strcmp(message,lastMessage)!=0;}
static int checks,failed;
static void check(int ok,const char *what){checks++;if(!ok)failed++;fprintf(out,"%s: %s\n",ok?"OK  ":"FAIL",what);fflush(out);}

int wmain(int argc,WCHAR **argv){
 WNDCLASSW wc={0};
 int isHost;
 if(argc<4)return 2;
 isHost=!lstrcmpW(argv[1],L"host");
 lstrcpynW(ini,argv[2],MAX_PATH);
 self=GetModuleHandleW(0);
 wc.lpfnWndProc=proc;wc.hInstance=self;wc.lpszClassName=isHost?L"KclNetHost":L"KclNetClient";RegisterClassW(&wc);
 window=CreateWindowExW(0,wc.lpszClassName,L"net test",WS_OVERLAPPED,0,0,100,100,0,0,self,0);
 loadLanguage();loadKeyBindings();applyPlacementKeys();
 netStart(argv[3]);
 if(isHost){
  pump(25000);              /* хост просто отвечает; клиент делает проверки */
  return 0;
 }
 out=_wfopen(argv[4],L"w");
 if(!out)return 3;
 fprintf(out,"client lang=%s, mode=%d\n",uiLang?"en":"ru",netMode);
 check(netMode==NET_CLIENT,"клиент читает [Net] Mode=client из .ini");
 check(waitFor(hostFound,8000),"клиент нашёл хоста по TCP и получил NM_HOST");
 if(netHostPeer())fprintf(out,"host name: %s, transport %d\n",netHostPeer()->name,netHostPeer()->transport);
 /* 1. Погрузка: у хоста нет игры — он должен ответить отказом на языке клиента. */
 {Vec c={10,2,20,0},t={12,2,18,0};
  selectedBody=0x1234;
  netClientLoad(FIRST_ASSET_KIND,c,t,&truckProfiles[0]);
  check(clientPending==1,"запрос погрузки отправлен (ждём ответа)");
  check(waitFor(answered,4000),"хост ответил на NM_LOAD");
  fprintf(out,"answer: %s\n",message);
  check(strstr(message,"The host cannot see your truck")!=0,"ответ хоста — на языке клиента (EN), причина: не видит грузовик");
  check(placement.body==0,"клиент не начал размещение без подтверждения хоста");
 }
 /* 2. Сдвиг без активного размещения — хост отвечает RS_DONE, клиент ничего не ломает. */
 {strcpy(lastMessage,message);
  ZeroMemory(&placement,sizeof(placement));placement.body=0x1234;placement.mode=PM_CLIENT;
  nudgePlacementEx(MOVE_FORWARD,0);
  check(waitFor(messageChanged,3000) || placement.body==0,"на сдвиг без размещения хост ответил");
  pump(300);
  check(placement.body==0 && placement.mode==PM_LOCAL,"клиент вышел из размещения по ответу хоста RS_DONE");
 }
 /* 3. Кнопки окна во время чужого размещения: finishPlacement у клиента отправляет команду. */
 {ZeroMemory(&placement,sizeof(placement));placement.body=0x1234;placement.mode=PM_CLIENT;
  check(finishPlacement(1)==1 && placement.body==0,"«Отменить» у клиента уходит хосту и снимает размещение");
  fprintf(out,"after cancel: %s\n",message);
 }
 /* 3б. Задержка: время от сдвига до ответа хоста (TCP 127.0.0.1, опрос сети ~10–15 мс). */
 {int k;DWORD total=0,worst=0;
  for(k=0;k<10;k++){
   DWORD t0;
   ZeroMemory(&placement,sizeof(placement));placement.body=0x1234;placement.mode=PM_CLIENT;
   t0=GetTickCount();
   nudgePlacementEx(MOVE_FORWARD,0);
   {DWORD end=t0+2000;while(placement.body && (LONG)(GetTickCount()-end)<0){MSG mm;while(PeekMessageW(&mm,0,0,0,PM_REMOVE)){TranslateMessage(&mm);DispatchMessageW(&mm);}Sleep(1);}}
   {DWORD dt=GetTickCount()-t0;total+=dt;if(dt>worst)worst=dt;}
  }
  fprintf(out,"nudge round trip: avg %lu ms, worst %lu ms\n",total/10,worst);
  check(total/10<40,"в среднем ответ хоста на нажатие быстрее 40 мс (при прежнем опросе раз в 50 мс — около 50–100 мс)");
 }
 /* 3в. Хост сообщил «груз в машине клиента» — клиент пробует поднять у себя. Без игры подъём
       невозможен, поэтому ждём аккуратный отказ: размещения нет, окно показано, причина в сообщении. */
 {NetMsg r;ZeroMemory(&r,sizeof(r));memcpy(r.magic,NET_MAGIC,4);r.type=NM_RESULT;r.status=RS_FAILED;r.flags=NETF_LOCAL;
  lstrcpyA(r.text,"local please");
  ZeroMemory(&placement,sizeof(placement));placement.body=0x1234;placement.mode=PM_CLIENT;
  clientPending=1;clientPendingBody=0x1234;clientPendingKind=FIRST_ASSET_KIND;
  clientResult(netHostPeer(),&r);
  fprintf(out,"after NETF_LOCAL: placement=%p mode=%d message=%s\n",(void*)placement.body,placement.mode,message);
  check(placement.body==0 && placement.mode==PM_LOCAL && !clientPending,"NETF_LOCAL: клиент снял ожидание и не завис в размещении");
  check(strstr(message,"Could not lift the cargo on your side")!=0,"NETF_LOCAL без игры: понятная причина отказа");
 }
 /* 4. Хост пропал: клиент через ~15 с перестаёт его видеть. */
 fprintf(out,"waiting for host exit...\n");fflush(out);
 {DWORD end=GetTickCount()+40000;while(netHostPeer() && (LONG)(GetTickCount()-end)<0)pump(100);}
 check(netHostPeer()==0,"после выхода хоста клиент перестал считать его доступным");
 {Vec c={0},t={0};netClientLoad(FIRST_ASSET_KIND,c,t,&truckProfiles[0]);}
 fprintf(out,"without host: %s\n",message);
 check(strstr(message,"Host not found")!=0,"без хоста погрузка не отправляется, понятное сообщение");
 fprintf(out,"checks %d, failed %d\n",checks,failed);
 fclose(out);
 return failed?1:0;
}
