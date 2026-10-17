// Brute force "Dan's truck could <w1><mid><w2><punct>" against the Outpost 2 cheat hash (exe 0x58642a).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t u32;
static inline u32 rol(u32 x,int n){n&=31;return n?(x<<n)|(x>>(32-n)):x;}
static u32 h(const unsigned char*b){ // b padded with >=8 zero bytes
  u32 ebx=0,edx,edi; int i=0; memcpy(&edx,b,4); edi=edx;
  for(;;){ int cl=(edi&0xff)>>4; u32 eax=rol(edx,8);
    for(int k=0;k<4;k++){ ebx=rol(ebx,cl)^edi; eax=(eax>>8)|(eax<<24); if((eax&0xff)==0) return ebx; }
    ebx=(ebx&~0xffu)|((ebx^eax)&0xff); edi=edx; i+=4; memcpy(&edx,b+i,4); } }
int main(int argc,char**argv){
  u32 T=strtoul(argv[1],0,16); const char*pre=argv[2],*mid=argv[3]; FILE*f=fopen(argv[4],"r");
  static char W[200000][16]; int n=0; while(n<200000&&fgets(W[n],16,f)){W[n][strcspn(W[n],"\n")]=0;n++;}
  const char*P[]={"",".","!","?","!!","..."};
  { unsigned char t[64]={0}; strcpy((char*)t,"Dan's dog could bite harder!"); if(h(t)!=0xabe0cc11u){fprintf(stderr,"hash self-test FAILED\n");return 1;} }
  #pragma omp parallel for schedule(dynamic,64)
  for(int a=0;a<n;a++){ unsigned char b[128]; int l0=sprintf((char*)b,"%s%s%s",pre,W[a],mid);
    for(int c=0;c<n;c++){ int l1=l0+sprintf((char*)b+l0,"%s",W[c]);
      for(int p=0;p<6;p++){ int l=l1+sprintf((char*)b+l1,"%s",P[p]); memset(b+l,0,12);
        if(h(b)==T){ 
          #pragma omp critical
          {printf("HIT %s\n",b);fflush(stdout);} } } } }
  return 0; }
