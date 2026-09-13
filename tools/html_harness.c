/* Mac 側ハーネス v2: html.c + jpfont.c を素の C として組み、16 ドット字形で PPM に描く。
   使い方: h2 index.html [i18n.js|-] [style.css|-] [幅] [en|ja]  ; FUZZ=1 で変異入力 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int IMW=720, IMH=6000;
static unsigned int *img;
void fill_rect(int x,int y,int w,int h,unsigned int c){ for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) if(i>=0&&j>=0&&i<IMW&&j<IMH) img[j*IMW+i]=c; }
void draw_bitmap_glyph(int px,int py,const unsigned char*rows,int w,int h,unsigned int fg,unsigned int bg,int scale,int tr){
  int bpr=(w+7)/8;
  for(int gy=0;gy<h;gy++) for(int gx=0;gx<w;gx++){ int on=rows[gy*bpr+(gx>>3)]&(0x80>>(gx&7)); if(!on&&tr) continue;
    fill_rect(px+gx*scale,py+gy*scale,scale,scale,on?fg:bg);} }
void html_layout(const char*,int,int); int html_height(void); int html_ops(void);
void html_draw(int,int,int,int,int,unsigned int); int html_text(char*,int);
int html_i18n_apply(const char*,int,const char*,int,char*,int); void html_set_css(const char*,int);
int html_link_at(int,int,const char**); int html_find_stylesheet(const char*,int,char*,int);
static char *rd(const char*p,int*n){ if(!strcmp(p,"-")){*n=0;return strdup("");} FILE*f=fopen(p,"rb"); if(!f){perror(p);exit(1);} fseek(f,0,2); long l=ftell(f); fseek(f,0,0); char*b=malloc(l+1); *n=fread(b,1,l,f); b[*n]=0; fclose(f); return b; }
static char en[65536]; static char txt[98304];
int main(int argc,char**argv){
  int hn,dn,cn; char*h=rd(argv[1],&hn); char*d=rd(argc>2?argv[2]:"-",&dn); char*c=rd(argc>3?argv[3]:"-",&cn);
  int w=argc>4?atoi(argv[4]):674; int ja=argc>5&&!strcmp(argv[5],"ja");
  const char*src=h; int sn=hn;
  if(dn&&!ja){ int el=html_i18n_apply(h,hn,d,dn,en,sizeof en); src=en; sn=el; fprintf(stderr,"i18n: %d -> %d bytes\n",hn,el); }
  html_set_css(c,cn);
  { char ss[256]; if(html_find_stylesheet(h,hn,ss,sizeof ss)) fprintf(stderr,"stylesheet: %s\n",ss); }
  if(getenv("FUZZ")){ img=calloc(IMW*IMH,4); srand(7); char*m=malloc(sn+64); char*mc=malloc(cn+64);
    for(int it=0;it<3000;it++){ int len=sn; memcpy(m,src,sn); int kind=it%5;
      if(kind==0) len=rand()%(sn+1);
      else if(kind==1){ for(int k=0;k<20;k++) m[rand()%sn]=(char)(rand()%256); }
      else if(kind==2){ int p=rand()%sn; const char*ins="<ul><li><b><a href=\"x\" style=\"color:#f00;padding:2rem\">"; int il=strlen(ins); memmove(m+p+il,m+p,sn-p); memcpy(m+p,ins,il); len=sn+il; }
      else if(kind==3){ for(int k=0;k<5;k++){ int p=rand()%sn; m[p]= "<>&\"'/{}:;"[rand()%10]; } }
      else { /* CSS の変異 */ int cl=cn; memcpy(mc,c,cn); for(int k=0;k<10&&cn;k++) mc[rand()%cn]=(char)(rand()%256); if(cn&&rand()%2) cl=rand()%(cn+1); html_set_css(mc,cl); }
      html_layout(m,len,w); html_text(txt,sizeof txt); html_draw(0,0,w,IMH,0,0xFF0A0E14U);
      const char*hr; html_link_at(rand()%w, rand()%2000, &hr);
      if(it%2==0) html_i18n_apply(m,len,d,dn,en,sizeof en);
      html_set_css(c,cn);
    }
    fprintf(stderr,"fuzz ok\n"); return 0; }
  html_layout(src,sn,w);
  int H=html_height(); fprintf(stderr,"ops=%d height=%d\n",html_ops(),H);
  html_text(txt,sizeof txt); fputs(txt,stdout);
  IMH=H+16; img=calloc(IMW*IMH,4); for(int i=0;i<IMW*IMH;i++) img[i]=0xFF0A0E14U;
  html_draw(8,8,w,IMH,0,0xFF0A0E14U);
  FILE*f=fopen(argc>6?argv[6]:"render.ppm","wb"); fprintf(f,"P6\n%d %d\n255\n",IMW,IMH);
  for(int i=0;i<IMW*IMH;i++){ unsigned int cc=img[i]; fputc((cc>>16)&255,f); fputc((cc>>8)&255,f); fputc(cc&255,f);} fclose(f);
  return 0; }
