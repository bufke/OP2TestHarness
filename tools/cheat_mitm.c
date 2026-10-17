// MITM search for "Dan's N could V<mid>O<punct>" hashing to a target (Outpost2.exe cheat hash, 0x58642a).
// usage: mitm target nouns verbs objs mid  (mid must be >= 7 chars incl. fixed text? no: tail bytes come from " could "+V+mid)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t u32;
static u32 rol(u32 x,int n){n&=31;return n?(x<<n)|(x>>(32-n)):x;}
static u32 ror(u32 x,int n){n&=31;return n?(x>>n)|(x<<(32-n)):x;}
static u32 dw(const unsigned char*p){return p[0]|p[1]<<8|p[2]<<16|(u32)p[3]<<24;}
static char **rdl(const char*f,int*n){FILE*F=fopen(f,"r");char b[256];int c=0,cap=1024;char**v=malloc(cap*sizeof*v);
 while(fgets(b,256,F)){b[strcspn(b,"\r\n")]=0;if(!*b)continue;if(c==cap)v=realloc(v,(cap*=2)*sizeof*v);v[c++]=strdup(b);}*n=c;return v;}
typedef struct{uint64_t key;int oi,pi;}E;
static int cmp(const void*a,const void*b){uint64_t x=((E*)a)->key,y=((E*)b)->key;return x<y?-1:x>y;}
int main(int argc,char**argv){
 u32 T=strtoul(argv[1],0,16);int nn,nv,no;char**N=rdl(argv[2],&nn),**V=rdl(argv[3],&nv),**O=rdl(argv[4],&no);const char*mid=argv[5];
 const char*P[]={"",".","!","!!","...","?"};int np=6;
 // distinct tails per a: last 4+a bytes of " could "+V+mid
 int cap=1<<20,ne=0;E*es=malloc(cap*sizeof(E));
 char (*tails)[8]=malloc(4*nv*8);int nt[4]={0};
 for(int a=0;a<4;a++){for(int v=0;v<nv;v++){char buf[300];snprintf(buf,300," could %s%s",V[v],mid);int l=strlen(buf);char t[8]={0};memcpy(t,buf+l-4-a,4+a);
   int dup=0;for(int i=0;i<nt[a];i++)if(!memcmp(tails[a*nv+i],t,8)){dup=1;break;}if(!dup)memcpy(tails[a*nv+nt[a]++],t,8);}}
 for(int a=0;a<4;a++)for(int ti=0;ti<nt[a];ti++){const char*t=tails[a*nv+ti];
  for(int o=0;o<no;o++)for(int p=0;p<np;p++){unsigned char X[400]={0};int l=4+a;memcpy(X,t,l);l+=sprintf((char*)X+l,"%s%s",O[o],P[p]);
   int z=l;int j=z/4,k=z%4;u32 s=T;
   {u32 Ed=dw(X+4*(j-1));int c=(Ed&0xff)>>4;for(int r=0;r<=k;r++)s=ror(s^Ed,c);}
   for(int i=j-1;i>=1;i--){s^=dw(X+4*i)>>24;u32 Ed=dw(X+4*(i-1));int c=(Ed&0xff)>>4;for(int r=0;r<4;r++)s=ror(s^Ed,c);}
   uint64_t tk=0;memcpy(&tk,t,4+a<8?4+a:8);
   if(ne==cap)es=realloc(es,(cap*=2)*sizeof(E));
   es[ne].key=((uint64_t)s<<32)^(uint64_t)(a)<<29^(tk*0x9E3779B97F4A7C15ull>>35);es[ne].oi=o;es[ne].pi=p;ne++;}}
 qsort(es,ne,sizeof(E),cmp);fprintf(stderr,"backward entries %d\n",ne);
 long hits=0;
 for(int n=0;n<nn;n++)for(int v=0;v<nv;v++){unsigned char B[400]={0};int L=sprintf((char*)B,"Dan's %s could %s%s",N[n],V[v],mid);
  int a=L%4,m=L/4;u32 s=0;
  for(int i=0;i<m;i++){u32 Ed=dw(B+4*(i?i-1:0));int c=(Ed&0xff)>>4;for(int r=0;r<4;r++)s=rol(s,c)^Ed;s^=dw(B+4*i)>>24;}
  uint64_t tk=0;memcpy(&tk,B+L-4-a,4+a<8?4+a:8);
  uint64_t key=((uint64_t)s<<32)^(uint64_t)(a)<<29^(tk*0x9E3779B97F4A7C15ull>>35);
  E q={key};E*f=bsearch(&q,es,ne,sizeof(E),cmp);if(!f)continue;while(f>es&&f[-1].key==key)f--;
  for(;f<es+ne&&f->key==key;f++){printf("HIT Dan's %s could %s%s%s%s\n",N[n],V[v],mid,O[f->oi],P[f->pi]);hits++;}}
 fprintf(stderr,"hits %ld\n",hits);}
