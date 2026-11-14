/* ───── Окно «Сеть»: адрес хоста для TCP, порт, Steam вкл/выкл ───── */
enum { NETW_W=440, NETW_H=300, NETW_ADDR_ID=300, NETW_PORT_ID, NETW_STEAM_ID, NETW_OK_ID, NETW_CANCEL_ID };
static HWND netWindow;
static HFONT netFont;
static HBRUSH netEditBrush;
static float netScale=1;
static int netSteamDraft;
static int ns(int v){return (int)(v*netScale+0.5f);}
static void netWindowSteamLabel(void){
 SetWindowTextW(GetDlgItem(netWindow,NETW_STEAM_ID),netSteamDraft?TR(L"Steam: вкл",L"Steam: on"):TR(L"Steam: выкл",L"Steam: off"));
 InvalidateRect(GetDlgItem(netWindow,NETW_STEAM_ID),0,FALSE);
}
static void netWindowSave(void){
 WCHAR addr[128],port[16];
 int p;
 GetWindowTextW(GetDlgItem(netWindow,NETW_ADDR_ID),addr,128);
 GetWindowTextW(GetDlgItem(netWindow,NETW_PORT_ID),port,16);
 p=_wtoi(port);
 if(p<=0 || p>65535)p=NET_DEFAULT_PORT;
 {WCHAR *s=addr,*e;while(*s==L' ')s++;e=s+lstrlenW(s);while(e>s && e[-1]==L' ')*--e=0;lstrcpynW(netHostAddress,s,128);}
 netTcpPort=p;netSteamOn=netSteamDraft;
 WritePrivateProfileStringW(L"Net",L"HostAddress",netHostAddress,ini);
 saveIniInt(L"Net",L"Port",netTcpPort);
 saveIniInt(L"Net",L"Steam",netSteamOn);
 /* Перезапуск TCP с новыми настройками. */
 netCloseTcp();netProblem[0]=0;netNextConnect=0;
 if(netMode==NET_HOST)netOpenListen();
 netLog("settings: steam %d, port %d, host address set: %d",netSteamOn,netTcpPort,netHostAddress[0]!=0);
 strcpy(message,TR("Настройки сети сохранены.","Network settings saved."));
 InvalidateRect(window,0,TRUE);
}
static void netPaint(HWND h){
 PAINTSTRUCT ps;
 HDC target=BeginPaint(h,&ps),dc;
 RECT client,r;
 HBITMAP bitmap;HGDIOBJ oldBitmap,oldFont;
 HFONT title,small;HBRUSH brush;
 int w,hh;
 GetClientRect(h,&client);
 w=client.right>0?client.right:1;hh=client.bottom>0?client.bottom:1;
 dc=CreateCompatibleDC(target);
 bitmap=CreateCompatibleBitmap(target,w,hh);
 oldBitmap=SelectObject(dc,bitmap);
 brush=CreateSolidBrush(COL_BG);FillRect(dc,&client,brush);DeleteObject(brush);
 SetMapMode(dc,MM_ANISOTROPIC);
 SetWindowExtEx(dc,NETW_W,NETW_H,0);
 SetViewportExtEx(dc,ns(NETW_W),ns(NETW_H),0);
 SetBkMode(dc,TRANSPARENT);
 title=radarFontW(20,FW_BOLD);small=radarFontW(13,FW_NORMAL);
 oldFont=SelectObject(dc,title);
 SetRect(&r,0,0,NETW_W,46);brush=CreateSolidBrush(COL_CARD);FillRect(dc,&r,brush);DeleteObject(brush);
 SetRect(&r,0,43,NETW_W,46);brush=CreateSolidBrush(COL_ACCENT);FillRect(dc,&r,brush);DeleteObject(brush);
 SetRect(&r,16,0,NETW_W,43);
 textW(dc,TR(L"Сеть",L"Network"),&r,DT_SINGLELINE|DT_VCENTER,COL_TEXT);
 SelectObject(dc,small);
 SetRect(&r,16,56,NETW_W-16,96);
 textW(dc,TR(L"Steam находит хоста среди друзей в этой игре сам. Адрес нужен только для связи по IP (локальная сеть, VPN, проброс порта).",
             L"Steam finds the host among your friends in this game. The address is only for IP links (LAN, VPN, port forwarding)."),&r,DT_WORDBREAK,COL_MUTED);
 SetRect(&r,16,100,NETW_W-16,118);
 textW(dc,TR(L"Адрес хоста (для клиента): IP или IP:порт",L"Host address (client): IP or IP:port"),&r,DT_SINGLELINE|DT_VCENTER,COL_TEXT);
 SetRect(&r,16,158,NETW_W-16,176);
 textW(dc,TR(L"TCP-порт (хост слушает его; Windows спросит разрешение брандмауэра)",L"TCP port (the host listens on it; Windows will ask for firewall access)"),&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS,COL_TEXT);
 SelectObject(dc,oldFont);
 DeleteObject(title);DeleteObject(small);
 SetMapMode(dc,MM_TEXT);
 BitBlt(target,0,0,w,hh,dc,0,0,SRCCOPY);
 SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
 EndPaint(h,&ps);
}
static LRESULT CALLBACK netProc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_PAINT){netPaint(h);return 0;}
 if(m==WM_ERASEBKGND)return 1;
 if(m==WM_DRAWITEM){drawButton((const DRAWITEMSTRUCT*)l);return TRUE;}
 if(m==WM_CTLCOLOREDIT){
  SetTextColor((HDC)w,COL_TEXT);SetBkColor((HDC)w,COL_CARD);
  if(!netEditBrush)netEditBrush=CreateSolidBrush(COL_CARD);
  return (LRESULT)netEditBrush;
 }
 if(m==WM_COMMAND){
  int id=LOWORD(w);
  if(id==NETW_STEAM_ID){netSteamDraft=!netSteamDraft;netWindowSteamLabel();}
  else if(id==NETW_OK_ID || id==IDOK){netWindowSave();DestroyWindow(h);}
  else if(id==NETW_CANCEL_ID || id==IDCANCEL)DestroyWindow(h);
  return 0;
 }
 if(m==WM_SETCURSOR && LOWORD(l)==HTCLIENT){SetCursor(radarCursor);return TRUE;}
 if(m==WM_CLOSE){DestroyWindow(h);return 0;}
 if(m==WM_DESTROY){
  netWindow=0;
  if(netFont){DeleteObject(netFont);netFont=0;}
  return 0;
 }
 return DefWindowProcW(h,m,w,l);
}
static void openNetWindow(void){
 static int registered;
 typedef UINT (WINAPI *DpiFn)(HWND);
 DpiFn getDpi=(DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow");
 RECT r,owner;
 WCHAR port[16];
 HWND e;
 if(netWindow){ShowWindow(netWindow,SW_SHOW);SetForegroundWindow(netWindow);return;}
 if(!registered){
  WNDCLASSW wc;
  ZeroMemory(&wc,sizeof(wc));
  wc.lpfnWndProc=netProc;wc.hInstance=self;wc.lpszClassName=L"KOTEuKA_CargoLoadNet";
  wc.hCursor=radarCursor;wc.hbrBackground=CreateSolidBrush(COL_BG);
  registered=RegisterClassW(&wc)!=0;
 }
 netScale=getDpi?getDpi(window)/96.0f:1.0f;
 if(netScale<1)netScale=1;
 SetRect(&r,0,0,ns(NETW_W),ns(NETW_H));
 AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_TOPMOST);
 GetWindowRect(window,&owner);
 netWindow=CreateWindowExW(WS_EX_TOPMOST,L"KOTEuKA_CargoLoadNet",TR(L"KOTEuKA_CargoLoad — сеть",L"KOTEuKA_CargoLoad — network"),WS_CAPTION|WS_SYSMENU,
                           owner.left+40,owner.top+120,r.right-r.left,r.bottom-r.top,window,0,self,0);
 if(!netWindow)return;
 netFont=radarFontW(ns(14),FW_SEMIBOLD);
 e=CreateWindowExW(0,L"EDIT",netHostAddress,WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,ns(16),ns(122),ns(NETW_W-32),ns(28),netWindow,(HMENU)NETW_ADDR_ID,self,0);
 SendMessageW(e,WM_SETFONT,(WPARAM)netFont,FALSE);SendMessageW(e,EM_LIMITTEXT,120,0);
 wsprintfW(port,L"%d",netTcpPort);
 e=CreateWindowExW(0,L"EDIT",port,WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP|ES_NUMBER,ns(16),ns(180),ns(110),ns(28),netWindow,(HMENU)NETW_PORT_ID,self,0);
 SendMessageW(e,WM_SETFONT,(WPARAM)netFont,FALSE);SendMessageW(e,EM_LIMITTEXT,5,0);
 netSteamDraft=netSteamOn;
 CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,ns(142),ns(180),ns(130),ns(28),netWindow,(HMENU)NETW_STEAM_ID,self,0);
 netWindowSteamLabel();
 CreateWindowW(L"BUTTON",TR(L"Сохранить",L"Save"),WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,ns(NETW_W-16-120-8-120),ns(NETW_H-50),ns(120),ns(34),netWindow,(HMENU)NETW_OK_ID,self,0);
 CreateWindowW(L"BUTTON",TR(L"Отмена",L"Cancel"),WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,ns(NETW_W-16-120),ns(NETW_H-50),ns(120),ns(34),netWindow,(HMENU)NETW_CANCEL_ID,self,0);
 ShowWindow(netWindow,SW_SHOW);
 SetForegroundWindow(netWindow);
}
