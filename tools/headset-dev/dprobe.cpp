#include <openvr.h>
#include <cstdio>
#include <unistd.h>
#include <time.h>
using namespace vr;
int main(int argc,char**argv){EVRInitError e;VR_Init(&e,VRApplication_Overlay);if(e)return 1;
int n=argc>1?atoi(argv[1]):30;
for(int i=0;i<n;i++){const char*keys[]={"valve.plasma.desktopmode","valve.steam.desktopgame.0",nullptr};
VROverlayHandle_t h=0;char found[128]="-";
for(int k=0;keys[k];k++){VROverlayHandle_t x=0;if(VROverlay()->FindOverlay(keys[k],&x)==VROverlayError_None){h=x;snprintf(found,128,"%s",keys[k]);}}
// any desktopgame key
timespec ts;clock_gettime(CLOCK_REALTIME,&ts);
printf("%ld.%03ld dash=%d ov=%s active=%d visible=%d\n",ts.tv_sec%1000,ts.tv_nsec/1000000,VROverlay()->IsDashboardVisible(),found,h?VROverlay()->IsActiveDashboardOverlay(h):-1,h?VROverlay()->IsOverlayVisible(h):-1);fflush(stdout);sleep(1);}
VR_Shutdown();}
