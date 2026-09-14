// device/genet/rp1fan.c — Raspberry Pi 5 のファン（RP1 PWM1 チャネル 3、GPIO45）。
//
// Linux の cooling_fan ノード（bcm2712-rpi-5-b.dts）と pwm-rp1.c / clk-rp1.c / pinctrl-rp1.c から:
//   pwms = <&rp1_pwm1 3 41566 PWM_POLARITY_INVERTED>   周期 41566 ns、極性は反転
//   cooling-levels = <0 75 125 175 250>                 duty は 0..255 のうちこの段
//   rp1_pwm1 = RP1 の 0x9c000（CPU 0x1F0009C000）、clk_pwm1 は 50 MHz（xosc を分周 1 で）
//   GPIO45 の FUNCSEL 0 = pwm1、pull-down
// Xinu は firmware が起動時に決めた状態のまま動くので、普段ファンは止まっている。
#include "uart.h"
#ifdef RP1_ETH_BASE
#define RP1_PWM1_BASE   0x1F0009C000UL
#define RP1_CLOCKS_BASE 0x1F00018000UL
#define RP1_GPIO_IO     0x1F000D0000UL
#define RP1_GPIO_PADS   0x1F000F0000UL
#define P(off)   (*(volatile unsigned int *)(RP1_PWM1_BASE + (off)))
#define C(off)   (*(volatile unsigned int *)(RP1_CLOCKS_BASE + (off)))
#define G_CTRL(bk,pn) (*(volatile unsigned int *)(RP1_GPIO_IO   + (unsigned long)(bk)*0x4000 + (unsigned long)(pn)*8 + 4))
#define G_PAD(bk,pn)  (*(volatile unsigned int *)(RP1_GPIO_PADS + (unsigned long)(bk)*0x4000 + 4 + (unsigned long)(pn)*4))
#define PWM_GLOBAL_CTRL     0x000
#define PWM_CHANNEL_CTRL(x) (0x014 + (x)*16)
#define PWM_RANGE(x)        (0x018 + (x)*16)
#define PWM_DUTY(x)         (0x020 + (x)*16)
#define PWM_CHANNEL_DEFAULT ((1u<<8)|(1u<<0))   /* pwm-rp1.c: trailing-edge mode + bit 8 */
#define PWM_POLARITY        (1u<<3)
#define SET_UPDATE          (1u<<31)
#define CLK_PWM1_CTRL     0x084
#define CLK_PWM1_DIV_INT  0x088
#define CLK_PWM1_DIV_FRAC 0x08c
#define CLK_CTRL_ENABLE   (1u<<11)
#define FAN_CH     3
#define FAN_RANGE  2078u        /* 41566 ns / 20 ns（50 MHz） */
static int g_fan_level = -1;    /* 0..255、-1 = まだ触っていない */

static int f_put(char *o,int p,int cap,const char *s){ while(*s&&p<cap-1) o[p++]=*s++; o[p]=0; return p; }
static int f_hex(char *o,int p,int cap,unsigned v){ const char *hx="0123456789abcdef"; p=f_put(o,p,cap,"0x"); for(int i=7;i>=0;i--) if(p<cap-1) o[p++]=hx[(v>>(i*4))&15]; o[p]=0; return p; }
static int f_num(char *o,int p,int cap,long v){ char nb[24]; int k=0; if(v<0){p=f_put(o,p,cap,"-");v=-v;} if(!v)nb[k++]='0'; while(v){nb[k++]=(char)('0'+v%10);v/=10;} while(k){ if(p<cap-1)o[p++]=nb[--k]; else k=0; } o[p]=0; return p; }

/* level: 0..255（Linux の cooling-levels と同じ物差し。250 で最速）。 */
int rp1fan_set(int level)
{
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    /* clk_pwm1: aux 源 2 = xosc(50 MHz)、分周 1、有効 */
    C(CLK_PWM1_DIV_INT)  = 1;
    C(CLK_PWM1_DIV_FRAC) = 0;
    C(CLK_PWM1_CTRL)     = (2u << 5) | 1u | CLK_CTRL_ENABLE;      /* AUXSRC=2, SRC=aux, ENABLE */
    /* GPIO45 = bank 2, pin 11: pull-down、出力有効、FUNCSEL 0 = pwm1 */
    { unsigned pad = G_PAD(2, 11); pad &= ~((1u<<7)|(1u<<3)); pad |= (1u<<2); G_PAD(2, 11) = pad; G_CTRL(2, 11) = 0u; }
    if (P(PWM_GLOBAL_CTRL) == 0xdeaddeadu) return -1;              /* PWM ブロックが無クロック */
    P(PWM_RANGE(FAN_CH)) = FAN_RANGE;
    P(PWM_DUTY(FAN_CH))  = (FAN_RANGE * (unsigned)level) / 255u;
    P(PWM_CHANNEL_CTRL(FAN_CH)) = PWM_CHANNEL_DEFAULT | PWM_POLARITY;   /* 反転極性（DT のとおり） */
    P(PWM_GLOBAL_CTRL) = P(PWM_GLOBAL_CTRL) | (1u << FAN_CH);
    P(PWM_GLOBAL_CTRL) = P(PWM_GLOBAL_CTRL) | SET_UPDATE;
    __asm__ volatile ("dsb sy" ::: "memory");
    g_fan_level = level;
    return 0;
}
int rp1fan_level(void) { return g_fan_level; }
int rp1fan_status(char *out, int cap)
{
    int p = 0;
    p = f_put(out, p, cap, "fan level="); p = f_num(out, p, cap, g_fan_level);
    p = f_put(out, p, cap, " clk_pwm1_ctrl="); p = f_hex(out, p, cap, C(CLK_PWM1_CTRL));
    p = f_put(out, p, cap, " div="); p = f_num(out, p, cap, (long)C(CLK_PWM1_DIV_INT));
    p = f_put(out, p, cap, " pwm_global="); p = f_hex(out, p, cap, P(PWM_GLOBAL_CTRL));
    p = f_put(out, p, cap, " ch3_ctrl="); p = f_hex(out, p, cap, P(PWM_CHANNEL_CTRL(FAN_CH)));
    p = f_put(out, p, cap, " range="); p = f_num(out, p, cap, (long)P(PWM_RANGE(FAN_CH)));
    p = f_put(out, p, cap, " duty="); p = f_num(out, p, cap, (long)P(PWM_DUTY(FAN_CH)));
    p = f_put(out, p, cap, " gpio45_ctrl="); p = f_hex(out, p, cap, G_CTRL(2, 11));
    p = f_put(out, p, cap, "\n");
    return p;
}
#else
int rp1fan_set(int level) { (void)level; return -1; }
int rp1fan_level(void) { return -1; }
int rp1fan_status(char *out, int cap) { const char *s = "fan: no RP1 on this board\n"; int i = 0; while (s[i] && i < cap - 1) { out[i] = s[i]; i++; } out[i] = 0; return i; }
#endif
