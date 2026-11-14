/* Проверка математики размещения без игры: поворот по курсу, наклон, жёсткое смещение частей.
   В мод не входит. Сборка: cl /utf-8 /TC /I src tools\pose_test.c /link user32.lib gdi32.lib */
#define AUTOLOAD_TEST
#include "KOTEuKA_CargoLoad.c"

static int checks,failed;
static void check(int ok,const char *what){checks++;if(!ok)failed++;printf("%s %s\n",ok?"OK  ":"FAIL",what);}
static float frand(void){return (float)rand()/RAND_MAX*2-1;}
static float vdist(Vec a,Vec b){return (float)sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));}
static Vec unitQuat(void){Vec q={frand(),frand(),frand(),frand()};float n=(float)sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);q.x/=n;q.y/=n;q.z/=n;q.w/=n;return q;}
/* Прежняя формула (до наклона) — эталон для tiltSteps=0. */
static void oldPose(const Placement *t,Vec *position,Vec *rotation){
 double h=t->turnSteps*(3.141592653589793/72.0);
 float s=sin(h),c=cos(h),ys=2*s*c,yc=1-2*s*s,ox=t->position.x-t->center.x,oz=t->position.z-t->center.z;
 Vec o=t->rotation,cen=t->center;
 cen.x+=t->along*t->forwardX-t->across*t->forwardZ;cen.z+=t->along*t->forwardZ+t->across*t->forwardX;
 position->x=cen.x+yc*ox+ys*oz;position->y=t->position.y+t->lift;position->z=cen.z-ys*ox+yc*oz;
 rotation->x=c*o.x+s*o.z;rotation->y=c*o.y+s*o.w;rotation->z=c*o.z-s*o.x;rotation->w=c*o.w-s*o.y;
}
int main(void){
 int i,worst=0;
 float maxErr=0;
 srand(7);
 /* 1. Без наклона новая формула совпадает со старой. */
 for(i=0;i<2000;i++){
  Placement t;Vec p1,r1,p2,r2,c,a;float h=(float)(frand()*3.14159);
  ZeroMemory(&t,sizeof(t));
  t.position.x=frand()*50;t.position.y=frand()*10;t.position.z=frand()*50;
  t.center.x=t.position.x+frand();t.center.y=t.position.y+frand();t.center.z=t.position.z+frand();
  t.rotation=unitQuat();t.forwardX=(float)cos(h);t.forwardZ=(float)sin(h);
  t.along=frand()*3;t.across=frand()*2;t.lift=frand();t.turnSteps=rand()%72-36;
  oldPose(&t,&p1,&r1);adjustedPose(&t,&p2,&r2,&c,&a);
  {float e=vdist(p1,p2)+fabsf(r1.x-r2.x)+fabsf(r1.y-r2.y)+fabsf(r1.z-r2.z)+fabsf(r1.w-r2.w);if(e>maxErr){maxErr=e;worst=i;}}
 }
 printf("max difference old/new (tilt 0): %g (case %d)\n",maxErr,worst);
 check(maxErr<1e-4f,"без наклона поза совпадает с прежней формулой");
 /* 2. Наклон 180°: «верх» груза смотрит вниз, ось вдоль кузова не меняется. */
 {Placement t;Vec p,r,c,a,up={0,1,0,0},fw,upAfter,fwAfter;
  ZeroMemory(&t,sizeof(t));
  t.rotation.w=1;t.forwardX=0.6f;t.forwardZ=0.8f;t.tiltSteps=36;
  t.position.x=1;t.position.y=2;t.position.z=3;t.center=t.position;
  adjustedPose(&t,&p,&r,&c,&a);
  fw.x=t.forwardX;fw.y=0;fw.z=t.forwardZ;fw.w=0;
  quatRotate(&r,&up,&upAfter);quatRotate(&r,&fw,&fwAfter);
  printf("up after 180 tilt: %.3f %.3f %.3f; forward: %.3f %.3f %.3f\n",upAfter.x,upAfter.y,upAfter.z,fwAfter.x,fwAfter.y,fwAfter.z);
  check(fabsf(upAfter.y+1)<1e-4f,"наклон 180° переворачивает груз (верх → низ)");
  check(fabsf(fwAfter.x-fw.x)<1e-4f && fabsf(fwAfter.z-fw.z)<1e-4f,"ось вдоль кузова при наклоне не меняется");
  check(vdist(p,t.position)<1e-4f,"центр масс на месте, если начало тела совпадает с ним");
 }
 /* 3. Наклон 90°: верх уходит вбок (поперёк кузова), вращение — вокруг центра масс. */
 {Placement t;Vec p,r,c,a,up={0,1,0,0},upAfter;
  ZeroMemory(&t,sizeof(t));
  t.rotation.w=1;t.forwardX=1;t.forwardZ=0;t.tiltSteps=18;
  t.center.x=0;t.center.y=5;t.center.z=0;t.position=t.center;t.position.y=4; /* начало тела на 1 м ниже ЦМ */
  adjustedPose(&t,&p,&r,&c,&a);
  quatRotate(&r,&up,&upAfter);
  printf("up after 90 tilt: %.3f %.3f %.3f; body origin %.3f %.3f %.3f\n",upAfter.x,upAfter.y,upAfter.z,p.x,p.y,p.z);
  check(fabsf(upAfter.y)<1e-4f && fabsf(fabsf(upAfter.z)-1)<1e-4f,"наклон 90° кладёт груз на бок поперёк кузова");
  check(fabsf(vdist(p,c)-1)<1e-4f && fabsf(p.y-c.y)<1e-4f,"начало тела поворачивается вокруг центра масс");
 }
 /* 3б. Наклон вперёд/назад: −90° опускает перед груза вниз, ось поперёк кузова не меняется. */
 {Placement t;Vec p,r,c,a,fw,right,fwAfter,rightAfter;
  ZeroMemory(&t,sizeof(t));
  t.rotation.w=1;t.forwardX=0.6f;t.forwardZ=0.8f;t.pitchSteps=-18;
  t.center.y=5;t.position=t.center;
  adjustedPose(&t,&p,&r,&c,&a);
  fw.x=t.forwardX;fw.y=0;fw.z=t.forwardZ;fw.w=0;
  right.x=-t.forwardZ;right.y=0;right.z=t.forwardX;right.w=0;
  quatRotate(&r,&fw,&fwAfter);quatRotate(&r,&right,&rightAfter);
  printf("forward after -90 pitch: %.3f %.3f %.3f\n",fwAfter.x,fwAfter.y,fwAfter.z);
  check(fabsf(fwAfter.y+1)<1e-4f,"наклон вперёд 90° опускает перед груза вниз");
  check(fabsf(rightAfter.x-right.x)<1e-4f && fabsf(rightAfter.z-right.z)<1e-4f && fabsf(rightAfter.y)<1e-4f,"поперечная ось при наклоне вперёд не меняется");
 }
 /* 3в. Все три оси сразу: поворот остаётся единичным кватернионом, расстояние до ЦМ сохраняется. */
 {int k,bad=0;
  for(k=0;k<500;k++){
   Placement t;Vec p,r,c,a;float h=(float)(frand()*3.14159),n;
   ZeroMemory(&t,sizeof(t));
   t.rotation=unitQuat();t.forwardX=(float)cos(h);t.forwardZ=(float)sin(h);
   t.center.x=frand()*9;t.center.y=3;t.center.z=frand()*9;
   t.position.x=t.center.x+frand();t.position.y=t.center.y+frand();t.position.z=t.center.z+frand();
   t.turnSteps=rand()%72-36;t.tiltSteps=rand()%72-36;t.pitchSteps=rand()%72-36;
   adjustedPose(&t,&p,&r,&c,&a);
   n=(float)sqrt(r.x*r.x+r.y*r.y+r.z*r.z+r.w*r.w);
   if(fabsf(n-1)>1e-4f || fabsf(vdist(p,c)-vdist(t.position,t.center))>1e-4f)bad++;
  }
  check(!bad,"три оси вместе: поворот единичный, начало тела на том же расстоянии от ЦМ");
 }
 /* 4. Составной груз: жёсткое смещение сохраняет расстояния и относительные повороты частей. */
 {Vec q0=unitQuat(),q1=unitQuat(),inv={-q0.x,-q0.y,-q0.z,q0.w},d,main0={3,1,2,0},main1={20,4,-7,0},part0={4.5f,0.6f,2.8f,0},off,ro,part1,pr0=unitQuat(),pr1,rel0,rel1,i1;
  quatMul(&q1,&inv,&d);
  off.x=part0.x-main0.x;off.y=part0.y-main0.y;off.z=part0.z-main0.z;off.w=0;
  quatRotate(&d,&off,&ro);
  part1.x=main1.x+ro.x;part1.y=main1.y+ro.y;part1.z=main1.z+ro.z;part1.w=0;
  quatMul(&d,&pr0,&pr1);
  /* относительный поворот часть/главное: q0⁻¹·pr0 до и q1⁻¹·pr1 после */
  quatMul(&inv,&pr0,&rel0);
  {Vec inv1={-q1.x,-q1.y,-q1.z,q1.w};quatMul(&inv1,&pr1,&rel1);}
  (void)i1;
  check(fabsf(vdist(part0,main0)-vdist(part1,main1))<1e-4f,"часть остаётся на том же расстоянии от главного тела");
  check(fabsf(rel0.x-rel1.x)+fabsf(rel0.y-rel1.y)+fabsf(rel0.z-rel1.z)+fabsf(rel0.w-rel1.w)<1e-4f,"поворот части относительно главного тела не меняется");
 }
 /* 5. Клавиши по умолчанию: наклон Z/C, коды DirectInput.
    loadKeyBindings сохраняет [Keys] — только во временный .ini, иначе Windows пишет в win.ini. */
 GetTempPathW(MAX_PATH,ini);lstrcatW(ini,L"kcl_pose_test.ini");DeleteFileW(ini);
 loadKeyBindings();applyPlacementKeys();
 DeleteFileW(ini);
 check(placementKeys[K_TILT_LEFT-1].key=='Z' && placementKeys[K_TILT_LEFT-1].scan==0x2c && placementKeys[K_TILT_LEFT-1].direction==TILT_LEFT,"наклон влево — Z (DIK 0x2C)");
 check(placementKeys[K_TILT_RIGHT-1].key=='C' && placementKeys[K_TILT_RIGHT-1].scan==0x2e && placementKeys[K_TILT_RIGHT-1].direction==TILT_RIGHT,"наклон вправо — C (DIK 0x2E)");
 check(placementKeys[K_PITCH_FORWARD-1].key=='T' && placementKeys[K_PITCH_FORWARD-1].scan==0x14 && placementKeys[K_PITCH_FORWARD-1].direction==PITCH_FORWARD,"наклон вперёд — T (DIK 0x14)");
 check(placementKeys[K_PITCH_BACK-1].key=='G' && placementKeys[K_PITCH_BACK-1].scan==0x22 && placementKeys[K_PITCH_BACK-1].direction==PITCH_BACK,"наклон назад — G (DIK 0x22)");
 check(placementKeys[PK_RELEASE].key==VK_RETURN && placementKeys[PK_CANCEL].key==VK_ESCAPE,"Enter/Esc остались на своих местах");
 printf("checks %d, failed %d\n",checks,failed);
 return failed?1:0;
}
