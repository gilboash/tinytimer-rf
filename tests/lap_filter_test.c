/* Host test: why the gate defaults to the EMA level filter.
 * cc -std=gnu11 -Igate/src/scan tests/lap_filter_test.c gate/src/scan/lap.c -lm -o /tmp/lap_filter_test && /tmp/lap_filter_test */
#include <stdio.h>
#include <stdlib.h>
#include "lap.h"
static int run(int filter, int width){
  lap_seat_t s; lap_init(&s,125,105); lap_set_filter(&s,filter);
  unsigned passes=0,last=0; uint32_t first=0;
  for(uint32_t t=0;t<9000;t++){
    int v=60+(rand()%5);
    for(int k=0;k<2;k++){ uint32_t c=1000+k*5000;
      if(t>=c&&t<c+width){ double h=width/2.0, d=(t-c<h)?(t-c)/h:(c+width-t)/h; v=60+(int)(140*d);} }
    lap_feed(&s,t,v,1000);
    if(s.lap!=last){last=s.lap;passes++; if(passes==1)first=s.pass_ms; else printf("  filter=%d width=%d: pass %u at %u ms (true centre %u), interval %u ms\n",filter,width,passes,s.pass_ms,1000+(passes-1)*5000+width/2,s.pass_ms-first);}
  }
  return passes;
}
int main(void){
  for(int f=0;f<2;f++) for(int w=100;w<=500;w+=200) printf("filter=%s width=%dms -> %d passes\n",f?"EMA":"MEDIAN",w,run(f,w));
  return 0;
}
