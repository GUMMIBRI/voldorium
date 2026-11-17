/* PC replace.txt subrectangle support for SC1 SDL GPU. Does not own GPU data. */
#include "pc-piece.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

extern unsigned char *stbi_load_from_memory(const unsigned char *,int,int *,int *,int *,int);
extern int stbi_info_from_memory(const unsigned char *,int,int *,int *,int *);
extern uint32_t hw_reg(uint32_t);
extern uint32_t tex_decode_rgba(uint32_t,uint32_t,uint32_t *,uint32_t *,uint32_t *);
#define MAX_PIECES 64
#define MAX_FILE (16*1024*1024)
#define MAX_SIDE 2048
#define VRAM_SIZE (8u*1024*1024)

typedef struct { uint64_t hash;uint32_t x,y,w,h;char filename[64]; } Piece;
static Piece pieces[MAX_PIECES];static unsigned count;static int initialized;
static char directory[1024];
static int valid_filename(const char *s){
 if(!*s || s[0]=='.')return 0;
 for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.'))return 0;
 return 1;
}
static void init(void){
 initialized=1;const char *root=getenv("M1_TEXTURES");if(!root||!*root||strlen(root)>=sizeof(directory))return;
 memcpy(directory,root,strlen(root)+1);char path[1100],line[512];
 if(snprintf(path,sizeof(path),"%s/replace.txt",directory)>=(int)sizeof(path))return;
 FILE *f=fopen(path,"rb");if(!f)return;
 while(count<MAX_PIECES && fgets(line,sizeof(line),f)){
  Piece p;unsigned long long h;char trailing;memset(&p,0,sizeof(p));
  if(line[0]=='#'||sscanf(line,"%llx %u %u %u %u %63s %c",&h,&p.x,&p.y,&p.w,&p.h,p.filename,&trailing)!=6)continue;
  if(!p.w||!p.h||!valid_filename(p.filename))continue;
  p.hash=(uint64_t)h;pieces[count++]=p;
 }
 fclose(f);fprintf(stderr,"SC1 PC textures: %u bounded manifest pieces available\n",count);
}
static unsigned char *load(const Piece *p,uint32_t *w,uint32_t *h){
 char path[1100];int n=snprintf(path,sizeof(path),"%s/%s",directory,p->filename);
 if(n<0||(size_t)n>=sizeof(path))return NULL;
 FILE *f=fopen(path,"rb");if(!f)return NULL;
 if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}long size=ftell(f);
 if(size<=0||size>MAX_FILE||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
 unsigned char *buf=malloc((size_t)size);if(!buf){fclose(f);return NULL;}
 if(fread(buf,1,(size_t)size,f)!=(size_t)size){free(buf);fclose(f);return NULL;}fclose(f);
 int iw=0,ih=0,ch=0;unsigned char *out=NULL;
 if(stbi_info_from_memory(buf,(int)size,&iw,&ih,&ch)&&iw>0&&ih>0&&iw<=MAX_SIDE&&ih<=MAX_SIDE)
  out=stbi_load_from_memory(buf,(int)size,&iw,&ih,&ch,4);
 free(buf);if(out){*w=(uint32_t)iw;*h=(uint32_t)ih;}return out;
}
static uint32_t rgba(unsigned r,unsigned g,unsigned b,unsigned a){return r|g<<8|b<<16|a<<24;}
static uint32_t palette(uint32_t index){
 uint32_t e=hw_reg(0x005f9000u+4*(index&1023));uint32_t fmt=hw_reg(0x005f8108)&3;
 if(fmt==0){unsigned r=(e>>10)&31,g=(e>>5)&31,b=e&31;return rgba(r<<3|r>>2,g<<3|g>>2,b<<3|b>>2,(e&0x8000)?255:0);}
 if(fmt==1){unsigned r=(e>>11)&31,g=(e>>5)&63,b=e&31;return rgba(r<<3|r>>2,g<<2|g>>4,b<<3|b>>2,255);}
 if(fmt==2)return rgba(((e>>8)&15)*17,((e>>4)&15)*17,(e&15)*17,((e>>12)&15)*17);
 return rgba((e>>16)&255,(e>>8)&255,e&255,e>>24);
}
static uint32_t lerp(uint32_t a,uint32_t b,unsigned t){uint32_t o=0;for(unsigned sh=0;sh<32;sh+=8)o|=((((a>>sh)&255)*(255-t)+((b>>sh)&255)*t+127)/255)<<sh;return o;}
/* Only non-VQ paletted no-mip textures with non-wrapping, identical PC/PS5
   source spans are supported. Unknown identity returns original safely. */
unsigned char *sc1_pc_piece(uint64_t hash,uint32_t tcw,uint32_t tsp,uint32_t start,uint32_t span,uint32_t *ow,uint32_t *oh){
 if(!ow||!oh)return NULL;uint32_t fmt=(tcw>>27)&7,w=8u<<((tsp>>3)&7),h=8u<<(tsp&7);
 if((fmt!=5&&fmt!=6)||(tcw&(3u<<30))||start!=((tcw&0x1fffff)<<3)||start>=VRAM_SIZE||span>VRAM_SIZE-start||span!=(uint64_t)w*h*(fmt==5?4:8)/8)return NULL;
 if(!initialized)init();if(!count)return NULL;
 unsigned indices[MAX_PIECES],n=0,k=0;unsigned char *images[MAX_PIECES]={0};uint32_t widths[MAX_PIECES],heights[MAX_PIECES];
 for(unsigned i=0;i<count;i++){
  Piece *p=&pieces[i];if(p->hash!=hash)continue;
  if(p->x>w||p->y>h||p->w>w-p->x||p->h>h-p->y)goto fail;
  uint32_t iw=0,ih=0;unsigned char *pixels=load(p,&iw,&ih);if(!pixels)goto fail;
  if(iw%p->w||ih%p->h||iw/p->w!=ih/p->h||iw/p->w==0){free(pixels);goto fail;}
  unsigned scale=iw/p->w;if((k&&scale!=k)||(uint64_t)w*scale>MAX_SIDE||(uint64_t)h*scale>MAX_SIDE){free(pixels);goto fail;}
  k=scale;indices[n]=i;images[n]=pixels;widths[n]=iw;heights[n]=ih;n++;
 }
 if(!n||!k)return NULL;
 uint32_t *original=malloc((size_t)w*h*4),*out=malloc((size_t)w*h*k*k*4);if(!original||!out){free(original);free(out);goto fail;}
 uint32_t dw=0,dh=0;if(!tex_decode_rgba(tcw,tsp,original,&dw,&dh)||dw!=w||dh!=h){free(original);free(out);goto fail;}
 for(uint32_t Y=0;Y<h*k;Y++){
  float fy=(Y+0.5f)/k-0.5f;int y0=(int)floorf(fy);float ty=fy-y0;
  for(uint32_t X=0;X<w*k;X++){
   float fx=(X+0.5f)/k-0.5f;int x0=(int)floorf(fx);float tx=fx-x0,acc[4]={0,0,0,0};
   for(int j=0;j<4;j++){
    int x=x0+(j&1),y=y0+(j>>1);x=x<0?0:x>=(int)w?(int)w-1:x;y=y<0?0:y>=(int)h?(int)h-1:y;
    float wt=((j&1)?tx:1-tx)*((j>>1)?ty:1-ty);uint32_t c=original[(uint32_t)y*w+(uint32_t)x];float a=(float)(c>>24);
    for(unsigned ch=0;ch<3;ch++)acc[ch]+=wt*((c>>(ch*8))&255)*a;acc[3]+=wt*a;
   }
   uint32_t c=(uint32_t)(acc[3]+0.5f)<<24;for(unsigned ch=0;ch<3&&acc[3]>0.5f;ch++){float v=acc[ch]/acc[3];c|=(uint32_t)(v>255?255:v+0.5f)<<(8*ch);}out[(size_t)Y*w*k+X]=c;
  }
 }
 free(original);uint32_t bank=fmt==5?((tcw>>21)&63)<<4:((tcw>>25)&3)<<8;
 for(unsigned i=0;i<n;i++){
  Piece *p=&pieces[indices[i]];
  for(uint32_t y=0;y<heights[i];y++)for(uint32_t x=0;x<widths[i];x++){
   const unsigned char *q=images[i]+((size_t)y*widths[i]+x)*4;
   uint32_t a=palette(bank+q[0]),b=palette(bank+q[1]),c=lerp(a,b,q[2]);
   c=(c&0xffffff)|((((c>>24)*q[3]+127)/255)<<24);
   out[(size_t)(p->y*k+y)*w*k+p->x*k+x]=c;
  }
 }
 for(unsigned i=0;i<n;i++)free(images[i]);*ow=w*k;*oh=h*k;
 fprintf(stderr,"SC1 PC textures: composed %u pieces for %016" PRIx64 " at %ux%u\n",n,hash,*ow,*oh);return (unsigned char*)out;
fail:
 for(unsigned i=0;i<n;i++)free(images[i]);return NULL;
}

