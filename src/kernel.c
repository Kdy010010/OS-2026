/*  OS/2026 – writable 2025FS + CLI + ELF32 loader (fixed 32-byte dirent)
 *
 *  build:
 *     gcc   -m32 -ffreestanding -nostdlib -c kernel.c
 *     ld    -m elf_i386 -T link.ld -nostdlib -o kernel.elf kernel.o
 *     objcopy -O binary kernel.elf kernel.bin
 *
 *  + Hangul IME (2-beolsik, 40 jamo: 19+21)
 *  + Ctrl+Space toggle [HAN]/[ENG]
 *  + VGA dynamic font upload on 0x80..0xFF (QEMU stable)
 *  + Neon Desktop GUI  – 'gui' from CLI, ESC exits
 *
 *  v3.1 – Hangul font v4 (complete rewrite):
 *   - cho_draw_V/H: 초성 수직/수평모음별 2가지 형태
 *   - jung_draw_V: 중성 x8..15 오른쪽 영역에만 그림
 *   - jong_draw: 받침 x2..12 y11..15 하단 중앙
 *   - 복합받침(ㄳㄵㄶㄺ..ㅄ) 좌/우 분리 렌더
 *   - compose: 수직모음=cho_draw_V, 수평모음=cho_draw_H 자동 선택
 *   - 모든 11172자 조합 가능 (ㄱ~ㅎ × ㅏ~ㅣ × 받침 포함)
 *   - THICK=1: Hangul strokes visible
 *   - Neon color palette (magenta/cyan/black desktop)
 *   - Window manager: up to 3 windows (File Explorer, Terminal, System Info)
 *   - Taskbar row 23: [>] START btn + open-window buttons + clock area
 *   - Start menu: File Explorer / Terminal / Info / Reboot / Halt
 *   - Shutdown confirm dialog (R=reboot, H=halt, ESC=cancel)
 *   - Mini terminal window: ls, mkdir, rm inside GUI
 *   - File Explorer: dual-panel, Tab/Enter/BS/Del
 *   - Hotkeys: S=start, 1/2/3=open window, W=cycle focus, Q=close window
 */

#include <stdint.h>
#define MAX_DEPTH 16

static uint32_t path_lba[MAX_DEPTH];
static char     path_name[MAX_DEPTH][24];
static int      path_depth = 0;

/* ---------- tiny C helpers ---------- */
static int  strlen(const char*s){int n=0;while(s[n])++n;return n;}
static int  strcmp(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return (unsigned char)*a-(unsigned char)*b;}
static int  strncmp(const char*a,const char*b,int n){while(n--&&*a&&*a==*b){++a;++b;}return n<0?0:(unsigned char)*a-(unsigned char)*b;}
static void strncpy(char*d,const char*s,int n){int i=0;for(;i<n&&s[i];++i)d[i]=s[i];for(;i<n;++i)d[i]=0;}
static void* memcpy(void*d,const void*s,uint32_t n){uint8_t*D=d;const uint8_t*S=s;while(n--)*D++=*S++;return d;}
static void* memset(void*d,int c,uint32_t n){uint8_t*D=d;while(n--)*D++=(uint8_t)c;return d;}

/* ---------- low-level I/O ---------- */
static inline void  outb(uint16_t p,uint8_t  v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline void  outw(uint16_t p,uint16_t v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}
static inline void  outl(uint16_t p,uint32_t v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t  inb(uint16_t p){uint8_t r;__asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p));return r;}
static inline uint16_t inw(uint16_t p){uint16_t r;__asm__ volatile("inw %1,%0":"=a"(r):"Nd"(p));return r;}
static inline uint32_t inl(uint16_t p){uint32_t r;__asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p));return r;}

/* ---------- VGA text console ---------- */
#define VGA ((uint16_t*)0xB8000)
#define COLS 80
#define ROWS 25

static uint8_t row = 0, col = 0;

static void scroll(void){
    for(int r = 1; r < ROWS; ++r)
        for(int c = 0; c < COLS; ++c)
            VGA[(r-1)*COLS + c] = VGA[r*COLS + c];
    for(int c = 0; c < COLS; ++c)
        VGA[(ROWS-1)*COLS + c] = 0x1F00 | ' ';
    row = ROWS - 1;
}

void putc(char c){
    if(c == '\n') { row++; col = 0; }
    else if(c=='\b'){
        if(col){ col--; VGA[row*COLS+col]=(0x1F<<8)|' '; }
    }
    else {
        VGA[row * COLS + col] = (0x1F << 8) | (uint8_t)c;
        if (++col == COLS) { col = 0; ++row; }
    }
    if (row == ROWS) scroll();
}

void puts(const char* s){while(*s)putc(*s++);}
static void print_uint(uint32_t x){char b[11];int i=0;if(!x){putc('0');return;}while(x){b[i++]='0'+(x%10);x/=10;}while(i--)putc(b[i]);}

/* =========================================================
 * VGA dynamic font upload (plane2) - QEMU stable + SEQ3 fix
 * ========================================================= */
static inline void outb16(uint16_t p, uint8_t idx, uint8_t val){ outb(p, idx); outb(p+1, val); }
static inline uint8_t inb16(uint16_t p, uint8_t idx){ outb(p, idx); return inb(p+1); }

static uint8_t s_seq1, s_seq2, s_seq3, s_seq4;
static uint8_t s_gc0, s_gc1, s_gc3, s_gc4, s_gc5, s_gc6, s_gc8;

static void vga_begin_font_write_plane2(void){
    s_seq1 = inb16(0x3C4, 0x01);
    s_seq2 = inb16(0x3C4, 0x02);
    s_seq3 = inb16(0x3C4, 0x03);
    s_seq4 = inb16(0x3C4, 0x04);

    s_gc0  = inb16(0x3CE, 0x00);
    s_gc1  = inb16(0x3CE, 0x01);
    s_gc3  = inb16(0x3CE, 0x03);
    s_gc4  = inb16(0x3CE, 0x04);
    s_gc5  = inb16(0x3CE, 0x05);
    s_gc6  = inb16(0x3CE, 0x06);
    s_gc8  = inb16(0x3CE, 0x08);

    /* screen off */
    outb16(0x3C4, 0x01, (uint8_t)(s_seq1 | 0x20));

    /* ★ force character map select to bank 0 */
    outb16(0x3C4, 0x03, 0x00);

    /* plane2 write enable */
    outb16(0x3C4, 0x02, 0x04);

    /* ★ font access mode */
    outb16(0x3C4, 0x04, 0x07);

    /* set/reset off, mask=FF */
    outb16(0x3CE, 0x00, 0x00);
    outb16(0x3CE, 0x01, 0x00);
    outb16(0x3CE, 0x03, 0x00);
    outb16(0x3CE, 0x08, 0xFF);

    /* read map plane2 */
    outb16(0x3CE, 0x04, 0x02);

    /* write mode 0 */
    outb16(0x3CE, 0x05, 0x00);

    /* map A0000 */
    outb16(0x3CE, 0x06, 0x04);
}

static void vga_end_font_write_restore(void){
    outb16(0x3C4, 0x02, s_seq2);
    outb16(0x3C4, 0x03, s_seq3);
    outb16(0x3C4, 0x04, s_seq4);

    outb16(0x3CE, 0x00, s_gc0);
    outb16(0x3CE, 0x01, s_gc1);
    outb16(0x3CE, 0x03, s_gc3);
    outb16(0x3CE, 0x04, s_gc4);
    outb16(0x3CE, 0x05, s_gc5);
    outb16(0x3CE, 0x06, s_gc6);
    outb16(0x3CE, 0x08, s_gc8);

    outb16(0x3C4, 0x01, s_seq1);
}

static void vga_write_glyph8x16(uint8_t ch, const uint8_t rows16[16]){
    vga_begin_font_write_plane2();
    volatile uint8_t *dst = (volatile uint8_t*)0xA0000;
    uint32_t base = (uint32_t)ch * 32;
    for(int y=0;y<16;y++) dst[base + y] = rows16[y];
    for(int y=16;y<32;y++) dst[base + y] = 0;
    vga_end_font_write_restore();
}

/* ---------- ATA-PIO ---------- */
#define ATA_DATA 0x1F0
static void ata_wait(void){while(inb(0x1F7)&0x80);}
static void ata_read28(uint32_t lba,uint8_t sc,void*dst){
    ata_wait();
    outb(0x1F2,sc);
    outb(0x1F3,lba);
    outb(0x1F4,lba>>8);
    outb(0x1F5,lba>>16);
    outb(0x1F6,0xE0|((lba>>24)&0xF));
    outb(0x1F7,0x20);
    uint16_t*buf=dst;
    for(int s=0;s<sc;++s){
        ata_wait();
        for(int i=0;i<256;++i) buf[s*256+i]=inw(ATA_DATA);
    }
}
static void ata_write28(uint32_t lba,uint8_t sc,const void*src){
    ata_wait();
    outb(0x1F2,sc);
    outb(0x1F3,lba);
    outb(0x1F4,lba>>8);
    outb(0x1F5,lba>>16);
    outb(0x1F6,0xE0|((lba>>24)&0xF));
    outb(0x1F7,0x30);
    const uint16_t*buf=src;
    for(int s=0;s<sc;++s){
        ata_wait();
        for(int i=0;i<256;++i) outw(ATA_DATA,buf[s*256+i]);
    }
    ata_wait();
}

/* =========================================================
 *  NETWORK STACK  –  PCI + RTL8139 + ETH + ARP + IP + UDP + TCP + DNS + HTTP
 *  QEMU 기본 네트워크 (-netdev user): 10.0.2.0/24
 *    My IP   : 10.0.2.15
 *    Gateway : 10.0.2.2
 *    DNS     : 10.0.2.3
 * ========================================================= */

/* ── 메모리 레이아웃 ──────────────────────────────────────
 * task 스택       : 0x500000 ~ 0x510000 (8 × 8KB)
 * RTL8139 RX ring : 0x600000 (64KB+16)   ← task 스택과 겹치지 않게 이전
 * TX buffers      : 0x612000 (4 × 2KB)   ← RX 끝(0x610010) 이후로 분리
 * Scratch/packet  : 0x620000 (32KB)
 * ────────────────────────────────────────────────────────*/
#define NIC_RX_BUF    ((volatile uint8_t*)0x600000)
#define NIC_TX_BUF0   ((volatile uint8_t*)0x612000)
#define NIC_SCRATCH   ((uint8_t*)0x620000)
#define NIC_RX_SIZE   (65536+16)

/* ── PCI アクセス ─────────────────────────────────────── */
static uint32_t pci_read(uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg){
    uint32_t addr=0x80000000u|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)func<<8)|(reg&0xFC);
    outl(0xCF8,addr);
    return inl(0xCFC);
}
static void pci_write(uint8_t bus,uint8_t dev,uint8_t func,uint8_t reg,uint32_t val){
    uint32_t addr=0x80000000u|((uint32_t)bus<<16)|((uint32_t)dev<<11)|((uint32_t)func<<8)|(reg&0xFC);
    outl(0xCF8,addr);
    outl(0xCFC,val);
}

/* ── RTL8139 레지스터 오프셋 ─────────────────────────── */
#define RTL_IDR0     0x00
#define RTL_MAR0     0x08
#define RTL_TSD0     0x10
#define RTL_TSAD0    0x20
#define RTL_RBSTART  0x30
#define RTL_CMD      0x37
#define RTL_CAPR     0x38
#define RTL_IMR      0x3C
#define RTL_ISR      0x3E
#define RTL_TCR      0x40
#define RTL_RCR      0x44
#define RTL_CONFIG1  0x52
#define RTL_TX_OK    0x8000   /* TSD bit15 = TOK (Transmit OK) */
#define RTL_TX_ABORT 0x40000000u /* TSD bit30 = TABT (Transmit Abort) */
#define RTL_RX_OK    0x01
#define RTL_BUFE     0x01

static uint16_t nic_iobase = 0;
static uint8_t  nic_mac[6] = {0};
static int      nic_ready  = 0;
static uint32_t nic_rx_ptr = 0;   /* CAPR 포인터 */

static void nic_outb(uint8_t reg, uint8_t val){ outb(nic_iobase+reg,val); }
static void nic_outw(uint8_t reg, uint16_t val){ outw(nic_iobase+reg,val); }
static void nic_outl(uint8_t reg, uint32_t val){ outl(nic_iobase+reg,val); }
static uint8_t  nic_inb(uint8_t reg){ return inb(nic_iobase+reg); }
static uint16_t nic_inw(uint8_t reg){ return inw(nic_iobase+reg); }
static uint32_t nic_inl(uint8_t reg){ return inl(nic_iobase+reg); }

/* ── NIC 초기화 ─────────────────────────────────────── */
static void nic_init(void){
    /* PCI 스캔: RTL8139 = vendor 0x10EC device 0x8139 */
    for(int bus=0;bus<4;bus++){
        for(int dev=0;dev<32;dev++){
            uint32_t id=pci_read((uint8_t)bus,(uint8_t)dev,0,0);
            if(id==0xFFFFFFFF||id==0) continue;
            if(id==0x813910EC){
                /* I/O base는 BAR0 */
                uint32_t bar0=pci_read((uint8_t)bus,(uint8_t)dev,0,0x10);
                nic_iobase=(uint16_t)(bar0&0xFFFC);
                /* 버스 마스터 + I/O 활성화 */
                uint32_t cmd=pci_read((uint8_t)bus,(uint8_t)dev,0,4);
                pci_write((uint8_t)bus,(uint8_t)dev,0,4,cmd|0x07);
                goto found;
            }
        }
    }
    return; /* NIC 없음 */
found:
    /* 소프트 리셋 */
    nic_outb(RTL_CMD,0x10);
    while(nic_inb(RTL_CMD)&0x10) __asm__("pause");

    /* CONFIG1 전원 켜기 */
    nic_outb(RTL_CONFIG1,0x00);

    /* MAC 읽기 */
    for(int i=0;i<6;i++) nic_mac[i]=nic_inb((uint8_t)(RTL_IDR0+i));

    /* RX 버퍼 설정 */
    nic_outl(RTL_RBSTART,(uint32_t)(uintptr_t)NIC_RX_BUF);
    nic_rx_ptr=0;

    /* TCR: IFG=11 DMA=unlimited */
    nic_outl(RTL_TCR,0x03000700);

    /* RCR: accept broadcast+unicast+multicast, nowrap, 64K buf
     * RBLEN=11(bits12:11)=64K+16 → NIC_RX_SIZE(65536+16)와 일치
     * MXDMA=111(bits10:8)=unlimited  0x1F0F = 0001_1111_0000_1111 */
    nic_outl(RTL_RCR,0x00001F0F);

    /* TX buf 주소 */
    nic_outl(RTL_TSAD0,(uint32_t)(uintptr_t)NIC_TX_BUF0);
    nic_outl(RTL_TSAD0+4,(uint32_t)(uintptr_t)(NIC_TX_BUF0+2048));
    nic_outl(RTL_TSAD0+8,(uint32_t)(uintptr_t)(NIC_TX_BUF0+4096));
    nic_outl(RTL_TSAD0+12,(uint32_t)(uintptr_t)(NIC_TX_BUF0+6144));

    /* RX+TX 활성화 */
    nic_outb(RTL_CMD,0x0C);

    /* CAPR 초기화: 0xFFF0 = (0 - 16) & 0xFFFF → NIC가 offset 0부터 쓸 수 있음 */
    nic_outw(RTL_CAPR, 0xFFF0);

    /* 인터럽트 마스크: RX OK + TX OK (폴링이므로 실제 IRQ는 필요없음) */
    nic_outw(RTL_IMR,0x0005);

    nic_ready=1;
}

/* ── 패킷 송신 ─────────────────────────────────────── */
static int nic_tx_idx=0;
static int nic_last_tx_ok=0;  /* 디버그: 마지막 TX 성공 여부 */
static void nic_send(const uint8_t *pkt, int len){
    if(!nic_ready) return;
    volatile uint8_t *tbuf=(volatile uint8_t*)(NIC_TX_BUF0 + (uint32_t)nic_tx_idx*2048);
    for(int i=0;i<len;i++) tbuf[i]=pkt[i];
    nic_outl((uint8_t)(RTL_TSD0+nic_tx_idx*4),(uint32_t)len & 0x1FFF);
    nic_last_tx_ok=0;
    for(int t=0;t<200000;t++){
        uint32_t tsd=nic_inl((uint8_t)(RTL_TSD0+nic_tx_idx*4));
        if(tsd & RTL_TX_OK){ nic_last_tx_ok=1; break; }   /* TOK: 전송 완료 */
        if(tsd & RTL_TX_ABORT){ break; }                  /* TABT: 전송 중단(에러) */
    }
    nic_tx_idx=(nic_tx_idx+1)%4;
}

/* ── 패킷 수신 (폴링) – pkt에 복사, 성공 시 길이 반환 ─ */
static int nic_recv(uint8_t *pkt, int maxlen){
    if(!nic_ready) return 0;
    if(nic_inb(RTL_CMD)&RTL_BUFE) return 0;  /* RX 버퍼 비어있음 */

    /* RX ring 헤더 읽기 */
    volatile uint8_t *rb=NIC_RX_BUF;
    uint32_t off=nic_rx_ptr;

    /* 헤더: status(2) + len(2) */
    uint16_t status=(uint16_t)(rb[off&0xFFFF] | (rb[(off+1)&0xFFFF]<<8));
    uint16_t plen =(uint16_t)(rb[(off+2)&0xFFFF] | (rb[(off+3)&0xFFFF]<<8));

    if(!(status&0x01)) return 0;  /* ROK 비트 없으면 불량 */
    /* plen은 CRC(4B) 포함 길이. 풀사이즈 프레임(payload 1514) = 1518 이므로
     * 상한을 1518로 둔다. (기존 1514는 MSS 풀세그먼트를 전부 버려 본문 유실) */
    if(plen<14||plen>1518) { nic_rx_ptr+=((plen+4+3)&~3); nic_outw(RTL_CAPR,(uint16_t)(nic_rx_ptr-0x10)); return 0; }

    int datalen=(int)plen-4;  /* CRC 제외 */
    if(datalen>maxlen) datalen=maxlen;

    for(int i=0;i<datalen;i++) pkt[i]=rb[(off+4+i)&0xFFFF];

    /* 링 버퍼 어드밴스: 헤더(4) + 데이터(plen), DWORD 정렬 */
    nic_rx_ptr+=(uint32_t)((plen+4+3)&~3);   /* 기존 +4 중복 제거 (버그픽스) */
    nic_outw(RTL_CAPR,(uint16_t)((nic_rx_ptr-16)&0xFFFF));

    /* ISR RX 클리어 */
    nic_outw(RTL_ISR,RTL_RX_OK);
    return datalen;
}

/* ═══════════════════════════════════════════════════════
 *  이더넷 / ARP / IP / UDP / TCP
 * ═══════════════════════════════════════════════════════ */

/* 고정 IPv4 설정 (QEMU SLIRP) */
static uint32_t my_ip  = 0x0A00020F; /* 10.0.2.15 */
static uint32_t gw_ip  = 0x0A000202; /* 10.0.2.2  */
static uint32_t dns_ip = 0x0A000203; /* 10.0.2.3  */
static uint8_t  my_mac[6];           /* nic_mac으로 채워짐 */
static uint8_t  gw_mac[6]={0};
static int      gw_mac_ok=0;

/* ── 바이트 순서 변환 ─── */
static uint16_t htons2(uint16_t x){ return (uint16_t)((x>>8)|(x<<8)); }
static uint32_t htonl2(uint32_t x){ return (x>>24)|((x>>8)&0xFF00)|((x<<8)&0xFF0000)|(x<<24); }
#define ntohs2 htons2
#define ntohl2 htonl2

/* ── 이더넷 프레임 전송 ─── */
static uint8_t eth_bcast[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void eth_send(const uint8_t *dst_mac, uint16_t ethertype,
                     const uint8_t *payload, int len){
    static uint8_t frame[1518];
    for(int i=0;i<6;i++) frame[i]=dst_mac[i];
    for(int i=0;i<6;i++) frame[6+i]=my_mac[i];
    frame[12]=(uint8_t)(ethertype>>8);
    frame[13]=(uint8_t)(ethertype);
    for(int i=0;i<len;i++) frame[14+i]=payload[i];
    int total=14+len;
    if(total<60){ for(int i=total;i<60;i++) frame[i]=0; total=60; }
    nic_send(frame,total);
}

/* ── ARP ─────────────────────────────────────────────── */
#define ARP_REQ  1
#define ARP_REP  2
typedef struct __attribute__((packed)){
    uint16_t htype,ptype;
    uint8_t  hlen,plen;
    uint16_t oper;
    uint8_t  sha[6]; uint32_t spa;
    uint8_t  tha[6]; uint32_t tpa;
} arp_pkt_t;

static void arp_send(uint32_t target_ip){
    arp_pkt_t a;
    a.htype=htons2(1); a.ptype=htons2(0x0800);
    a.hlen=6; a.plen=4; a.oper=htons2(ARP_REQ);
    for(int i=0;i<6;i++) a.sha[i]=my_mac[i];
    a.spa=htonl2(my_ip);
    for(int i=0;i<6;i++) a.tha[i]=0;
    a.tpa=htonl2(target_ip);
    eth_send(eth_bcast,0x0806,(uint8_t*)&a,sizeof(a));
}

/* ARP 응답 처리 → gw_mac 캐시 */
static void arp_handle(const uint8_t *pkt, int len){
    if(len<(int)sizeof(arp_pkt_t)+14) return;
    const arp_pkt_t *a=(const arp_pkt_t*)(pkt+14);
    if(ntohs2(a->oper)!=ARP_REP) return;
    uint32_t spa=ntohl2(a->spa);
    if(spa==gw_ip){
        for(int i=0;i<6;i++) gw_mac[i]=a->sha[i];
        gw_mac_ok=1;
    }
}

/* GW MAC 취득 (ARP 요청 + 폴링 대기) */
static int arp_resolve_gw(void){
    if(gw_mac_ok) return 1;
    arp_send(gw_ip);
    static uint8_t pkt[1520];
    for(int t=0;t<50000;t++){
        int n=nic_recv(pkt,1520);
        if(n>=42){
            uint16_t et=(uint16_t)((pkt[12]<<8)|pkt[13]);
            if(et==0x0806) arp_handle(pkt,n);
            if(gw_mac_ok) return 1;
        }
    }
    return 0;
}

/* ── IP 체크섬 ─── */
static uint16_t ip_checksum(const uint8_t *buf, int len){
    uint32_t sum=0;
    for(int i=0;i<len-1;i+=2) sum+=(uint32_t)((buf[i]<<8)|buf[i+1]);
    if(len&1) sum+=buf[len-1]<<8;
    while(sum>>16) sum=(sum&0xFFFF)+(sum>>16);
    return (uint16_t)(~sum);
}

/* ── IP ヘッダー送信 ─── */
typedef struct __attribute__((packed)){
    uint8_t  vihl, tos;
    uint16_t tot_len, id;
    uint16_t frag_off, ttl_proto;
    uint16_t checksum;
    uint32_t src, dst;
} ip_hdr_t;

static uint16_t ip_id=1;
static void ip_send(uint32_t dst_ip, uint8_t proto,
                    const uint8_t *payload, int plen){
    static uint8_t ipbuf[1500];
    ip_hdr_t *h=(ip_hdr_t*)ipbuf;
    h->vihl=0x45; h->tos=0;
    h->tot_len=htons2((uint16_t)(20+plen));
    h->id=htons2(ip_id++);
    /* ttl_proto는 little-endian uint16_t: low=TTL, high=proto
     * → 메모리상 byte[8]=TTL(64), byte[9]=proto (네트워크 바이트 순서와 일치) */
    h->frag_off=0; h->ttl_proto=(uint16_t)((proto<<8)|64);
    h->checksum=0;
    h->src=htonl2(my_ip); h->dst=htonl2(dst_ip);
    h->checksum=htons2(ip_checksum(ipbuf,20)); /* ★ htons2 필수: 체크섬은 네트워크 바이트 순서로 저장 */
    for(int i=0;i<plen;i++) ipbuf[20+i]=payload[i];
    /* MAC 결정: 같은 서브넷이면 직접, 아니면 GW */
    uint8_t *dmac=gw_mac;  /* 항상 GW 경유 (QEMU SLIRP) */
    eth_send(dmac,0x0800,ipbuf,20+plen);
}

/* ── UDP ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)){
    uint16_t sport, dport, len, checksum;
} udp_hdr_t;

static void udp_send(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                     const uint8_t *data, int dlen){
    static uint8_t ubuf[1480];
    udp_hdr_t *h=(udp_hdr_t*)ubuf;
    h->sport=htons2(sport); h->dport=htons2(dport);
    h->len=htons2((uint16_t)(8+dlen));
    h->checksum=0;
    for(int i=0;i<dlen;i++) ubuf[8+i]=data[i];
    /* UDP 체크섬은 선택사항 – 0으로 남김 */
    ip_send(dst_ip,17,ubuf,8+dlen);
}

/* ── 패킷 수신 (IP 레이어까지 디스패치) ─── */
static int  udp_recv_port=0;
static uint8_t udp_recv_buf[512];
static int  udp_recv_len=0;
static uint16_t udp_recv_sport=0;
static uint32_t udp_recv_src_ip=0;

/* TCP 수신 버퍼 (단순 단방향 - HTTP response 용) */
static uint8_t  tcp_recv_buf[65536];
static int      tcp_recv_len=0;
static int      tcp_recv_done=0; /* FIN 수신 */

/* TCP 연결 상태 */
static uint32_t tcp_remote_ip=0;
static uint16_t tcp_local_port=0, tcp_remote_port=0;
static uint32_t tcp_seq=0, tcp_ack=0;
static int tcp_state=0; /* 0=CLOSED 1=SYN_SENT 2=ESTABLISHED 3=CLOSE_WAIT 4=FIN_WAIT */
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_CLOSE_WAIT  3
#define TCP_FIN_WAIT    4

typedef struct __attribute__((packed)){
    uint16_t sport, dport;
    uint32_t seq, ack_num;
    uint8_t  data_off, flags;
    uint16_t window, checksum, urg;
} tcp_hdr_t;
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* TCP 체크섬 (의사헤더 포함) */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                              const uint8_t *tcpseg, int len){
    uint32_t sum=0;
    /* 의사 헤더 */
    sum+=(src_ip>>16)&0xFFFF; sum+=src_ip&0xFFFF;
    sum+=(dst_ip>>16)&0xFFFF; sum+=dst_ip&0xFFFF;
    sum+=6; sum+=(uint32_t)len;
    /* TCP 세그먼트 */
    for(int i=0;i<len-1;i+=2) sum+=(uint32_t)((tcpseg[i]<<8)|tcpseg[i+1]);
    if(len&1) sum+=tcpseg[len-1]<<8;
    while(sum>>16) sum=(sum&0xFFFF)+(sum>>16);
    return (uint16_t)(~sum);
}

/* TCP 세그먼트 송신 */
static void tcp_send_seg(uint8_t flags, const uint8_t *data, int dlen){
    static uint8_t tbuf[1500];
    tcp_hdr_t *h=(tcp_hdr_t*)tbuf;
    h->sport=htons2(tcp_local_port);
    h->dport=htons2(tcp_remote_port);
    h->seq=htonl2(tcp_seq);
    h->ack_num=htonl2(tcp_ack);
    h->data_off=(uint8_t)((flags&TCP_SYN)?0x60:0x50); /* SYN에 MSS 옵션 추가 예정 */
    h->flags=flags;
    h->window=htons2(8192);
    h->checksum=0; h->urg=0;
    int hlen=(flags&TCP_SYN)?24:20;
    /* SYN: MSS 옵션 */
    if(flags&TCP_SYN){
        tbuf[20]=2; tbuf[21]=4; tbuf[22]=0x05; tbuf[23]=0xB4; /* MSS=1460 */
        h->data_off=0x60;
        hlen=24;
    } else { hlen=20; h->data_off=0x50; }
    for(int i=0;i<dlen;i++) tbuf[hlen+i]=data[i];
    h->checksum=htons2(tcp_checksum(my_ip,tcp_remote_ip,tbuf,hlen+dlen)); /* ★ htons2 필수 */
    ip_send(tcp_remote_ip,6,tbuf,hlen+dlen);
    if(!(flags&(TCP_SYN|TCP_FIN))) tcp_seq+=dlen;
}

/* 수신 패킷 처리 */
static void net_handle_pkt(const uint8_t *pkt, int len){
    if(len<14) return;
    uint16_t et=(uint16_t)((pkt[12]<<8)|pkt[13]);

    if(et==0x0806){ arp_handle(pkt,len); return; }
    if(et!=0x0800) return;
    if(len<34) return;

    const ip_hdr_t *iph=(const ip_hdr_t*)(pkt+14);
    /* ttl_proto little-endian: low byte=TTL, high byte=Protocol */
    uint8_t  proto=(uint8_t)((iph->ttl_proto>>8)&0xFF);
    uint32_t src_ip=ntohl2(iph->src);
    uint32_t dst_ip=ntohl2(iph->dst);
    int ip_hlen=(iph->vihl&0xF)*4;
    if(dst_ip!=my_ip&&dst_ip!=0xFFFFFFFF) return;

    /* ★ 이더넷 최소프레임(60B) 패딩이 payload로 잘못 계산되던 버그 수정.
     *   실제 길이는 IP total-length 필드(offset 2-3)로 구한다. */
    uint16_t ip_total = ((uint16_t)pkt[16]<<8) | (uint16_t)pkt[17];
    int eff_len = 14 + (int)ip_total;
    if(eff_len > len) eff_len = len;      /* 방어: 잘린 프레임 */
    if(eff_len < 14) eff_len = len;

    if(proto==17){
        /* UDP */
        if(len<14+ip_hlen+8) return;
        const udp_hdr_t *uh=(const udp_hdr_t*)(pkt+14+ip_hlen);
        uint16_t dport=ntohs2(uh->dport);
        int udp_dlen=ntohs2(uh->len)-8;
        if(udp_recv_port && dport==(uint16_t)udp_recv_port && udp_dlen>0){
            if(udp_dlen>511) udp_dlen=511;
            const uint8_t *ud=pkt+14+ip_hlen+8;
            for(int i=0;i<udp_dlen;i++) udp_recv_buf[i]=ud[i];
            udp_recv_buf[udp_dlen]=0;
            udp_recv_len=udp_dlen;
            udp_recv_sport=ntohs2(uh->sport);
            udp_recv_src_ip=src_ip;
        }
    } else if(proto==6){
        /* TCP */
        if(len<14+ip_hlen+20) return;
        const tcp_hdr_t *th=(const tcp_hdr_t*)(pkt+14+ip_hlen);
        uint16_t sport2=ntohs2(th->sport), dport2=ntohs2(th->dport);
        if(sport2!=tcp_remote_port||dport2!=tcp_local_port) return;
        if(src_ip!=tcp_remote_ip) return;

        uint32_t seg_seq=ntohl2(th->seq);
        uint32_t seg_ack=ntohl2(th->ack_num);
        uint8_t  flags2=th->flags;
        int tcp_hlen2=(th->data_off>>4)*4;
        int payload_len=eff_len-(14+ip_hlen+tcp_hlen2);   /* 패딩 제외 실제 데이터 길이 */
        if(payload_len<0) payload_len=0;

        if(flags2&TCP_RST){ tcp_state=TCP_CLOSED; return; }

        if(tcp_state==TCP_SYN_SENT&&(flags2&(TCP_SYN|TCP_ACK))==(TCP_SYN|TCP_ACK)){
            tcp_ack=seg_seq+1;
            tcp_seq=seg_ack;
            tcp_state=TCP_ESTABLISHED;
            tcp_send_seg(TCP_ACK,0,0);
            return;
        }

        if(tcp_state==TCP_ESTABLISHED||tcp_state==TCP_CLOSE_WAIT){
            if(payload_len>0&&seg_seq==tcp_ack){
                const uint8_t *payload2=pkt+14+ip_hlen+tcp_hlen2;
                int room=(int)(sizeof(tcp_recv_buf)-1)-tcp_recv_len;
                if(room>0){
                    int cp=payload_len>room?room:payload_len;
                    for(int i=0;i<cp;i++) tcp_recv_buf[tcp_recv_len+i]=payload2[i];
                    tcp_recv_len+=cp;
                    tcp_recv_buf[tcp_recv_len]=0;
                }
                tcp_ack+=payload_len;
                tcp_send_seg(TCP_ACK,0,0);
            } else if(payload_len>0){
                /* 중복/뒤처진 세그먼트는 현재 기대값으로 ACK 재전송해 서버를 깨운다 */
                tcp_send_seg(TCP_ACK,0,0);
            }
            if(flags2&TCP_FIN){
                tcp_ack++;
                tcp_recv_done=1;
                tcp_send_seg(TCP_ACK,0,0);
                tcp_state=TCP_CLOSE_WAIT;
            }
        }
    }
}

/* 수신 폴링 (timeouts 기반) */
static void net_poll(int iters){
    static uint8_t pkt[1520];
    for(int t=0;t<iters;t++){
        int n=nic_recv(pkt,1520);
        if(n>0) net_handle_pkt(pkt,n);
    }
}

/* ═══════════════════════════════════════════════════════
 *  DNS  –  A 레코드 조회 (UDP 포트 53)
 * ═══════════════════════════════════════════════════════ */
static uint32_t dns_query(const char *hostname){
    /* 최대 3회 재시도 */
    for(int attempt=0; attempt<3; attempt++){

    /* 빌드 DNS 쿼리 패킷 */
    static uint8_t q[512];
    int qi=0;
    /* 헤더 */
    q[qi++]=0x00; q[qi++]=0x01; /* ID */
    q[qi++]=0x01; q[qi++]=0x00; /* 표준 재귀 요청 */
    q[qi++]=0x00; q[qi++]=0x01; /* QDCOUNT=1 */
    q[qi++]=0x00; q[qi++]=0x00; /* ANCOUNT */
    q[qi++]=0x00; q[qi++]=0x00; /* NSCOUNT */
    q[qi++]=0x00; q[qi++]=0x00; /* ARCOUNT */
    /* QNAME */
    const char *p=hostname;
    while(*p){
        const char *dot=p;
        while(*dot&&*dot!='.') dot++;
        int llen=(int)(dot-p);
        q[qi++]=(uint8_t)llen;
        for(int i=0;i<llen;i++) q[qi++]=(uint8_t)p[i];
        p=dot; if(*p=='.') p++;
    }
    q[qi++]=0;       /* 종료 레이블 */
    q[qi++]=0; q[qi++]=1;  /* QTYPE=A */
    q[qi++]=0; q[qi++]=1;  /* QCLASS=IN */

    udp_recv_port=10053;
    udp_recv_len=0;

    udp_send(dns_ip,10053,53,q,qi);
    if(!nic_last_tx_ok){ puts("DNS TX failed\n"); continue; }

    for(int t=0;t<500000;t++){
        static uint8_t dbg_pkt[1520];
        int dn=nic_recv(dbg_pkt,1520);
        if(dn>0) net_handle_pkt(dbg_pkt,dn);
        if(udp_recv_len>12) goto parse_reply;
    }
    continue; /* 타임아웃 → 재시도 */

parse_reply:;
    const uint8_t *r=udp_recv_buf;
    int rlen=udp_recv_len;
    int ri=12;
    /* ANCOUNT */
    int ancount=(r[6]<<8)|r[7];

    /* QNAME 스킵 */
    while(ri<rlen&&r[ri]!=0) ri++; ri++; ri+=4;

    /* 응답 파싱 */
    for(int ai=0;ai<ancount&&ri<rlen;ai++){
        /* 이름 (포인터 또는 레이블) */
        if((r[ri]&0xC0)==0xC0) ri+=2; else { while(ri<rlen&&r[ri]) ri++; ri++; }
        if(ri+10>rlen) break;
        uint16_t rtype=(uint16_t)((r[ri]<<8)|r[ri+1]);
        /* uint16_t rclass=(uint16_t)((r[ri+2]<<8)|r[ri+3]); */
        ri+=8; /* type+class+ttl */
        uint16_t rdlen=(uint16_t)((r[ri]<<8)|r[ri+1]); ri+=2;
        if(rtype==1&&rdlen==4&&ri+4<=rlen){
            uint32_t ip2=((uint32_t)r[ri]<<24)|((uint32_t)r[ri+1]<<16)|((uint32_t)r[ri+2]<<8)|r[ri+3];
            udp_recv_port=0;
            return ip2;
        }
        ri+=rdlen;
    }
    udp_recv_port=0;
    return 0;

    } /* end for(attempt) */
    return 0; /* 3회 모두 타임아웃 */
}

/* ═══════════════════════════════════════════════════════
 *  TCP 연결 + HTTP 클라이언트
 * ═══════════════════════════════════════════════════════ */
static uint16_t g_tcp_port_counter=40000;

static int tcp_connect(uint32_t dst_ip, uint16_t dst_port){
    tcp_remote_ip=dst_ip; tcp_remote_port=dst_port;
    tcp_local_port=g_tcp_port_counter++;
    tcp_seq=0xDEAD1234; tcp_ack=0;
    tcp_state=TCP_SYN_SENT;
    tcp_recv_len=0; tcp_recv_done=0;

    tcp_send_seg(TCP_SYN,0,0);
    tcp_seq++;  /* SYN은 seq 1 소비 */

    /* SYN+ACK 기다리기 */
    for(int t=0;t<200000;t++){
        net_poll(5);
        if(tcp_state==TCP_ESTABLISHED) return 1;
        if(tcp_state==TCP_CLOSED) return 0;
    }
    return 0;
}

static void tcp_send_data(const uint8_t *data, int len){
    /* MSS=1460 단위로 분할. seq는 tcp_send_seg() 내부에서 이미 dlen만큼
     * 증가하므로 여기서 또 더하면 안 된다(중복 증가 버그 수정). */
    while(len>0){
        int chunk=len>1460?1460:len;
        tcp_send_seg(TCP_PSH|TCP_ACK,data,chunk);
        data+=chunk; len-=chunk;
        /* ACK 수신 대기 (짧게) */
        for(int t=0;t<5000;t++) net_poll(2);
    }
}

static void tcp_close(void){
    if(tcp_state==TCP_ESTABLISHED){
        tcp_send_seg(TCP_FIN|TCP_ACK,0,0);
        tcp_seq++;
        tcp_state=TCP_FIN_WAIT;
    }
    tcp_state=TCP_CLOSED;
}

/* ─── HTTP GET 요청 ──────────────────────────────────── */
/*
 * http_get(url, out_buf, out_max) → 다운로드된 바이트 수
 * url 형식: http://hostname/path  또는 http://hostname:port/path
 */
/* POST 지원: non-null이면 다음 요청은 POST(본문 g_post_body, 길이 g_post_len) */
static const char *g_post_body = 0;
static int g_post_len = 0;

static int http_get(const char *url, char *out_buf, int out_max){
    if(!nic_ready){ puts("NIC not initialized\n"); return -1; }
    if(!arp_resolve_gw()){ puts("ARP: gateway not found\n"); return -1; }

    /* POST 상태를 한 번만 소비 */
    const char *post_body=g_post_body; int post_len=g_post_len;
    g_post_body=0; g_post_len=0;

    /* URL 파싱 */
    const char *p=url;
    if(p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]==':'&&p[5]=='/'&&p[6]=='/') p+=7;
    else { puts("Only http:// supported\n"); return -1; }

    char hostname[128]={0};
    uint16_t port=80;
    int hi=0;
    while(*p&&*p!='/'&&*p!=':') hostname[hi++]=*p++;
    hostname[hi]=0;
    if(*p==':'){
        p++; port=0;
        while(*p>='0'&&*p<='9') port=(uint16_t)(port*10+(*p++)-'0');
    }
    const char *path=(*p=='/')?p:"/";

    /* DNS 조회 */
    puts("Resolving "); puts(hostname); puts("...\n");
    uint32_t server_ip=dns_query(hostname);
    if(!server_ip){ puts("DNS failed\n"); return -1; }

    /* 해석된 IP를 점 십진수로 출력 */
    char ipbuf2[20]; int ibi=0;
    uint8_t ibs[4]={(uint8_t)(server_ip>>24),(uint8_t)(server_ip>>16),(uint8_t)(server_ip>>8),(uint8_t)server_ip};
    for(int q2=0;q2<4;q2++){
        uint8_t bv=ibs[q2];
        if(bv>=100) ipbuf2[ibi++]=(char)('0'+bv/100);
        if(bv>=10)  ipbuf2[ibi++]=(char)('0'+(bv/10)%10);
        ipbuf2[ibi++]=(char)('0'+bv%10);
        if(q2<3) ipbuf2[ibi++]='.';
    }
    ipbuf2[ibi]=0;
    puts("Resolved IP: "); puts(ipbuf2); puts("\n");

    /* TCP 연결 */
    puts("Connecting...\n");
    if(!tcp_connect(server_ip,port)){ puts("TCP connect failed\n"); return -1; }
    puts("Connected!\n");

    /* HTTP 요청 빌드 (GET 또는 POST) */
    static char req[1024];
    int ri2=0;
    const char *ms = post_body ? "POST " : "GET ";
    while(*ms) req[ri2++]=*ms++;
    const char *pp=path; while(*pp) req[ri2++]=*pp++;
    const char *v=" HTTP/1.0\r\nHost: "; while(*v) req[ri2++]=*v++;
    const char *hn=hostname; while(*hn) req[ri2++]=*hn++;
    if(post_body){
        const char *ct="\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
        while(*ct) req[ri2++]=*ct++;
        char nb[12]; int nn=0; int Ln=post_len; if(!Ln){ nb[nn++]='0'; } else { while(Ln){ nb[nn++]=(char)('0'+Ln%10); Ln/=10; } }
        while(nn) req[ri2++]=nb[--nn];
        const char *t3="\r\nConnection: close\r\nUser-Agent: OS2026\r\n\r\n";
        while(*t3) req[ri2++]=*t3++;
        for(int b=0;b<post_len && ri2<1020;b++) req[ri2++]=post_body[b];
    } else {
        const char *t3="\r\nConnection: close\r\nUser-Agent: OS2026\r\n\r\n";
        while(*t3) req[ri2++]=*t3++;
    }
    req[ri2]=0;

    tcp_send_data((uint8_t*)req, ri2);

    /* 응답 수신 */
    puts("Waiting response...\n");
    for(int t=0;t<2000000;t++){
        net_poll(10);
        if(tcp_recv_done) break;
        if(tcp_state==TCP_CLOSED) break;
    }
    tcp_close();

    if(tcp_recv_len<=0){ puts("No data received\n"); return 0; }

    /* HTTP 헤더 스킵 → body */
    char *body=(char*)tcp_recv_buf;
    int blen=tcp_recv_len;
    /* \r\n\r\n 찾기 */
    for(int i=0;i<blen-3;i++){
        if(body[i]=='\r'&&body[i+1]=='\n'&&body[i+2]=='\r'&&body[i+3]=='\n'){
            body+=i+4; blen-=i+4; break;
        }
    }
    if(blen<0) blen=0;
    if(blen>out_max-1) blen=out_max-1;
    for(int i=0;i<blen;i++) out_buf[i]=body[i];
    out_buf[blen]=0;
    return blen;
}

/* ─────────────────────────────────────────────────────── */
/* cmd_fetch, cmd_ping, cmd_netinfo는 FS 함수 다음에 정의 */

#define MAX_ENV 16
#define MAX_ENV_KEY 20
#define MAX_ENV_VAL 64
typedef struct { char key[MAX_ENV_KEY]; char val[MAX_ENV_VAL]; int used; } env_var_t;
static env_var_t env_table[MAX_ENV];

static const char* env_get(const char *key){
    for(int i=0;i<MAX_ENV;i++)
        if(env_table[i].used && !strcmp(env_table[i].key, key))
            return env_table[i].val;
    return 0;
}
static void env_set(const char *key, const char *val){
    /* update existing */
    for(int i=0;i<MAX_ENV;i++){
        if(env_table[i].used && !strcmp(env_table[i].key, key)){
            strncpy(env_table[i].val, val, MAX_ENV_VAL-1);
            env_table[i].val[MAX_ENV_VAL-1]=0;
            return;
        }
    }
    /* new slot */
    for(int i=0;i<MAX_ENV;i++){
        if(!env_table[i].used){
            strncpy(env_table[i].key, key, MAX_ENV_KEY-1);
            env_table[i].key[MAX_ENV_KEY-1]=0;
            strncpy(env_table[i].val, val, MAX_ENV_VAL-1);
            env_table[i].val[MAX_ENV_VAL-1]=0;
            env_table[i].used=1;
            return;
        }
    }
    puts("env full\n");
}
static void env_unset(const char *key){
    for(int i=0;i<MAX_ENV;i++)
        if(env_table[i].used && !strcmp(env_table[i].key, key)){
            env_table[i].used=0; return;
        }
}

/* $VAR 확장: src → dst (최대 dstmax-1 bytes) */
static void env_expand(const char *src, char *dst, int dstmax){
    int di=0;
    for(int i=0; src[i] && di<dstmax-1; ){
        if(src[i]=='$' && src[i+1]){
            char kbuf[MAX_ENV_KEY]; int kl=0;
            i++;
            while(src[i] && (src[i]=='_' ||(src[i]>='A'&&src[i]<='Z')||(src[i]>='a'&&src[i]<='z')||(src[i]>='0'&&src[i]<='9')) && kl<MAX_ENV_KEY-1)
                kbuf[kl++]=src[i++];
            kbuf[kl]=0;
            const char *v=env_get(kbuf);
            if(v){ int vl=strlen(v); for(int j=0;j<vl&&di<dstmax-1;j++) dst[di++]=v[j]; }
        } else {
            dst[di++]=src[i++];
        }
    }
    dst[di]=0;
}

/* =========================================================
 *  Pipe buffer  (single inter-command pipe, 4KB)
 * ========================================================= */
#define PIPE_BUF_SIZE 4096
static char   pipe_buf[PIPE_BUF_SIZE];
static int    pipe_buf_len = 0;
static int    pipe_active  = 0;  /* 1 = 현재 파이프 모드로 출력 중 */
static int    pipe_read_pos= 0;  /* 파이프에서 읽는 위치 */

/* pipe putc: pipe_active이면 버퍼에, 아니면 VGA에 */
static void pipe_putc(char c){
    if(pipe_buf_len < PIPE_BUF_SIZE-1)
        pipe_buf[pipe_buf_len++] = c;
}
static void pipe_puts(const char *s){ while(*s) pipe_putc(*s++); }

/* =========================================================
 *  Redirect buffer  (write/append > file, 8KB)
 * ========================================================= */
#define REDIR_BUF_SIZE 8192
static char   redir_buf[REDIR_BUF_SIZE];
static int    redir_buf_len = 0;
static int    redir_active  = 0;   /* 1=쓰기, 2=덧붙이기 */
static char   redir_file[24];
/* redir_flush()는 FS 함수 이후에 정의됨 */

/* =========================================================
 *  RTC (CMOS) real-time clock reader
 * ========================================================= */
static uint8_t rtc_read(uint8_t reg){
    outb(0x70, reg);
    return inb(0x71);
}
static uint8_t bcd2bin(uint8_t b){ return (uint8_t)((b>>4)*10 + (b&0xF)); }

typedef struct { uint8_t sec,min,hour,day,mon; uint16_t year; } rtc_time_t;

static rtc_time_t rtc_now(void){
    rtc_time_t t;
    /* wait for update-in-progress flag to clear */
    while(rtc_read(0x0A)&0x80);
    t.sec  = bcd2bin(rtc_read(0x00));
    t.min  = bcd2bin(rtc_read(0x02));
    t.hour = bcd2bin(rtc_read(0x04));
    t.day  = bcd2bin(rtc_read(0x07));
    t.mon  = bcd2bin(rtc_read(0x08));
    uint8_t yr= bcd2bin(rtc_read(0x09));
    t.year = (uint16_t)(2000 + yr);
    return t;
}

static void print2d(uint8_t v){ putc((char)('0'+v/10)); putc((char)('0'+v%10)); }

static void cmd_date(void){
    rtc_time_t t = rtc_now();
    print_uint(t.year); putc('-'); print2d(t.mon); putc('-'); print2d(t.day);
    putc(' ');
    print2d(t.hour); putc(':'); print2d(t.min); putc(':'); print2d(t.sec);
    putc('\n');
}

/* RTC 시계 문자열을 taskbar용 버퍼에 채움 (7바이트 "HH:MM  ") */
static void rtc_clock_str(char out[8]){
    rtc_time_t t = rtc_now();
    out[0]=(char)('0'+t.hour/10); out[1]=(char)('0'+t.hour%10);
    out[2]=':';
    out[3]=(char)('0'+t.min/10);  out[4]=(char)('0'+t.min%10);
    out[5]=':';
    out[6]=(char)('0'+t.sec/10);  out[7]=(char)('0'+t.sec%10);
    /* no null – caller uses fixed width */
}

/* forward decls for syscall_dispatch - dirent_t 정의 이후에만 사용가능 */
static uint8_t getch_raw_u8_blocking(void);
void task_exit(void);
void yield(void);
int  getpid(void);

/* =========================================================
 *  INT 0x80 Syscall  (software interrupt dispatcher)
 *
 *  사용법 (어셈블리에서):
 *    mov eax, <syscall_num>
 *    [mov ebx, arg1]  [mov ecx, arg2]  [mov edx, arg3]
 *    int 0x80
 *
 *  syscall 번호:
 *    0  = sys_putc(char ebx)
 *    1  = sys_puts(const char* ebx)
 *    2  = sys_getc()            → eax = char
 *    3  = sys_exit()
 *    4  = sys_yield()
 *    5  = sys_getpid()          → eax = pid
 *    6  = sys_fs_find(dir ebx, name ecx, dirent* edx)  → eax=1/0
 *    7  = sys_fs_load(dirent* ebx, void* ecx)          → eax=size
 *    8  = sys_fs_write(name ebx, void* ecx, size edx)  → eax=lba
 *    9  = sys_date(rtc_time_t* ebx)
 *   10  = sys_env_get(key ebx, val_buf ecx)            → eax=0/1
 *   11  = sys_env_set(key ebx, val ecx)
 *   12  = sys_gfx_px(x ebx, y ecx, color edx)
 *   13  = sys_gfx_rect([x,y] ebx,ecx, [w,h,c]* edx)
 *   14  = sys_mouse_xy(int* x_out ebx, int* y_out ecx)
 *   15  = sys_mouse_btn()                              → eax=btn
 *   16  = sys_notify(const char* msg ebx)
 *   17  = sys_win_open(type ebx)                       → eax=wid
 *   18  = sys_win_close(wid ebx)
 *   19  = sys_fs_mkdir(name ebx)                       → eax=1/0
 *   20  = sys_fs_rm(name ebx)                          → eax=1/0
 *   21  = sys_fs_ls(buf ebx, maxn ecx)                 → eax=count
 *   22  = sys_getkey_poll()                            → eax=key
 *   23  = sys_reboot()
 *   24  = sys_halt()
 *
 *  IDT 설정 + 핸들러
 * ========================================================= */

/* IDT descriptor: 올바른 x86 gate 레이아웃
 *  [15:0]  offset low
 *  [31:16] segment selector
 *  [39:32] 0 (reserved)
 *  [47:40] type/attr  (P|DPL|0|type)
 *  [63:48] offset high
 */
typedef struct {
    uint16_t off_lo;   /* handler[15:0]  */
    uint16_t sel;      /* code segment   */
    uint8_t  zero;     /* always 0       */
    uint8_t  type_attr;/* P|DPL|0|type   */
    uint16_t off_hi;   /* handler[31:16] */
} __attribute__((packed)) idt_entry_t;

/* IDT (256 entries) */
static idt_entry_t g_idt[256];

typedef struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idtr_t;
static idtr_t g_idtr;

/* syscall_dispatch is defined later (after FS) – forward decl */
static uint32_t syscall_dispatch(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

/* ── 공용 stub: iret만 하는 더미 핸들러 ──
 *  예외/IRQ 핸들러가 없으면 Triple Fault → QEMU 재시작.
 *  모든 벡터에 이 stub을 달아두면 안전하게 무시합니다.
 *  일부 예외(#DF=8, #TS=10, #NP=11, #SS=12, #GP=13, #PF=14)는
 *  스택에 error code를 push하므로 별도 stub이 필요합니다.
 */
__attribute__((naked))
static void isr_stub_noerr(void){
    /* 에러코드 없는 예외/IRQ: 그냥 iret */
    __asm__ volatile("iret\n");
}

__attribute__((naked))
static void isr_stub_err(void){
    /* 에러코드 있는 예외: error code pop 후 iret */
    __asm__ volatile(
        "add $4, %esp\n"   /* error code 제거 */
        "iret\n"
    );
}

/* IRQ용 stub: PIC EOI(0x20) 전송 후 iret */
__attribute__((naked))
static void irq_stub(void){
    __asm__ volatile(
        "push %eax\n"
        "movb $0x20, %al\n"
        "outb %al, $0x20\n"   /* master PIC EOI */
        "pop %eax\n"
        "iret\n"
    );
}

/* syscall 핸들러 (naked trampoline) */
__attribute__((naked))
static void syscall_handler_asm(void){
    __asm__ volatile(
        "pusha\n"
        "push %edx\n"
        "push %ecx\n"
        "push %ebx\n"
        "push %eax\n"
        "call syscall_dispatch\n"
        "add $16, %esp\n"
        "mov %eax, 28(%esp)\n"   /* pusha 스택의 eax 자리에 반환값 저장 */
        "popa\n"
        "iret\n"
    );
}

static void idt_set_gate(uint8_t n, void(*handler)(void), uint8_t type_attr){
    uint32_t h = (uint32_t)handler;
    g_idt[n].off_lo   = (uint16_t)(h & 0xFFFF);
    g_idt[n].sel      = 0x08;   /* ring-0 code segment */
    g_idt[n].zero     = 0;
    g_idt[n].type_attr= type_attr;
    g_idt[n].off_hi   = (uint16_t)(h >> 16);
}

static void syscall_init(void){
    memset(g_idt, 0, sizeof(g_idt));

    /* ── 예외 벡터 0~7, 9, 15~31: 에러코드 없음 ── */
    uint8_t noerr_vec[] = {0,1,2,3,4,5,6,7,9,15,16,17,18,19,20,21,22,23,
                            24,25,26,27,28,29,30,31};
    for(int i=0;i<(int)(sizeof(noerr_vec)/sizeof(noerr_vec[0]));i++)
        idt_set_gate(noerr_vec[i], isr_stub_noerr, 0x8E); /* P=1, DPL=0, 32-bit interrupt gate */

    /* ── 예외 벡터 8,10,11,12,13,14: 에러코드 있음 ── */
    uint8_t err_vec[] = {8,10,11,12,13,14};
    for(int i=0;i<6;i++)
        idt_set_gate(err_vec[i], isr_stub_err, 0x8E);

    /* ── IRQ 0~15 (벡터 0x20~0x2F): PIC EOI 전송 ── */
    for(int i=0;i<16;i++)
        idt_set_gate((uint8_t)(0x20+i), irq_stub, 0x8E);

    /* ── 0x80: syscall (DPL=3, ring-3에서도 호출 가능) ── */
    idt_set_gate(0x80, syscall_handler_asm, 0xEE);

    /* ── PIC 재프로그래밍: IRQ 0-7 → 0x20, IRQ 8-15 → 0x28 ──
     *  BIOS 기본값(0x08~0x0F)이면 CPU 예외와 겹쳐서 Triple Fault 발생.
     *  표준 Linux/GRUB 배치(0x20~0x2F)로 재매핑합니다.
     */
    /* master PIC init */
    outb(0x20, 0x11);  /* ICW1: init + ICW4 needed */
    outb(0x21, 0x20);  /* ICW2: IRQ0 → INT 0x20    */
    outb(0x21, 0x04);  /* ICW3: slave at IRQ2       */
    outb(0x21, 0x01);  /* ICW4: 8086 mode           */
    /* slave PIC init */
    outb(0xA0, 0x11);
    outb(0xA1, 0x28);  /* ICW2: IRQ8 → INT 0x28    */
    outb(0xA1, 0x02);  /* ICW3: slave id=2          */
    outb(0xA1, 0x01);
    /* ── IRQ 마스크: 커널은 순수 polling 방식 ──
     *  IRQ를 열어두면 키 입력 시 irq_stub이 스캔코드를 읽지 않아
     *  키보드 컨트롤러가 다음 키를 받지 못하고 QEMU 창이 닫힘.
     *  모든 IRQ를 마스크하고, 스캔코드/마우스는 polling으로 읽음.
     */
    outb(0x21, 0xFF);  /* master PIC: 모든 IRQ 마스크 */
    outb(0xA1, 0xFF);  /* slave  PIC: 모든 IRQ 마스크 */

    /* LIDT 후 STI */
    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (uint32_t)g_idt;
    __asm__ volatile("lidt (%0)" :: "r"(&g_idtr));
    __asm__ volatile("sti");
}

/* ---------- 2025FS structures ---------- */
#define ROOT_LBA     100
#define DIR_SECTORS  8   /* 8 sectors => 128 entries (>=100 files) */
#define DIR_BYTES    (DIR_SECTORS*512)
#define DIR_ENTRIES  (DIR_BYTES/sizeof(dirent_t))
typedef struct {
    char     name[24];
    uint32_t lba;
    uint32_t size;   /* size==0 → directory */
} __attribute__((packed)) dirent_t;
_Static_assert(sizeof(dirent_t)==32,"dirent_t must be 32 bytes");

static uint32_t cur_dir = ROOT_LBA;
static uint32_t next_free_lba;
static uint8_t  sec[512];              /* 일반 1섹터 버퍼 */
static uint8_t  dirbuf[DIR_BYTES];     /* 디렉터리(다섹터) 버퍼 */

/* --- 공간 회수(free-space reclaim) ---
 *  기존엔 next_free_lba만 증가하는 bump 할당이라 rm 해도 공간이 회수되지 않았다.
 *  삭제된 확장(extent)을 free 리스트에 모아 두었다가 재사용하고,
 *  디스크 끝과 인접한 확장은 즉시 흡수해 next_free_lba를 다시 낮춘다.  */
#define FS_FREELIST_MAX 64
typedef struct { uint32_t lba, sc; } free_ext_t;
static free_ext_t fs_freelist[FS_FREELIST_MAX];
static int        fs_free_cnt = 0;

/* 최상단(디스크 끝)과 인접한 free 확장을 흡수해 next_free_lba를 낮춘다 */
static void fs_freelist_coalesce_top(void){
    int changed = 1;
    while(changed){
        changed = 0;
        for(int i=0;i<fs_free_cnt;i++){
            if(fs_freelist[i].lba + fs_freelist[i].sc == next_free_lba){
                next_free_lba   = fs_freelist[i].lba;
                fs_freelist[i]  = fs_freelist[--fs_free_cnt];
                changed = 1;
                break;
            }
        }
    }
}

/* 삭제된 영역을 회수: 끝과 인접하면 즉시 반납, 아니면 재사용용으로 보관 */
static void fs_free_extent(uint32_t lba, uint32_t sc){
    if(!sc) return;
    if(lba < ROOT_LBA + DIR_SECTORS) return;   /* 루트/시스템 영역 보호 */
    if(lba + sc == next_free_lba){             /* LIFO: 즉시 회수 */
        next_free_lba = lba;
        fs_freelist_coalesce_top();
        return;
    }
    /* 이미 등록된 확장과 인접하면 병합 */
    for(int i=0;i<fs_free_cnt;i++){
        if(fs_freelist[i].lba + fs_freelist[i].sc == lba){ fs_freelist[i].sc += sc; fs_freelist_coalesce_top(); return; }
        if(lba + sc == fs_freelist[i].lba){ fs_freelist[i].lba = lba; fs_freelist[i].sc += sc; fs_freelist_coalesce_top(); return; }
    }
    if(fs_free_cnt < FS_FREELIST_MAX){
        fs_freelist[fs_free_cnt].lba = lba;
        fs_freelist[fs_free_cnt].sc  = sc;
        fs_free_cnt++;
    }
}

/* 사용 가능한 여유 섹터 수(리포트용) */
static uint32_t fs_free_sectors(void){
    uint32_t n = 0;
    for(int i=0;i<fs_free_cnt;i++) n += fs_freelist[i].sc;
    return n;
}

/* --- FS helpers --- */
static void fs_write_dir(uint32_t dir){ata_write28(dir,1,sec);}
static void fs_scan_root(void){
    /* root directory is DIR_SECTORS sectors */
    ata_read28(ROOT_LBA, DIR_SECTORS, dirbuf);

    uint32_t max = ROOT_LBA + DIR_SECTORS; /* root dir area itself */

    for(int idx=0; idx<DIR_ENTRIES; ++idx){
        dirent_t* e = (dirent_t*)(dirbuf + idx*sizeof(dirent_t));
        if(!e->name[0]) continue; /* ★ hole allowed */

        uint32_t used_sectors;
        if(e->size == 0){
            used_sectors = DIR_SECTORS; /* directory occupies DIR_SECTORS sectors */
        }else{
            used_sectors = (e->size + 511)/512;
        }

        uint32_t end = e->lba + used_sectors;
        if(end > max) max = end;
    }

    next_free_lba = max;
    fs_free_cnt   = 0;   /* 부팅 시 free 리스트 초기화 */
}

static uint32_t get_free_lba(uint32_t sectors){
    if(!sectors) sectors = 1;
    /* 1) free 리스트에서 best-fit 재사용 */
    int best = -1;
    for(int i=0;i<fs_free_cnt;i++){
        if(fs_freelist[i].sc >= sectors){
            if(best < 0 || fs_freelist[i].sc < fs_freelist[best].sc) best = i;
        }
    }
    if(best >= 0){
        uint32_t lba = fs_freelist[best].lba;
        if(fs_freelist[best].sc == sectors){
            fs_freelist[best] = fs_freelist[--fs_free_cnt];  /* 조각 소진 */
        } else {
            fs_freelist[best].lba += sectors;                /* 앞부분만 사용 */
            fs_freelist[best].sc  -= sectors;
        }
        return lba;
    }
    /* 2) 디스크 끝에서 새로 확보 */
    uint32_t lba = next_free_lba;
    next_free_lba += sectors;
    return lba;
}

static int fs_find(uint32_t dir, const char*name, dirent_t*out){
    ata_read28(dir, DIR_SECTORS, dirbuf);
    for(int idx=0; idx<DIR_ENTRIES; ++idx){
        dirent_t* e = (dirent_t*)(dirbuf + idx*sizeof(dirent_t));
        if(!e->name[0]) continue; /* ★ hole skip */
        if(!strcmp(name, e->name)){
            if(out) *out = *e;
            return 1;
        }
    }
    return 0;
}

static int fs_add_entry(uint32_t dir, const char*name, uint32_t lba, uint32_t sz){
    ata_read28(dir, DIR_SECTORS, dirbuf);
    for(int idx=0; idx<DIR_ENTRIES; ++idx){
        dirent_t* e = (dirent_t*)(dirbuf + idx*sizeof(dirent_t));
        if(!e->name[0]){
            memset(e, 0, sizeof(dirent_t));
            strncpy(e->name, name, 24);
            e->lba  = lba;
            e->size = sz;
            ata_write28(dir, DIR_SECTORS, dirbuf);
            return 1;
        }
    }
    return 0; /* dir full */
}

static int fs_load_file(const dirent_t*e,void*dst){
    uint32_t sc=(e->size+511)/512;
    ata_read28(e->lba,sc,dst);
    return (int)e->size;
}

/* ---------- ELF32 loader ---------- */
typedef struct{uint8_t e[52];} Elf32_Ehdr;
typedef struct{uint8_t p[32];} Elf32_Phdr;
#define PT_LOAD 1

/* =========================================================
 *  Cooperative Multitasking (v1.4)
 * ========================================================= */
#define MAX_TASKS 8
#define TASK_STACK_SIZE 8192
#define TASK_STACK_BASE 0x500000

typedef struct task {
    uint32_t esp;
    uint32_t stack_base;
    void   (*entry)(void);
    int used;
    int priority;      /* 1(낮음)~4(높음), 기본값=2 */
    int timeslice;     /* 남은 슬라이스 카운트 */
} task_t;

static task_t tasks[MAX_TASKS];
static int current_task = -1;
static int fg_pid = -1;
static int shell_paused = 0;
static uint32_t kernel_esp = 0;
static int kernel_esp_saved = 0;

int getpid(void){ return current_task; }

__attribute__((naked))
static void task_switch(uint32_t *old_esp, uint32_t new_esp){
    __asm__ volatile(
        "pusha\n"
        "mov  36(%esp), %eax\n"
        "mov  %esp, (%eax)\n"
        "mov  40(%esp), %esp\n"
        "popa\n"
        "ret\n"
    );
}

void task_exit(void);

static void task_trampoline(void){
    int pid = current_task;
    if(pid < 0 || pid >= MAX_TASKS || !tasks[pid].used) task_exit();
    tasks[pid].entry();
    task_exit();
}

static int task_create(void (*entry)(void)){
    for(int pid=0; pid<MAX_TASKS; ++pid){
        if(!tasks[pid].used){
            uint32_t stack = TASK_STACK_BASE + pid * TASK_STACK_SIZE;
            memset((void*)stack, 0, TASK_STACK_SIZE);

            uint32_t *sp = (uint32_t*)(stack + TASK_STACK_SIZE);

            *(--sp) = (uint32_t)task_trampoline;
            *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
            *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

            tasks[pid].esp = (uint32_t)sp;
            tasks[pid].stack_base = stack;
            tasks[pid].entry = entry;
            tasks[pid].used = 1;
            tasks[pid].priority = 2;
            tasks[pid].timeslice = 2;
            return pid;
        }
    }
    puts("task limit\n");
    return -1;
}

static int any_task_used(void){
    for(int i=0;i<MAX_TASKS;i++) if(tasks[i].used) return 1;
    return 0;
}

static int pick_next_pid(int after){
    for(int k=1;k<=MAX_TASKS;k++){
        int pid = (after + k) % MAX_TASKS;
        if(tasks[pid].used) return pid;
    }
    return -1;
}
static void schedule(void){
    if(!any_task_used()) return;

    int next = -1;
    if(fg_pid >= 0 && fg_pid < MAX_TASKS && tasks[fg_pid].used){
        next = fg_pid;
    } else {
        if(fg_pid != -1){ fg_pid = -1; shell_paused = 0; }

        /* 우선순위 기반: timeslice 남아있으면 현재 태스크 유지 */
        if(current_task >= 0 && current_task < MAX_TASKS && tasks[current_task].used){
            if(tasks[current_task].timeslice > 1){
                tasks[current_task].timeslice--;
                return; /* 같은 태스크 계속 */
            }
        }

        /* 가장 높은 우선순위 탐색 */
        int best_prio = 0;
        for(int k=0;k<MAX_TASKS;k++)
            if(tasks[k].used && tasks[k].priority > best_prio)
                best_prio = tasks[k].priority;

        /* best_prio 그룹에서 current 다음 순서 선택 */
        int start = (current_task < 0) ? 0 : (current_task + 1) % MAX_TASKS;
        for(int k=0;k<MAX_TASKS;k++){
            int pid = (start + k) % MAX_TASKS;
            if(tasks[pid].used && tasks[pid].priority == best_prio){ next = pid; break; }
        }
        /* fallback: 아무 태스크나 */
        if(next < 0) next = pick_next_pid(current_task < 0 ? 0 : current_task);
        if(next < 0) return;

        /* timeslice 재설정 */
        tasks[next].timeslice = tasks[next].priority;
    }

    if(current_task == -1){
        current_task = next;
        kernel_esp_saved = 1;
        task_switch(&kernel_esp, tasks[next].esp);
        return;
    }

    if(current_task != next){
        int prev = current_task;
        current_task = next;
        task_switch(&tasks[prev].esp, tasks[next].esp);
    }
}

void yield(void){ schedule(); }

void task_exit(void){
    int pid = current_task;

    if(pid >= 0 && pid < MAX_TASKS){
        tasks[pid].used = 0;
        if(fg_pid == pid){
            fg_pid = -1;
            shell_paused = 0;
        }
    }

    int next = -1;
    if(fg_pid >= 0 && fg_pid < MAX_TASKS && tasks[fg_pid].used){
        next = fg_pid;
    } else {
        next = pick_next_pid(pid);
    }

    if(next >= 0){
        int prev = pid;
        current_task = next;
        task_switch(&tasks[prev].esp, tasks[next].esp);
    }

    /* ★ 더 이상 태스크가 없으면 커널(쉘)로 “정상 복귀” */
    current_task = -1;
    fg_pid = -1;
    shell_paused = 0;

    if(kernel_esp_saved){
        uint32_t dead = 0;              // 저장 대상은 의미 없음(커널로 복귀하므로)
        task_switch(&dead, kernel_esp); // kernel_esp는 task_switch가 저장한 “정상 프레임”
    }

    for(;;) __asm__ volatile("hlt");
}

/* =========================================================
 *  Keyboard (Shift/Caps/Arrows + Ctrl+C/Ctrl+E + Ctrl+Space)
 * ========================================================= */
#define KEY_NONE   0
#define KEY_LEFT   0x80
#define KEY_RIGHT  0x81
#define KEY_UP     0x82
#define KEY_DOWN   0x83
#define KEY_DEL    0x84
#define KEY_HOME   0x85
#define KEY_END    0x86
#define KEY_CTRL_C 0x03
#define KEY_CTRL_E 0x05
#define KEY_TOGGLE_HAN 0x06

static int kbd_shift = 0;
static int kbd_caps  = 0;
static int kbd_ctrl  = 0;
static int kbd_e0    = 0;

static const char keymap[128]={
  0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,'\t',
  'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
  'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
  'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};

static const char keymap_shift[128]={
  0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,'\t',
  'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
  'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
  'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',
};

static inline int is_alpha(char c){
    return (c>='a'&&c<='z') || (c>='A'&&c<='Z');
}
static inline char apply_caps(char c){
    if(c>='a'&&c<='z') return (char)(c - 'a' + 'A');
    if(c>='A'&&c<='Z') return (char)(c - 'A' + 'a');
    return c;
}

static uint8_t kbd_process_scancode(uint8_t sc){
    if(sc == 0xE0){ kbd_e0 = 1; return KEY_NONE; }

    int release = (sc & 0x80) != 0;
    uint8_t code = (uint8_t)(sc & 0x7F);

    if(kbd_e0){
        kbd_e0 = 0;

        if(code == 0x1D){
            kbd_ctrl = release ? 0 : 1;
            return KEY_NONE;
        }

        if(release) return KEY_NONE;

        switch(code){
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            case 0x48: return KEY_UP;
            case 0x50: return KEY_DOWN;
            case 0x53: return KEY_DEL;
            case 0x47: return KEY_HOME;
            case 0x4F: return KEY_END;
            default:   return KEY_NONE;
        }
    }

    if(code == 0x2A || code == 0x36){
        kbd_shift = release ? 0 : 1;
        return KEY_NONE;
    }
    if(code == 0x1D){
        kbd_ctrl = release ? 0 : 1;
        return KEY_NONE;
    }
    if(!release && code == 0x3A){
        kbd_caps ^= 1;
        return KEY_NONE;
    }

    if(release) return KEY_NONE;

    char c = kbd_shift ? keymap_shift[code] : keymap[code];
    if(!c) return KEY_NONE;

    if(kbd_ctrl && c==' ') return KEY_TOGGLE_HAN;
    if(kbd_ctrl && (c=='c' || c=='C')) return KEY_CTRL_C;
    if(kbd_ctrl && (c=='e' || c=='E')) return KEY_CTRL_E;
    /* Generic Ctrl+letter → ASCII control code */
    if(kbd_ctrl && ((c>='a'&&c<='z')||(c>='A'&&c<='Z'))) return (uint8_t)(c&0x1F);

    if(kbd_caps && is_alpha(c)) c = apply_caps(c);
    return (uint8_t)c;
}

/* bit5=1 이면 aux(마우스) 데이터 → 버린다 */
static inline int kbd_data_ready(void){
    uint8_t st = inb(0x64);
    return (st & 1) && !(st & 0x20);   /* data=1, aux=0 */
}
static inline void kbd_drain_aux(void){
    /* aux 데이터가 남아 있으면 소비해서 버린다 */
    while(1){
        uint8_t st = inb(0x64);
        if(!(st & 1)) break;        /* 데이터 없음 */
        if(!(st & 0x20)) break;     /* kbd 데이터면 중단 (건드리지 않음) */
        (void)inb(0x60);            /* aux 데이터 버림 */
    }
}
static uint8_t getch_raw_u8_blocking(void){
    for(;;){
        kbd_drain_aux();
        if(kbd_data_ready()){
            uint8_t sc = inb(0x60);
            return kbd_process_scancode(sc);
        }
    }
}
static uint8_t getch_raw_u8_poll(void){
    kbd_drain_aux();
    if(!kbd_data_ready()) return KEY_NONE;
    uint8_t sc = inb(0x60);
    return kbd_process_scancode(sc);
}

/* =========================================================
 *  PS/2 Mouse  (IRQ12 / aux port 0x60/0x64)
 *  mouse_x/y now track in MODE13H pixel coordinates (0..319, 0..199)
 * ========================================================= */
static int  mouse_x   = 160;   /* pixel x */
static int  mouse_y   = 100;   /* pixel y */
static int  mouse_btn = 0;      /* bit0=L bit1=R bit2=M */
static int  mouse_enabled = 0;
/* Sub-pixel accumulator for smooth movement */
static int  mouse_ax  = 0;
static int  mouse_ay  = 0;

static void mouse_send(uint8_t b){
    while(inb(0x64)&2);
    outb(0x64,0xD4);
    while(inb(0x64)&2);
    outb(0x60,b);
}
static uint8_t mouse_recv(void){
    int t=200000;
    while(t-- && !(inb(0x64)&1));
    return inb(0x60);
}
static void mouse_init(void){
    /* enable aux */
    while(inb(0x64)&2); outb(0x64,0xA8);
    /* set compaq status byte bit 1 */
    while(inb(0x64)&2); outb(0x64,0x20);
    uint8_t st = mouse_recv() | 2;
    while(inb(0x64)&2); outb(0x64,0x60);
    while(inb(0x64)&2); outb(0x60,st);
    /* reset + defaults */
    mouse_send(0xFF); mouse_recv(); mouse_recv(); mouse_recv();
    mouse_send(0xF6); mouse_recv();
    mouse_send(0xF4); mouse_recv();  /* enable */
    mouse_enabled = 1;
}

/* aux 포트에서 1바이트 읽기 (bit5=aux 확인 필수) */
static int mouse_read_byte(uint8_t *out){
    int t = 100000;
    while(t--){
        uint8_t st = inb(0x64);
        if((st & 0x21) == 0x21){   /* data=1, aux=1 */
            *out = inb(0x60);
            return 1;
        }
        if(st & 1) return 0;       /* kbd 데이터 → 건드리지 않음 */
    }
    return 0;
}

/* poll one 3-byte packet, non-blocking; returns 1 if got packet */
static int mouse_poll_packet(int *dx,int *dy,int *btn){
    /* 첫 바이트: aux 데이터여야 함 */
    uint8_t st = inb(0x64);
    if((st & 0x21) != 0x21) return 0;
    uint8_t b0 = inb(0x60);
    if(!(b0 & 0x08)) return 0;    /* always-1 sanity */
    uint8_t b1=0, b2=0;
    if(!mouse_read_byte(&b1)) return 0;
    if(!mouse_read_byte(&b2)) return 0;
    *dx  =  (int)(int8_t)b1;
    *dy  = -(int)(int8_t)b2;
    *btn =  (int)(b0 & 7);
    return 1;
}

static uint16_t mcur_saved = 0;
static int      mcur_sr=-1, mcur_sc=-1;

static void mouse_cursor_erase(void){
    if(mcur_sr>=0&&mcur_sc>=0)
        VGA[mcur_sr*COLS+mcur_sc]=mcur_saved;
}
static void mouse_cursor_draw(void){
    int r=mouse_y, c=mouse_x;
    if(r<0)r=0; if(r>=ROWS)r=ROWS-1;
    if(c<0)c=0; if(c>=COLS)c=COLS-1;
    mcur_sr=r; mcur_sc=c;
    mcur_saved=VGA[r*COLS+c];
    VGA[r*COLS+c]=0x7F00|0xFEu;  /* bright inv-block */
}
static void mouse_process(void){
    int dx,dy,btn;
    if(!mouse_enabled) return;
    while(mouse_poll_packet(&dx,&dy,&btn)){
        /* Scale: PS/2 delta already in pixels (signed 8-bit), apply 1:1 */
        mouse_ax += dx * 2;   /* slight acceleration for usability */
        mouse_ay += dy * 2;
        int mx = mouse_ax >> 1;
        int my = mouse_ay >> 1;
        mouse_ax -= mx << 1;
        mouse_ay -= my << 1;
        mouse_x += mx;
        mouse_y += my;
        if(mouse_x < 0) mouse_x = 0;
        if(mouse_x >= 320) mouse_x = 319;
        if(mouse_y < 0) mouse_y = 0;
        if(mouse_y >= 200) mouse_y = 199;
        mouse_btn = btn;
    }
}

static void kill_foreground_from_kbd(char why){
    if(fg_pid < 0 || fg_pid >= MAX_TASKS) return;
    if(!tasks[fg_pid].used){ fg_pid = -1; shell_paused = 0; return; }
    tasks[fg_pid].used = 0;
    fg_pid = -1;
    shell_paused = 0;
    putc('^'); putc(why); putc('\n');
}

/* getch exported */
char getch(void){
    for(;;){
        uint8_t k = getch_raw_u8_blocking();

        if(k == KEY_CTRL_C){
            if(shell_paused && fg_pid != -1){
                kill_foreground_from_kbd('C');
                continue;
            }
            return (char)KEY_CTRL_C;
        }
        if(k == KEY_CTRL_E){
            if(shell_paused && fg_pid != -1){
                kill_foreground_from_kbd('E');
                continue;
            }
            return (char)KEY_CTRL_E;
        }

        if(k < 0x80 && k != 0) return (char)k;
    }
}

/* ---- cursor helpers ---- */
static void cursor_left(void){
    if(col > 0) col--;
    else if(row > 0){ row--; col = COLS-1; }
}
static void cursor_right(void){
    if(col < COLS-1) col++;
    else { col = 0; row++; if(row==ROWS) scroll(); }
}

/* =========================================================
 * Hangul font v6  —  모든 자모 좌우 올바르게 검증
 *
 *  비트 레이아웃:
 *    bmp_setpx(b, x, y): row[y] |= 0x8000>>x
 *    x=0 = 왼쪽(MSB), x=15 = 오른쪽(LSB)
 *    split: left[y]=row>>8 (x=0..7), right[y]=row&0xFF (x=8..15)
 *    VGA: 각 바이트 bit7 = 가장 왼쪽 픽셀
 *
 *  구역:
 *    수직모음(ㅏ계):  초성 x=0..7 y=0..9 (left-glyph)
 *                    중성 x=9..15 y=0..15 (right-glyph)
 *    수평모음(ㅗ계):  초성 x=0..15 y=0..6
 *                    중성 x=0..15 y=7..15
 *    받침:           x=1..14 y=10..15
 *
 *  자모별 방향 검증 (수직모음 기준 x=0..7):
 *    ㄱ: 위가로(y=0) + 오른세로(x=7) ← 오른쪽에서 내려옴
 *    ㄴ: 왼세로(x=0) + 아래가로(y=9) ← 왼쪽에서 내려와 오른쪽으로
 *    ㄷ: 위가로 + 왼세로 + 아래가로  (ㄷ = 위아래+왼)
 *    ...
 *
 *  폰트 에디터: CLI 명령 "fontedit" 으로 접근
 * ========================================================= */
typedef struct { uint16_t row[16]; } bmp16_t;

static bmp16_t font_cho[19];
static bmp16_t font_jung[21];
static bmp16_t font_jong[19];

/* ─── 픽셀 기본 함수 ──────────────────────────── */
static inline void bmp_clear(bmp16_t *b){
    for(int i=0;i<16;i++) b->row[i]=0;
}
/* x=0=왼쪽(MSB) x=15=오른쪽(LSB) */
static inline void bmp_setpx(bmp16_t *b, int x, int y){
    if((unsigned)x>=16||(unsigned)y>=16) return;
    b->row[y] |= (uint16_t)(0x8000u >> x);
}
static void bmp_hline(bmp16_t *b, int x0,int x1,int y){
    for(int x=x0;x<=x1;x++) bmp_setpx(b,x,y);
}
static void bmp_vline(bmp16_t *b, int x,int y0,int y1){
    for(int y=y0;y<=y1;y++) bmp_setpx(b,x,y);
}
/* 대각선 (두께 포함) */
static void bmp_diag(bmp16_t *b, int x0,int y0,int x1,int y1){
    int dx=x1>x0?x1-x0:x0-x1, sx=x0<x1?1:-1;
    int dy=y1>y0?y0-y1:y1-y0, sy=y0<y1?1:-1;
    int err=dx+dy;
    for(;;){
        bmp_setpx(b,x0,y0);
        if(dx > -dy) bmp_setpx(b,x0,y0+(sy>0?1:-1));
        else         bmp_setpx(b,x0+(sx>0?1:-1),y0);
        if(x0==x1&&y0==y1) break;
        int e2=err*2;
        if(e2>=dy){err+=dy;x0+=sx;}
        if(e2<=dx){err+=dx;y0+=sy;}
    }
}
/* 타원 테두리 */
static void bmp_oval(bmp16_t *b, int cx,int cy,int rx,int ry){
    if(rx<=0||ry<=0) return;
    long ry2=(long)ry*ry, rx2=(long)rx*rx, T=rx2*ry2;
    for(int yy=-ry;yy<=ry;yy++) for(int xx=-rx;xx<=rx;xx++){
        long in=(long)xx*xx*ry2 + (long)yy*yy*rx2;
        if(in>T) continue;                       /* 타원 바깥 */
        /* 4이웃 중 하나라도 바깥이면 경계 픽셀 → 깔끔한 링 */
        long l=(long)(xx-1)*(xx-1)*ry2 + (long)yy*yy*rx2;
        long r=(long)(xx+1)*(xx+1)*ry2 + (long)yy*yy*rx2;
        long u=(long)xx*xx*ry2 + (long)(yy-1)*(yy-1)*rx2;
        long d=(long)xx*xx*ry2 + (long)(yy+1)*(yy+1)*rx2;
        if(l>T||r>T||u>T||d>T) bmp_setpx(b,cx+xx,cy+yy);
    }
}

/* ─── 초성 · 수직모음 x=0..7, y=0..9 ──────────
 *  x=0=LEFT, x=7=RIGHT  (이미지 왼쪽=오른쪽 획)
 *  ㄱ: 위가로 + 오른세로(x=7)
 *  ㄴ: 왼세로(x=0) + 아래가로
 *  ㄷ: 위가로 + 왼세로 + 아래가로
 *  ㄹ: ㄷ+Z자
 *  ㅁ: 사각형
 *  ㅂ: 사각형+가운데가로
 *  ㅅ: 위에서 아래 대각선 좌우
 *  ㅇ: 타원
 *  ㅈ: 위가로+아래로 대각선 좌우
 * ─────────────────────────────────────────── */
static void cho_draw_V(int L, bmp16_t *o){
    /* x=0..7, y=0..9  중심(mx=3 or 4) */
    switch(L){
    /*  ㄱ: 위가로 + 오른쪽세로 */
    case  0: bmp_hline(o,0,7,0); bmp_vline(o,7,0,9); break;
    /*  ㄲ: ㄱ + 안쪽 ㄱ */
    case  1: bmp_hline(o,0,7,0); bmp_vline(o,7,0,9);
             bmp_hline(o,0,5,2); bmp_vline(o,5,2,9); break;
    /*  ㄴ: 왼쪽세로 + 아래가로 */
    case  2: bmp_vline(o,0,0,9); bmp_hline(o,0,7,9); break;
    /*  ㄷ: 위가로 + 왼쪽세로 + 아래가로 */
    case  3: bmp_hline(o,0,7,0); bmp_vline(o,0,0,9); bmp_hline(o,0,7,9); break;
    /*  ㄸ: ㄷ + 안쪽 가로 */
    case  4: bmp_hline(o,0,7,0); bmp_vline(o,0,0,9); bmp_hline(o,0,7,9);
             bmp_hline(o,2,7,3); break;
    /*  ㄹ: 위가로+왼세로+중가로+오른세로+중가로+왼세로+아래가로 */
    case  5: bmp_hline(o,0,7,0); bmp_vline(o,0,0,3); bmp_hline(o,0,7,3);
             bmp_vline(o,7,3,6); bmp_hline(o,0,7,6); bmp_vline(o,0,6,9);
             bmp_hline(o,0,7,9); break;
    /*  ㅁ: 사각형 */
    case  6: bmp_hline(o,0,7,0); bmp_vline(o,0,0,9);
             bmp_vline(o,7,0,9); bmp_hline(o,0,7,9); break;
    /*  ㅂ: 사각형 + 가운데 가로 + 위 두 세로획 */
    case  7: bmp_hline(o,0,7,0); bmp_vline(o,0,0,9);
             bmp_vline(o,7,0,9); bmp_hline(o,0,7,9);
             bmp_hline(o,0,7,5); break;
    /*  ㅃ: ㅂ + 안쪽 세로 */
    case  8: bmp_hline(o,0,7,0); bmp_vline(o,0,0,9);
             bmp_vline(o,7,0,9); bmp_hline(o,0,7,9);
             bmp_hline(o,0,7,5); bmp_vline(o,4,0,5); break;
    /*  ㅅ: 꼭대기→왼아래, 꼭대기→오른아래  (꼭대기 x=4) */
    case  9: bmp_diag(o,4,0,0,9); bmp_diag(o,4,0,7,9); break;
    /*  ㅆ: ㅅ + 오른쪽 ㅅ */
    case 10: bmp_diag(o,4,0,0,9); bmp_diag(o,4,0,7,9);
             bmp_diag(o,5,0,2,9); break;
    /*  ㅇ: 타원 */
    case 11: bmp_oval(o,3,5,3,4); break;
    /*  ㅈ: 위가로 + 아래 V자 */
    /*  ㅈ: 위가로 + 아래 V자 (끝점 x=3.5 근사) */
    case 12: bmp_hline(o,0,7,0); bmp_diag(o,0,1,3,9); bmp_diag(o,7,1,3,9); break;
    /*  ㅉ: ㅈ + 위가로 하나 더 */
    case 13: bmp_hline(o,0,7,0); bmp_hline(o,0,7,2);
             bmp_diag(o,0,3,3,9); bmp_diag(o,7,3,3,9); break;
    /*  ㅊ: 위 점획 + 가로 + 아래 V */
    case 14: bmp_hline(o,2,5,0); bmp_hline(o,0,7,2);
             bmp_diag(o,0,3,3,9); bmp_diag(o,7,3,3,9); break;
    /*  ㅋ: ㄱ + 중간 가로 */
    case 15: bmp_hline(o,0,7,0); bmp_vline(o,7,0,9);
             bmp_hline(o,0,7,5); break;
    /*  ㅌ: ㄷ + 중간 가로 */
    case 16: bmp_hline(o,0,7,0); bmp_vline(o,0,0,9);
             bmp_hline(o,0,7,9); bmp_hline(o,0,7,5); break;
    /*  ㅍ: 위가로+아래가로+두 세로 */
    case 17: bmp_hline(o,0,7,0); bmp_hline(o,0,7,9);
             bmp_vline(o,2,0,9); bmp_vline(o,5,0,9); break;
    /*  ㅎ: 위 점+가로+동그라미 */
    case 18: bmp_hline(o,2,5,0); bmp_hline(o,0,7,2);
             bmp_oval(o,3,6,3,3); break;
    }
}

/* ─── 초성 · 수평모음 x=0..15, y=0..6 ─────────
 *  전체 폭 사용, ㄱ의 오른쪽 세로획 = x=15
 * ─────────────────────────────────────────── */
static void cho_draw_H(int L, bmp16_t *o){
    switch(L){
    case  0: bmp_hline(o,0,15,0); bmp_vline(o,15,0,6); break;
    case  1: bmp_hline(o,0,15,0); bmp_vline(o,15,0,6);
             bmp_hline(o,0,12,2); bmp_vline(o,12,2,6); break;
    case  2: bmp_vline(o,0,0,6);  bmp_hline(o,0,15,6); break;
    case  3: bmp_hline(o,0,15,0); bmp_vline(o,0,0,6); bmp_hline(o,0,15,6); break;
    case  4: bmp_hline(o,0,15,0); bmp_vline(o,0,0,6); bmp_hline(o,0,15,6);
             bmp_hline(o,3,15,2); break;
    case  5: bmp_hline(o,0,15,0); bmp_vline(o,0,0,2); bmp_hline(o,0,15,2);
             bmp_vline(o,15,2,4); bmp_hline(o,0,15,4); bmp_vline(o,0,4,6);
             bmp_hline(o,0,15,6); break;
    case  6: bmp_hline(o,0,15,0); bmp_vline(o,0,0,6);
             bmp_vline(o,15,0,6); bmp_hline(o,0,15,6); break;
    case  7: bmp_hline(o,0,15,0); bmp_vline(o,0,0,6);
             bmp_vline(o,15,0,6); bmp_hline(o,0,15,6);
             bmp_hline(o,0,15,3); break;
    case  8: bmp_hline(o,0,15,0); bmp_vline(o,0,0,6);
             bmp_vline(o,15,0,6); bmp_hline(o,0,15,6);
             bmp_hline(o,0,15,3); bmp_vline(o,8,0,3); break;
    case  9: bmp_diag(o,8,0,0,6); bmp_diag(o,8,0,15,6); break;
    case 10: bmp_diag(o,8,0,0,6); bmp_diag(o,8,0,15,6);
             bmp_diag(o,10,0,3,6); break;
    case 11: bmp_oval(o,7,3,6,3); break;
    case 12: bmp_hline(o,0,15,0); bmp_diag(o,0,1,7,6); bmp_diag(o,15,1,7,6); break;
    case 13: bmp_hline(o,0,15,0); bmp_hline(o,0,15,1);
             bmp_diag(o,0,2,7,6); bmp_diag(o,15,2,7,6); break;
    case 14: bmp_hline(o,4,11,0); bmp_hline(o,0,15,1);
             bmp_diag(o,0,2,7,6); bmp_diag(o,15,2,7,6); break;
    case 15: bmp_hline(o,0,15,0); bmp_vline(o,15,0,6);
             bmp_hline(o,0,15,3); break;
    case 16: bmp_hline(o,0,15,0); bmp_vline(o,0,0,6);
             bmp_hline(o,0,15,6); bmp_hline(o,0,15,3); break;
    case 17: bmp_hline(o,0,15,0); bmp_hline(o,0,15,6);
             bmp_vline(o,4,0,6); bmp_vline(o,11,0,6); break;
    case 18: bmp_hline(o,4,11,0); bmp_hline(o,0,15,1);
             bmp_oval(o,7,4,6,2); break;
    }
}

/* ─── 중성 ─────────────────────────────────────
 *  수직계: 기둥 x=14, 획은 x=9..15 방향
 *  수평계: 가로 y=8 기준
 * ─────────────────────────────────────────── */
static void build_jung(int V, bmp16_t *o){
    switch(V){
    /* ─ 수직계 기둥 x=14 ─ */
    case  0:/*ㅏ*/ bmp_vline(o,14,0,15); bmp_hline(o,14,15,7); break;
    case  1:/*ㅐ*/ bmp_vline(o,14,0,15); bmp_hline(o,14,15,7);
                   bmp_vline(o,11,0,15); break;
    case  2:/*ㅑ*/ bmp_vline(o,14,0,15); bmp_hline(o,14,15,4);
                   bmp_hline(o,14,15,10); break;
    case  3:/*ㅒ*/ bmp_vline(o,14,0,15); bmp_hline(o,14,15,4);
                   bmp_hline(o,14,15,10); bmp_vline(o,11,0,15); break;
    case  4:/*ㅓ*/ bmp_vline(o,14,0,15); bmp_hline(o,9,14,7); break;
    case  5:/*ㅔ*/ bmp_vline(o,14,0,15); bmp_hline(o,9,14,7);
                   bmp_vline(o,11,0,15); break;
    case  6:/*ㅕ*/ bmp_vline(o,14,0,15); bmp_hline(o,9,14,4);
                   bmp_hline(o,9,14,10); break;
    case  7:/*ㅖ*/ bmp_vline(o,14,0,15); bmp_hline(o,9,14,4);
                   bmp_hline(o,9,14,10); bmp_vline(o,11,0,15); break;
    /* ─ 수평계 ─ */
    case  8:/*ㅗ*/ bmp_hline(o,1,14,8);  bmp_vline(o,7,3,8); break;
    case  9:/*ㅘ*/ bmp_hline(o,1,14,8);  bmp_vline(o,4,3,8);
                   bmp_vline(o,13,8,14); bmp_hline(o,13,15,11); break;
    case 10:/*ㅙ*/ bmp_hline(o,1,14,8);  bmp_vline(o,4,3,8);
                   bmp_vline(o,11,8,14); bmp_hline(o,11,15,11);
                   bmp_vline(o,13,3,14); break;
    case 11:/*ㅚ*/ bmp_hline(o,1,14,8);  bmp_vline(o,4,3,8);
                   bmp_vline(o,13,3,14); break;
    case 12:/*ㅛ*/ bmp_hline(o,1,14,8);  bmp_vline(o,5,3,8);
                   bmp_vline(o,11,3,8); break;
    case 13:/*ㅜ*/ bmp_hline(o,1,14,7);  bmp_vline(o,7,7,13); break;
    case 14:/*ㅝ*/ bmp_hline(o,1,14,7);  bmp_vline(o,4,7,13);
                   bmp_vline(o,13,4,13); bmp_hline(o,9,13,10); break;
    case 15:/*ㅞ*/ bmp_hline(o,1,14,7);  bmp_vline(o,4,7,13);
                   bmp_vline(o,11,4,13); bmp_hline(o,9,11,10);
                   bmp_vline(o,13,4,13); break;
    case 16:/*ㅟ*/ bmp_hline(o,1,14,7);  bmp_vline(o,4,7,13);
                   bmp_vline(o,13,4,13); break;
    case 17:/*ㅠ*/ bmp_hline(o,1,14,7);  bmp_vline(o,5,7,13);
                   bmp_vline(o,11,7,13); break;
    case 18:/*ㅡ*/ bmp_hline(o,1,14,9); break;
    case 19:/*ㅢ*/ bmp_hline(o,1,14,9); bmp_vline(o,14,3,15); break;
    case 20:/*ㅣ*/ bmp_vline(o,14,0,15); break;
    }
}

static int is_vowel_vertical(int V){
    return !(V==8||V==12||V==13||V==17||V==18);
}

/* ─── 받침 ─ ax..bx, ay..by 영역 ──────────────
 *  방향: ax=왼쪽, bx=오른쪽  (ㄱ=위+오른세로)
 * ─────────────────────────────────────────── */
static void jong_at(int L, bmp16_t *o, int ax,int bx,int ay,int by){
    int mx=(ax+bx)/2;
    switch(L){
    case  0: bmp_hline(o,ax,bx,ay); bmp_vline(o,bx,ay,by); break;
    case  1: bmp_hline(o,ax,bx,ay); bmp_vline(o,bx,ay,by);
             bmp_hline(o,ax,bx-2,ay+1); bmp_vline(o,bx-2,ay+1,by); break;
    case  2: bmp_vline(o,ax,ay,by); bmp_hline(o,ax,bx,by); break;
    case  3: bmp_hline(o,ax,bx,ay); bmp_vline(o,ax,ay,by); bmp_hline(o,ax,bx,by); break;
    case  4: bmp_hline(o,ax,bx,ay); bmp_vline(o,ax,ay,by); bmp_hline(o,ax,bx,by);
             bmp_hline(o,ax+1,bx,ay+1); break;
    case  5: { int h1=ay+2,h2=ay+4;
               bmp_hline(o,ax,bx,ay); bmp_vline(o,ax,ay,h1); bmp_hline(o,ax,bx,h1);
               bmp_vline(o,bx,h1,h2); bmp_hline(o,ax,bx,h2); bmp_vline(o,ax,h2,by);
               bmp_hline(o,ax,bx,by); break; }
    case  6: bmp_hline(o,ax,bx,ay); bmp_vline(o,ax,ay,by);
             bmp_vline(o,bx,ay,by); bmp_hline(o,ax,bx,by); break;
    case  7: bmp_hline(o,ax,bx,ay); bmp_vline(o,ax,ay,by);
             bmp_vline(o,bx,ay,by); bmp_hline(o,ax,bx,by);
             bmp_hline(o,ax,bx,(ay+by)/2); break;
    case  8: bmp_hline(o,ax,bx,ay); bmp_vline(o,ax,ay,by);
             bmp_vline(o,bx,ay,by); bmp_hline(o,ax,bx,by);
             bmp_hline(o,ax,bx,(ay+by)/2); bmp_vline(o,mx,ay,(ay+by)/2); break;
    case  9: bmp_diag(o,mx,ay,ax,by); bmp_diag(o,mx,ay,bx,by); break;
    case 10: bmp_diag(o,mx,ay,ax,by); bmp_diag(o,mx,ay,bx,by);
             bmp_diag(o,mx+1,ay,ax+1,by); break;
    case 11: bmp_oval(o,mx,(ay+by)/2,(bx-ax)/2,(by-ay)/2); break;
    case 12: bmp_hline(o,ax,bx,ay);
             bmp_diag(o,ax,ay+1,mx,by); bmp_diag(o,bx,ay+1,mx,by); break;
    case 13: bmp_hline(o,ax,bx,ay); bmp_hline(o,ax,bx,ay+1);
             bmp_diag(o,ax,ay+2,mx,by); bmp_diag(o,bx,ay+2,mx,by); break;
    case 14: bmp_hline(o,ax+1,bx-1,ay); bmp_hline(o,ax,bx,ay+1);
             bmp_diag(o,ax,ay+2,mx,by); bmp_diag(o,bx,ay+2,mx,by); break;
    case 15: bmp_hline(o,ax,bx,ay); bmp_vline(o,bx,ay,by);
             bmp_hline(o,ax,bx,ay+2); break;
    case 16: bmp_hline(o,ax,bx,ay); bmp_vline(o,ax,ay,by);
             bmp_hline(o,ax,bx,ay+2); bmp_hline(o,ax,bx,by); break;
    case 17: bmp_hline(o,ax,bx,ay); bmp_hline(o,ax,bx,by);
             bmp_vline(o,ax+2,ay,by); bmp_vline(o,bx-2,ay,by); break;
    case 18: bmp_hline(o,ax+1,bx-1,ay); bmp_hline(o,ax,bx,ay+1);
             bmp_oval(o,mx,(ay+by)/2,(bx-ax)/2-1,(by-ay)/2-1); break;
    }
}
static void jong_draw(int L, bmp16_t *o){ jong_at(L,o,1,14,10,15); }

static int jong_to_cho(int jT){
    switch(jT){
        case  1:return  0; case  2:return  1; case  4:return  2;
        case  7:return  3; case  8:return  5; case 16:return  6;
        case 17:return  7; case 19:return  9; case 20:return 10;
        case 21:return 11; case 22:return 12; case 23:return 14;
        case 24:return 15; case 25:return 16; case 26:return 17;
        case 27:return 18; default:return -1;
    }
}
static void jong_composite(int jT,int*a,int*b){
    *a=-1;*b=-1;
    switch(jT){
        case  3:*a=0;*b=9;break;  case  5:*a=2;*b=12;break;
        case  6:*a=2;*b=18;break; case  9:*a=5;*b=0;break;
        case 10:*a=5;*b=6;break;  case 11:*a=5;*b=7;break;
        case 12:*a=5;*b=9;break;  case 13:*a=5;*b=16;break;
        case 14:*a=5;*b=17;break; case 15:*a=5;*b=18;break;
        case 18:*a=7;*b=9;break;
    }
}

/* ─── 폰트 초기화 ─────────────────────────────── */
static void init_jamo_fonts(void){
    for(int i=0;i<19;i++){ bmp_clear(&font_cho[i]);  cho_draw_V(i,&font_cho[i]); }
    for(int i=0;i<21;i++){ bmp_clear(&font_jung[i]); build_jung(i,&font_jung[i]); }
    for(int i=0;i<19;i++){ bmp_clear(&font_jong[i]); jong_draw(i,&font_jong[i]); }
}

/* ─── 폰트 디스크 저장/로드 (LBA 90) ─────────── */
#define FONT_SAVE_LBA   90      /* 4섹터 사용 */
#define FONT_MAGIC      0xF04E7401u

static void font_save_disk(void){
    /* 레이아웃: [4 magic] [19*32 cho] [21*32 jung] [19*32 jong] = 1892 bytes < 4섹터 */
    static uint8_t fbuf[4*512];
    uint32_t *magic = (uint32_t*)fbuf;
    *magic = FONT_MAGIC;
    uint8_t *p = fbuf + 4;
    for(int i=0;i<19;i++){ for(int r=0;r<16;r++){ p[r*2]=(uint8_t)(font_cho[i].row[r]>>8); p[r*2+1]=(uint8_t)(font_cho[i].row[r]&0xFF); } p+=32; }
    for(int i=0;i<21;i++){ for(int r=0;r<16;r++){ p[r*2]=(uint8_t)(font_jung[i].row[r]>>8); p[r*2+1]=(uint8_t)(font_jung[i].row[r]&0xFF); } p+=32; }
    for(int i=0;i<19;i++){ for(int r=0;r<16;r++){ p[r*2]=(uint8_t)(font_jong[i].row[r]>>8); p[r*2+1]=(uint8_t)(font_jong[i].row[r]&0xFF); } p+=32; }
    ata_write28(FONT_SAVE_LBA, 4, fbuf);
}

static void font_load_disk(void){
    static uint8_t fbuf[4*512];
    ata_read28(FONT_SAVE_LBA, 4, fbuf);
    uint32_t magic = *(uint32_t*)fbuf;
    if(magic != FONT_MAGIC) return;   /* 저장된 폰트 없음 */
    uint8_t *p = fbuf + 4;
    for(int i=0;i<19;i++){ for(int r=0;r<16;r++){ font_cho[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); } p+=32; }
    for(int i=0;i<21;i++){ for(int r=0;r<16;r++){ font_jung[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); } p+=32; }
    for(int i=0;i<19;i++){ for(int r=0;r<16;r++){ font_jong[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); } p+=32; }
}

/* ─── 조합 16×16 → 8×16 × 2 ──────────────────── */
static void bmp16_clear(uint16_t out[16]){ for(int i=0;i<16;i++) out[i]=0; }
static void bmp16_or(uint16_t out[16],const bmp16_t*src){
    for(int y=0;y<16;y++) out[y]|=src->row[y];
}

static void compose_hangul_16x16(uint16_t out[16],int L,int V,int jT){
    bmp16_clear(out);
    int vert=is_vowel_vertical(V);
    bmp16_t tmp; bmp_clear(&tmp);
    if(vert) cho_draw_V(L,&tmp); else cho_draw_H(L,&tmp);
    bmp16_or(out,&tmp);
    bmp16_or(out,&font_jung[V]);
    if(jT!=0){
        int base=jong_to_cho(jT);
        if(base>=0){ bmp16_or(out,&font_jong[base]); }
        else {
            int a,b; jong_composite(jT,&a,&b);
            if(a>=0){ bmp16_t tA; bmp_clear(&tA); jong_at(a,&tA,1,7,10,15);  bmp16_or(out,&tA); }
            if(b>=0){ bmp16_t tB; bmp_clear(&tB); jong_at(b,&tB,8,14,10,15); bmp16_or(out,&tB); }
        }
    }
}

/* ─── 독립 자모(U+3131–U+3163) 16×16 렌더 ──────
 *  U+3131 ㄱ → U+314E ㅎ : 자음 (30글자, 일부 복합)
 *  U+314F ㅏ → U+3163 ㅣ : 모음 (21글자)
 * ─────────────────────────────────────────────── */
static void draw_standalone_jamo_16x16(uint32_t cp, uint16_t out[16]){
    bmp16_clear(out);
    bmp16_t tmp; bmp_clear(&tmp);

    if(cp >= 0x314F && cp <= 0x3163){
        /* 모음: jung[] 그대로 사용 (전체 폭에 맞게 그려져 있음) */
        int v = (int)(cp - 0x314F);
        if(v < 21) bmp16_or(out, &font_jung[v]);
        return;
    }

    /* 자음: 큰 형태로 단독 렌더
     * jong_at(L, out, ax, bx, ay, by) – 전체 셀(0-15) 크게 */
    int offset = (int)(cp - 0x3131);
    /* 단순 매핑: offset → cho 인덱스 (복합자음은 두 획 분리) */
    static const int8_t c2cho[] = {
    /* ㄱ=0 ㄲ=1 ㄳ=-1 ㄴ=2 ㄵ=-2 ㄶ=-3 ㄷ=3 ㄸ=4 ㄹ=5 */
      0, 1,-1, 2,-2,-3, 3, 4, 5,
    /* ㄺ=-4 ㄻ=-5 ㄼ=-6 ㄽ=-7 ㄾ=-8 ㄿ=-9 ㅀ=-10 ㅁ=6 */
     -4,-5,-6,-7,-8,-9,-10,6,
    /* ㅂ=7 ㅃ=8 ㅄ=-11 ㅅ=9 ㅆ=10 ㅇ=11 ㅈ=12 ㅉ=13 ㅊ=14 ㅋ=15 ㅌ=16 ㅍ=17 ㅎ=18 */
      7, 8,-11, 9,10,11,12,13,14,15,16,17,18
    };
    if(offset < 0 || offset > 29) return;
    int m = c2cho[offset];

    if(m >= 0){
        /* 단순 자음: 전체 셀에 크게 그림 */
        jong_at(m, &tmp, 1, 14, 1, 14);
        bmp16_or(out, &tmp);
    } else {
        /* 복합 자음: 왼쪽에 첫 자음, 오른쪽에 두 번째 자음 */
        int a=-1, b=-1;
        switch(offset){
            /* ㄳ(2) = ㄱ+ㅅ */ case 2:  a=0; b=9;  break;
            /* ㄵ(4) = ㄴ+ㅈ */ case 4:  a=2; b=12; break;
            /* ㄶ(5) = ㄴ+ㅎ */ case 5:  a=2; b=18; break;
            /* ㄺ(9) = ㄹ+ㄱ */ case 9:  a=5; b=0;  break;
            /* ㄻ(10)= ㄹ+ㅁ */ case 10: a=5; b=6;  break;
            /* ㄼ(11)= ㄹ+ㅂ */ case 11: a=5; b=7;  break;
            /* ㄽ(12)= ㄹ+ㅅ */ case 12: a=5; b=9;  break;
            /* ㄾ(13)= ㄹ+ㅌ */ case 13: a=5; b=16; break;
            /* ㄿ(14)= ㄹ+ㅍ */ case 14: a=5; b=17; break;
            /* ㅀ(15)= ㄹ+ㅎ */ case 15: a=5; b=18; break;
            /* ㅄ(19)= ㅂ+ㅅ */ case 19: a=7; b=9;  break;
            default: return;
        }
        bmp16_t tA; bmp_clear(&tA); jong_at(a, &tA, 1, 7,  1, 14); bmp16_or(out, &tA);
        bmp16_t tB; bmp_clear(&tB); jong_at(b, &tB, 8, 14, 1, 14); bmp16_or(out, &tB);
    }
}

static void split_16x16_to_8x16(uint16_t src[16],uint8_t left[16],uint8_t right[16]){
    for(int y=0;y<16;y++){
        left[y] =(uint8_t)(src[y]>>8);
        right[y]=(uint8_t)(src[y]&0xFF);
    }
}

/* compat shims */
static int jong_to_baseL(int jT){ return jong_to_cho(jT); }
static void jong_composite_parts(int jT,int*a,int*b){ jong_composite(jT,a,b); }


#define HGLYPH_FIRST 0x80
#define HGLYPH_PAIRS 64
static uint32_t hcache_key[HGLYPH_PAIRS];
static uint8_t  hcache_used[HGLYPH_PAIRS];
static uint8_t  hcache_hand = 0;

static uint32_t make_hkey(int L,int V,int jongT){
    return ((uint32_t)(L & 31) << 16) | ((uint32_t)(V & 31) << 8) | (uint32_t)(jongT & 63);
}
static int hcache_slot_for(int L,int V,int jongT){
    uint32_t key = make_hkey(L,V,jongT);
    for(int i=0;i<HGLYPH_PAIRS;i++){
        if(hcache_used[i] && hcache_key[i]==key) return i;
    }
    int slot = (int)(hcache_hand++ % HGLYPH_PAIRS);
    hcache_used[slot]=1;
    hcache_key[slot]=key;
    return slot;
}
static void put_hangul_LVT(int L,int V,int jongT){
    int slot = hcache_slot_for(L,V,jongT);
    uint8_t chL = (uint8_t)(HGLYPH_FIRST + slot*2 + 0);
    uint8_t chR = (uint8_t)(HGLYPH_FIRST + slot*2 + 1);

    uint16_t g16[16];
    uint8_t gl[16], gr[16];
    compose_hangul_16x16(g16, L,V,jongT);
    split_16x16_to_8x16(g16, gl, gr);

    vga_write_glyph8x16(chL, gl);
    vga_write_glyph8x16(chR, gr);

    putc((char)chL);
    putc((char)chR);
}

/* UTF-8 encode/decode (minimal) */
static int utf8_decode_one(const uint8_t *s, uint32_t *cp_out){
    uint8_t c0 = s[0];
    if(c0 < 0x80){ *cp_out = c0; return 1; }
    if((c0 & 0xF0) == 0xE0){
        uint8_t c1=s[1], c2=s[2];
        if((c1 & 0xC0)!=0x80 || (c2 & 0xC0)!=0x80) return 0;
        *cp_out = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) | (uint32_t)(c2 & 0x3F);
        return 3;
    }
    if((c0 & 0xE0) == 0xC0){
        uint8_t c1=s[1];
        if((c1 & 0xC0)!=0x80) return 0;
        *cp_out = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
        return 2;
    }
    return 0;
}
static int utf8_encode(uint32_t cp, char *out){
    if(cp < 0x80){ out[0]=(char)cp; return 1; }
    if(cp < 0x800){
        out[0]=(char)(0xC0 | (cp>>6));
        out[1]=(char)(0x80 | (cp&0x3F));
        return 2;
    }
    out[0]=(char)(0xE0 | (cp>>12));
    out[1]=(char)(0x80 | ((cp>>6)&0x3F));
    out[2]=(char)(0x80 | (cp&0x3F));
    return 3;
}
static uint32_t make_syllable_cp(int L,int V,int jongT){
    return 0xAC00u + (uint32_t)((L*21 + V)*28 + jongT);
}
static void put_utf8(const char *s){
    const uint8_t *p = (const uint8_t*)s;
    while(*p){
        uint32_t cp=0;
        int n = utf8_decode_one(p, &cp);
        if(n<=0){ putc('?'); p++; continue; }
        p += (uint8_t)n;

        if(cp >= 0xAC00 && cp <= 0xD7A3){
            uint32_t S = cp - 0xAC00;
            int L = (int)(S / (21*28));
            int V = (int)((S / 28) % 21);
            int jongT = (int)(S % 28);
            put_hangul_LVT(L,V,jongT);
        }else if(cp >= 0x3131 && cp <= 0x3163){
            /* 독립 자모: 16×16 글리프를 slot에 캐싱 후 출력 */
            int slot = (int)(cp - 0x3131) + HGLYPH_PAIRS; /* 별도 슬롯 영역 피하기 위해 offset */
            slot = slot % HGLYPH_PAIRS; /* 단순 해시 */
            uint8_t chL = (uint8_t)(HGLYPH_FIRST + slot*2);
            uint8_t chR = (uint8_t)(HGLYPH_FIRST + slot*2 + 1);
            uint16_t g16[16];
            uint8_t gl[16], gr[16];
            draw_standalone_jamo_16x16(cp, g16);
            split_16x16_to_8x16(g16, gl, gr);
            vga_write_glyph8x16(chL, gl);
            vga_write_glyph8x16(chR, gr);
            putc((char)chL); putc((char)chR);
        }else if(cp < 0x80){
            putc((char)cp);
        }else{
            putc('?');
        }
    }
}

/* =========================================================
 * 2-beolsik IME
 * ========================================================= */
static int ime_hangul = 0;
static int ime_L = -1;
static int ime_V = -1;
static int ime_T = 0;
static void ime_reset(void){ ime_L=-1; ime_V=-1; ime_T=0; }

static int map_L_from_2beolsik(char c){
    switch(c){
        case 'r': return 0;  case 'R': return 1;
        case 's': return 2;
        case 'e': return 3;  case 'E': return 4;
        case 'f': return 5;
        case 'a': return 6;
        case 'q': return 7;  case 'Q': return 8;
        case 't': return 9;  case 'T': return 10;
        case 'd': return 11;
        case 'w': return 12; case 'W': return 13;
        case 'c': return 14;
        case 'z': return 15;
        case 'x': return 16;
        case 'v': return 17;
        case 'g': return 18;
        default: return -1;
    }
}
static int map_V_from_2beolsik(char c){
    switch(c){
        case 'k': return 0;
        case 'o': return 1;
        case 'i': return 2;
        case 'O': return 3;
        case 'j': return 4;
        case 'p': return 5;
        case 'u': return 6;
        case 'P': return 7;
        case 'h': return 8;
        case 'y': return 12;
        case 'n': return 13;
        case 'b': return 17;
        case 'm': return 18;
        case 'l': return 20;
        default: return -1;
    }
}
static int combine_V(int V, int newV){
    if(V==8 && newV==0)  return 9;
    if(V==8 && newV==1)  return 10;
    if(V==8 && newV==20) return 11;
    if(V==13 && newV==4)  return 14;
    if(V==13 && newV==5)  return 15;
    if(V==13 && newV==20) return 16;
    if(V==18 && newV==20) return 19;
    return -1;
}
static int L_to_T_simple(int L){
    switch(L){
        case 0:  return 1;  case 1:  return 2;  case 2:  return 4;  case 3:  return 7;
        case 5:  return 8;  case 6:  return 16; case 7:  return 17; case 9:  return 19;
        case 10: return 20; case 11: return 21; case 12: return 22; case 14: return 23;
        case 15: return 24; case 16: return 25; case 17: return 26; case 18: return 27;
        default: return -1;
    }
}
static int combine_T(int T0, int newL){
    if(T0==1  && newL==9)  return 3;
    if(T0==4  && newL==12) return 5;
    if(T0==4  && newL==18) return 6;
    if(T0==8  && newL==0)  return 9;
    if(T0==8  && newL==6)  return 10;
    if(T0==8  && newL==7)  return 11;
    if(T0==8  && newL==9)  return 12;
    if(T0==8  && newL==16) return 13;
    if(T0==8  && newL==17) return 14;
    if(T0==8  && newL==18) return 15;
    if(T0==17 && newL==9)  return 18;
    return -1;
}

/* 자음(초성 인덱스) → 독립 자모 유니코드 오프셋 (U+3131 기준) */
static const uint8_t cho_to_jamo_offset[] = {
    0,  /* ㄱ=U+3131 */  1,  /* ㄲ=U+3132 */  3,  /* ㄴ=U+3134 */
    6,  /* ㄷ=U+3137 */  7,  /* ㄸ=U+3138 */  8,  /* ㄹ=U+3139 */
   16,  /* ㅁ=U+3141 */ 17,  /* ㅂ=U+3142 */ 18,  /* ㅃ=U+3143 */
   20,  /* ㅅ=U+3145 */ 21,  /* ㅆ=U+3146 */ 22,  /* ㅇ=U+3147 */
   23,  /* ㅈ=U+3148 */ 24,  /* ㅉ=U+3149 */ 25,  /* ㅊ=U+314A */
   26,  /* ㅋ=U+314B */ 27,  /* ㅌ=U+314C */ 28,  /* ㅍ=U+314D */
   29   /* ㅎ=U+314E */
};

static void ime_commit_to_buf(char *buf, int *len, int max){
    if(ime_L<0 && ime_V<0) return;
    if(ime_L<0 && ime_V>=0) ime_L = 11; /* implicit ㅇ */

    if(ime_L>=0 && ime_V>=0){
        uint32_t cp = make_syllable_cp(ime_L, ime_V, ime_T);
        char tmp[4]={0,0,0,0};
        int n = utf8_encode(cp, tmp);
        if(*len + n < max){
            for(int i=0;i<n;i++) buf[(*len)++] = tmp[i];
            buf[*len]=0;
            char pbuf[4]={tmp[0],tmp[1],tmp[2],0};
            put_utf8(pbuf);
        }
    }else if(ime_L>=0 && ime_V<0){
        /* 자음 단독 → 독립 자모로 렌더 */
        uint32_t cp2 = 0x3131u + cho_to_jamo_offset[ime_L];
        char tmp2[4]={0,0,0,0};
        int n2 = utf8_encode(cp2, tmp2);
        if(*len + n2 < max){
            for(int i=0;i<n2;i++) buf[(*len)++] = tmp2[i];
            buf[*len]=0;
            put_utf8(tmp2);
        }
    }
    ime_reset();
}

static int ime_backspace(void){
    if(ime_T){ ime_T=0; return 1; }
    if(ime_V>=0){ ime_V=-1; return 1; }
    if(ime_L>=0){ ime_L=-1; return 1; }
    return 0;
}

/* ---------- line editor ---------- */
#define HIST_MAX 8
static char hist[HIST_MAX][128];
static int hist_len = 0;
static int hist_pos = -1;

static void redraw_line(const char* buf, int len, int pos){
    for(int i=0;i<pos;i++) cursor_left();
    for(int i=0;i<len;i++){ VGA[row*COLS+col] = (0x1F<<8) | ' '; cursor_right(); }
    for(int i=0;i<len;i++) cursor_left();
    for(int i=0;i<len;i++){ VGA[row*COLS+col] = (0x1F<<8) | (uint8_t)buf[i]; cursor_right(); }
    for(int i=0;i<(len-pos);i++) cursor_left();
}

static int getline(char*buf,int max){
    int len=0, pos=0;
    hist_pos=-1;
    ime_reset();

    for(;;){
        uint8_t k = getch_raw_u8_blocking();
        if(k == 0) continue;

        if(k == KEY_TOGGLE_HAN){
            ime_hangul ^= 1;
            puts(ime_hangul ? "[HAN]" : "[ENG]");
            continue;
        }

        if(k == KEY_CTRL_C){ puts("^C\n"); buf[0]=0; ime_reset(); return 0; }
        if(k == KEY_CTRL_E){ puts("^E\n"); buf[0]=0; ime_reset(); return 0; }

        if(k == '\n'){
            if(ime_hangul) ime_commit_to_buf(buf,&len,max);
            buf[len]=0;
            putc('\n');
            if(len>0){
                int idx = hist_len % HIST_MAX;
                strncpy(hist[idx], buf, 127);
                hist[idx][127]=0;
                hist_len++;
            }
            return len;
        }

        if(k == 8){
            if(ime_hangul){
                if(ime_backspace()) continue;
                if(len>0){
                    int rm=1;
                    while(len-rm>0 && ((buf[len-rm] & 0xC0) == 0x80)) rm++;
                    for(int i=0;i<rm;i++) putc('\b');
                    for(int i=0;i<rm;i++) buf[--len]=0;
                }
                continue;
            }
            if(pos>0){
                for(int i=pos-1;i<len-1;i++) buf[i]=buf[i+1];
                len--; pos--;
                redraw_line(buf,len,pos);
            }
            continue;
        }

        /* ENG input */
        if(!ime_hangul){
            if(k == KEY_LEFT){ if(pos>0){pos--;cursor_left();} continue; }
            if(k == KEY_RIGHT){ if(pos<len){pos++;cursor_right();} continue; }
            if(k == KEY_HOME){ while(pos>0){pos--;cursor_left();} continue; }
            if(k == KEY_END){ while(pos<len){pos++;cursor_right();} continue; }
            if(k == KEY_DEL){
                if(pos<len){
                    for(int i=pos;i<len-1;i++) buf[i]=buf[i+1];
                    len--;
                    redraw_line(buf,len,pos);
                }
                continue;
            }
            if(k == KEY_UP){
                if(hist_len==0) continue;
                if(hist_pos==-1) hist_pos=hist_len-1;
                else if(hist_pos>0) hist_pos--;
                int idx = hist_pos % HIST_MAX;

                while(pos>0){pos--;cursor_left();}
                for(int i=0;i<len;i++){ VGA[row*COLS+col]=(0x1F<<8)|' '; cursor_right(); }
                for(int i=0;i<len;i++) cursor_left();

                strncpy(buf, hist[idx], max-1); buf[max-1]=0;
                len=strlen(buf); pos=len;
                for(int i=0;i<len;i++){ VGA[row*COLS+col]=(0x1F<<8)|(uint8_t)buf[i]; cursor_right(); }
                continue;
            }
            if(k == KEY_DOWN){
                if(hist_len==0 || hist_pos==-1) continue;
                if(hist_pos < hist_len-1) hist_pos++;
                else{
                    hist_pos=-1;
                    while(pos>0){pos--;cursor_left();}
                    for(int i=0;i<len;i++){ VGA[row*COLS+col]=(0x1F<<8)|' '; cursor_right(); }
                    for(int i=0;i<len;i++) cursor_left();
                    len=0; pos=0; buf[0]=0;
                    continue;
                }
                int idx=hist_pos%HIST_MAX;

                while(pos>0){pos--;cursor_left();}
                for(int i=0;i<len;i++){ VGA[row*COLS+col]=(0x1F<<8)|' '; cursor_right(); }
                for(int i=0;i<len;i++) cursor_left();

                strncpy(buf, hist[idx], max-1); buf[max-1]=0;
                len=strlen(buf); pos=len;
                for(int i=0;i<len;i++){ VGA[row*COLS+col]=(0x1F<<8)|(uint8_t)buf[i]; cursor_right(); }
                continue;
            }

            if(k < 0x80 && k >= 32){
                if(len < max-1){
                    if(pos==len){ buf[len++]=(char)k; pos=len; putc((char)k); }
                    else{
                        for(int i=len;i>pos;i--) buf[i]=buf[i-1];
                        buf[pos++]=(char)k; len++;
                        redraw_line(buf,len,pos);
                    }
                }
            }
            continue;
        }

        /* HAN compose */
        if(k < 0x80 && k >= 32){
            char c=(char)k;

            if(c==' ' || c=='\t'){
                ime_commit_to_buf(buf,&len,max);
                if(len<max-1){ buf[len++]=c; buf[len]=0; putc(c); }
                continue;
            }

            int newL = map_L_from_2beolsik(c);
            int newV = map_V_from_2beolsik(c);

            if(newV >= 0){
                if(ime_L < 0 && ime_V < 0){ ime_L=11; ime_V=newV; continue; }
                if(ime_L >= 0 && ime_V < 0){ ime_V=newV; continue; }
                if(ime_L >= 0 && ime_V >= 0 && ime_T==0){
                    int comb = combine_V(ime_V, newV);
                    if(comb>=0){ ime_V=comb; continue; }
                    ime_commit_to_buf(buf,&len,max);
                    ime_L=11; ime_V=newV; ime_T=0; continue;
                }
                if(ime_L>=0 && ime_V>=0 && ime_T!=0){
                    /* 받침 뒤에 모음이 오면 받침을 다음 음절의 초성으로 이동.
                     * 겹받침(ㄳ,ㄵ,ㄺ,ㅄ …)이면 앞자음만 받침으로 남기고
                     * 뒷자음만 초성으로 넘긴다. 예) "값"+"ㅏ" → "갑사". */
                    int save_T=ime_T;
                    int a,b; jong_composite(save_T,&a,&b);
                    if(b>=0){
                        ime_T=L_to_T_simple(a);        /* 앞자음을 받침으로 */
                        ime_commit_to_buf(buf,&len,max);/* 현재 음절 확정 */
                        ime_L=b; ime_V=newV; ime_T=0;  /* 뒷자음이 새 초성 */
                    }else{
                        int next_L=jong_to_cho(save_T);
                        ime_T=0;
                        ime_commit_to_buf(buf,&len,max);/* 받침 없이 확정 */
                        ime_L=(next_L>=0)?next_L:11; ime_V=newV; ime_T=0;
                    }
                    continue;
                }
                continue;
            }

            if(newL >= 0){
                if(ime_L < 0 && ime_V < 0){ ime_L=newL; continue; }
                if(ime_L >= 0 && ime_V < 0){
                    ime_commit_to_buf(buf,&len,max);
                    ime_L=newL; ime_V=-1; ime_T=0; continue;
                }
                if(ime_L>=0 && ime_V>=0){
                    if(ime_T==0){
                        int t=L_to_T_simple(newL);
                        if(t>=0){ ime_T=t; continue; }
                        ime_commit_to_buf(buf,&len,max);
                        ime_L=newL; ime_V=-1; ime_T=0; continue;
                    }else{
                        int combT = combine_T(ime_T, newL);
                        if(combT>=0){ ime_T=combT; continue; }
                        ime_commit_to_buf(buf,&len,max);
                        ime_L=newL; ime_V=-1; ime_T=0; continue;
                    }
                }
                continue;
            }

            ime_commit_to_buf(buf,&len,max);
            if(len<max-1){ buf[len++]=c; buf[len]=0; putc(c); }
        }
    }
}

/* ---------- redirect flush (FS 함수 이후 정의) ---------- */
static void redir_flush(void){
    if(!redir_active) return;
    uint8_t *wb = (uint8_t*)0x300000;
    uint32_t base_len = 0;
    dirent_t e;
    int exists = fs_find(cur_dir, redir_file, &e);
    if(redir_active == 2 && exists && e.size>0){
        fs_load_file(&e, wb);
        base_len = e.size;
    }
    uint32_t total = base_len + (uint32_t)redir_buf_len;
    for(int i=0;i<redir_buf_len;i++) wb[base_len+i]=(uint8_t)redir_buf[i];
    uint32_t sc=(total+511)/512; if(!sc)sc=1;
    uint32_t lba = get_free_lba(sc);
    ata_write28(lba, sc, wb);
    if(!exists || redir_active==1){
        if(exists){
            ata_read28(cur_dir, DIR_SECTORS, dirbuf);
            for(int idx=0;idx<DIR_ENTRIES;++idx){
                dirent_t *de=(dirent_t*)(dirbuf+idx*sizeof(dirent_t));
                if(!strcmp(de->name,redir_file)){ memset(de,0,sizeof(dirent_t)); break; }
            }
            ata_write28(cur_dir, DIR_SECTORS, dirbuf);
        }
        fs_add_entry(cur_dir, redir_file, lba, total);
    } else {
        ata_read28(cur_dir, DIR_SECTORS, dirbuf);
        for(int idx=0;idx<DIR_ENTRIES;++idx){
            dirent_t *de=(dirent_t*)(dirbuf+idx*sizeof(dirent_t));
            if(!strcmp(de->name,redir_file)){ de->lba=lba; de->size=total; break; }
        }
        ata_write28(cur_dir, DIR_SECTORS, dirbuf);
    }
    redir_active=0; redir_buf_len=0;
}

/* ---------- GUI forward declarations for syscall_dispatch ---------- */
static char g_mbar_msg[40] = "Auroral 3.0";  /* defined here, initialized later */
/* GUI syscall hook – set to non-null when GUI is active */
typedef uint32_t (*gui_syscall_fn_t)(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);
static gui_syscall_fn_t gui_syscall_hook = 0;

/* ---------- syscall_dispatch 구현 (FS 함수 이후) ---------- */
/*
 *  확장된 syscall 번호:
 *   0  = sys_putc(char ebx)
 *   1  = sys_puts(const char* ebx)
 *   2  = sys_getc()                     → eax=char
 *   3  = sys_exit()
 *   4  = sys_yield()
 *   5  = sys_getpid()                   → eax=pid
 *   6  = sys_fs_find(dir ebx, name ecx, dirent* edx) → eax=1/0
 *   7  = sys_fs_load(dirent* ebx, void* ecx)          → eax=size
 *   8  = sys_fs_write(name ebx, void* ecx, size edx)  → eax=lba
 *   9  = sys_date(rtc_time_t* ebx)
 *  10  = sys_env_get(key ebx, val_buf ecx)             → eax=0/1
 *  11  = sys_env_set(key ebx, val ecx)
 *  12  = sys_gfx_px(x ebx, y ecx, color edx)          – 그래픽 픽셀 쓰기
 *  13  = sys_gfx_rect(x ebx, y ecx, [w,h,c] *edx)     – 사각형
 *  14  = sys_mouse_xy(int* x_out ebx, int* y_out ecx)  – 마우스 좌표
 *  15  = sys_mouse_btn()                               → eax=btn
 *  16  = sys_notify(const char* msg ebx)               – 메뉴바 메시지
 *  17  = sys_win_open(type ebx)                        → eax=wid (-1=실패)
 *  18  = sys_win_close(wid ebx)
 *  19  = sys_fs_mkdir(name ebx)                        → eax=1/0
 *  20  = sys_fs_rm(name ebx)                           → eax=1/0
 *  21  = sys_fs_ls(buf ebx, maxn ecx)                  → eax=count
 *  22  = sys_getkey_poll()                             → eax=key (0=없음)
 *  23  = sys_reboot()
 *  24  = sys_halt()
 */
static uint32_t syscall_dispatch(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx){
    switch(eax){
        case 0: putc((char)(uint8_t)ebx); return 0;
        case 1: puts((const char*)ebx);   return 0;
        case 2: return (uint32_t)(uint8_t)getch_raw_u8_blocking();
        case 3: task_exit(); return 0;
        case 4: yield(); return 0;
        case 5: return (uint32_t)getpid();
        case 6: return (uint32_t)fs_find((uint32_t)ebx, (const char*)ecx, (dirent_t*)edx);
        case 7: return (uint32_t)fs_load_file((const dirent_t*)ebx, (void*)ecx);
        case 8: {
            const char *nm=(const char*)ebx;
            void *buf=(void*)ecx;
            uint32_t sz=edx;
            dirent_t dummy;
            if(strlen(nm)>23||fs_find(cur_dir,nm,&dummy)) return 0;
            uint32_t sc=(sz+511)/512;
            uint32_t lba=get_free_lba(sc);
            ata_write28(lba,(uint8_t)sc,buf);
            fs_add_entry(cur_dir,nm,lba,sz);
            return lba;
        }
        case 9: {
            rtc_time_t *tp = (rtc_time_t*)ebx;
            if(tp) *tp = rtc_now();
            return 0;
        }
        case 10: {
            const char *v = env_get((const char*)ebx);
            if(!v) return 0;
            strncpy((char*)ecx, v, MAX_ENV_VAL-1);
            ((char*)ecx)[MAX_ENV_VAL-1]=0;
            return 1;
        }
        case 11: env_set((const char*)ebx, (const char*)ecx); return 0;
        /* FS 확장 syscall */
        case 19: {
            const char *nm=(const char*)ebx;
            if(!nm||strlen(nm)>23) return 0;
            dirent_t dummy;
            if(fs_find(cur_dir,nm,&dummy)) return 0;
            uint32_t lba=get_free_lba(DIR_SECTORS);
            memset(dirbuf,0,DIR_BYTES);
            ata_write28(lba,DIR_SECTORS,dirbuf);
            return (uint32_t)fs_add_entry(cur_dir,nm,lba,0);
        }
        case 20: {
            const char *nm=(const char*)ebx;
            dirent_t e;
            if(!fs_find(cur_dir,nm,&e)||e.size==0) return 0;
            ata_read28(cur_dir,DIR_SECTORS,dirbuf);
            for(int i=0;i<DIR_ENTRIES;i++){
                dirent_t*de=(dirent_t*)(dirbuf+i*sizeof(dirent_t));
                if(!strcmp(de->name,nm)){ fs_free_extent(de->lba,(de->size+511)/512); memset(de,0,sizeof(dirent_t)); ata_write28(cur_dir,DIR_SECTORS,dirbuf); return 1; }
            }
            return 0;
        }
        case 21: {
            char (*buf)[24]=(char(*)[24])ebx;
            uint32_t maxn=ecx;
            ata_read28(cur_dir,DIR_SECTORS,dirbuf);
            uint32_t cnt=0;
            for(int i=0;i<DIR_ENTRIES&&cnt<maxn;i++){
                dirent_t*e=(dirent_t*)(dirbuf+i*sizeof(dirent_t));
                if(!e->name[0]) continue;
                strncpy(buf[cnt],e->name,23); buf[cnt][23]=0;
                cnt++;
            }
            return cnt;
        }
        case 22: return (uint32_t)getch_raw_u8_poll();
        case 23: outb(0x64,0xFE); return 0;
        case 24: __asm__ volatile("cli\nhlt\n"); return 0;
        /* 25 = http_get(url ebx, out_buf ecx, out_max edx) → eax=len (fetch는 커널 유지) */
        case 25: return (uint32_t)http_get((const char*)ebx, (char*)ecx, (int)edx);
        /* 26 = http_post: ebx=&{const char*url; const char*body; int blen; char*out; int omax;} */
        case 26: {
            struct httpreq { const char *url; const char *body; int blen; char *out; int omax; };
            struct httpreq *rq = (struct httpreq*)ebx;
            g_post_body = rq->body; g_post_len = rq->blen;   /* POST 무장 → http_get이 소비 */
            return (uint32_t)http_get(rq->url, rq->out, rq->omax);
        }
        /* 그래픽/창/마우스/알림 syscall → GUI hook으로 위임 */
        default:
            if(gui_syscall_hook) return gui_syscall_hook(eax,ebx,ecx,edx);
            return (uint32_t)-1;
    }
}

/* ---------- stat 명령 ---------- */
static void cmd_stat(const char *name){
    dirent_t e;
    if(!fs_find(cur_dir, name, &e)){ puts("no such file\n"); return; }
    puts("Name : "); puts(e.name); putc('\n');
    puts("Type : "); puts(e.size==0?"directory\n":"file\n");
    puts("LBA  : "); print_uint(e.lba); putc('\n');
    puts("Size : "); print_uint(e.size); puts(" bytes\n");
    uint32_t sc=(e.size==0)?DIR_SECTORS:(e.size+511)/512;
    puts("Secs : "); print_uint(sc); putc('\n');
}

/* ---------- wc 명령 ---------- */
static void cmd_wc(const char *name){
    dirent_t e;
    if(!fs_find(cur_dir, name, &e)){ puts("no file\n"); return; }
    if(e.size==0){ puts("is dir\n"); return; }
    uint8_t *buf=(void*)0x200000;
    fs_load_file(&e, buf);
    uint32_t lines=0, words=0, bytes=e.size;
    int in_word=0;
    for(uint32_t i=0;i<bytes;i++){
        uint8_t c=buf[i];
        if(c=='\n') lines++;
        if(c==' '||c=='\n'||c=='\t'||c=='\r'){ if(in_word){words++;in_word=0;} }
        else in_word=1;
    }
    if(in_word) words++;
    print_uint(lines); putc(' ');
    print_uint(words); putc(' ');
    print_uint(bytes); putc(' ');
    puts(name); putc('\n');
}

/* ---------- grep 명령 ---------- */
static void cmd_grep(const char *keyword, const char *fname){
    dirent_t e;
    if(!fs_find(cur_dir, fname, &e)){ puts("no file\n"); return; }
    if(e.size==0){ puts("is dir\n"); return; }
    uint8_t *buf=(void*)0x200000;
    fs_load_file(&e, buf);
    buf[e.size]=0;

    int klen=strlen(keyword);
    uint32_t line_start=0;
    uint32_t linenum=1;
    for(uint32_t i=0;i<=e.size;i++){
        if(buf[i]=='\n'||buf[i]==0){
            /* 현재 줄: [line_start, i) 안에 keyword 있는지 검사 */
            int found=0;
            for(uint32_t j=line_start; (int)(i-j)>=klen; j++){
                if(!strncmp((char*)(buf+j),keyword,klen)){found=1;break;}
            }
            if(found){
                print_uint(linenum); putc(':');
                for(uint32_t j=line_start;j<i;j++) putc((char)buf[j]);
                putc('\n');
            }
            line_start=i+1;
            linenum++;
        }
    }
}

/* ---------- 텍스트 에디터 (edit) ---------- */
#define EDIT_LINES  20
#define EDIT_LINE_W 78
static char edit_lines[EDIT_LINES][EDIT_LINE_W+1];
static int  edit_linecount;
static int  edit_cx, edit_cy;  /* 커서: 열, 행 */
static char edit_filename[24];

/* TUI helper functions (used by edit command and text fallback) */
static void tui_putch(int r, int c, char ch, uint8_t attr){
    if(r<0||r>=ROWS||c<0||c>=COLS) return;
    VGA[r*COLS+c]=((uint16_t)attr<<8)|(uint8_t)ch;
}
static void tui_puts_at(int r, int c, const char*s, uint8_t attr){
    while(*s&&c<COLS) tui_putch(r,c++,*s++,attr);
}
static void tui_fill(int r, int c0, int c1, char ch, uint8_t attr){
    for(int c=c0;c<=c1&&c<COLS;c++) tui_putch(r,c,ch,attr);
}
static void tui_fill_rect(int r0,int c0,int r1,int c1,char ch,uint8_t attr){
    for(int r=r0;r<=r1;r++) tui_fill(r,c0,c1,ch,attr);
}
static void tui_box(int r0,int c0,int r1,int c1,uint8_t attr,uint8_t fill){
    tui_putch(r0,c0,'\xDA',attr); tui_putch(r0,c1,'\xBF',attr);
    tui_putch(r1,c0,'\xC0',attr); tui_putch(r1,c1,'\xD9',attr);
    for(int c=c0+1;c<c1;c++){tui_putch(r0,c,'\xC4',attr);tui_putch(r1,c,'\xC4',attr);}
    for(int r=r0+1;r<r1;r++){tui_putch(r,c0,'\xB3',attr);tui_putch(r,c1,'\xB3',attr);}
    for(int r=r0+1;r<r1;r++) tui_fill(r,c0+1,c1-1,' ',fill);
}

/* 숫자→우측정렬 문자열 (width+1 바이트 필요) */
static void uint_to_str(uint32_t v, char *buf, int width){
    char tmp[12]; int n=0;
    if(!v){tmp[n++]='0';}
    else{while(v){tmp[n++]='0'+(char)(v%10);v/=10;}}
    for(int i=0;i<width;i++) buf[i]=' ';
    for(int i=0;i<n&&i<width;i++) buf[width-1-i]=tmp[i];
    buf[width]=0;
}

/* =========================================================
 *  폰트 에디터 (CLI 명령: fontedit)
 *  사용법:
 *    fontedit cho <0-18>   → 초성 N번 편집
 *    fontedit jung <0-20>  → 중성 N번 편집
 *    fontedit jong <0-18>  → 받침 N번 편집
 *    fontedit reset        → 기본 폰트 복원
 *    fontedit show cho <N> → 현재 비트맵 표시
 *
 *  에디터 안에서:
 *    WASD / 화살표 → 커서 이동
 *    Space         → 픽셀 토글
 *    X             → 현재 행 전체 클리어
 *    Z             → 전체 클리어
 *    S             → 저장 후 종료
 *    Q             → 저장없이 종료
 * ========================================================= */
static const char *cho_names[19]  = {"ㄱ","ㄲ","ㄴ","ㄷ","ㄸ","ㄹ","ㅁ","ㅂ","ㅃ","ㅅ","ㅆ","ㅇ","ㅈ","ㅉ","ㅊ","ㅋ","ㅌ","ㅍ","ㅎ"};
static const char *jung_names[21] = {"ㅏ","ㅐ","ㅑ","ㅒ","ㅓ","ㅔ","ㅕ","ㅖ","ㅗ","ㅘ","ㅙ","ㅚ","ㅛ","ㅜ","ㅝ","ㅞ","ㅟ","ㅠ","ㅡ","ㅢ","ㅣ"};
static const char *jong_names2[19]= {"ㄱ","ㄲ","ㄴ","ㄷ","ㄸ","ㄹ","ㅁ","ㅂ","ㅃ","ㅅ","ㅆ","ㅇ","ㅈ","ㅉ","ㅊ","ㅋ","ㅌ","ㅍ","ㅎ"};

/* 비트맵을 ASCII로 표시 (16×16) */
static void fontedit_show(const bmp16_t *b){
    puts("  0123456789ABCDEF\n");
    for(int y=0;y<16;y++){
        char buf[4]; buf[0]=' ';
        int h=y; buf[1]=(char)(h<10?'0'+h:'A'+h-10); buf[2]=' '; buf[3]=0;
        puts(buf);
        for(int x=0;x<16;x++){
            uint16_t bit=(uint16_t)(0x8000u>>x);
            putc((b->row[y]&bit)?'#':'.');
        }
        putc('\n');
    }
}

/* VGA 전체 지우기 (에디터 화면) */
static void fontedit_clear_screen(void){
    for(int r=0;r<25;r++) tui_fill(r,0,80,' ',0x17);
    row=0; col=0;
}

/* 에디터 UI: 16×16 픽셀 확대 표시 (각 픽셀 → 2문자) */
#define FE_ROW0  2
#define FE_COL0  4
static void fontedit_draw_grid(const bmp16_t *b, int cx, int cy){
    for(int y=0;y<16;y++){
        for(int x=0;x<16;x++){
            uint16_t bit=(uint16_t)(0x8000u>>x);
            int on=(b->row[y]&bit)?1:0;
            uint8_t attr = on ? 0xF0u : 0x17u;
            if(x==cx&&y==cy) attr = 0x4Fu;  /* 커서: 빨강 */
            VGA[(FE_ROW0+y)*80+(FE_COL0+x*2)]   = (uint16_t)((attr<<8)|' ');
            VGA[(FE_ROW0+y)*80+(FE_COL0+x*2+1)] = (uint16_t)((attr<<8)|' ');
        }
    }
    tui_puts_at(0,0,"Hangul Font Editor  WASD/Arrows=move SPACE=toggle x=clrrow z=clrall Enter=save q=quit",0x0B);
    tui_puts_at(1,40,"Preview:",0x0A);
    char info[32];
    info[0]='x'; info[1]=(char)('0'+cx/10); info[2]=(char)('0'+cx%10);
    info[3]=' '; info[4]='y'; info[5]=(char)('0'+cy/10); info[6]=(char)('0'+cy%10);
    info[7]=0;
    tui_puts_at(1,0,info,0x0E);
}

static void fontedit_run(bmp16_t *target, const char *label){
    bmp16_t working; working = *target;
    int cx=0, cy=0;

    fontedit_clear_screen();
    tui_puts_at(0,50,label,0x0E);
    fontedit_draw_grid(&working,cx,cy);

    for(;;){
        uint8_t k=getch_raw_u8_blocking();
        /* ★ 저장은 Enter 또는 Ctrl+S로 처리 (s/S 이동키와 충돌 방지) */
        if(k=='\n'||k=='\r'||k==19){  /* Enter or Ctrl+S */
            *target = working;
            font_save_disk();   /* ★ ATA에 영구 저장 */
            init_jamo_fonts();
            fontedit_clear_screen();
            puts("Saved to disk!\n");
            return;
        }
        if(k=='q'||k=='Q'){
            fontedit_clear_screen();
            puts("Cancelled.\n");
            return;
        }
        /* 이동 (WASD + 방향키) */
        if(k==KEY_UP    || k=='w'){ if(cy>0)  cy--; }
        if(k==KEY_DOWN  || k=='s'){ if(cy<15) cy++; }
        if(k==KEY_LEFT  || k=='a'){ if(cx>0)  cx--; }
        if(k==KEY_RIGHT || k=='d'){ if(cx<15) cx++; }
        /* 편집 */
        if(k==' '){ working.row[cy] ^= (uint16_t)(0x8000u>>cx); }
        if(k=='x') working.row[cy]=0;
        if(k=='z'){ for(int i=0;i<16;i++) working.row[i]=0; }
        fontedit_draw_grid(&working,cx,cy);
    }
}

/* =========================================================
 *  fontload – LBA 90에 dd로 쓴 .kfont를 직접 로드
 *
 *  사용법:
 *    fontload          → LBA 90에서 바로 읽기 (dd로 넣은 경우)
 *    fontload <file>   → 파일시스템의 파일에서 읽기
 * ========================================================= */
static void cmd_fontload(const char *fname){
    if(!fname || !fname[0]){
        /* 인자 없으면 LBA 90 직접 읽기 (font_load_disk와 동일) */
        static uint8_t fbuf[4*512];
        ata_read28(FONT_SAVE_LBA, 4, fbuf);
        uint32_t magic = *(uint32_t*)fbuf;
        if(magic != FONT_MAGIC){
            puts("fontload: LBA 90에 유효한 폰트 없음\n");
            return;
        }
        uint8_t *p = fbuf + 4;
        for(int i=0;i<19;i++){ for(int r=0;r<16;r++) font_cho[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); p+=32; }
        for(int i=0;i<21;i++){ for(int r=0;r<16;r++) font_jung[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); p+=32; }
        for(int i=0;i<19;i++){ for(int r=0;r<16;r++) font_jong[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); p+=32; }
        puts("fontload: OK (LBA 90)\n");
        return;
    }

    /* 인자 있으면 파일시스템에서 읽기 */
    dirent_t e;
    if(!fs_find(cur_dir, fname, &e)){ puts("fontload: file not found\n"); return; }
    if(e.size == 0){ puts("fontload: that's a directory\n"); return; }
    if(e.size < 1892){ puts("fontload: file too small\n"); return; }

    static uint8_t fbuf2[4*512];
    fs_load_file(&e, fbuf2);
    uint32_t magic = *(uint32_t*)fbuf2;
    if(magic != FONT_MAGIC){ puts("fontload: bad magic\n"); return; }

    uint8_t *p = fbuf2 + 4;
    for(int i=0;i<19;i++){ for(int r=0;r<16;r++) font_cho[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); p+=32; }
    for(int i=0;i<21;i++){ for(int r=0;r<16;r++) font_jung[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); p+=32; }
    for(int i=0;i<19;i++){ for(int r=0;r<16;r++) font_jong[i].row[r]=(uint16_t)((p[r*2]<<8)|p[r*2+1]); p+=32; }
    font_save_disk();
    puts("fontload: OK\n");
}

static void cmd_fontedit(const char *args){
    /* args: "cho N" | "jung N" | "jong N" | "reset" | "show cho N" */
    if(!args||!args[0]){ puts("Usage: fontedit cho|jung|jong <N>\n       fontedit reset\n"); return; }

    if(!strncmp(args,"reset",5)){
        init_jamo_fonts();
        puts("Font reset to default.\n");
        return;
    }

    /* show 명령 */
    if(!strncmp(args,"show ",5)){
        args+=5;
        bmp16_t *tgt=0;
        if(!strncmp(args,"cho ",4)){ int n=(int)(args[4]-'0'); if(n>=0&&n<19) tgt=&font_cho[n]; }
        else if(!strncmp(args,"jung ",5)){ int n=(int)(args[5]-'0'); if(n>=0&&n<21) tgt=&font_jung[n]; }
        else if(!strncmp(args,"jong ",5)){ int n=(int)(args[5]-'0'); if(n>=0&&n<19) tgt=&font_jong[n]; }
        if(tgt){ fontedit_show(tgt); return; }
        puts("Usage: fontedit show cho|jung|jong <N>\n"); return;
    }

    /* 에디터 모드 */
    bmp16_t *tgt=0;
    const char *label="";
    if(!strncmp(args,"cho ",4)){
        int n=0; const char*p=args+4; while(*p==' ')p++;
        while(*p>='0'&&*p<='9'){ n=n*10+(*p-'0'); p++; }
        if(n>=0&&n<19){ tgt=&font_cho[n]; label=cho_names[n]; }
    } else if(!strncmp(args,"jung ",5)){
        int n=0; const char*p=args+5; while(*p==' ')p++;
        while(*p>='0'&&*p<='9'){ n=n*10+(*p-'0'); p++; }
        if(n>=0&&n<21){ tgt=&font_jung[n]; label=jung_names[n]; }
    } else if(!strncmp(args,"jong ",5)){
        int n=0; const char*p=args+5; while(*p==' ')p++;
        while(*p>='0'&&*p<='9'){ n=n*10+(*p-'0'); p++; }
        if(n>=0&&n<19){ tgt=&font_jong[n]; label=jong_names2[n]; }
    }
    if(!tgt){ puts("Unknown type. Use cho/jung/jong\n"); return; }
    fontedit_run(tgt, label);
}

static void edit_redraw(void){
    /* 상단 바 */
    tui_fill(0, 0, COLS-1, ' ', 0x70);
    char hdr[80];
    int hi=0;
    const char *fn=edit_filename;
    while(*fn&&hi<60) hdr[hi++]=*fn++;
    const char *hint=" | Ctrl+S=Save  Ctrl+Q=Quit  Arrows=Move";
    while(*hint&&hi<79) hdr[hi++]=*hint++;
    hdr[hi]=0;
    tui_puts_at(0, 0, hdr, 0x70);

    /* 편집 영역 */
    for(int r=0;r<ROWS-2;r++){
        tui_fill(r+1, 0, COLS-1, ' ', 0x17);
        if(r < edit_linecount)
            tui_puts_at(r+1, 0, edit_lines[r], 0x17);
    }
    /* 커서 표시 */
    int vr=edit_cy+1, vc=edit_cx;
    if(vr<ROWS-1&&vc<COLS){
        uint16_t ch=VGA[vr*COLS+vc];
        VGA[vr*COLS+vc]=(uint16_t)((0x70<<8)|(ch&0xFF));
    }
    /* 하단 상태 */
    tui_fill(ROWS-1, 0, COLS-1, ' ', 0x70);
    char stat[40]; int si=0;
    stat[si++]='L'; stat[si++]='n'; stat[si++]=':'; stat[si++]=' ';
    uint32_t v=edit_cy+1; if(!v){stat[si++]='0';} else { char tmp[6];int ti=0;while(v){tmp[ti++]='0'+v%10;v/=10;}while(ti--) stat[si++]=tmp[ti+1];}
    stat[si++]=' '; stat[si++]='C'; stat[si++]='o'; stat[si++]='l'; stat[si++]=':'; stat[si++]=' ';
    v=edit_cx+1; {char tmp[6];int ti=0;if(!v){stat[si++]='0';}else{while(v){tmp[ti++]='0'+v%10;v/=10;}while(ti--) stat[si++]=tmp[ti+1];}}
    stat[si]=0;
    tui_puts_at(ROWS-1, 0, stat, 0x70);
}

static void edit_save(void){
    uint8_t *wb=(uint8_t*)0x300000;
    uint32_t wlen=0;
    for(int r=0;r<edit_linecount;r++){
        int ll=strlen(edit_lines[r]);
        for(int j=0;j<ll;j++) wb[wlen++]=(uint8_t)edit_lines[r][j];
        wb[wlen++]='\n';
    }
    dirent_t dummy;
    int exists=fs_find(cur_dir,edit_filename,&dummy);
    if(exists){
        /* 삭제 후 재생성 – 기존 확장 회수 */
        fs_free_extent(dummy.lba,(dummy.size+511)/512);
        ata_read28(cur_dir,DIR_SECTORS,dirbuf);
        for(int idx=0;idx<DIR_ENTRIES;++idx){
            dirent_t *de=(dirent_t*)(dirbuf+idx*sizeof(dirent_t));
            if(!strcmp(de->name,edit_filename)){ memset(de,0,sizeof(dirent_t)); break; }
        }
        ata_write28(cur_dir,DIR_SECTORS,dirbuf);
    }
    uint32_t sc=(wlen+511)/512; if(!sc)sc=1;
    uint32_t lba=get_free_lba(sc);
    ata_write28(lba,sc,wb);
    fs_add_entry(cur_dir,edit_filename,lba,wlen);
}

static void cmd_edit(const char *fname){
    strncpy(edit_filename,fname,23); edit_filename[23]=0;
    edit_cx=0; edit_cy=0; edit_linecount=0;
    memset(edit_lines,0,sizeof(edit_lines));

    /* 기존 파일 로드 */
    dirent_t e;
    if(fs_find(cur_dir,fname,&e)&&e.size>0){
        uint8_t *buf=(uint8_t*)0x200000;
        fs_load_file(&e,buf);
        buf[e.size]=0;
        uint32_t ls=0;
        for(uint32_t i=0;i<=e.size&&edit_linecount<EDIT_LINES;i++){
            if(buf[i]=='\n'||buf[i]==0){
                int ll=(int)(i-ls); if(ll>EDIT_LINE_W)ll=EDIT_LINE_W;
                strncpy(edit_lines[edit_linecount],(char*)(buf+ls),ll);
                edit_lines[edit_linecount][ll]=0;
                edit_linecount++;
                ls=i+1;
            }
        }
    }
    if(edit_linecount==0){ edit_lines[0][0]=0; edit_linecount=1; }

    /* 전체 화면 클리어 */
    for(int r=0;r<ROWS;r++) tui_fill(r,0,COLS-1,' ',0x17);
    edit_redraw();

    /* 편집 루프 */
    for(;;){
        uint8_t k=getch_raw_u8_blocking();
        if(k==KEY_CTRL_C||k==KEY_CTRL_E){ /* Ctrl+Q 역할 – 저장 안 하고 종료 */
            break;
        }
        /* Ctrl+S (keycode 19) */
        if(k==19){ edit_save(); /* 저장 알림 */ tui_puts_at(ROWS-1,60,"  [Saved!]  ",0x2F); continue; }
        if(k==KEY_UP)   { if(edit_cy>0){ edit_cy--; int ll=strlen(edit_lines[edit_cy]); if(edit_cx>ll)edit_cx=ll; } }
        else if(k==KEY_DOWN) { if(edit_cy<edit_linecount-1){ edit_cy++; int ll=strlen(edit_lines[edit_cy]); if(edit_cx>ll)edit_cx=ll; } }
        else if(k==KEY_LEFT) { if(edit_cx>0)edit_cx--; else if(edit_cy>0){edit_cy--;edit_cx=strlen(edit_lines[edit_cy]);} }
        else if(k==KEY_RIGHT){ int ll=strlen(edit_lines[edit_cy]); if(edit_cx<ll)edit_cx++; else if(edit_cy<edit_linecount-1){edit_cy++;edit_cx=0;} }
        else if(k==KEY_HOME) { edit_cx=0; }
        else if(k==KEY_END)  { edit_cx=strlen(edit_lines[edit_cy]); }
        else if(k=='\n'||k=='\r'){
            /* 줄 삽입 */
            if(edit_linecount<EDIT_LINES){
                for(int r=edit_linecount;r>edit_cy+1;r--) strncpy(edit_lines[r],edit_lines[r-1],EDIT_LINE_W);
                /* 현재 줄의 커서 이후를 새 줄로 */
                strncpy(edit_lines[edit_cy+1], edit_lines[edit_cy]+edit_cx, EDIT_LINE_W);
                edit_lines[edit_cy+1][EDIT_LINE_W]=0;
                edit_lines[edit_cy][edit_cx]=0;
                edit_linecount++;
                edit_cy++; edit_cx=0;
            }
        }
        else if(k==8){  /* backspace */
            if(edit_cx>0){
                char *l=edit_lines[edit_cy]; int ll=strlen(l);
                for(int j=edit_cx-1;j<ll-1;j++) l[j]=l[j+1];
                l[ll-1]=0; edit_cx--;
            } else if(edit_cy>0){
                /* 이전 줄에 합치기 */
                int prevlen=strlen(edit_lines[edit_cy-1]);
                char *prev=edit_lines[edit_cy-1];
                char *cur=edit_lines[edit_cy];
                int curlen=strlen(cur);
                for(int j=0;j<curlen&&prevlen+j<EDIT_LINE_W;j++) prev[prevlen+j]=cur[j];
                prev[prevlen+curlen<EDIT_LINE_W?prevlen+curlen:EDIT_LINE_W]=0;
                edit_cx=prevlen;
                /* 줄 제거 */
                for(int r=edit_cy;r<edit_linecount-1;r++) strncpy(edit_lines[r],edit_lines[r+1],EDIT_LINE_W);
                edit_lines[edit_linecount-1][0]=0;
                edit_linecount--; edit_cy--;
            }
        }
        else if(k==KEY_DEL){
            char *l=edit_lines[edit_cy]; int ll=strlen(l);
            if(edit_cx<ll){
                for(int j=edit_cx;j<ll-1;j++) l[j]=l[j+1];
                l[ll-1]=0;
            }
        }
        else if(k>=32&&k<128){
            char *l=edit_lines[edit_cy]; int ll=strlen(l);
            if(ll<EDIT_LINE_W){
                for(int j=ll;j>edit_cx;j--) l[j]=l[j-1];
                l[edit_cx++]=(char)k;
                l[ll+1]=0;
            }
        }
        edit_redraw();
    }

    /* 화면 복원 */
    for(int r=0;r<ROWS;r++) tui_fill(r,0,COLS-1,' ',0x17);
    row=0; col=0;
}

/* ---------- nice 명령 ---------- */
static void cmd_nice(const char *args){
    /* 사용법: nice <pid> <priority 1-4> */
    uint32_t pid=0, prio=0;
    int i=0;
    while(args[i]==' ') i++;
    while(args[i]>='0'&&args[i]<='9') pid=pid*10+(uint32_t)(args[i++]-'0');
    while(args[i]==' ') i++;
    while(args[i]>='0'&&args[i]<='9') prio=prio*10+(uint32_t)(args[i++]-'0');
    if(pid>=MAX_TASKS||!tasks[pid].used){ puts("no such pid\n"); return; }
    if(prio<1||prio>4){ puts("priority must be 1-4\n"); return; }
    tasks[pid].priority=(int)prio;
    tasks[pid].timeslice=(int)prio;
    puts("priority set\n");
}

/* ---------- echo + 리다이렉션 파서 ---------- */
/* 줄에서 리다이렉션 기호(>, >>)를 찾아 분리
   반환: 0=없음, 1=>, 2=>>
   cmd_part → '>' 이전 부분 (수정), file_part → 파일명 */
static int parse_redirect(char *line_buf, char **file_out){
    int len=strlen(line_buf);
    *file_out=0;
    for(int i=0;i<len-1;i++){
        if(line_buf[i]=='>'&&line_buf[i+1]=='>'){
            line_buf[i]=0;
            char *f=line_buf+i+2;
            while(*f==' ') f++;
            *file_out=f;
            return 2;
        }
        if(line_buf[i]=='>' && line_buf[i+1]!='='){
            line_buf[i]=0;
            char *f=line_buf+i+1;
            while(*f==' ') f++;
            *file_out=f;
            return 1;
        }
    }
    return 0;
}

/* 파이프('|')로 두 명령 분리
   반환: 1=파이프 있음, 0=없음
   left_cmd와 right_cmd로 분리 (라인 수정) */
static int parse_pipe(char *line_buf, char **right_out){
    *right_out=0;
    for(int i=0;line_buf[i];i++){
        if(line_buf[i]=='|'){
            line_buf[i]=0;
            char *r=line_buf+i+1;
            while(*r==' ') r++;
            *right_out=r;
            return 1;
        }
    }
    return 0;
}

/* echo 출력 (리다이렉션 고려) */
static void cmd_echo_to(const char *text, int to_redir){
    if(to_redir){
        /* redir_buf에 씀 */
        for(const char *p=text;*p;p++){
            if(redir_buf_len<REDIR_BUF_SIZE-1)
                redir_buf[redir_buf_len++]=*p;
        }
        if(redir_buf_len<REDIR_BUF_SIZE-1)
            redir_buf[redir_buf_len++]='\n';
    } else if(pipe_active){
        pipe_puts(text); pipe_putc('\n');
    } else {
        puts(text); putc('\n');
    }
}

/* 파이프 오른쪽 명령 실행 (grep 지원) */
static void exec_pipe_right(const char *cmd){
    /* trim leading spaces */
    while(*cmd==' ') cmd++;
    if(!strncmp(cmd,"grep ",5)){
        const char *kw=cmd+5;
        /* keyword는 파이프 버퍼에서 검색 */
        int klen=strlen(kw);
        pipe_buf[pipe_buf_len]=0;
        uint32_t ls=0;
        for(int i=0;i<pipe_buf_len+1;i++){
            char c=pipe_buf[i];
            if(c=='\n'||c==0){
                /* search keyword in [ls,i) */
                int found=0;
                for(int j=ls; (i-j)>=klen; j++){
                    if(!strncmp(pipe_buf+j,kw,klen)){found=1;break;}
                }
                if(found){
                    for(int j=ls;j<i;j++) putc(pipe_buf[j]);
                    putc('\n');
                }
                ls=(uint32_t)(i+1);
            }
        }
    } else if(!strncmp(cmd,"wc",2)){
        int lines=0,words=0,bytes=pipe_buf_len; int iw=0;
        for(int i=0;i<pipe_buf_len;i++){
            char c=pipe_buf[i];
            if(c=='\n')lines++;
            if(c==' '||c=='\n'||c=='\t'){if(iw){words++;iw=0;}}else iw=1;
        }
        if(iw)words++;
        print_uint((uint32_t)lines); putc(' '); print_uint((uint32_t)words); putc(' '); print_uint((uint32_t)bytes); putc('\n');
    } else {
        /* 모르는 명령: 그냥 파이프 내용 출력 */
        pipe_buf[pipe_buf_len]=0;
        puts(pipe_buf);
    }
}

/* ---------- CLI commands ---------- */
static void cmd_ls(void){
    ata_read28(cur_dir, DIR_SECTORS, dirbuf);
    puts("Name                LBA   Size  T\n");
    for(int idx=0; idx<DIR_ENTRIES; ++idx){
        dirent_t* e = (dirent_t*)(dirbuf + idx*sizeof(dirent_t));
        if(!e->name[0]) continue; /* ★ hole skip */

        puts(e->name);
        for(int i=strlen(e->name); i<20; ++i) putc(' ');
        print_uint(e->lba); putc(' ');
        print_uint(e->size); putc(' ');
        putc(e->size==0 ? 'D' : 'F');
        putc('\n');
    }
}

static void file_write_or_append(const char*fn,int append){
    dirent_t e;uint32_t len=0,lba=0;uint8_t*buf=(void*)0x300000;
    if(append){
        if(!fs_find(cur_dir,fn,&e)){puts("no such file\n");return;}
        if(e.size==0){puts("is directory\n");return;}
        len=e.size;fs_load_file(&e,buf);lba=e.lba;
    }else{
        if(strlen(fn)>23||fs_find(cur_dir,fn,&e)){puts("exists or name too long\n");return;}
    }
    puts("Enter text (single '.' line ends)\n");
    for(;;){
        char t[100];
        ime_hangul = 1;
        getline(t,100);
        if(!strcmp(t,"."))break;
        int l=strlen(t);
        memcpy(buf+len,t,l);
        len+=l;
        buf[len++]='\n';
    }
    uint32_t sc_old= append ? (e.size+511)/512 : 0;
    uint32_t sc_new=(len+511)/512;
    if(append && sc_new>sc_old){ fs_free_extent(e.lba,sc_old); lba=get_free_lba(sc_new); } /* ★ 기존 확장 회수 후 재할당 */
    else if(!append){ lba=get_free_lba(sc_new); }
    memset(buf+len,0,sc_new*512-len);
    ata_write28(lba,sc_new,buf);
    if(append){e.lba=lba;e.size=len;}
    if(!append) fs_add_entry(cur_dir,fn,lba,len);
    else{
        ata_read28(cur_dir,1,sec);
        for(int o=0;o<512;o+=sizeof(dirent_t)){
            dirent_t*de=(dirent_t*)(sec+o);
            if(!strcmp(de->name,fn)){*de=e;break;}
        }
        fs_write_dir(cur_dir);
    }
    puts(append?"appended\n":"written\n");
    ime_hangul = 0;
}

static void cmd_mkdir(const char*dn){
    dirent_t dummy;

    if(strlen(dn) > 23 || fs_find(cur_dir, dn, &dummy)){
        puts("exists or name too long\n");
        return;
    }

    /* allocate DIR_SECTORS for the new directory */
    uint32_t lba = get_free_lba(DIR_SECTORS);

    /* clear directory area */
    memset(dirbuf, 0, DIR_BYTES);
    ata_write28(lba, DIR_SECTORS, dirbuf);

    /* add entry to current directory */
    if(fs_add_entry(cur_dir, dn, lba, 0)) puts("directory created\n");
    else puts("dir full\n");
}

static void cmd_rm(const char*name){
    dirent_t e;
    if(!fs_find(cur_dir, name, &e)){ puts("no such file\n"); return; }
    if(e.size==0){ puts("not a file\n"); return; }

    ata_read28(cur_dir, DIR_SECTORS, dirbuf);
    for(int idx=0; idx<DIR_ENTRIES; ++idx){
        dirent_t* de = (dirent_t*)(dirbuf + idx*sizeof(dirent_t));
        if(!de->name[0]) continue;
        if(!strcmp(de->name, name)){
            fs_free_extent(de->lba, (de->size+511)/512); /* ★ 공간 회수 */
            memset(de, 0, sizeof(dirent_t)); /* hole 만들기 */
            ata_write28(cur_dir, DIR_SECTORS, dirbuf);
            puts("file deleted\n");
            return;
        }
    }
    puts("internal err\n");
}

static void cmd_rmdir(const char*name){
    dirent_t e;
    if(!fs_find(cur_dir, name, &e)){ puts("no such dir\n"); return; }
    if(e.size!=0){ puts("not a directory\n"); return; }

    ata_read28(e.lba, DIR_SECTORS, dirbuf);
    for(int idx=0; idx<DIR_ENTRIES; ++idx){
        dirent_t* de = (dirent_t*)(dirbuf + idx*sizeof(dirent_t));
        if(de->name[0]){ puts("not empty\n"); return; }
    }

    ata_read28(cur_dir, DIR_SECTORS, dirbuf);
    for(int idx=0; idx<DIR_ENTRIES; ++idx){
        dirent_t* de = (dirent_t*)(dirbuf + idx*sizeof(dirent_t));
        if(!de->name[0]) continue;
        if(!strcmp(de->name, name)){
            fs_free_extent(de->lba, DIR_SECTORS); /* ★ 디렉터리 영역 회수 */
            memset(de,0,sizeof(dirent_t));
            ata_write28(cur_dir, DIR_SECTORS, dirbuf);
            puts("directory removed\n");
            return;
        }
    }
}


static void cmd_devs(void){
    puts("\n  OS/2025 Contributors:\n  ----------------------\n  - kdy010010  (Lead developer)\n  - modemaker\n  - tajo1243\n  - JK\n\n");
}

/* df: 디스크 사용/여유 현황 (rm 공간 회수 확인용) */
static void cmd_df(void){
    uint32_t used = next_free_lba - ROOT_LBA;           /* 할당 상한까지의 섹터 */
    uint32_t freed = fs_free_sectors();                  /* 재사용 대기 중인 섹터 */
    puts("Filesystem 2025FS\n");
    puts("  data start LBA : "); print_uint(ROOT_LBA); putc('\n');
    puts("  high-water LBA : "); print_uint(next_free_lba); putc('\n');
    puts("  used sectors   : "); print_uint(used);
    puts(" ("); print_uint(used*512/1024); puts(" KB)\n");
    puts("  reclaimed free : "); print_uint(freed);
    puts(" sectors in "); print_uint((uint32_t)fs_free_cnt); puts(" holes\n");
}

/* ── fetch 명령 ───────────────────────────────────────
 * fetch <url>         → 내용 출력
 * fetch <url> <file>  → 파일에 저장
 * ──────────────────────────────────────────────────── */
/* ═══════════════════════════════════════════════════════════
 *  내장 텍스트 웹 브라우저 (Lynx 스타일)
 *   - URL 입력/이동, HTML 파싱→텍스트 렌더
 *   - <script>/<style> 제거, 링크 하이라이트
 *   - 방향키로 링크 이동(↑↓) / 페이지 스크롤(←→), Enter로 열기
 *   - g=주소이동, b=뒤로, q=종료
 *  (http_get, tui_*, 방향키 KEY_* 재사용)
 * ═══════════════════════════════════════════════════════════ */

/* ── 한글 IME 전방선언 (정의는 GUI 쪽, 파일 뒤편) ── */
static int  gui_ime_on;
static int  gui_ime_L, gui_ime_V, gui_ime_T;
static void gui_ime_reset(void);
static int  gui_ime_flush(char *buf, int *len, int max);
static int  gui_ime_key(uint8_t k, char *buf, int *len, int max);

/* 코드포인트(음절 또는 단독자모)를 화면 (r,c)에 2칸으로 그린다.
 * 동적 폰트 슬롯에 글리프를 합성해 CP437 2글자로 출력. */
static uint32_t gcache_key[HGLYPH_PAIRS];
static uint8_t  gcache_used[HGLYPH_PAIRS];
static uint8_t  gcache_hand=0;
static int gcache_slot_for(uint32_t cp){
    for(int i=0;i<HGLYPH_PAIRS;i++) if(gcache_used[i]&&gcache_key[i]==cp) return i;
    int slot=gcache_hand++ % HGLYPH_PAIRS;
    gcache_used[slot]=1; gcache_key[slot]=cp;
    return slot;
}
static void wb_draw_hangul(int r, int c, uint32_t cp, uint8_t attr){
    uint16_t g16[16];
    if(cp>=0xAC00 && cp<=0xD7A3){
        int n=(int)cp-0xAC00;
        compose_hangul_16x16(g16, n/(21*28), (n/28)%21, n%28);
    } else if(cp>=0x3131 && cp<=0x3163){
        draw_standalone_jamo_16x16(cp, g16);   /* 단독 자모 ㄱ~ㅣ */
    } else {
        tui_putch(r,c,'?',attr); return;
    }
    int slot=gcache_slot_for(cp);
    uint8_t chL=(uint8_t)(HGLYPH_FIRST+slot*2), chR=(uint8_t)(chL+1);
    uint8_t gl[16], gr[16];
    split_16x16_to_8x16(g16,gl,gr);
    vga_write_glyph8x16(chL,gl);
    vga_write_glyph8x16(chR,gr);
    tui_putch(r,c,(char)chL,attr);
    tui_putch(r,c+1,(char)chR,attr);
}
/* 코드포인트가 한글(음절/단독자모)로 2칸 렌더 대상인지 */
static int wb_is_wide(uint32_t cp){ return (cp>=0xAC00&&cp<=0xD7A3)||(cp>=0x3131&&cp<=0x3163); }
/* UTF-8 문자열을 화면 (r,c0)에 width칸에 그린다(한글 지원). tail=1이면 끝부분 우선. */
static void wb_draw_utf8(int r,int c0,int width,const char *s,uint8_t attr,int tail){
    /* 먼저 코드포인트 배열로 분해 */
    uint16_t cps[128]; int nc=0;
    const uint8_t *p=(const uint8_t*)s;
    while(*p && nc<128){ uint32_t cp; int n=utf8_decode_one(p,&cp); if(n<=0){p++;continue;} cps[nc++]=(cp<=0xFFFF?(uint16_t)cp:'?'); p+=n; }
    /* 표시 폭 계산 */
    int total=0; for(int i=0;i<nc;i++) total += wb_is_wide(cps[i])?2:1;
    int start=0;
    if(tail && total>width){ int used=0; for(start=0;start<nc;start++){ int w=wb_is_wide(cps[start])?2:1; if(total-used<=width) break; used+=w; } }
    int col=0;
    for(int i=start;i<nc && col<width;i++){
        uint16_t cp=cps[i];
        if(wb_is_wide(cp)){ if(col+2>width) break; wb_draw_hangul(r,c0+col,cp,attr); col+=2; }
        else { tui_putch(r,c0+col,(char)(cp<128?cp:'?'),attr); col++; }
    }
    for(;col<width;col++) tui_putch(r,c0+col,' ',attr);
}

/* ═══════════════════════════════════════════════════════════
 *  내장 텍스트 웹 브라우저
 * ═══════════════════════════════════════════════════════════ */
#define WB_HTMLMAX   65534
#define WB_MAXLINES  600
#define WB_LINEW     78
#define WB_MAXANCHOR 200
#define WB_HREFLEN   180
#define WB_MAXSPAN   500
#define WB_WORDMAX   256
#define WB_VIS       (ROWS-2)   /* 본문 표시 줄 수 (헤더1 + 상태1 제외) */

static char wb_html[WB_HTMLMAX+2];
static uint16_t wb_cell[WB_MAXLINES][WB_LINEW];   /* 셀당 코드포인트(0=끝, 0xFFFF=한글 연속칸) */
static uint8_t  wb_attr[WB_MAXLINES][WB_LINEW];   /* 셀당 색상 */
static int  wb_nlines;
/* 포커스 가능한 항목(링크/폼 요소) 통합 모델 */
#define WB_MAXITEM  WB_MAXANCHOR
#define WB_IT_LINK   0
#define WB_IT_TEXT   1
#define WB_IT_AREA   2
#define WB_IT_SUBMIT 3
#define WB_IT_CHECK  4
#define WB_IT_RADIO  5
#define WB_IT_HIDDEN 6
struct wb_item_s {
    uint8_t type;
    int     form;                 /* 소속 폼 index, 없으면 -1 */
    char    href[WB_HREFLEN];     /* LINK: 대상 URL */
    char    name[32];             /* 폼필드 name */
    char    value[80];            /* 값(텍스트는 편집됨, UTF-8) */
    uint8_t checked;              /* check/radio 상태 */
    uint8_t width;                /* 텍스트필드 표시 폭(칸) */
};
static struct wb_item_s wb_item[WB_MAXITEM];
static int  wb_nanchor;           /* = 항목 개수 */

#define WB_MAXFORM 32
struct wb_form_s { char action[WB_HREFLEN]; uint8_t method; }; /* method 0=GET 1=POST */
static struct wb_form_s wb_form[WB_MAXFORM];
static int wb_nform;
static int wb_curform;            /* 렌더 중 현재 폼 index */
static struct { int line, col, len, anchor; } wb_span[WB_MAXSPAN];
static int  wb_nspan;
static char wb_url[256];
static char wb_title[80];

/* 렌더 중 상태(현재 줄) */
static uint16_t wb_curc[WB_LINEW+2];
static uint8_t  wb_cura[WB_LINEW+2];
static int      wb_curlen;         /* 현재 줄의 셀 수 */
static int      wb_cur_anchor;

/* 강조 상태 (b/i/heading/code) → 현재 글자 색상 */
static int wb_bold, wb_italic, wb_heading, wb_code, wb_override_attr;
static uint8_t wb_style_now(void){
    if(wb_override_attr>=0) return (uint8_t)wb_override_attr;
    if(wb_heading)              return 0x0E;   /* 노랑: 제목 */
    if(wb_bold>0 && wb_italic>0)return 0x0D;   /* 자홍: 굵은기울임 */
    if(wb_bold>0)               return 0x0F;   /* 밝은흰: 굵게 */
    if(wb_italic>0)             return 0x0B;   /* 하늘: 기울임 */
    if(wb_code>0)               return 0x0A;   /* 초록: 코드 */
    return 0x07;                                /* 기본 회색 */
}
static int wb_cpw(uint16_t cp){ return wb_is_wide(cp)?2:1; }

static void wb_flush_line(void){
    if(wb_nlines>=WB_MAXLINES){ wb_curlen=0; return; }
    for(int i=0;i<wb_curlen;i++){ wb_cell[wb_nlines][i]=wb_curc[i]; wb_attr[wb_nlines][i]=wb_cura[i]; }
    if(wb_curlen<WB_LINEW) wb_cell[wb_nlines][wb_curlen]=0;  /* 종료 표시 */
    wb_nlines++;
    wb_curlen=0;
}
static void wb_blank_line(void){
    if(wb_curlen>0){ wb_flush_line(); }
    if(wb_nlines>0 && wb_cell[wb_nlines-1][0]==0) return;
    wb_flush_line();
}
static void wb_cell_push(uint16_t cp, uint8_t at){
    if(wb_curlen<WB_LINEW){ wb_curc[wb_curlen]=cp; wb_cura[wb_curlen]=at; wb_curlen++; }
}
/* 코드포인트 배열 하나(단어)를 방출 */
static void wb_emit_cps(const uint16_t *cps, int n, uint8_t at){
    if(n<=0) return;
    int w=0; for(int i=0;i<n;i++) w+=wb_cpw(cps[i]);
    if(w>WB_LINEW) w=WB_LINEW;
    if(wb_curlen>0 && wb_curlen+1+w>WB_LINEW) wb_flush_line();
    if(wb_curlen>0) wb_cell_push(' ',at);
    int colstart=wb_curlen;
    for(int i=0;i<n;i++){
        int cw=wb_cpw(cps[i]);
        if(wb_curlen+cw>WB_LINEW) break;
        wb_cell_push(cps[i],at);
        if(cw==2) wb_cell_push(0xFFFF,at);
    }
    if(wb_cur_anchor>=0 && wb_nspan<WB_MAXSPAN){
        wb_span[wb_nspan].line=wb_nlines;
        wb_span[wb_nspan].col=colstart;
        wb_span[wb_nspan].len=wb_curlen-colstart;
        wb_span[wb_nspan].anchor=wb_cur_anchor;
        wb_nspan++;
    }
}
/* ASCII 문자열 방출(위젯/[img] 등) */
static void wb_emit_ascii(const char *s, int n, uint8_t at){
    uint16_t tmp[128]; int m=0;
    for(int i=0;i<n && m<128;i++) tmp[m++]=(uint8_t)s[i];
    wb_emit_cps(tmp,m,at);
}
static void wb_emit_forced(const char *w, int wl, uint8_t attr){ wb_emit_ascii(w,wl,attr); }
/* 문자열 안전 복사 */
static void wb_scopy(char *dst, const char *src, int max){
    int k=0; for(;src[k]&&k<max-1;k++) dst[k]=src[k]; dst[k]=0;
}
/* 항목 위젯을 현재 위치에 방출(span에 항목 index 기록) */
static void wb_emit_widget(int idx, const char *box, int bl, uint8_t attr){
    int save=wb_cur_anchor; wb_cur_anchor=idx;
    wb_emit_ascii(box,bl,attr);
    wb_cur_anchor=save;
}

/* 태그에서 속성 값 추출: name="value" 또는 name=value */
static int wb_get_attr(const char *tag, const char *attr, char *out, int outmax){
    int al=strlen(attr);
    for(const char *p=tag; *p; p++){
        /* attr 이름 매칭 (앞이 공백/시작) */
        if((p==tag || p[-1]==' ') && strncmp(p,attr,al)==0){
            const char *q=p+al;
            while(*q==' ') q++;
            if(*q!='='){ continue; }
            q++;
            while(*q==' ') q++;
            char quote=0;
            if(*q=='"'||*q=='\''){ quote=*q; q++; }
            int oi=0;
            while(*q && oi<outmax-1){
                if(quote && *q==quote) break;
                if(!quote && (*q==' '||*q=='>')) break;
                out[oi++]=*q++;
            }
            out[oi]=0;
            return 1;
        }
    }
    out[0]=0;
    return 0;
}

/* 불리언 속성 존재 여부 (checked, selected 등) */
static int wb_has_attr(const char *tag, const char *attr){
    int al=strlen(attr);
    for(const char *p=tag; *p; p++)
        if((p==tag || p[-1]==' ') && strncmp(p,attr,al)==0){
            char nx=p[al];
            if(nx==0||nx==' '||nx=='='||nx=='>') return 1;
        }
    return 0;
}

/* 새 항목 생성 → index 반환(-1=꽉참) */
static int wb_new_item(uint8_t type){
    if(wb_nanchor>=WB_MAXITEM) return -1;
    int idx=wb_nanchor++;
    struct wb_item_s *it=&wb_item[idx];
    it->type=type; it->form=wb_curform;
    it->href[0]=0; it->name[0]=0; it->value[0]=0; it->checked=0; it->width=0;
    return idx;
}
static int wb_entity(const char *s, char *outc){
    /* s[0]=='&' 가정 */
    int i=1; char name[10]; int ni=0;
    if(s[1]=='#'){
        int val=0; i=2;
        if(s[i]=='x'||s[i]=='X'){ i++; while(((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F'))){ char c=s[i]; int d=(c<='9')?c-'0':((c|0x20)-'a'+10); val=val*16+d; i++; } }
        else { while(s[i]>='0'&&s[i]<='9'){ val=val*10+(s[i]-'0'); i++; } }
        if(s[i]==';') i++;
        *outc = (val>=32&&val<127)?(char)val:(val==160?' ':'?');
        return i;
    }
    while(s[i] && s[i]!=';' && ni<9){ name[ni++]=s[i]; i++; }
    name[ni]=0;
    if(s[i]==';') i++;
    char c='?';
    if(!strcmp(name,"amp")) c='&';
    else if(!strcmp(name,"lt")) c='<';
    else if(!strcmp(name,"gt")) c='>';
    else if(!strcmp(name,"quot")) c='"';
    else if(!strcmp(name,"apos")) c='\'';
    else if(!strcmp(name,"nbsp")) c=' ';
    else if(!strcmp(name,"copy")) c='c';
    else if(!strcmp(name,"reg")) c='R';
    else if(!strcmp(name,"mdash")||!strcmp(name,"ndash")) c='-';
    else if(!strcmp(name,"hellip")) c='.';
    else if(!strcmp(name,"middot")) c='*';
    else { *outc='&'; return 1; } /* 알 수 없으면 & 그대로 */
    *outc=c;
    return i;
}

/* 태그 이름이 리스트에 있는지 (블록 태그 판별) */
static int wb_is_block(const char *n){
    static const char *blk[]={"p","div","br","hr","h1","h2","h3","h4","h5","h6",
        "ul","ol","li","tr","table","blockquote","section","article","header",
        "footer","nav","pre","form","dd","dt","figure","main","aside",0};
    for(int i=0;blk[i];i++) if(!strcmp(n,blk[i])) return 1;
    return 0;
}

/* HTML → 줄/링크로 렌더 */
static void wb_render(const char *html, int len){
    wb_nlines=0; wb_nanchor=0; wb_nspan=0; wb_curlen=0; wb_cur_anchor=-1;
    wb_title[0]=0;
    wb_bold=0; wb_italic=0; wb_heading=0; wb_code=0; wb_override_attr=-1;
    wb_nform=0; wb_curform=-1;
    uint16_t word[WB_WORDMAX]; int wl=0;
    int in_skip=0;   /* script/style 내부 */
    int in_title=0;  /* title 내부 */
    int in_area=-1;  /* textarea 내부면 항목 index, 아니면 -1 */
    int in_btn=-1;   /* button 내부면 항목 index, 아니면 -1 (value 없을 때 라벨 캡처) */
    int i=0;

    while(i<len){
        char c=html[i];
        if(c=='<'){
            /* 단어 flush */
            if(wl>0 && !in_skip && !in_title && in_area<0 && in_btn<0){ wb_emit_cps(word,wl,wb_style_now()); }
            wl=0;
            /* 주석 */
            if(html[i+1]=='!'&&html[i+2]=='-'&&html[i+3]=='-'){
                i+=4;
                while(i<len && !(html[i]=='-'&&html[i+1]=='-'&&html[i+2]=='>')) i++;
                i+=3; continue;
            }
            /* 태그 텍스트 수집 */
            int j=i+1;
            char tag[128]; int ti=0;
            while(j<len && html[j]!='>'){ if(ti<127) tag[ti++]=html[j]; j++; }
            tag[ti]=0;
            i=(j<len)?j+1:len;
            /* 태그 이름 */
            const char *tp=tag; int closing=0;
            if(*tp=='/'){ closing=1; tp++; }
            char name[16]; int nn=0;
            while(*tp && *tp!=' ' && *tp!='\t' && *tp!='/' && nn<15){
                char lc=*tp; if(lc>='A'&&lc<='Z') lc+=32; name[nn++]=lc; tp++;
            }
            name[nn]=0;

            if(!strcmp(name,"script")||!strcmp(name,"style")){ in_skip=!closing; continue; }
            if(in_skip) continue;

            if(!strcmp(name,"title")){ in_title=!closing; continue; }

            if(!strcmp(name,"a")){
                if(closing){ wb_cur_anchor=-1; }
                else {
                    char href[WB_HREFLEN];
                    if(wb_get_attr(tag,"href",href,WB_HREFLEN) && href[0]){
                        int idx=wb_new_item(WB_IT_LINK);
                        if(idx>=0){ wb_scopy(wb_item[idx].href,href,WB_HREFLEN); wb_cur_anchor=idx; }
                        else wb_cur_anchor=-1;
                    } else wb_cur_anchor=-1;
                }
                continue;
            }

            /* ── 폼 요소 ── */
            if(!strcmp(name,"form")){
                if(closing){ wb_curform=-1; }
                else if(wb_nform<WB_MAXFORM){
                    char act[WB_HREFLEN], meth[8];
                    wb_get_attr(tag,"action",act,WB_HREFLEN);
                    wb_scopy(wb_form[wb_nform].action,act,WB_HREFLEN);
                    wb_form[wb_nform].method=0;
                    if(wb_get_attr(tag,"method",meth,8) && (meth[0]|32)=='p') wb_form[wb_nform].method=1;
                    wb_curform=wb_nform; wb_nform++;
                }
                continue;
            }
            if(!strcmp(name,"input") && !closing){
                char typ[16]; if(!wb_get_attr(tag,"type",typ,16)){ typ[0]='t'; typ[1]=0; }
                for(int z=0;typ[z];z++) if(typ[z]>='A'&&typ[z]<='Z') typ[z]+=32;
                char nm[32], val[80];
                wb_get_attr(tag,"name",nm,32);
                wb_get_attr(tag,"value",val,80);
                if(!strcmp(typ,"hidden")){
                    int idx=wb_new_item(WB_IT_HIDDEN);
                    if(idx>=0){ wb_scopy(wb_item[idx].name,nm,32); wb_scopy(wb_item[idx].value,val,80); }
                    continue;
                }
                if(!strcmp(typ,"submit")||!strcmp(typ,"button")||!strcmp(typ,"image")||!strcmp(typ,"reset")){
                    int idx=wb_new_item(WB_IT_SUBMIT);
                    if(idx>=0){
                        wb_scopy(wb_item[idx].name,nm,32);
                        wb_scopy(wb_item[idx].value, val[0]?val:"Submit",80);
                        char box[72]; int bi=0; box[bi++]='['; box[bi++]=' ';
                        for(int k=0; wb_item[idx].value[k]&&bi<68; k++) box[bi++]=wb_item[idx].value[k];
                        box[bi++]=' '; box[bi++]=']'; box[bi]=0;
                        wb_emit_widget(idx,box,bi,0x20);
                    }
                    continue;
                }
                if(!strcmp(typ,"checkbox")||!strcmp(typ,"radio")){
                    int idx=wb_new_item(typ[0]=='c'?WB_IT_CHECK:WB_IT_RADIO);
                    if(idx>=0){
                        wb_scopy(wb_item[idx].name,nm,32);
                        wb_scopy(wb_item[idx].value, val[0]?val:"on",80);
                        wb_item[idx].checked=(uint8_t)wb_has_attr(tag,"checked");
                        wb_emit_widget(idx, (typ[0]=='c')?"[ ]":"( )", 3, 0x2F);
                    }
                    continue;
                }
                /* 나머지 = 텍스트류(text/search/password/email/url/tel/number ...) */
                {
                    int idx=wb_new_item(WB_IT_TEXT);
                    if(idx>=0){
                        wb_scopy(wb_item[idx].name,nm,32);
                        wb_scopy(wb_item[idx].value,val,80);
                        int w=20; char sz[8];
                        if(wb_get_attr(tag,"size",sz,8)){ int v=0; for(int z=0;sz[z]>='0'&&sz[z]<='9';z++) v=v*10+(sz[z]-'0'); if(v>=3&&v<=48) w=v; }
                        wb_item[idx].width=(uint8_t)w;
                        char box[52]; int bi=0; box[bi++]='[';
                        for(int k=0;k<w;k++) box[bi++]=' ';
                        box[bi++]=']'; box[bi]=0;
                        wb_emit_widget(idx,box,bi,0x1F);
                    }
                    continue;
                }
            }
            if(!strcmp(name,"textarea")){
                if(closing){ in_area=-1; }
                else {
                    int idx=wb_new_item(WB_IT_AREA);
                    if(idx>=0){
                        char nm[32]; wb_get_attr(tag,"name",nm,32); wb_scopy(wb_item[idx].name,nm,32);
                        wb_item[idx].value[0]=0; wb_item[idx].width=40;
                        char box[52]; int bi=0; box[bi++]='[';
                        for(int k=0;k<40;k++) box[bi++]=' ';
                        box[bi++]=']'; box[bi]=0;
                        wb_emit_widget(idx,box,bi,0x1F);
                        in_area=idx;
                    }
                }
                continue;
            }
            if(!strcmp(name,"button")){
                if(closing){
                    if(in_btn>=0){
                        char box[72]; int bi=0; box[bi++]='['; box[bi++]=' ';
                        const char*lb=wb_item[in_btn].value[0]?wb_item[in_btn].value:"Button";
                        for(int k=0; lb[k]&&bi<68; k++) box[bi++]=lb[k];
                        box[bi++]=' '; box[bi++]=']'; box[bi]=0;
                        wb_emit_widget(in_btn,box,bi,0x20);
                        in_btn=-1;
                    }
                } else {
                    int idx=wb_new_item(WB_IT_SUBMIT);
                    if(idx>=0){
                        char nm[32], val[80];
                        wb_get_attr(tag,"name",nm,32); wb_get_attr(tag,"value",val,80);
                        wb_scopy(wb_item[idx].name,nm,32);
                        wb_scopy(wb_item[idx].value,val,80);   /* 없으면 라벨로 채움 */
                        in_btn=idx;
                    }
                }
                continue;
            }
            if(!strcmp(name,"select")||!strcmp(name,"option")||!strcmp(name,"label")) continue; /* 무시 */

            if(!strcmp(name,"li")){ wb_flush_line(); wb_emit_ascii("*",1,wb_style_now()); continue; }
            if(!strcmp(name,"hr")){ wb_flush_line(); wb_emit_ascii("----------------------------------------",40,0x08); wb_flush_line(); continue; }
            if(!strcmp(name,"br")){ wb_flush_line(); continue; }

            /* 강조 태그 → 색상 */
            if(!strcmp(name,"b")||!strcmp(name,"strong")){ wb_bold += closing?-1:1; if(wb_bold<0)wb_bold=0; continue; }
            if(!strcmp(name,"i")||!strcmp(name,"em"))     { wb_italic += closing?-1:1; if(wb_italic<0)wb_italic=0; continue; }
            if(!strcmp(name,"u"))                          { wb_italic += closing?-1:1; if(wb_italic<0)wb_italic=0; continue; }
            if(!strcmp(name,"code")||!strcmp(name,"tt")||!strcmp(name,"kbd")){ wb_code += closing?-1:1; if(wb_code<0)wb_code=0; continue; }

            /* 이미지 → [img] / [img:alt] 자리표시 */
            if(!strcmp(name,"img")){
                char alt[48];
                if(wb_get_attr(tag,"alt",alt,48) && alt[0]){
                    char ph[64]; int pi=0; const char*pre="[img:";
                    for(int k=0;pre[k];k++) ph[pi++]=pre[k];
                    for(int k=0;alt[k]&&pi<62;k++) ph[pi++]=alt[k];
                    ph[pi++]=']'; ph[pi]=0;
                    wb_emit_forced(ph,pi,0x0D);
                } else {
                    wb_emit_forced("[img]",5,0x0D);
                }
                continue;
            }

            if(name[0]=='p' && name[1]==0){ wb_blank_line(); continue; }
            if(name[0]=='h' && name[1]>='1'&&name[1]<='6'&&name[2]==0){ wb_blank_line(); wb_heading=!closing; continue; }
            if(wb_is_block(name)){ wb_flush_line(); continue; }
            continue;
        }

        if(in_skip){ i++; continue; }

        /* title/textarea/button 값 캡처 (UTF-8 바이트 그대로 보존) */
        if(in_title || in_area>=0 || in_btn>=0){
            char *dst = in_title ? wb_title : (in_area>=0 ? wb_item[in_area].value : wb_item[in_btn].value);
            if(c=='&'){
                char ec; int used=wb_entity(html+i,&ec);
                int vl=strlen(dst); if(vl<79){ dst[vl]=ec; dst[vl+1]=0; }
                i+=used; continue;
            }
            if(c==' '||c=='\t'||c=='\n'||c=='\r'){
                int vl=strlen(dst); if(vl>0&&vl<79&&dst[vl-1]!=' '){ dst[vl]=' '; dst[vl+1]=0; }
                i++; continue;
            }
            int vl=strlen(dst); if(vl<79){ dst[vl]=c; dst[vl+1]=0; }  /* 바이트 그대로 */
            i++; continue;
        }

        if(c=='&'){
            char ec; int used=wb_entity(html+i,&ec);
            if(wl<WB_WORDMAX-1) word[wl++]=(uint16_t)(uint8_t)ec;
            i+=used; continue;
        }
        if(c==' '||c=='\t'||c=='\n'||c=='\r'){
            if(wl>0){ wb_emit_cps(word,wl,wb_style_now()); wl=0; }
            i++; continue;
        }
        if((uint8_t)c < 0x80){
            if(wl<WB_WORDMAX-1) word[wl++]=(uint16_t)(uint8_t)c;
            i++; continue;
        }
        /* UTF-8 멀티바이트 → 코드포인트 */
        {
            uint32_t cp; int n=utf8_decode_one((const uint8_t*)(html+i), &cp);
            if(n<=0){ i++; continue; }
            if(wl<WB_WORDMAX-1) word[wl++]=(cp<=0xFFFF)?(uint16_t)cp:(uint16_t)'?';
            i+=n;
        }
    }
    if(wl>0 && !in_skip){ wb_emit_cps(word,wl,wb_style_now()); }
    wb_flush_line();
    if(wb_nlines==0){ wb_cell[0][0]=0; wb_nlines=1; }
}

/* 상대/절대 URL 해석. 반환 1=이동가능, 0=불가(#, javascript 등) */
static int wb_resolve(const char *base, const char *href, char *out, int outmax){
    while(*href==' ') href++;
    /* 프래그먼트 제거한 복사본 */
    char h[WB_HREFLEN]; int hi=0;
    for(const char*q=href; *q && *q!='#' && hi<WB_HREFLEN-1; q++) h[hi++]=*q;
    h[hi]=0;
    if(h[0]==0) return 0;                     /* #앵커 등 → 이동 안 함 */
    if(!strncmp(h,"javascript:",11)) return 0;
    if(!strncmp(h,"mailto:",7)) return 0;
    if(!strncmp(h,"tel:",4)) return 0;
    /* ./ 제거 */
    while(!strncmp(h,"./",2)){ for(int k=0;;k++){ h[k]=h[k+2]; if(!h[k])break; } }

    int oi=0;
    #define WB_PUT(str) do{ for(const char*z=(str); *z && oi<outmax-1; z++) out[oi++]=*z; }while(0)

    if(!strncmp(h,"http://",7)){ WB_PUT(h); out[oi]=0; return 1; }
    if(!strncmp(h,"https://",8)){ WB_PUT("http://"); WB_PUT(h+8); out[oi]=0; return 1; }
    if(h[0]=='/'&&h[1]=='/'){ WB_PUT("http:"); WB_PUT(h); out[oi]=0; return 1; }

    /* base = http://host/path... 에서 host 추출 */
    const char *bh=base;
    if(!strncmp(bh,"http://",7)) bh+=7;
    char host[128]; int hoi=0;
    while(*bh && *bh!='/' && hoi<127) host[hoi++]=*bh++;
    host[hoi]=0;
    /* bh 는 이제 host 뒤 경로 시작('/....' 또는 끝) */

    if(h[0]=='/'){
        WB_PUT("http://"); WB_PUT(host); WB_PUT(h); out[oi]=0; return 1;
    }
    /* 상대경로: base 경로의 디렉터리 + h */
    WB_PUT("http://"); WB_PUT(host);
    /* base 경로에서 마지막 '/'까지 */
    const char *path=bh;                 /* '/...' 또는 "" */
    int lastslash=-1;
    for(int k=0; path[k]; k++){ if(path[k]=='/') lastslash=k; if(path[k]=='?') break; }
    if(lastslash<0){ if(oi<outmax-1) out[oi++]='/'; }
    else { for(int k=0;k<=lastslash && oi<outmax-1;k++) out[oi++]=path[k]; }
    WB_PUT(h);
    out[oi]=0;
    #undef WB_PUT
    return 1;
}

/* 선택 링크의 첫 span 줄 번호 (스크롤 타겟) */
static int wb_anchor_line(int anchor){
    for(int s=0;s<wb_nspan;s++) if(wb_span[s].anchor==anchor) return wb_span[s].line;
    return 0;
}
/* 화면에 그려지는(span 있는) 항목만 포커스 대상 */
static int wb_has_span(int idx){ for(int s=0;s<wb_nspan;s++) if(wb_span[s].anchor==idx) return 1; return 0; }
static int wb_first_sel(void){ for(int i=0;i<wb_nanchor;i++) if(wb_has_span(i)) return i; return -1; }
static int wb_next_sel(int cur){ for(int i=cur+1;i<wb_nanchor;i++) if(wb_has_span(i)) return i; return cur; }
static int wb_prev_sel(int cur){ for(int i=cur-1;i>=0;i--) if(wb_has_span(i)) return i; return cur; }

/* 주소창(텍스트 박스)을 그린다. focus=1이면 커서 표시 */
static void wb_draw_addr(const char *text, int focus, int caret){
    tui_fill(0,0,COLS-1,' ',0x30);
    tui_puts_at(0,0," URL:",0x30);
    /* 입력 필드: 흰 글자/파란 배경으로 텍스트 박스처럼 */
    int fx=6, fw=COLS-1-fx;                 /* 필드 시작열/폭 */
    tui_fill(0,fx-1,COLS-1,' ',0x1F);
    int len=strlen(text);
    int off=0;
    if(caret>fw-1) off=caret-(fw-1);        /* 길면 스크롤 */
    for(int c=0;c<fw;c++){
        char ch=(off+c<len)?text[off+c]:' ';
        tui_putch(0,fx+c,ch,0x1F);
    }
    if(focus){
        int cx=fx+(caret-off);
        if(cx>=fx && cx<COLS) tui_putch(0,cx,'_',0x1E);
    }
}

static void wb_draw(int scroll, int sel){
    tui_fill_rect(0,0,ROWS-1,COLS-1,' ',0x07);
    /* 헤더: 주소 텍스트 박스 */
    wb_draw_addr(wb_url,0,0);
    /* 본문 (셀 단위, 한글은 2칸 합성) */
    for(int i=0;i<WB_VIS;i++){
        int ln=scroll+i;
        if(ln>=wb_nlines) break;
        int r=1+i;
        for(int c=0; c<WB_LINEW; c++){
            uint16_t cp=wb_cell[ln][c];
            if(cp==0) break;
            if(cp==0xFFFF) continue;             /* 한글 두 번째 칸 */
            uint8_t at=wb_attr[ln][c];
            if(cp<128) tui_putch(r,1+c,(char)cp,at);
            else if(wb_is_wide(cp)) wb_draw_hangul(r,1+c,cp,at);
            else tui_putch(r,1+c,'?',at);
        }
    }
    /* 링크/폼요소 하이라이트 (본문 위에 덧그림) */
    for(int s=0;s<wb_nspan;s++){
        int ln=wb_span[s].line;
        if(ln<scroll||ln>=scroll+WB_VIS) continue;
        int r=1+(ln-scroll);
        int idx=wb_span[s].anchor;
        if(idx<0||idx>=wb_nanchor) continue;
        struct wb_item_s *it=&wb_item[idx];
        int focus=(idx==sel);
        int c0=1+wb_span[s].col;
        uint8_t a;
        if(it->type==WB_IT_LINK || it->type==WB_IT_SUBMIT){
            a=focus?0x4F:(it->type==WB_IT_LINK?0x09:0x20);
            for(int c=0;c<wb_span[s].len;c++){
                uint16_t cp=wb_cell[ln][wb_span[s].col+c];
                if(cp==0xFFFF) continue;
                if(wb_is_wide(cp)) wb_draw_hangul(r,c0+c,cp,a);
                else tui_putch(r,c0+c,(char)(cp<128?cp:'?'),a);
            }
        } else if(it->type==WB_IT_CHECK||it->type==WB_IT_RADIO){
            a=focus?0x4F:0x2F;
            char open =(it->type==WB_IT_CHECK)?'[':'(';
            char close=(it->type==WB_IT_CHECK)?']':')';
            char mark =it->checked?(it->type==WB_IT_CHECK?'x':'*'):' ';
            tui_putch(r,c0,open,a); tui_putch(r,c0+1,mark,a); tui_putch(r,c0+2,close,a);
        } else if(it->type==WB_IT_TEXT||it->type==WB_IT_AREA){
            a=focus?0x4E:0x1F;                      /* 입력필드: 파란배경 / 선택 빨강 */
            int w=it->width; if(w<=0)w=20;
            tui_putch(r,c0,'[',focus?0x4E:0x17);
            wb_draw_utf8(r,c0+1,w,it->value,a,1);   /* 값을 한글 포함해 표시(끝부분) */
            tui_putch(r,c0+1+w,']',focus?0x4E:0x17);
        }
    }
    /* 상태바 */
    tui_fill(ROWS-1,0,COLS-1,' ',0x70);
    tui_puts_at(ROWS-1,0," UP/DN:move  L/R:scroll  ENTER:action  Ctrl+Space:HAN  g:URL  b:back  q:quit ",0x70);
}

/* 주소 텍스트 박스 편집. buf에 초깃값(현재 URL) 넣어 호출.
 * 방향키로 커서 이동, 삽입/삭제, Enter=이동, Esc=취소. 반환 1=확정,0=취소 */
static int wb_address_edit(char *buf, int max){
    int len=strlen(buf);
    int caret=len;                 /* 커서 위치 */
    for(;;){
        wb_draw_addr(buf,1,caret);
        uint8_t k=getch_raw_u8_blocking();
        if(k=='\n'||k=='\r'){ buf[len]=0; return len>0; }
        if(k==0x1B){ return 0; }
        if(k==KEY_LEFT){ if(caret>0) caret--; continue; }
        if(k==KEY_RIGHT){ if(caret<len) caret++; continue; }
        if(k==KEY_HOME){ caret=0; continue; }
        if(k==KEY_END){ caret=len; continue; }
        if(k==0x08){ /* backspace */
            if(caret>0){ for(int i=caret-1;i<len;i++) buf[i]=buf[i+1]; len--; caret--; }
            continue;
        }
        if(k==KEY_DEL){
            if(caret<len){ for(int i=caret;i<len;i++) buf[i]=buf[i+1]; len--; }
            continue;
        }
        if(k>=32 && k<127 && len<max-1){
            for(int i=len;i>caret;i--) buf[i]=buf[i-1];
            buf[caret]=(char)k; len++; caret++; buf[len]=0;
        }
    }
}

/* 히스토리 (뒤로가기) */
static char wb_hist[10][256];
static int  wb_histn=0;

/* 조합 중(미완성) 글자를 UTF-8로 out에 써서 길이 반환(없으면 0) */
static int wb_ime_preview(char *out){
    if(gui_ime_L<0 && gui_ime_V<0) return 0;
    uint32_t cp;
    if(gui_ime_V>=0){ int L=(gui_ime_L>=0)?gui_ime_L:11; cp=make_syllable_cp(L,gui_ime_V,gui_ime_T); }
    else { if(gui_ime_L<0||gui_ime_L>=19) return 0; cp=0x3131u+cho_to_jamo_offset[gui_ime_L]; }
    return utf8_encode(cp,out);
}

/* 텍스트 입력 필드 편집(한글 IME 지원). 화면 (r,c0), 폭 w. 값=wb_item[idx].value(UTF-8).
 * Ctrl+Space로 한/영 전환. 조합 append 방식(끝에 입력). */
static void wb_field_edit(int idx, int r, int c0, int w){
    struct wb_item_s *it=&wb_item[idx];
    int len=strlen(it->value);
    gui_ime_reset();
    for(;;){
        /* 표시 문자열 = 확정값 + 조합중 글자 */
        char disp[96]; int dl=0;
        for(int i=0;i<len&&dl<90;i++) disp[dl++]=it->value[i];
        char prev[4]; int pn=wb_ime_preview(prev);
        for(int i=0;i<pn&&dl<95;i++) disp[dl++]=prev[i];
        disp[dl]=0;
        /* 필드 렌더(한글 포함) */
        tui_putch(r,c0,'[',0x4E);
        wb_draw_utf8(r,c0+1,w,disp,0x4E,1);
        tui_putch(r,c0+1+w,']',0x4E);
        /* 한/영 표시 */
        tui_puts_at(ROWS-1,COLS-7, gui_ime_on?"[HAN]":"[ENG]", 0x71);

        uint8_t k=getch_raw_u8_blocking();
        if(k==KEY_TOGGLE_HAN){ if(gui_ime_on) gui_ime_flush(it->value,&len,80); gui_ime_on^=1; continue; }
        if(k=='\n'||k=='\r'||k==0x1B){ gui_ime_flush(it->value,&len,80); it->value[len]=0; return; }
        if(k==0x08){ /* 백스페이스: 조합중이면 조합 취소, 아니면 UTF-8 한 글자 삭제 */
            if(gui_ime_L>=0||gui_ime_V>=0){ gui_ime_reset(); }
            else if(len>0){ len--; while(len>0 && ((uint8_t)it->value[len]&0xC0)==0x80) len--; it->value[len]=0; }
            continue;
        }
        if(k>=32 && k<127){
            if(gui_ime_on){ gui_ime_key(k, it->value, &len, 80); }
            else if(len<79){ it->value[len++]=(char)k; it->value[len]=0; }
            continue;
        }
        /* 그 외(화살표 등)는 무시 */
    }
}

/* URL 인코딩: dst에 append. 반환=쓴 길이 위치 갱신 */
static int wb_urlenc(char *dst, int di, int dmax, const char *src){
    const char *hx="0123456789ABCDEF";
    for(const char *s=src; *s && di<dmax-3; s++){
        char ch=*s;
        if((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='_'||ch=='.'||ch=='~'){
            dst[di++]=ch;
        } else if(ch==' '){
            dst[di++]='+';
        } else {
            dst[di++]='%'; dst[di++]=hx[(uint8_t)ch>>4]; dst[di++]=hx[(uint8_t)ch&0xF];
        }
    }
    return di;
}

/* 폼 전송: submit 항목 idx 클릭 → GET 쿼리 조립 후 이동. 반환 1=이동함 */
static int wb_submit_form(int submit_idx, char *outurl, int outmax){
    int f=wb_item[submit_idx].form;
    if(f<0) return 0;
    /* 쿼리 문자열 조립 */
    char q[512]; int qi=0; int first=1;
    for(int i=0;i<wb_nanchor;i++){
        struct wb_item_s *it=&wb_item[i];
        if(it->form!=f) continue;
        if(it->name[0]==0) continue;
        if(it->type==WB_IT_SUBMIT && i!=submit_idx) continue; /* 눌린 버튼만 포함 */
        if((it->type==WB_IT_CHECK||it->type==WB_IT_RADIO) && !it->checked) continue;
        if(!first && qi<510) q[qi++]='&';
        first=0;
        qi=wb_urlenc(q,qi,512,it->name);
        if(qi<510) q[qi++]='=';
        qi=wb_urlenc(q,qi,512,it->value);
    }
    q[qi]=0;
    /* action 해석 */
    char action[WB_HREFLEN];
    if(wb_form[f].action[0]) wb_scopy(action,wb_form[f].action,WB_HREFLEN);
    else wb_scopy(action,wb_url,WB_HREFLEN);       /* 빈 action → 현재 URL */
    char resolved[256];
    if(!wb_resolve(wb_url, action, resolved, 256)){ wb_scopy(resolved,wb_url,256); }

    if(wb_form[f].method==1){
        /* POST: 본문에 쿼리, URL은 action 그대로 */
        static char post_buf[512];
        int k=0; for(;q[k]&&k<511;k++) post_buf[k]=q[k]; post_buf[k]=0;
        g_post_body=post_buf; g_post_len=k;
        int oi=0; for(k=0;resolved[k]&&oi<outmax-1;k++) outurl[oi++]=resolved[k]; outurl[oi]=0;
        return 1;
    }
    /* GET: resolved?query */
    int oi=0;
    for(int k=0;resolved[k]&&oi<outmax-2;k++) outurl[oi++]=resolved[k];
    if(qi>0){
        char sep = '?';
        for(int k=0;resolved[k];k++) if(resolved[k]=='?'){ sep='&'; break; }
        if(oi<outmax-2) outurl[oi++]=sep;
        for(int k=0;q[k]&&oi<outmax-1;k++) outurl[oi++]=q[k];
    }
    outurl[oi]=0;
    return 1;
}

static void wb_load(const char *url, int push);

static void wb_load(const char *url, int push){
    if(push && wb_url[0] && wb_histn<10){
        int k=0; for(;wb_url[k]&&k<255;k++) wb_hist[wb_histn][k]=wb_url[k];
        wb_hist[wb_histn][k]=0; wb_histn++;
    }
    int k=0; for(;url[k]&&k<255;k++) wb_url[k]=url[k]; wb_url[k]=0;

    /* 로딩 표시 */
    tui_fill_rect(0,0,ROWS-1,COLS-1,' ',0x07);
    tui_puts_at(0,0,"[web] loading...",0x30);
    tui_puts_at(1,0,wb_url,0x07);
    /* http_get 은 자체 진행 메시지를 텍스트로 출력한다(그 위에 다시 그림) */
    row=3; col=0;

    int n=http_get(wb_url, wb_html, WB_HTMLMAX);
    if(n<=0){
        wb_nlines=0; wb_nspan=0; wb_nanchor=0; wb_title[0]=0;
        const char*msg=(n<0)?"[error] request failed (offline? https? bad url)":"[empty] no data";
        int mi=0; for(;msg[mi]&&mi<WB_LINEW;mi++){ wb_cell[0][mi]=(uint8_t)msg[mi]; wb_attr[0][mi]=0x0C; }
        if(mi<WB_LINEW) wb_cell[0][mi]=0;
        wb_nlines=1;
    } else {
        wb_render(wb_html, n);
    }
}

/* 브라우저 진입점 */
static void cmd_web(const char *arg){
    if(!nic_ready){ puts("NIC not initialized\n"); return; }
    /* 시작 URL */
    char start[256];
    int si=0; const char*p=arg?arg:"";
    while(*p==' ') p++;
    while(*p && si<255) start[si++]=*p++;
    start[si]=0;
    wb_url[0]=0; wb_histn=0; wb_title[0]=0;

    if(start[0]==0){
        /* 주소 텍스트 박스에서 입력받기 */
        tui_fill_rect(0,0,ROWS-1,COLS-1,' ',0x07);
        tui_fill(ROWS-1,0,COLS-1,' ',0x70);
        tui_puts_at(ROWS-1,0," Type a URL and press Enter  (Esc = cancel) ",0x70);
        start[0]=0;
        if(!wb_address_edit(start, 256)) return;
    }
    /* http:// 없으면 붙여줌 */
    char url[256];
    if(strncmp(start,"http://",7)!=0 && strncmp(start,"https://",8)!=0){
        int u=0; const char*pre="http://";
        for(int k=0;pre[k];k++) url[u++]=pre[k];
        for(int k=0;start[k]&&u<255;k++) url[u++]=start[k];
        url[u]=0;
    } else { int u=0; for(;start[u]&&u<255;u++) url[u]=start[u]; url[u]=0; }

    wb_load(url, 0);

    int scroll=0;
    int sel = wb_first_sel();
    if(sel>=0){ scroll = wb_anchor_line(sel) - 1; }
    if(scroll<0) scroll=0;
    wb_draw(scroll, sel);

    for(;;){
        uint8_t k=getch_raw_u8_blocking();
        if(k=='q'||k=='Q'||k==0x1B) break;

        if(k==KEY_DOWN){
            if(sel>=0){
                sel=wb_next_sel(sel);
                int ln=wb_anchor_line(sel);
                if(ln<scroll) scroll=ln;
                if(ln>=scroll+WB_VIS) scroll=ln-WB_VIS+1;
            } else {
                if(scroll<wb_nlines-1) scroll++;
            }
        }
        else if(k==KEY_UP){
            if(sel>=0){
                sel=wb_prev_sel(sel);
                int ln=wb_anchor_line(sel);
                if(ln<scroll) scroll=ln;
                if(ln>=scroll+WB_VIS) scroll=ln-WB_VIS+1;
            } else {
                if(scroll>0) scroll--;
            }
        }
        else if(k==KEY_RIGHT){         /* 한 화면 아래로 */
            scroll+=WB_VIS-1;
            if(scroll>wb_nlines-1) scroll=wb_nlines-1;
            if(scroll<0) scroll=0;
        }
        else if(k==KEY_LEFT){          /* 한 화면 위로 */
            scroll-=WB_VIS-1;
            if(scroll<0) scroll=0;
        }
        else if(k==KEY_HOME){ scroll=0; }
        else if(k==KEY_END){ scroll=wb_nlines-WB_VIS; if(scroll<0)scroll=0; }
        else if(k=='\n'||k=='\r'){
            if(sel>=0 && sel<wb_nanchor){
                struct wb_item_s *it=&wb_item[sel];
                if(it->type==WB_IT_LINK){
                    char nu[256];
                    if(wb_resolve(wb_url, it->href, nu, 256)){
                        wb_load(nu, 1);
                        scroll=0; sel=wb_first_sel();
                        if(sel>=0){ scroll=wb_anchor_line(sel)-1; if(scroll<0)scroll=0; }
                    }
                } else if(it->type==WB_IT_TEXT||it->type==WB_IT_AREA){
                    /* 현재 화면상 위젯 위치 찾아 편집 */
                    for(int s=0;s<wb_nspan;s++){
                        if(wb_span[s].anchor==sel && wb_span[s].line>=scroll && wb_span[s].line<scroll+WB_VIS){
                            int r=1+(wb_span[s].line-scroll), c0=1+wb_span[s].col;
                            int w=it->width; if(w<=0)w=20;
                            wb_field_edit(sel, r, c0, w);
                            break;
                        }
                    }
                } else if(it->type==WB_IT_CHECK){
                    it->checked=!it->checked;
                } else if(it->type==WB_IT_RADIO){
                    /* 같은 폼·같은 name 라디오는 하나만 */
                    for(int i=0;i<wb_nanchor;i++)
                        if(wb_item[i].type==WB_IT_RADIO && wb_item[i].form==it->form && !strcmp(wb_item[i].name,it->name))
                            wb_item[i].checked=0;
                    it->checked=1;
                } else if(it->type==WB_IT_SUBMIT){
                    char nu[256];
                    if(wb_submit_form(sel, nu, 256)){
                        wb_load(nu, 1);
                        scroll=0; sel=wb_first_sel();
                        if(sel>=0){ scroll=wb_anchor_line(sel)-1; if(scroll<0)scroll=0; }
                    }
                }
            }
        }
        else if(k=='g'||k=='G'){
            char nu[256];
            int j=0; for(;wb_url[j]&&j<255;j++) nu[j]=wb_url[j]; nu[j]=0; /* 현재 주소 미리 채움 */
            if(wb_address_edit(nu, 256)){
                char full[256];
                if(strncmp(nu,"http://",7)!=0 && strncmp(nu,"https://",8)!=0){
                    int u=0; const char*pre="http://"; for(int m=0;pre[m];m++) full[u++]=pre[m];
                    for(int m=0;nu[m]&&u<255;m++) full[u++]=nu[m]; full[u]=0;
                } else { int u=0; for(;nu[u]&&u<255;u++) full[u]=nu[u]; full[u]=0; }
                wb_load(full,1);
                scroll=0; sel=wb_first_sel();
                if(sel>=0){ scroll=wb_anchor_line(sel)-1; if(scroll<0)scroll=0; }
            }
        }
        else if(k=='b'||k=='B'){
            if(wb_histn>0){
                wb_histn--;
                char back[256]; int j=0; for(;wb_hist[wb_histn][j]&&j<255;j++) back[j]=wb_hist[wb_histn][j]; back[j]=0;
                wb_load(back,0);
                scroll=0; sel=wb_first_sel();
                if(sel>=0){ scroll=wb_anchor_line(sel)-1; if(scroll<0)scroll=0; }
            }
        }
        wb_draw(scroll, sel);
    }
    /* 텍스트 화면 정리 */
    tui_fill_rect(0,0,ROWS-1,COLS-1,' ',0x07);
    row=0; col=0;
}

static void cmd_fetch(const char *args){
    if(!args||!args[0]){ puts("Usage: fetch <url> [file]\n"); return; }

    char url[256]={0}; char savefile[24]={0};
    int ui=0;
    const char *p=args;
    while(*p==' ') p++;
    while(*p&&*p!=' '&&ui<255) url[ui++]=*p++;
    url[ui]=0;
    while(*p==' ') p++;
    if(*p){ int fi=0; while(*p&&fi<23) savefile[fi++]=*p++; savefile[fi]=0; }

    static char out_body[65535];
    int n=http_get(url,out_body,65534);
    if(n<0) return;

    puts("Received "); print_uint((uint32_t)n); puts(" bytes\n");

    if(savefile[0]){
        uint32_t sc=(n+511)/512; if(!sc)sc=1;
        uint32_t lba=get_free_lba(sc);
        ata_write28(lba,sc,(uint8_t*)out_body);
        dirent_t dummy2;
        if(fs_find(cur_dir,savefile,&dummy2)){
            fs_free_extent(dummy2.lba,(dummy2.size+511)/512); /* ★ 기존 확장 회수 */
            ata_read28(cur_dir,DIR_SECTORS,dirbuf);
            for(int i=0;i<DIR_ENTRIES;i++){
                dirent_t*de=(dirent_t*)(dirbuf+i*sizeof(dirent_t));
                if(!strcmp(de->name,savefile)){memset(de,0,sizeof(dirent_t));break;}
            }
            ata_write28(cur_dir,DIR_SECTORS,dirbuf);
        }
        fs_add_entry(cur_dir,savefile,lba,(uint32_t)n);
        puts("Saved to "); puts(savefile); puts("\n");
    } else {
        puts(out_body); putc('\n');
    }
}

/* ── ping ─────────────────────────────────────────── */
typedef struct __attribute__((packed)){
    uint8_t type, code; uint16_t checksum, id, seq;
} icmp_hdr_t;

static void cmd_ping(const char *host){
    if(!nic_ready){puts("NIC not ready\n");return;}
    if(!arp_resolve_gw()){puts("No gateway\n");return;}

    uint32_t target_ip;
    int is_ip=1; const char *hp=host;
    while(*hp){ if(!(*hp>='0'&&*hp<='9')&&*hp!='.'){is_ip=0;break;} hp++; }
    if(is_ip){
        uint8_t a[4]={0,0,0,0}; int ai=0; hp=host;
        while(*hp&&ai<4){ while(*hp>='0'&&*hp<='9') a[ai]=(uint8_t)(a[ai]*10+(*hp++)-'0'); if(*hp=='.'){hp++;ai++;} else ai++; }
        target_ip=((uint32_t)a[0]<<24)|((uint32_t)a[1]<<16)|((uint32_t)a[2]<<8)|a[3];
    } else {
        puts("Resolving... "); target_ip=dns_query(host);
        if(!target_ip){puts("DNS failed\n");return;}
        /* IP 출력 */
        uint8_t ibs2[4]={(uint8_t)(target_ip>>24),(uint8_t)(target_ip>>16),(uint8_t)(target_ip>>8),(uint8_t)target_ip};
        for(int q2=0;q2<4;q2++){
            uint8_t bv=ibs2[q2]; if(!bv){putc('0');}
            else{ char tb[4];int ti=0;while(bv){tb[ti++]='0'+bv%10;bv/=10;} while(ti--) putc(tb[ti+1]); }
            if(q2<3) putc('.');
        }
        putc('\n');
    }

    static uint8_t icmpbuf[64];
    icmp_hdr_t *ic=(icmp_hdr_t*)icmpbuf;
    ic->type=8; ic->code=0; ic->id=htons2(0x1234); ic->checksum=0;
    for(int i=0;i<56;i++) icmpbuf[8+i]=(uint8_t)i;

    for(int seq2=1;seq2<=4;seq2++){
        ic->seq=htons2((uint16_t)seq2); ic->checksum=0;
        ic->checksum=ip_checksum(icmpbuf,64);
        ip_send(target_ip,1,icmpbuf,64);
        puts("ping "); print_uint((uint32_t)seq2); puts("... ");
        int got=0;
        for(int t=0;t<100000;t++){
            static uint8_t pp[1520]; int pn=nic_recv(pp,1520);
            if(pn>34){
                if(((pp[12]<<8)|pp[13])==0x0800 && pp[14+9]==1){
                    icmp_hdr_t *ri=(icmp_hdr_t*)(pp+34);
                    if(ri->type==0&&ri->id==htons2(0x1234)){ puts("ok\n"); got=1; break; }
                }
            }
        }
        if(!got) puts("timeout\n");
    }
}

/* ── netinfo ─────────────────────────────────────── */
static void cmd_netinfo(void){
    if(!nic_ready){ puts("NIC: not found (no RTL8139)\n"); return; }
    puts("NIC: RTL8139  iobase="); print_uint(nic_iobase); puts("\n");
    puts("MAC: ");
    for(int i=0;i<6;i++){
        uint8_t b=nic_mac[i];
        putc("0123456789ABCDEF"[b>>4]); putc("0123456789ABCDEF"[b&0xF]);
        if(i<5) putc(':');
    }
    puts("\nIP : 10.0.2.15\nGW : 10.0.2.2\nDNS: 10.0.2.3\n");
    puts(gw_mac_ok?"GW MAC: resolved\n":"GW MAC: (not resolved yet)\n");
}

static int split_path(const char*path, char*dir, char*name){
    int len = strlen(path);
    int slash = -1;
    for(int i=0;i<len;i++) if(path[i]=='/') slash=i;
    if(slash==-1){
        dir[0]='\0'; strncpy(name,path,24);
    } else {
        strncpy(dir,path,slash);
        dir[slash]='\0';
        strncpy(name,path+slash+1,24);
    }
    return slash!=-1;
}

static uint32_t resolve_dir(const char*path){
    if(!path[0]) return cur_dir;
    if(!strcmp(path,"/")) return ROOT_LBA;

    uint32_t dir = (path[0]=='/') ? ROOT_LBA : cur_dir;
    char token[24]; int i=0,j=0;
    while(1){
        if(path[i]=='/' || path[i]=='\0'){
            if(j>0){
                token[j]='\0';
                dirent_t e;
                if(!fs_find(dir,token,&e) || e.size!=0) return 0;
                dir = e.lba;
                j=0;
            }
            if(path[i]=='\0') break;
        } else token[j++]=path[i];
        i++;
    }
    return dir;
}

/* multitask CLI helpers */
static void cmd_jobs(void){
    puts("PID  MODE  STATE\n");
    for(int pid=0; pid<MAX_TASKS; ++pid){
        if(!tasks[pid].used) continue;
        print_uint((uint32_t)pid);
        puts("   ");
        if(pid == fg_pid) puts("FG    ");
        else puts("BG    ");
        puts("RUN\n");
    }
    if(fg_pid==-1) puts("(no foreground)\n");
}
static void cmd_kill(int pid){
    if(pid<0 || pid>=MAX_TASKS || !tasks[pid].used){ puts("no such pid\n"); return; }
    tasks[pid].used = 0;
    if(fg_pid == pid){ fg_pid = -1; shell_paused = 0; }
    puts("killed\n");
}
static int parse_uint(const char* s, uint32_t* out){
    uint32_t v=0; int ok=0;
    while(*s==' ') s++;
    while(*s>='0' && *s<='9'){ ok=1; v = v*10 + (uint32_t)(*s-'0'); s++; }
    if(!ok) return 0;
    *out=v; return 1;
}

/* ELF spawn */
static int elf_spawn(void*img){
    Elf32_Ehdr*eh=img;
    if(eh->e[0]!=0x7F||eh->e[1]!='E'||eh->e[2]!='L'||eh->e[3]!='F'){
        puts("bad ELF\n");
        return -1;
    }
    uint32_t phoff=*(uint32_t*)(eh->e+0x1C);
    uint16_t phnum=*(uint16_t*)(eh->e+0x2C);
    for(uint16_t i=0;i<phnum;++i){
        Elf32_Phdr*ph=(void*)((uint8_t*)img+phoff+i*sizeof(Elf32_Phdr));
        if(*(uint32_t*)ph->p!=PT_LOAD)continue;
        uint32_t off  =*(uint32_t*)(ph->p+4);
        uint32_t vaddr=*(uint32_t*)(ph->p+8);
        uint32_t sz   =*(uint32_t*)(ph->p+16);
        memcpy((void*)vaddr,(uint8_t*)img+off,sz);
    }
    void(*entry)(void)=(void*)*(uint32_t*)(eh->e+0x18);
    int pid = task_create(entry);
    if(pid>=0){ puts("spawned pid="); print_uint((uint32_t)pid); putc('\n'); }
    return pid;
}

static int load_file_for_exec(const char *fname, uint8_t *buf_out){
    dirent_t e;
    if(!fs_find(cur_dir, fname, &e)){
        dirent_t bin;
        if(fs_find(ROOT_LBA, "bin", &bin) && bin.size==0){
            if(!fs_find(bin.lba, fname, &e)){
                puts("no file\n"); return 0;
            }
        } else { puts("no file\n"); return 0; }
    }
    if(e.size==0){ puts("dir\n"); return 0; }
    fs_load_file(&e, buf_out);
    return 1;
}

static void cat_print_utf8(uint8_t *buf, uint32_t sz){
    uint8_t save = buf[sz];
    buf[sz]=0;
    put_utf8((char*)buf);
    buf[sz]=save;
}

/* =========================================================
 *  OS/2026 Desktop GUI  v3.0  – Neon Theme
 *
 *  Screen layout (80x25):
 *   Row  0       : Desktop background (neon scanlines)
 *   Rows 1..22   : Window area (up to 3 overlapping windows)
 *   Row  23      : Taskbar  [ [>] START | win1 | win2 | ... | clock ]
 *   Row  24      : Hotkey hint bar
 *
 *  Windows (F1-F3 open):
 *   F1  = File Explorer  (dual-panel, Tab switch, Enter open, BS up, Del delete)
 *   F2  = Terminal       (mini shell inside a window)
 *   F3  = System Info
 *
 *  Start menu (Enter on START or S key):
 *   [1] File Explorer   [2] Terminal   [3] System Info
 *   [R] Reboot          [H] Halt/Shutdown
 *   ESC = close menu
 *
 *  ESC outside menu = exit GUI back to CLI
 * ========================================================= */

/* ═══════════════════════════════════════════════════════════════════
 *  METHANE 2.0  –  OS/2026 Modern Desktop Environment
 *
 *  Mode 13h: 320×200, 256색, 프레임버퍼 0xA0000
 *
 *  앱 목록:
 *   · File Manager  – 아이콘뷰 + 리스트뷰, 더블클릭 열기
 *   · Notepad       – 텍스트 편집기 (읽기/쓰기)
 *   · File Viewer   – 바이너리/텍스트 뷰어
 *   · Terminal      – 셸 (ls cd mkdir rm cat run...)
 *   · App Launcher  – ELF 실행 → 전용 콘솔 창
 *   · System Info
 *
 *  마우스: 클릭/드래그 창 이동, 클릭 UI 조작
 * ═══════════════════════════════════════════════════════════════════ */

/* ─── 해상도 ─── */
#define SCR_W  320
#define SCR_H  200
#define VRAM   ((volatile uint8_t*)0xA0000)          /* 실제 VGA 프레임버퍼 */

/* ─── 더블 버퍼링 ───
 *  모든 그리기는 RAM 상의 백버퍼에 먼저 하고, gfx_present()에서 한 번에
 *  VGA 로 복사한다. 이렇게 하면 배경을 다시 칠하는 과정이 화면에 노출되지
 *  않아 깜빡임/티어링이 사라진다. */
static uint8_t g_backbuf[SCR_W*SCR_H] __attribute__((aligned(16)));

static inline void px(int x,int y,uint8_t c){
    if((unsigned)x<SCR_W&&(unsigned)y<SCR_H) g_backbuf[y*SCR_W+x]=c;
}
static inline uint8_t gpx(int x,int y){
    if((unsigned)x<SCR_W&&(unsigned)y<SCR_H) return g_backbuf[y*SCR_W+x];
    return 0;
}

/* 백버퍼 → VGA 로 한 번에 전송(수직 귀선 동기화로 티어링 방지) */
static void gfx_present(void){
    /* 수직 귀선(vertical retrace) 대기: 화면 갱신 중이 아닐 때 복사한다.
     * 진행 중이면 끝날 때까지, 아니면 다음 귀선이 시작될 때까지 기다린다.
     * guard 로 만약의 무한루프를 방지. */
    int guard;
    for(guard=0; ( inb(0x3DA)&0x08) && guard<200000; guard++);
    for(guard=0; (!(inb(0x3DA)&0x08)) && guard<200000; guard++);
    /* 32비트 단위 고속 복사 (64000B → 16000 워드) */
    volatile uint32_t *d=(volatile uint32_t*)0xA0000;
    const uint32_t    *s=(const uint32_t*)g_backbuf;
    for(int i=0;i<SCR_W*SCR_H/4;i++) d[i]=s[i];
}

/* ─── 팔레트 색 번호 ─── */
/* 0-15 CGA */
#define P_TRANSPARENT 255

/* 배경 */
#define P_DESK0   16   /* 바탕 진파랑 */
#define P_DESK1   17
#define P_DESK2   18

/* 창 */
#define P_WIN_BG   20   /* 창 배경 */
#define P_WIN_LITE 21   /* 창 밝은면 */
#define P_WIN_DRK  22   /* 창 어두운면 */
#define P_WIN_BRD  23   /* 창 테두리 */
#define P_WIN_SHD  24   /* 그림자 */

/* 타이틀바 */
#define P_TTL_A0   25   /* active 밝음 */
#define P_TTL_A1   26   /* active 어둠 */
#define P_TTL_IN   27   /* inactive */

/* 버튼 3종 (macOS) */
#define P_BTN_R    28   /* 닫기 빨강 */
#define P_BTN_Y    29   /* 최소화 노랑 */
#define P_BTN_G    30   /* 최대화 초록 */

/* 텍스트 */
#define P_TXT_W    31   /* 흰 텍스트 */
#define P_TXT_LT   32   /* 연회색 */
#define P_TXT_DK   33   /* 진회색 */
#define P_TXT_BLK  34   /* 검정 */

/* 선택/포커스 */
#define P_SEL0     35   /* 선택 밝음 */
#define P_SEL1     36   /* 선택 어둠 */

/* 도크/태스크바 */
#define P_DOCK_BG  37
#define P_DOCK_BD  38

/* 아이콘 색 */
#define P_ICO_DIR  39   /* 폴더 골드 */
#define P_ICO_TXT  40   /* txt 파란 */
#define P_ICO_ELF  41   /* 실행 초록 */
#define P_ICO_UNK  42   /* 기타 회색 */

/* 단말기 */
#define P_TERM_BG  43
#define P_TERM_GR  44   /* 터미널 그린 */
#define P_TERM_CY  45   /* 터미널 시안 */
#define P_TERM_YL  46   /* 터미널 노랑 */
#define P_TERM_RD  47   /* 터미널 빨강 */

/* 스크롤바 */
#define P_SCR_BG   48
#define P_SCR_TH   49

/* 입력박스 */
#define P_INP_BG   50
#define P_INP_BD   51

/* 메뉴 */
#define P_MNU_BG   52
#define P_MNU_HL   53
#define P_MNU_SEP  54

/* 추가 */
#define P_ACCENT   55   /* 액센트 시안 */
#define P_STAR0    56
#define P_STAR1    57
#define P_STAR2    58

/* ─── Mode13h 진입/복귀 ─── */
static void gfx_enter_mode13(void){
    outb(0x3C2,0x63);
    outb(0x3C4,0x00); outb(0x3C5,0x03);
    outb(0x3C4,0x01); outb(0x3C5,0x01);
    outb(0x3C4,0x02); outb(0x3C5,0x0F);
    outb(0x3C4,0x03); outb(0x3C5,0x00);
    outb(0x3C4,0x04); outb(0x3C5,0x0E);
    outb(0x3D4,0x11); outb(0x3D5,inb(0x3D5)&0x7F);
    static const uint8_t cr[]={
        0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,
        0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
        0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF};
    for(int i=0;i<25;i++){outb(0x3D4,(uint8_t)i);outb(0x3D5,cr[i]);}
    static const uint8_t gc[]={0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF};
    for(int i=0;i<9;i++){outb(0x3CE,(uint8_t)i);outb(0x3CF,gc[i]);}
    inb(0x3DA);
    static const uint8_t ac[]={
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x41,0x00,0x0F,0x00,0x00};
    for(int i=0;i<21;i++){outb(0x3C0,(uint8_t)i);outb(0x3C0,ac[i]);}
    outb(0x3C0,0x20);
}
static void gfx_return_text(void){
    outb(0x3C2,0x67);
    outb(0x3C4,0x00);outb(0x3C5,0x03);
    outb(0x3C4,0x01);outb(0x3C5,0x00);
    outb(0x3C4,0x02);outb(0x3C5,0x03);
    outb(0x3C4,0x03);outb(0x3C5,0x00);
    outb(0x3C4,0x04);outb(0x3C5,0x02);
    outb(0x3D4,0x11);outb(0x3D5,inb(0x3D5)&0x7F);
    static const uint8_t cr[]={
        0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,
        0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,
        0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF};
    for(int i=0;i<25;i++){outb(0x3D4,(uint8_t)i);outb(0x3D5,cr[i]);}
    static const uint8_t gc[]={0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF};
    for(int i=0;i<9;i++){outb(0x3CE,(uint8_t)i);outb(0x3CF,gc[i]);}
    inb(0x3DA);
    static const uint8_t ac[]={
        0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
        0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
        0x0C,0x00,0x0F,0x08,0x00};
    for(int i=0;i<21;i++){outb(0x3C0,(uint8_t)i);outb(0x3C0,ac[i]);}
    outb(0x3C0,0x20);
}

/* mode13h는 VGA plane2(폰트)를 픽셀 데이터로 덮어씀
 * → 텍스트 모드 복귀 후 반드시 폰트를 다시 써야 함 */
/* ─── DAC 팔레트 ─── */
static void dac(uint8_t i,uint8_t r,uint8_t g,uint8_t b){
    outb(0x3C8,i); outb(0x3C9,r>>2); outb(0x3C9,g>>2); outb(0x3C9,b>>2);
}
static void gfx_init_palette(void){
    /* CGA 16 */
    static const uint8_t cga[16][3]={
        {0,0,0},{0,0,170},{0,170,0},{0,170,170},
        {170,0,0},{170,0,170},{170,85,0},{170,170,170},
        {85,85,85},{85,85,255},{85,255,85},{85,255,255},
        {255,85,85},{255,85,255},{255,255,85},{255,255,255}};
    for(int i=0;i<16;i++) dac((uint8_t)i,cga[i][0],cga[i][1],cga[i][2]);

    /* ─── Auroral: 보라/핑크/시안 그라디언트 배경 ─── */
    dac(P_DESK0, 18, 10, 42);   /* 깊은 보라 */
    dac(P_DESK1, 30, 10, 60);   /* 중간 보라 */
    dac(P_DESK2, 48, 12, 80);   /* 밝은 보라 */

    /* 창 – 반투명 느낌의 어두운 유리 */
    dac(P_WIN_BG,  24, 20, 48);
    dac(P_WIN_LITE,36, 28, 70);
    dac(P_WIN_DRK, 16, 12, 36);
    dac(P_WIN_BRD, 100, 60,160);
    dac(P_WIN_SHD,  4,  2, 10);

    /* 타이틀바 – 핑크~보라 그라디언트 */
    dac(P_TTL_A0, 180, 40,200);  /* 핑크-보라 밝음 */
    dac(P_TTL_A1, 120, 20,170);  /* 핑크-보라 어둠 */
    dac(P_TTL_IN,  50, 40, 80);  /* 비활성 */

    /* 버튼 – macOS 스타일이지만 오로라 색 */
    dac(P_BTN_R, 255, 70,120);   /* 핑크 닫기 */
    dac(P_BTN_Y, 255,200, 50);   /* 노랑 최소화 */
    dac(P_BTN_G,  60,220,180);   /* 민트 최대화 */

    /* 텍스트 */
    dac(P_TXT_W,  240,230,255);
    dac(P_TXT_LT, 180,160,220);
    dac(P_TXT_DK, 140,120,180);
    dac(P_TXT_BLK, 20, 15, 40);

    /* 선택 – 시안/핑크 */
    dac(P_SEL0,  60,210,240);    /* 시안 */
    dac(P_SEL1,  30,160,200);

    /* 도크 – 진한 보라 반투명 */
    dac(P_DOCK_BG, 14, 10, 30);
    dac(P_DOCK_BD, 80, 50,140);

    /* 아이콘 */
    dac(P_ICO_DIR, 255,200, 80);  /* 금 폴더 */
    dac(P_ICO_TXT,  80,180,255);  /* 파랑 문서 */
    dac(P_ICO_ELF,  80,255,160);  /* 민트 실행 */
    dac(P_ICO_UNK, 140,120,180);  /* 회보라 기타 */

    /* 터미널 */
    dac(P_TERM_BG,   8,  5, 20);
    dac(P_TERM_GR,  60,255,160);  /* 민트 그린 */
    dac(P_TERM_CY,  60,220,255);  /* 밝은 시안 */
    dac(P_TERM_YL, 255,220, 80);
    dac(P_TERM_RD, 255, 80,120);

    /* 스크롤바 */
    dac(P_SCR_BG,  40, 28, 80);
    dac(P_SCR_TH, 120, 80,180);

    /* 입력박스 */
    dac(P_INP_BG,  22, 16, 50);
    dac(P_INP_BD,  90, 60,150);

    /* 메뉴 */
    dac(P_MNU_BG,  20, 14, 45);
    dac(P_MNU_HL,  80, 40,160);
    dac(P_MNU_SEP, 60, 40,100);

    /* 기타 */
    dac(P_ACCENT,  60,220,255);   /* 시안 액센트 */
    dac(P_STAR0,  255,255,255);
    dac(P_STAR1,  200,150,255);   /* 보라 별 */
    dac(P_STAR2,   80,200,255);   /* 시안 별 */
}

/* ─── 8×8 비트맵 폰트 ─── */
static const uint8_t M_FONT[95][8]={
/* ' ' */{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
/* '!' */{0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x00},
/* '"' */{0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
/* '#' */{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
/* '$' */{0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
/* '%' */{0x63,0x66,0x0C,0x18,0x30,0x66,0x63,0x00},
/* '&' */{0x1C,0x36,0x1C,0x38,0x6F,0x66,0x3B,0x00},
/* ''' */{0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
/* '(' */{0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
/* ')' */{0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
/* '*' */{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
/* '+' */{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
/* ',' */{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
/* '-' */{0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
/* '.' */{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
/* '/' */{0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
/* '0' */{0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
/* '1' */{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
/* '2' */{0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},
/* '3' */{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
/* '4' */{0x06,0x0E,0x1E,0x66,0x7F,0x06,0x06,0x00},
/* '5' */{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
/* '6' */{0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00},
/* '7' */{0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00},
/* '8' */{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
/* '9' */{0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00},
/* ':' */{0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
/* ';' */{0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
/* '<' */{0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
/* '=' */{0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
/* '>' */{0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
/* '?' */{0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},
/* '@' */{0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00},
/* 'A' */{0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00},
/* 'B' */{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
/* 'C' */{0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
/* 'D' */{0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
/* 'E' */{0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00},
/* 'F' */{0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00},
/* 'G' */{0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00},
/* 'H' */{0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
/* 'I' */{0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
/* 'J' */{0x1E,0x0C,0x0C,0x0C,0x6C,0x6C,0x38,0x00},
/* 'K' */{0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
/* 'L' */{0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
/* 'M' */{0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
/* 'N' */{0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},
/* 'O' */{0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
/* 'P' */{0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},
/* 'Q' */{0x3C,0x66,0x66,0x66,0x6E,0x3C,0x0E,0x00},
/* 'R' */{0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00},
/* 'S' */{0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
/* 'T' */{0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
/* 'U' */{0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
/* 'V' */{0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
/* 'W' */{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
/* 'X' */{0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
/* 'Y' */{0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
/* 'Z' */{0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},
/* '[' */{0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
/* '\\'*/{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
/* ']' */{0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
/* '^' */{0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
/* '_' */{0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
/* '`' */{0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
/* 'a' */{0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00},
/* 'b' */{0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},
/* 'c' */{0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00},
/* 'd' */{0x06,0x06,0x3E,0x66,0x66,0x66,0x3B,0x00},
/* 'e' */{0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
/* 'f' */{0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00},
/* 'g' */{0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x7C},
/* 'h' */{0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},
/* 'i' */{0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
/* 'j' */{0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00},
/* 'k' */{0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
/* 'l' */{0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
/* 'm' */{0x00,0x00,0x63,0x77,0x7F,0x6B,0x63,0x00},
/* 'n' */{0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
/* 'o' */{0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
/* 'p' */{0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
/* 'q' */{0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x06},
/* 'r' */{0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x00},
/* 's' */{0x00,0x00,0x3C,0x60,0x3C,0x06,0x7C,0x00},
/* 't' */{0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00},
/* 'u' */{0x00,0x00,0x66,0x66,0x66,0x66,0x3B,0x00},
/* 'v' */{0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
/* 'w' */{0x00,0x00,0x63,0x6B,0x7F,0x77,0x63,0x00},
/* 'x' */{0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
/* 'y' */{0x00,0x00,0x66,0x66,0x3E,0x06,0x66,0x3C},
/* 'z' */{0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},
/* '{' */{0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
/* '|' */{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
/* '}' */{0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
/* '~' */{0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* mode13h는 VGA plane2(폰트)를 픽셀로 덮어씀 – 텍스트 모드 복귀 후 반드시 복원 */
static void gfx_restore_font(void){
    static const uint8_t blank[16] = {0};
    /* 0x00-0x1F: 빈 칸 */
    for(int c=0;c<0x20;c++) vga_write_glyph8x16((uint8_t)c, blank);
    /* 0x20-0x7E: M_FONT (8×8) → 8×16 중앙 정렬 */
    for(int c=0x20;c<=0x7E;c++){
        uint8_t g16[16] = {0};
        const uint8_t *f = M_FONT[c - 0x20];
        for(int r=0;r<8;r++) g16[r+4] = f[r];
        vga_write_glyph8x16((uint8_t)c, g16);
    }
    vga_write_glyph8x16(0x7F, blank);
    /* 0x80-0xFF: 한글 캐시 무효화 후 폰트 재초기화 */
    for(int i=0;i<HGLYPH_PAIRS;i++) hcache_used[i]=0;
    hcache_hand = 0;
    init_jamo_fonts();
}

static void gfx_ch(int x,int y,char c,uint8_t fg,uint8_t bg){
    if(c<0x20||c>0x7E) c=' ';
    const uint8_t *g=M_FONT[(uint8_t)(c-0x20)];
    for(int r=0;r<8;r++){
        uint8_t b=g[r];
        for(int col=0;col<8;col++)
            px(x+col,y+r,(b&(0x80>>col))?fg:bg);
    }
}
static void gfx_str(int x,int y,const char*s,uint8_t fg,uint8_t bg){
    while(*s){gfx_ch(x,y,*s++,fg,bg);x+=8;}
}
/* 투명배경 글자 (배경픽셀 건드리지 않음) */
static void gfx_str_t(int x,int y,const char*s,uint8_t fg){
    while(*s){
        if(*s>=0x20&&*s<=0x7E){
            const uint8_t*g=M_FONT[(uint8_t)(*s-0x20)];
            for(int r=0;r<8;r++){uint8_t b=g[r];for(int c=0;c<8;c++) if(b&(0x80>>c)) px(x+c,y+r,fg);}
        }
        s++;x+=8;
    }
}
static int gfx_strw(const char*s){int n=0;while(*s++){n++;}return n*8;}

/* ─── 한글 16×16 픽셀 렌더 (mode13h 프레임버퍼 직접) ─── */
static void gfx_hangul_glyph(int x, int y, int L, int V, int jT, uint8_t fg){
    uint16_t g16[16];
    compose_hangul_16x16(g16, L, V, jT);
    for(int r=0;r<16;r++){
        uint16_t row2 = g16[r];
        for(int c=0;c<16;c++)
            if(row2 & (0x8000u >> c))
                px(x+c, y+r, fg);
    }
}

/* ─── UTF-8 문자열 렌더 (ASCII 8px + 한글 16px) ─── */
static int gfx_utf8_str_t(int x, int y, const char *s, uint8_t fg){
    const uint8_t *p = (const uint8_t*)s;
    int cx = x;
    while(*p){
        uint32_t cp=0;
        int n = utf8_decode_one(p, &cp);
        if(n<=0){ p++; continue; }
        p += (uint32_t)n;
        if(cp >= 0xAC00 && cp <= 0xD7A3){
            uint32_t S = cp - 0xAC00;
            int L2 = (int)(S / (21*28));
            int V2 = (int)((S / 28) % 21);
            int jT2 = (int)(S % 28);
            gfx_hangul_glyph(cx, y, L2, V2, jT2, fg);
            cx += 16;
        } else if(cp >= 0x3131 && cp <= 0x3163){
            /* 독립 자모 직접 픽셀 렌더 */
            uint16_t g16[16];
            draw_standalone_jamo_16x16(cp, g16);
            for(int r=0;r<16;r++){
                uint16_t rowb = g16[r];
                for(int c2=0;c2<16;c2++) if(rowb&(0x8000u>>c2)) px(cx+c2,y+r,fg);
            }
            cx += 16;
        } else if(cp >= 0x20 && cp <= 0x7E){
            const uint8_t *g = M_FONT[cp - 0x20];
            for(int r=0;r<8;r++){
                uint8_t b=g[r];
                for(int c2=0;c2<8;c2++) if(b&(0x80>>c2)) px(cx+c2, y+r, fg);
            }
            cx += 8;
        } else {
            cx += 8; /* unknown – skip */
        }
    }
    return cx - x; /* 렌더된 총 너비 */
}
/* 한글 포함 문자열 너비 계산 */
static int gfx_utf8_strw(const char *s){
    const uint8_t *p = (const uint8_t*)s;
    int w = 0;
    while(*p){
        uint32_t cp=0;
        int n = utf8_decode_one(p, &cp);
        if(n<=0){ p++; w+=8; continue; }
        p += (uint32_t)n;
        w += (cp >= 0xAC00 && cp <= 0xD7A3) ? 16 : 8;
    }
    return w;
}

/* ─── 프리미티브 ─── */
static void gfx_rect(int x,int y,int w,int h,uint8_t c){
    for(int r=0;r<h;r++) for(int cc=0;cc<w;cc++) px(x+cc,y+r,c);
}
static void gfx_hline(int x,int y,int w,uint8_t c){for(int i=0;i<w;i++)px(x+i,y,c);}
static void gfx_vline(int x,int y,int h,uint8_t c){for(int i=0;i<h;i++)px(x,y+i,c);}
static void gfx_border(int x,int y,int w,int h,uint8_t c){
    gfx_hline(x,y,w,c); gfx_hline(x,y+h-1,w,c);
    gfx_vline(x,y,h,c); gfx_vline(x+w-1,y,h,c);
}

/* ─── 둥근 사각형 (Auroral 곡선 UI) ─── */
static void gfx_rounded_rect(int x,int y,int w,int h,int r2,uint8_t c){
    /* 내부 채우기 */
    gfx_rect(x+r2,y,w-2*r2,h,c);
    gfx_rect(x,y+r2,r2,h-2*r2,c);
    gfx_rect(x+w-r2,y+r2,r2,h-2*r2,c);
    /* 모서리 타원으로 채우기 */
    for(int dy=-r2;dy<=r2;dy++) for(int dx=-r2;dx<=r2;dx++){
        if(dx*dx+dy*dy<=r2*r2){
            px(x+r2+dx,     y+r2+dy,     c);
            px(x+w-1-r2+dx, y+r2+dy,     c);
            px(x+r2+dx,     y+h-1-r2+dy, c);
            px(x+w-1-r2+dx, y+h-1-r2+dy, c);
        }
    }
}
static void gfx_rounded_border(int x,int y,int w,int h,int r2,uint8_t c){
    gfx_hline(x+r2,y,w-2*r2,c);
    gfx_hline(x+r2,y+h-1,w-2*r2,c);
    gfx_vline(x,y+r2,h-2*r2,c);
    gfx_vline(x+w-1,y+r2,h-2*r2,c);
    /* 모서리 원호 */
    for(int i=0;i<=r2;i++){
        int j=(int)((r2*r2-i*i)); /* j^2 = r^2 - i^2 */
        int jj=0; while((jj+1)*(jj+1)<=j) jj++;
        px(x+r2-i,     y+r2-jj,     c);
        px(x+w-1-r2+i, y+r2-jj,     c);
        px(x+r2-i,     y+h-1-r2+jj, c);
        px(x+w-1-r2+i, y+h-1-r2+jj, c);
        px(x+r2-jj,    y+r2-i,      c);
        px(x+w-1-r2+jj,y+r2-i,      c);
        px(x+r2-jj,    y+h-1-r2+i,  c);
        px(x+w-1-r2+jj,y+h-1-r2+i,  c);
    }
}
/* ─── 작은 원(버튼용) 채우기 ─── */
static void gfx_circle(int cx,int cy,int r2,uint8_t c){
    for(int dy=-r2;dy<=r2;dy++) for(int dx=-r2;dx<=r2;dx++)
        if(dx*dx+dy*dy<=r2*r2) px(cx+dx,cy+dy,c);
}
/* 글라스/그라디언트 타이틀바 */
static void gfx_titlebar(int x,int y,int w,int h,int active){
    uint8_t t=active?P_TTL_A0:P_TTL_IN;
    uint8_t b=active?P_TTL_A1:P_TTL_IN;
    int mid=h/2;
    for(int r=0;r<h;r++){
        uint8_t c=(r<mid)?t:b;
        gfx_hline(x,y+r,w,c);
    }
    /* 상단 시안 하이라이트 라인 */
    if(active) gfx_hline(x,y,w,P_ACCENT);
}
/* 창 그림자 */
static void gfx_shadow(int x,int y,int w,int h){
    for(int i=1;i<=3;i++){
        for(int r=i;r<h+i;r++) if(x+w-1+i<SCR_W&&y+r<SCR_H) px(x+w-1+i,y+r,P_WIN_SHD);
        for(int cc=i;cc<w+i;cc++) if(x+cc<SCR_W&&y+h-1+i<SCR_H) px(x+cc,y+h-1+i,P_WIN_SHD);
    }
}

/* ─── 창 시스템 ─── */
#define MW_MAX    6
#define TT_NONE   0
#define TT_FILES  1
#define TT_TERM   2
#define TT_NOTE   3   /* 노트패드 */
#define TT_VIEW   4   /* 파일 뷰어 */
#define TT_INFO   5   /* 시스템 정보 */
#define TT_CON    6   /* 콘솔 (ELF 실행) */

#define TBAR_H  13   /* 타이틀바 높이 */
#define DOCK_H  20   /* 도크 높이 */
#define MBAR_H  11   /* 메뉴바 높이 */

/* 터미널 출력 히스토리 */
#define TERM_H  16
#define TERM_W  38

/* 노트패드 */
#define NOTE_LINES  20
#define NOTE_COLS   38

/* 파일뷰어 */
#define VIEW_LINES  14

/* 파일 목록 */
#define FM_MAX  80

typedef struct {
    int open, type, focused;
    int x,y,w,h;
    int drag, ddx, ddy;
    /* 최대화/최소화 상태 */
    int maximized;       /* 1=최대화 */
    int minimized;       /* 1=최소화 (도크에만 표시) */
    int pre_x,pre_y,pre_w,pre_h; /* 최대화 전 원래 크기 */
    /* 파일매니저 */
    int fm_sel, fm_top;
    uint32_t fm_dir;
    /* 터미널 */
    char te_buf[TERM_H][TERM_W+1];
    int  te_n;
    char te_in[64];
    int  te_ilen;
    /* 노트패드 */
    char np_lines[NOTE_LINES][NOTE_COLS+1];
    int  np_nlines;
    int  np_cur, np_col;   /* 현재 라인, 컬럼 */
    int  np_top;
    char np_fname[24];
    int  np_dirty;
    /* 뷰어 */
    uint8_t *vw_buf;       /* 파일 내용 포인터 */
    uint32_t vw_size;
    int  vw_top;
    char vw_fname[24];
    /* 콘솔 */
    char co_buf[TERM_H][TERM_W+1];
    int  co_n;
    char co_fname[24];
    int  co_running;
} mw_t;

typedef struct { char name[24]; uint32_t size; int is_dir; } fm_ent_t;

static mw_t       g_mw[MW_MAX];
static int        g_focus=-1;
static fm_ent_t   g_fm_ents[FM_MAX];
static int        g_fm_cnt=0;
/* g_mbar_msg is declared earlier for syscall access */
/* 뷰어 전용 파일 버퍼 (0x280000) */
#define VIEW_BUF  ((uint8_t*)0x280000)

/* 클럭 */
static char g_clock[9]="00:00:00";
static int  g_ck_tick=0;
static void clock_tick(void){
    if(++g_ck_tick<500)return; g_ck_tick=0;
    rtc_clock_str(g_clock); g_clock[8]=0;
}

/* ─── uint → 문자열 ─── */
static int u32str(uint32_t v,char*b,int cap){
    if(!v){if(cap>1){b[0]='0';b[1]=0;}return 1;}
    char t[12];int n=0;
    while(v){t[n++]=(char)('0'+v%10);v/=10;}
    if(n>=cap){b[0]='?';b[1]=0;return 1;}
    for(int i=0;i<n;i++) b[i]=t[n-1-i];
    b[n]=0; return n;
}

/* ─── 파일 목록 로드 ─── */
static void fm_load(int wid){
    mw_t*w=&g_mw[wid];
    g_fm_cnt=0;
    ata_read28(w->fm_dir,DIR_SECTORS,dirbuf);
    for(int i=0;i<DIR_ENTRIES&&g_fm_cnt<FM_MAX;i++){
        dirent_t*e=(dirent_t*)(dirbuf+i*sizeof(dirent_t));
        if(!e->name[0]) continue;
        strncpy(g_fm_ents[g_fm_cnt].name,e->name,23);
        g_fm_ents[g_fm_cnt].size=e->size;
        g_fm_ents[g_fm_cnt].is_dir=(e->size==0);
        g_fm_cnt++;
    }
}

/* ─── 창 열기 ─── */
static void mw_open(int wid,int type){
    mw_t*w=&g_mw[wid];
    w->type=type; w->open=1; w->drag=0;
    /* 기본 크기/위치 */
    switch(type){
    case TT_FILES: w->w=200;w->h=130;w->x=10+wid*6;w->y=MBAR_H+4+wid*6;break;
    case TT_TERM:  w->w=195;w->h=120;w->x=60+wid*5;w->y=MBAR_H+8+wid*5;break;
    case TT_NOTE:  w->w=200;w->h=130;w->x=20+wid*6;w->y=MBAR_H+6+wid*6;break;
    case TT_VIEW:  w->w=200;w->h=120;w->x=15+wid*5;w->y=MBAR_H+5+wid*5;break;
    case TT_INFO:  w->w=170;w->h=110;w->x=75+wid*4;w->y=MBAR_H+6+wid*4;break;
    case TT_CON:   w->w=200;w->h=110;w->x=40+wid*5;w->y=MBAR_H+8+wid*5;break;
    default:       w->w=160;w->h=100;w->x=50;w->y=30;break;
    }
    /* clamp */
    if(w->x+w->w>SCR_W-2) w->x=SCR_W-2-w->w;
    if(w->y<MBAR_H) w->y=MBAR_H;
    if(w->y+w->h>SCR_H-DOCK_H-2) w->y=SCR_H-DOCK_H-2-w->h;

    if(type==TT_FILES){
        w->fm_dir=cur_dir; w->fm_sel=0; w->fm_top=0;
        fm_load(wid);
    }
    if(type==TT_TERM){
        w->te_n=0; w->te_in[0]=0; w->te_ilen=0;
        /* 환영 */
        strncpy(w->te_buf[0],"methane terminal  v2.0",TERM_W);
        strncpy(w->te_buf[1],"type 'help' for commands",TERM_W);
        w->te_n=2;
    }
    if(type==TT_NOTE){
        w->np_nlines=1; w->np_cur=0; w->np_col=0; w->np_top=0;
        w->np_fname[0]=0; w->np_dirty=0;
        w->np_lines[0][0]=0;
    }
    if(type==TT_CON){
        w->co_n=0; w->co_fname[0]=0; w->co_running=0;
    }
    g_focus=wid;
}

static void mw_close(int wid){
    g_mw[wid].open=0;
    g_focus=-1;
    for(int i=MW_MAX-1;i>=0;i--) if(g_mw[i].open){g_focus=i;break;}
}

/* ─── 도크 아이콘 그리기 ─── */
#define DOCK_ICON_SZ  16
static void draw_dock_icon(int x,int y,uint8_t color,const char*label,int active){
    /* 아이콘 배경 */
    uint8_t bg=active?P_WIN_LITE:P_WIN_DRK;
    gfx_rect(x,y,DOCK_ICON_SZ,DOCK_ICON_SZ,bg);
    gfx_border(x,y,DOCK_ICON_SZ,DOCK_ICON_SZ,active?P_SEL0:P_WIN_BRD);
    /* 색상 심볼 */
    gfx_rect(x+3,y+3,DOCK_ICON_SZ-6,DOCK_ICON_SZ-6,color);
    /* 레이블 */
    if(label&&label[0]){
        int lw=gfx_strw(label);
        int lx=x+(DOCK_ICON_SZ-lw)/2;
        gfx_str_t(lx,y+DOCK_ICON_SZ+1,label,P_TXT_LT);
    }
    /* 활성 점 */
    if(active){ px(x+DOCK_ICON_SZ/2,y+DOCK_ICON_SZ+9,P_ACCENT); }
}

/* ─── 메뉴바 ─── */
static void draw_mbar(void){
    /* 배경 */
    for(int y2=0;y2<MBAR_H;y2++){
        uint8_t c=(y2<MBAR_H-1)?P_WIN_BG:P_WIN_BRD;
        gfx_hline(0,y2,SCR_W,c);
    }
    /* 로고 */
    gfx_str_t(4,2,"methane",P_TTL_A0);
    /* 메뉴 항목 */
    gfx_str_t(64,2,"File",P_TXT_DK);
    gfx_str_t(96,2,"Apps",P_TXT_DK);
    gfx_str_t(128,2,"View",P_TXT_DK);
    /* 메시지 */
    gfx_str_t(160,2,g_mbar_msg,P_TXT_DK);
    /* 시계 */
    int cw=gfx_strw(g_clock);
    gfx_str_t(SCR_W-cw-4,2,g_clock,P_TXT_DK);
}

/* ─── 도크 ─── */
struct { const char*lbl; uint8_t col; int type; } g_dock[]={
    {"Files",P_ICO_DIR,TT_FILES},
    {"Term", P_ICO_ELF,TT_TERM},
    {"Note", P_ICO_TXT,TT_NOTE},
    {"Info", P_ICO_UNK,TT_INFO},
};
#define DOCK_ITEMS  4

static void draw_dock(void){
    int dy=SCR_H-DOCK_H;
    /* 도크 배경 */
    for(int y2=dy;y2<SCR_H;y2++) gfx_hline(0,y2,SCR_W,P_DOCK_BG);
    gfx_hline(0,dy,SCR_W,P_DOCK_BD);

    int total=DOCK_ITEMS*(DOCK_ICON_SZ+18);
    int sx=(SCR_W-total)/2;
    for(int i=0;i<DOCK_ITEMS;i++){
        int ix=sx+i*(DOCK_ICON_SZ+18);
        /* 해당 타입 창이 열려있나? */
        int active=0;
        for(int j=0;j<MW_MAX;j++) if(g_mw[j].open&&g_mw[j].type==g_dock[i].type) active=1;
        draw_dock_icon(ix,dy+2,g_dock[i].col,g_dock[i].lbl,active);
        /* 최소화된 창은 점으로 표시 */
        int minimized_any=0;
        for(int j=0;j<MW_MAX;j++) if(g_mw[j].open&&g_mw[j].type==g_dock[i].type&&g_mw[j].minimized) minimized_any=1;
        if(minimized_any) gfx_rect(ix+DOCK_ICON_SZ/2-1,dy+DOCK_ICON_SZ+4,2,2,P_BTN_Y);
    }
}

/* ─── 창 프레임 그리기 ─── */
static void draw_win_frame(int wid){
    mw_t*w=&g_mw[wid];
    if(!w->open)return;
    if(w->minimized)return;  /* 최소화: 도크에만 표시 */
    int focused=(g_focus==wid);

    /* 그림자 */
    gfx_shadow(w->x,w->y,w->w,w->h);

    /* 창 본체 - 둥근 사각형 */
    gfx_rounded_rect(w->x,w->y,w->w,w->h,4,P_WIN_BG);
    gfx_rounded_border(w->x,w->y,w->w,w->h,4,P_WIN_BRD);

    /* 타이틀바 */
    gfx_titlebar(w->x+1,w->y+1,w->w-2,TBAR_H,focused);

    /* macOS 스타일 버튼 – 8px 원, 클릭하기 충분한 크기 */
    int bcy = w->y + 1 + TBAR_H/2;
    /* 닫기 – 핑크 원 */
    gfx_circle(w->x+8, bcy, 4, P_BTN_R);
    /* 최소화 – 노랑 원 */
    gfx_circle(w->x+20, bcy, 4, P_BTN_Y);
    /* 최대화 – 민트 원 */
    gfx_circle(w->x+32, bcy, 4, P_BTN_G);

    /* 타이틀 텍스트 */
    static const char*ttls[]={"","File Manager","Terminal","Notepad","File Viewer","System Info","Console"};
    const char*title=(w->type>=0&&w->type<=6)?ttls[w->type]:"?";
    /* 파일명 추가 */
    char tbuf[36]; int ti=0;
    const char*t2=title; while(*t2&&ti<20) tbuf[ti++]=*t2++;
    /* notepad/viewer 에서 파일명 */
    if(w->type==TT_NOTE&&w->np_fname[0]){
        tbuf[ti++]=' '; tbuf[ti++]='-'; tbuf[ti++]=' ';
        const char*fn=w->np_fname; while(*fn&&ti<34) tbuf[ti++]=*fn++;
        if(w->np_dirty){ tbuf[ti++]='*'; }
    }
    if(w->type==TT_VIEW&&w->vw_fname[0]){
        tbuf[ti++]=' '; tbuf[ti++]='-'; tbuf[ti++]=' ';
        const char*fn=w->vw_fname; while(*fn&&ti<34) tbuf[ti++]=*fn++;
    }
    if(w->type==TT_CON&&w->co_fname[0]){
        tbuf[ti++]=' '; tbuf[ti++]='-'; tbuf[ti++]=' ';
        const char*fn=w->co_fname; while(*fn&&ti<34) tbuf[ti++]=*fn++;
    }
    tbuf[ti]=0;
    int tw=gfx_strw(tbuf);
    int tx=w->x+(w->w-tw)/2; if(tx<w->x+40) tx=w->x+40;
    gfx_str_t(tx,w->y+1+(TBAR_H-8)/2,tbuf,P_TXT_W);

    /* 타이틀바 하단 구분선 */
    gfx_hline(w->x+1,w->y+1+TBAR_H,w->w-2,P_WIN_DRK);
}

/* ─── 클라이언트 영역 좌표 ─── */
#define CLI_X(w)  ((w)->x+2)
#define CLI_Y(w)  ((w)->y+1+TBAR_H+1)
#define CLI_W(w)  ((w)->w-4)
#define CLI_H(w)  ((w)->h-TBAR_H-4)

/* ─── 파일 매니저 내용 ─── */
static void draw_files(int wid){
    mw_t*w=&g_mw[wid];
    int cx=CLI_X(w),cy=CLI_Y(w),cw=CLI_W(w),ch=CLI_H(w);
    gfx_rect(cx,cy,cw,ch,P_WIN_BG);

    /* 경로 바 */
    gfx_rect(cx,cy,cw,10,P_WIN_DRK);
    gfx_str_t(cx+3,cy+1,"/",P_TXT_DK);
    /* TODO: 경로 표시 */

    /* 헤더 */
    int hy=cy+11;
    gfx_rect(cx,hy,cw,9,P_WIN_DRK);
    gfx_str_t(cx+18,hy+1,"Name",P_TXT_DK);
    gfx_str_t(cx+cw-38,hy+1,"Size",P_TXT_DK);
    gfx_hline(cx,hy+9,cw,P_WIN_BRD);

    /* 항목들 */
    int row_h=9;
    int list_y=hy+10;
    int visible=(ch-32)/row_h; if(visible<1)visible=1;

    for(int i=0;i<visible;i++){
        int idx=w->fm_top+i;
        if(idx>=g_fm_cnt) break;
        int ry=list_y+i*row_h;
        int sel=(idx==w->fm_sel);

        uint8_t rbg=sel?P_SEL0:((i%2)?P_WIN_BG:P_WIN_LITE);
        gfx_rect(cx,ry,cw,row_h,rbg);

        fm_ent_t*e=&g_fm_ents[idx];

        /* 아이콘 */
        uint8_t ic=e->is_dir?P_ICO_DIR:(
            (e->name[0]&&e->name[strlen(e->name)-1]=='f'&&e->name[strlen(e->name)-2]=='l'&&e->name[strlen(e->name)-3]=='e')?P_ICO_ELF:
            P_ICO_TXT);
        gfx_rect(cx+3,ry+1,7,7,ic);

        /* 이름 (최대 18자) */
        char nb[20]; int ni=0;
        while(e->name[ni]&&ni<18){nb[ni]=e->name[ni];ni++;}
        nb[ni]=0;
        uint8_t tc=sel?P_TXT_W:(e->is_dir?P_SEL0:P_TXT_BLK);
        gfx_str_t(cx+13,ry+1,nb,tc);

        /* 크기 */
        if(!e->is_dir){
            char sz[10]; u32str(e->size,sz,10);
            gfx_str_t(cx+cw-42,ry+1,sz,sel?P_TXT_LT:P_TXT_DK);
        } else {
            gfx_str_t(cx+cw-38,ry+1,"<DIR>",sel?P_TXT_LT:P_TXT_DK);
        }
    }

    /* 상태바 */
    int sb=w->y+w->h-11;
    gfx_rect(w->x+1,sb,w->w-2,10,P_WIN_DRK);
    gfx_hline(w->x+1,sb,w->w-2,P_WIN_BRD);
    char sb2[36]; int si=0;
    if(g_fm_cnt>0&&w->fm_sel<g_fm_cnt){
        sb2[si++]=' ';
        const char*n=g_fm_ents[w->fm_sel].name;
        while(*n&&si<28) sb2[si++]=*n++;
    }
    sb2[si]=0;
    gfx_str_t(w->x+4,sb+2,sb2,P_TXT_DK);
    /* 개수 */
    char cb[8]="("; int ci=1;
    u32str((uint32_t)g_fm_cnt,cb+1,6); while(cb[ci])ci++;
    cb[ci]=')'; cb[ci+1]=0;
    gfx_str_t(w->x+w->w-ci*8-8,sb+2,cb,P_TXT_DK);
}

/* ─── GUI IME forward declarations (정의는 아래 GUI IME 섹션) ─── */
static int gui_ime_on;          /* 0=ENG 1=HAN – 아래에서 초기화 */
static int gui_ime_L, gui_ime_V, gui_ime_T;
static void gui_ime_reset(void);
static int  gui_ime_preview(char *out4);

/* ─── 터미널 내용 ─── */
static void draw_term(int wid){
    mw_t*w=&g_mw[wid];
    int cx=CLI_X(w),cy=CLI_Y(w),cw=CLI_W(w),ch=CLI_H(w);
    gfx_rect(cx,cy,cw,ch,P_TERM_BG);

    int line_h=8;
    int input_y=cy+ch-13;
    int hist_h=input_y-cy-1;
    int vis=hist_h/line_h;

    int start=w->te_n-vis; if(start<0)start=0;
    for(int i=0;i<vis&&start+i<w->te_n;i++){
        const char*line=w->te_buf[start+i];
        uint8_t fc=P_TERM_GR;
        if(line[0]=='$') fc=P_TERM_CY;
        else if(line[0]=='!'||line[0]=='E') fc=P_TERM_RD;
        else if(line[0]==' '&&line[1]=='>'||line[0]=='>') fc=P_TERM_YL;
        /* UTF-8 렌더 (한글 포함) */
        gfx_utf8_str_t(cx+2, cy+1+i*line_h, line, fc);
    }

    /* 입력 구분선 */
    gfx_hline(cx,input_y,cw,P_DOCK_BD);
    gfx_rounded_rect(cx,input_y+1,cw,12,3,P_WIN_DRK);

    /* 프롬프트 */
    int px2=cx+2;
    gfx_str_t(px2,input_y+3,"$ ",P_TERM_CY); px2+=16;

    /* 확정된 입력 */
    px2 += gfx_utf8_str_t(px2, input_y+3, w->te_in, P_TXT_W);

    /* 조합 중인 음절 미리보기 (노란색 밑줄) */
    if(g_focus==wid && gui_ime_on){
        char prev[4]={0,0,0,0};
        if(gui_ime_preview(prev)){
            gfx_utf8_str_t(px2, input_y+3, prev, P_TERM_YL);
            gfx_hline(px2, input_y+11, 16, P_TERM_YL); /* 밑줄 */
            px2 += 16;
        }
    }

    /* 커서 */
    if(g_focus==wid){
        if(px2 < cx+cw-4) gfx_rect(px2, input_y+3, 2, 8, P_TERM_GR);
    }

    /* IME 상태 표시 */
    if(g_focus==wid && gui_ime_on)
        gfx_str_t(cx+cw-24, input_y+3, "HAN", P_ACCENT);
}

/* ─── 노트패드 내용 ─── */
static void draw_note(int wid){
    mw_t*w=&g_mw[wid];
    int cx=CLI_X(w),cy=CLI_Y(w),cw=CLI_W(w),ch=CLI_H(w);

    /* 툴바 */
    gfx_rect(cx,cy,cw,10,P_WIN_DRK);
    gfx_str_t(cx+3,cy+1,"[New]",P_TXT_DK);
    gfx_str_t(cx+43,cy+1,"[Open]",P_TXT_DK);
    gfx_str_t(cx+91,cy+1,"[Save]",P_TXT_DK);
    /* IME 상태 */
    if(g_focus==wid)
        gfx_str_t(cx+cw-28,cy+1, gui_ime_on?"[HAN]":"[ENG]",
                  gui_ime_on?P_ACCENT:P_TXT_DK);
    gfx_hline(cx,cy+10,cw,P_WIN_BRD);

    /* 편집 영역 */
    int ea_y=cy+11;
    int ea_h=ch-11;
    gfx_rect(cx,ea_y,cw,ea_h,P_INP_BG);

    int line_h=10;  /* 한글 16px에 맞게 10px 줄간격 (8px 기준이지만 여유) */
    int vis=ea_h/line_h;
    for(int i=0;i<vis&&w->np_top+i<w->np_nlines;i++){
        int li=w->np_top+i;
        int ly=ea_y+i*line_h;
        /* 현재 라인 강조 */
        if(li==w->np_cur) gfx_rect(cx,ly,cw,line_h,P_SEL1);

        /* 확정된 텍스트 */
        int tx = gfx_utf8_str_t(cx+2,ly, w->np_lines[li], P_TXT_BLK);

        /* 현재 커서 라인 + 포커스: 조합 중 미리보기 */
        if(li==w->np_cur && g_focus==wid){
            /* 커서 위치까지 너비 계산 */
            char tmp[NOTE_COLS+1];
            int ci2=0, nb=0;
            const char *lp=w->np_lines[li];
            /* np_col은 바이트 위치 */
            int col_b = w->np_col;
            /* 커서까지 UTF-8 너비 */
            int cx2=cx+2;
            const uint8_t *pp=(const uint8_t*)lp;
            int bi=0;
            while(*pp && bi < col_b){
                uint32_t cp2=0;
                int nn=utf8_decode_one(pp,&cp2);
                if(nn<=0){pp++;bi++;continue;}
                pp+=(uint32_t)nn; bi+=nn;
                cx2 += (cp2>=0xAC00&&cp2<=0xD7A3)?16:8;
            }
            (void)tx;(void)tmp;(void)ci2;(void)nb;
            /* IME 미리보기 */
            if(gui_ime_on){
                char prev[4]={0,0,0,0};
                if(gui_ime_preview(prev)){
                    gfx_utf8_str_t(cx2,ly,prev,P_ACCENT);
                    gfx_hline(cx2,ly+line_h-2,16,P_ACCENT);
                    cx2+=16;
                }
            }
            /* 커서 바 */
            gfx_rect(cx2,ly,1,line_h,P_SEL0);
        }
    }
}

/* ─── 파일 뷰어 내용 ─── */
static void draw_viewer(int wid){
    mw_t*w=&g_mw[wid];
    int cx=CLI_X(w),cy=CLI_Y(w),cw=CLI_W(w),ch=CLI_H(w);
    gfx_rect(cx,cy,cw,ch,P_INP_BG);

    if(!w->vw_buf||!w->vw_size){
        gfx_str_t(cx+4,cy+4,"(empty)",P_TXT_DK);
        return;
    }

    int line_h=8;
    int vis=(ch-4)/line_h;
    /* 텍스트 라인으로 분할 출력 */
    int line=0, pos=0;
    /* skip w->vw_top lines */
    while(pos<(int)w->vw_size&&line<w->vw_top){
        if(w->vw_buf[pos]=='\n') line++;
        pos++;
    }
    int drawn=0;
    int lx=cx+2;
    char lbuf[TERM_W+1]; int lpos=0;
    while(pos<=(int)w->vw_size&&drawn<vis){
        uint8_t c=(pos<(int)w->vw_size)?w->vw_buf[pos]:'\n';
        if(c=='\n'||c=='\r'||pos==(int)w->vw_size){
            lbuf[lpos]=0;
            gfx_str_t(lx,cy+2+drawn*line_h,lbuf,P_TXT_BLK);
            drawn++; lpos=0;
        } else {
            if(c>=0x20&&c<=0x7E&&lpos<TERM_W-1) lbuf[lpos++]=(char)c;
            else if(lpos<TERM_W-1) lbuf[lpos++]='.';
        }
        pos++;
    }

    /* 스크롤바 */
    if(w->vw_size>0){
        int sbx=w->x+w->w-5;
        int sby=CLI_Y(w);
        int sbh=ch;
        gfx_vline(sbx,sby,sbh,P_SCR_BG);
        /* 썸 위치 */
        int tpos=w->vw_top*sbh/(w->vw_size/40+1);
        if(tpos>sbh-6)tpos=sbh-6;
        gfx_rect(sbx,sby+tpos,4,6,P_SCR_TH);
    }
}

/* ─── 콘솔 창 내용 ─── */
static void draw_console(int wid){
    mw_t*w=&g_mw[wid];
    int cx=CLI_X(w),cy=CLI_Y(w),cw=CLI_W(w),ch=CLI_H(w);
    gfx_rect(cx,cy,cw,ch,P_TERM_BG);

    if(!w->co_fname[0]){
        gfx_str_t(cx+4,cy+4,"No program loaded",P_TXT_DK);
        return;
    }

    int line_h=8;
    int vis=(ch-4)/line_h;
    int start=w->co_n-vis; if(start<0)start=0;
    for(int i=0;i<vis&&start+i<w->co_n;i++){
        const char*line=w->co_buf[start+i];
        uint8_t fc=(line[0]=='>')?P_TERM_YL:(line[0]=='!'?P_TERM_RD:P_TERM_GR);
        gfx_str_t(cx+2,cy+2+i*line_h,line,fc);
    }

    /* 상태 */
    int sy=w->y+w->h-11;
    gfx_hline(w->x+1,sy,w->w-2,P_WIN_BRD);
    gfx_rect(w->x+1,sy+1,w->w-2,9,P_WIN_DRK);
    gfx_str_t(w->x+4,sy+2,w->co_running?"Running...":"Finished",P_TXT_DK);
}

/* ─── 시스템 정보 ─── */
static void draw_sysinfo(int wid){
    mw_t*w=&g_mw[wid];
    int cx=CLI_X(w),cy=CLI_Y(w),cw=CLI_W(w),ch=CLI_H(w);
    gfx_rect(cx,cy,cw,ch,P_WIN_BG);

    /* 헤더 배너 */
    gfx_rect(cx,cy,cw,14,P_TTL_A1);
    gfx_str_t(cx+6,cy+3,"methane 2.0  /  OS/2026",P_TXT_W);

    struct{const char*k;const char*v;uint8_t c;}rows[]={
        {"Kernel ","OS/2026 v3.1",P_ACCENT},
        {"Arch   ","i386 (32-bit)",P_TXT_BLK},
        {"GUI    ","Methane 2.0 GFX",P_ACCENT},
        {"FS     ","2025FS",P_TXT_BLK},
        {"IME    ","Hangul 2-beolsik",P_TXT_BLK},
        {"Mouse  ","PS/2 enabled",P_TXT_BLK},
        {"Clock  ",g_clock,P_ACCENT},
    };
    int nr=(int)(sizeof(rows)/sizeof(rows[0]));
    int ry=cy+17;
    for(int i=0;i<nr&&ry+8<w->y+w->h-4;i++){
        gfx_str_t(cx+4,ry,rows[i].k,P_TXT_DK);
        gfx_str_t(cx+60,ry,rows[i].v,rows[i].c);
        ry+=9;
    }
}

/* ─── 배경 렌더링 (별+그라디언트) ─── */
static uint32_t g_rng=0xCAFE1234;
static uint32_t rng_next(void){g_rng=g_rng*1664525+1013904223;return g_rng;}

static void draw_wallpaper(void){
    /* Aurora 수직 그라디언트 - 보라/핑크/시안 */
    for(int y2=0;y2<SCR_H;y2++){
        uint8_t c;
        if(y2 < SCR_H/3) c=P_DESK0;
        else if(y2 < SCR_H*2/3) c=P_DESK1;
        else c=P_DESK2;
        gfx_hline(0,y2,SCR_W,c);
    }
    /* 오로라 곡선 데코 - 타원형 빛 줄기 */
    /* 대형 시안 타원호 */
    for(int i=0;i<180;i++){
        int x2 = (int)(SCR_W/2 + (SCR_W/3)*i/180);
        int y2 = MBAR_H + 20 + (SCR_H/4)*i/180;
        if((unsigned)x2<SCR_W&&(unsigned)y2<SCR_H) px(x2,y2,P_STAR2);
        x2 = (int)(SCR_W/2 - (SCR_W/3)*i/180);
        if((unsigned)x2<SCR_W&&(unsigned)y2<SCR_H) px(x2,y2,P_ACCENT);
    }
    /* 핑크 곡선 아치 */
    for(int i=0;i<SCR_W;i++){
        int y2 = SCR_H/2 + (int)(20*(i*(SCR_W-i))/(SCR_W*SCR_W/4));
        if((unsigned)y2<SCR_H) px(i,y2,P_STAR1);
        if((unsigned)(y2+1)<SCR_H) px(i,y2+1,P_TTL_A1);
    }
    /* 별 */
    g_rng = 0xCAFE1234;
    for(int i=0;i<100;i++){
        int sx=(int)(rng_next()%SCR_W);
        int sy=(int)(rng_next()%(SCR_H-DOCK_H-MBAR_H))+MBAR_H;
        uint8_t sc=(uint8_t)(rng_next()%3==0?P_STAR0:(rng_next()%2?P_STAR1:P_STAR2));
        px(sx,sy,sc);
        /* 밝은 별은 + 형태 */
        if(rng_next()%5==0){ px(sx+1,sy,sc); px(sx-1,sy,sc); px(sx,sy+1,sc); px(sx,sy-1,sc); }
    }
}

/* ─── 바탕화면 파일 아이콘 ─── */
static void draw_desktop_icons(void){
    /* 루트 디렉토리에서 최대 8개 파일/폴더 아이콘 */
    uint8_t dbuf[512];
    ata_read28(ROOT_LBA, 1, dbuf);
    int ix=6, iy=MBAR_H+6;
    int count=0;
    for(int i=0;i<(int)(512/sizeof(dirent_t))&&count<8;i++){
        dirent_t*e=(dirent_t*)(dbuf+i*sizeof(dirent_t));
        if(!e->name[0]) continue;
        /* 아이콘 배경 - 둥근 반투명 */
        gfx_rounded_rect(ix-2, iy-2, 18, 22, 3, P_WIN_DRK);
        /* 아이콘 심볼 */
        uint8_t ic = e->size==0 ? P_ICO_DIR : P_ICO_TXT;
        gfx_rect(ix+1, iy, 14, 12, ic);
        /* 폴더면 탭 표시 */
        if(e->size==0){ gfx_rect(ix+1, iy-2, 6, 2, P_ICO_DIR); }
        /* 이름 */
        char nm[10]; int ni=0;
        while(e->name[ni]&&ni<8){nm[ni]=e->name[ni];ni++;}nm[ni]=0;
        gfx_str_t(ix-2, iy+13, nm, P_TXT_LT);
        ix += 24;
        if(ix > SCR_W-24){ ix=6; iy+=32; }
        count++;
    }
}

/* ─── 전체 렌더링 ─── */
static void draw_all(void){
    draw_wallpaper();
    draw_desktop_icons();
    draw_mbar();
    /* 비포커스 창 먼저 */
    for(int i=0;i<MW_MAX;i++){
        if(!g_mw[i].open||i==g_focus) continue;
        draw_win_frame(i);
        switch(g_mw[i].type){
        case TT_FILES: draw_files(i);  break;
        case TT_TERM:  draw_term(i);   break;
        case TT_NOTE:  draw_note(i);   break;
        case TT_VIEW:  draw_viewer(i); break;
        case TT_INFO:  draw_sysinfo(i);break;
        case TT_CON:   draw_console(i);break;
        }
    }
    /* 포커스 창 */
    if(g_focus>=0&&g_mw[g_focus].open){
        int i=g_focus;
        draw_win_frame(i);
        switch(g_mw[i].type){
        case TT_FILES: draw_files(i);  break;
        case TT_TERM:  draw_term(i);   break;
        case TT_NOTE:  draw_note(i);   break;
        case TT_VIEW:  draw_viewer(i); break;
        case TT_INFO:  draw_sysinfo(i);break;
        case TT_CON:   draw_console(i);break;
        }
    }
    draw_dock();
}

/* 메뉴바 시계만 부분 갱신 */
static void redraw_clock(void){
    int cw=gfx_strw(g_clock)+4;
    int cx2=SCR_W-cw-4;
    gfx_rect(cx2,0,cw+4,MBAR_H-1,P_WIN_BG);
    gfx_str_t(cx2,2,g_clock,P_TXT_DK);
}

/* ═══════════════════════════════════════════════════════════════════
 *  터미널 명령 실행
 * ═══════════════════════════════════════════════════════════════════ */
static void te_push(mw_t*w,const char*s){
    if(w->te_n>=TERM_H){
        for(int i=0;i<TERM_H-1;i++) strncpy(w->te_buf[i],w->te_buf[i+1],TERM_W);
        w->te_n=TERM_H-1;
    }
    strncpy(w->te_buf[w->te_n++],s,TERM_W);
}

static void te_exec(int wid){
    mw_t*w=&g_mw[wid];
    char*cmd=w->te_in;

    /* 에코 */
    char prompt[TERM_W+1]; int pi=0;
    prompt[pi++]='$'; prompt[pi++]=' ';
    const char*p2=cmd; while(*p2&&pi<TERM_W-1) prompt[pi++]=*p2++;
    prompt[pi]=0;
    te_push(w,prompt);

    if(!cmd[0]){
    } else if(!strcmp(cmd,"help")){
        te_push(w,"ls cd mkdir rm cat");
        te_push(w,"run <file>  open <file>");
        te_push(w,"view <file> edit <file>");
        te_push(w,"clear reboot");
    } else if(!strcmp(cmd,"ls")){
        ata_read28(cur_dir,DIR_SECTORS,dirbuf);
        for(int i=0;i<DIR_ENTRIES;i++){
            dirent_t*e=(dirent_t*)(dirbuf+i*sizeof(dirent_t));
            if(!e->name[0]) continue;
            char buf[TERM_W+1]; int bi=0;
            buf[bi++]=(e->size==0?'d':'f'); buf[bi++]=' ';
            const char*n2=e->name; while(*n2&&bi<TERM_W-1) buf[bi++]=*n2++;
            if(e->size>0){ buf[bi++]=' '; u32str(e->size,buf+bi,8); while(buf[bi])bi++; }
            buf[bi]=0; te_push(w,buf);
        }
    } else if(!strcmp(cmd,"clear")){
        w->te_n=0;
    } else if(!strncmp(cmd,"cd ",3)){
        dirent_t e; if(fs_find(cur_dir,cmd+3,&e)&&e.size==0){
            if(path_depth<MAX_DEPTH-1){path_lba[path_depth]=cur_dir;strncpy(path_name[path_depth],cmd+3,23);path_depth++;}
            cur_dir=e.lba; te_push(w,"> ok");
        } else te_push(w,"! not found");
    } else if(!strcmp(cmd,"cd..")||!strcmp(cmd,"cd ..")){
        if(path_depth>0){path_depth--;cur_dir=path_lba[path_depth];te_push(w,"> ok");}
        else te_push(w,"! already root");
    } else if(!strncmp(cmd,"mkdir ",6)){
        cmd_mkdir(cmd+6); te_push(w,"> done");
    } else if(!strncmp(cmd,"rm ",3)){
        cmd_rm(cmd+3); te_push(w,"> done");
    } else if(!strcmp(cmd,"df")){
        char lb[TERM_W+1]; int li;
        uint32_t used=next_free_lba-ROOT_LBA, freed=fs_free_sectors();
        li=0; const char*p="used ";  while(*p)lb[li++]=*p++;
        { char t[12]; int n=0; uint32_t v=used*512/1024; if(!v)t[n++]='0'; while(v){t[n++]='0'+v%10;v/=10;} while(n)lb[li++]=t[--n]; }
        lb[li++]='K'; lb[li++]='B'; lb[li]=0; te_push(w,lb);
        li=0; p="free ";  while(*p)lb[li++]=*p++;
        { char t[12]; int n=0; uint32_t v=freed; if(!v)t[n++]='0'; while(v){t[n++]='0'+v%10;v/=10;} while(n)lb[li++]=t[--n]; }
        p=" sectors"; while(*p)lb[li++]=*p++; lb[li]=0; te_push(w,lb);
    } else if(!strncmp(cmd,"cat ",4)){
        dirent_t e;
        if(fs_find(cur_dir,cmd+4,&e)&&e.size>0){
            uint8_t*buf=(void*)0x200000; fs_load_file(&e,buf);
            uint32_t sz=e.size; if(sz>256)sz=256;
            char lb[TERM_W+1]; int li=0;
            for(uint32_t i2=0;i2<sz;i2++){
                char c2=(char)buf[i2];
                if(c2=='\n'||c2=='\r'){ lb[li]=0; te_push(w,lb); li=0; }
                else if(c2>=0x20&&li<TERM_W-1) lb[li++]=c2;
            }
            if(li>0){lb[li]=0;te_push(w,lb);}
        } else te_push(w,"! no file");
    } else if(!strncmp(cmd,"run ",4)||!strncmp(cmd,"exec ",5)){
        /* ELF 실행 → 콘솔 창 오픈 */
        const char*fname=(cmd[0]=='r')?cmd+4:cmd+5;
        dirent_t e;
        if(fs_find(cur_dir,fname,&e)&&e.size>0){
            /* 콘솔 창 찾기/열기 */
            int cid=-1;
            for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){cid=i;break;}
            if(cid>=0){
                mw_open(cid,TT_CON);
                strncpy(g_mw[cid].co_fname,fname,23);
                g_mw[cid].co_running=1;
                char lb2[TERM_W+1]; int lbi=0;
                lb2[lbi++]='>';lb2[lbi++]=' ';
                const char*fn2=fname; while(*fn2&&lbi<TERM_W-1) lb2[lbi++]=*fn2++;
                lb2[lbi]=0;
                /* 콘솔에 시작 메시지 */
                strncpy(g_mw[cid].co_buf[0],lb2,TERM_W); g_mw[cid].co_n=1;
                strncpy(g_mw[cid].co_buf[1],"Loading ELF...",TERM_W); g_mw[cid].co_n=2;
                /* 실제 실행 */
                uint8_t*buf2=(void*)0x400000;
                fs_load_file(&e,buf2);
                int pid=elf_spawn(buf2);
                if(pid>=0){
                    char pb[TERM_W+1]; int pbi=0;
                    pb[pbi++]='P';pb[pbi++]='I';pb[pbi++]='D';pb[pbi++]='=';
                    u32str((uint32_t)pid,pb+pbi,6);
                    strncpy(g_mw[cid].co_buf[g_mw[cid].co_n++],pb,TERM_W);
                    strncpy(g_mw[cid].co_buf[g_mw[cid].co_n++],"Running...",TERM_W);
                } else {
                    strncpy(g_mw[cid].co_buf[g_mw[cid].co_n++],"! spawn failed",TERM_W);
                    g_mw[cid].co_running=0;
                }
                te_push(w,"> launched console window");
            } else te_push(w,"! no free window slot");
        } else te_push(w,"! file not found");
    } else if(!strncmp(cmd,"view ",5)){
        /* 뷰어 열기 */
        dirent_t e;
        if(fs_find(cur_dir,cmd+5,&e)&&e.size>0){
            int vid=-1;
            for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){vid=i;break;}
            if(vid>=0){
                mw_open(vid,TT_VIEW);
                fs_load_file(&e,VIEW_BUF);
                g_mw[vid].vw_buf=VIEW_BUF;
                g_mw[vid].vw_size=e.size;
                g_mw[vid].vw_top=0;
                strncpy(g_mw[vid].vw_fname,cmd+5,23);
                te_push(w,"> viewer opened");
            } else te_push(w,"! no free window slot");
        } else te_push(w,"! file not found");
    } else if(!strncmp(cmd,"edit ",5)){
        /* 노트패드 열기 */
        int nid=-1;
        for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){nid=i;break;}
        if(nid>=0){
            mw_open(nid,TT_NOTE);
            strncpy(g_mw[nid].np_fname,cmd+5,23);
            /* 기존 파일 로드 */
            dirent_t e;
            if(fs_find(cur_dir,cmd+5,&e)&&e.size>0){
                uint8_t*buf3=(void*)0x200000;
                fs_load_file(&e,buf3);
                /* 라인 분할 */
                int li2=0,ci2=0;
                for(uint32_t i2=0;i2<e.size&&li2<NOTE_LINES;i2++){
                    char c3=(char)buf3[i2];
                    if(c3=='\n'||c3=='\r'){
                        g_mw[nid].np_lines[li2][ci2]=0; li2++; ci2=0;
                    } else if(ci2<NOTE_COLS-1){
                        g_mw[nid].np_lines[li2][ci2++]=c3;
                    }
                }
                if(ci2>0){g_mw[nid].np_lines[li2][ci2]=0;li2++;}
                g_mw[nid].np_nlines=li2>0?li2:1;
            }
            te_push(w,"> editor opened");
        } else te_push(w,"! no free window slot");
    } else if(!strcmp(cmd,"reboot")){
        outb(0x64,0xFE);
    } else {
        char eb[TERM_W+1]; int ei=0;
        eb[ei++]='!'; eb[ei++]=' ';
        const char*e2="unknown: "; while(*e2&&ei<TERM_W-1) eb[ei++]=*e2++;
        const char*e3=cmd; while(*e3&&ei<TERM_W-1) eb[ei++]=*e3++;
        eb[ei]=0; te_push(w,eb);
    }
    w->te_in[0]=0; w->te_ilen=0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  노트패드 저장
 * ═══════════════════════════════════════════════════════════════════ */
static void np_save(int wid){
    mw_t*w=&g_mw[wid];
    if(!w->np_fname[0]) return;
    uint8_t*buf=(void*)0x300000;
    uint32_t len=0;
    for(int i=0;i<w->np_nlines;i++){
        const char*l=w->np_lines[i];
        while(*l) buf[len++]=(uint8_t)*l++;
        buf[len++]='\n';
    }
    /* 기존 파일 덮어쓰기 or 새 파일 */
    dirent_t e;
    if(fs_find(cur_dir,w->np_fname,&e)){
        fs_free_extent(e.lba,(e.size+511)/512); /* ★ 기존 확장 회수 후 재할당 */
        uint32_t sc=(len+511)/512;
        uint32_t lba=get_free_lba(sc);
        ata_write28(lba,sc,buf);
        e.lba=lba; e.size=len;
        ata_read28(cur_dir,DIR_SECTORS,dirbuf);
        for(int i=0;i<DIR_ENTRIES;i++){
            dirent_t*de=(dirent_t*)(dirbuf+i*sizeof(dirent_t));
            if(!strcmp(de->name,w->np_fname)){*de=e; break;}
        }
        ata_write28(cur_dir,DIR_SECTORS,dirbuf);
    } else {
        uint32_t sc=(len+511)/512;
        uint32_t lba=get_free_lba(sc);
        ata_write28(lba,sc,buf);
        fs_add_entry(cur_dir,w->np_fname,lba,len);
    }
    w->np_dirty=0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  마우스 커서
 * ═══════════════════════════════════════════════════════════════════ */
static uint8_t g_cur_save[12*12];
static int     g_cur_sx=-1, g_cur_sy=-1;
#define CUR_W 10
#define CUR_H 12
static const uint8_t CUR_MASK[CUR_H]={
    0xFF,0xC0,0xE0,0xF0,0xF8,0xFC,0xFE,0xFC,0xEC,0xC6,0x06,0x03
};

static void cur_draw(void){
    int cx=mouse_x, cy=mouse_y;
    if(cx<0)cx=0; if(cx>SCR_W-CUR_W-1)cx=SCR_W-CUR_W-1;
    if(cy<0)cy=0; if(cy>SCR_H-CUR_H-1)cy=SCR_H-CUR_H-1;

    if(g_cur_sx>=0){
        for(int r=0;r<CUR_H;r++) for(int c=0;c<CUR_W;c++)
            px(g_cur_sx+c,g_cur_sy+r,g_cur_save[r*CUR_W+c]);
    }
    g_cur_sx=cx; g_cur_sy=cy;
    for(int r=0;r<CUR_H;r++) for(int c=0;c<CUR_W;c++)
        g_cur_save[r*CUR_W+c]=gpx(cx+c,cy+r);
    /* 그리기: 흰 화살표 + 검정 윤곽 */
    for(int r=0;r<CUR_H;r++){
        uint8_t m=CUR_MASK[r];
        for(int c=0;c<CUR_W;c++){
            if(m&(0x80>>c)){
                px(cx+c,cy+r,P_TXT_W);
            }
        }
    }
    /* 1픽셀 검정 윤곽선 */
    for(int r=0;r<CUR_H;r++){
        uint8_t m=CUR_MASK[r];
        for(int c=0;c<CUR_W-1;c++){
            if((m&(0x80>>c))&&!(m&(0x80>>(c+1)))) px(cx+c+1,cy+r,P_WIN_SHD);
        }
        if(r+1<CUR_H){
            uint8_t mn=CUR_MASK[r+1];
            for(int c=0;c<CUR_W;c++){
                if((m&(0x80>>c))&&!(mn&(0x80>>c))) px(cx+c,cy+r+1,P_WIN_SHD);
            }
        }
    }
}
static void cur_erase(void){
    if(g_cur_sx<0) return;
    for(int r=0;r<CUR_H;r++) for(int c=0;c<CUR_W;c++)
        px(g_cur_sx+c,g_cur_sy+r,g_cur_save[r*CUR_W+c]);
    g_cur_sx=-1;
}

/* 마우스 픽셀 좌표 - now directly in mode13h pixel space */
static void mxy(int*px2,int*py2){ *px2=mouse_x; *py2=mouse_y; }

/* 창 히트테스트 */
static int win_hit(int px2,int py2,int*out){
    if(g_focus>=0){
        mw_t*w=&g_mw[g_focus];
        if(w->open&&!w->minimized&&px2>=w->x&&px2<w->x+w->w&&py2>=w->y&&py2<w->y+w->h){*out=g_focus;return 1;}
    }
    for(int i=MW_MAX-1;i>=0;i--){
        mw_t*w=&g_mw[i];
        if(!w->open||w->minimized)continue;
        if(px2>=w->x&&px2<w->x+w->w&&py2>=w->y&&py2<w->y+w->h){*out=i;return 1;}
    }
    return 0;
}

/* 파일매니저에서 더블클릭 처리 */
static int g_last_click_wid=-1;
static int g_last_click_row=-1;
static int g_click_count=0;
static int g_click_timer=0;

static void fm_open_selected(int wid){
    mw_t*w=&g_mw[wid];
    if(g_fm_cnt<=0||w->fm_sel>=g_fm_cnt) return;
    fm_ent_t*e=&g_fm_ents[w->fm_sel];
    if(e->is_dir){
        /* 디렉토리 진입 */
        dirent_t de;
        if(fs_find(w->fm_dir,e->name,&de)&&de.size==0){
            if(path_depth<MAX_DEPTH-1){path_lba[path_depth]=w->fm_dir;strncpy(path_name[path_depth],e->name,23);path_depth++;}
            w->fm_dir=de.lba; w->fm_sel=0; w->fm_top=0;
            cur_dir=de.lba;
            fm_load(wid);
        }
    } else {
        /* 파일 열기: 확장자 판단 */
        const char*nm=e->name;
        int nlen=strlen(nm);
        /* .elf → 콘솔 실행 */
        int is_elf=(nlen>4&&nm[nlen-4]=='.'&&nm[nlen-3]=='e'&&nm[nlen-2]=='l'&&nm[nlen-1]=='f');
        int is_text=(!is_elf);  /* 나머지는 텍스트로 */
        if(is_elf){
            int cid=-1;
            for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){cid=i;break;}
            if(cid>=0){
                mw_open(cid,TT_CON);
                strncpy(g_mw[cid].co_fname,nm,23);
                g_mw[cid].co_running=1;
                strncpy(g_mw[cid].co_buf[0],"> Launching...",TERM_W);
                g_mw[cid].co_n=1;
                dirent_t de2;
                if(fs_find(w->fm_dir,nm,&de2)){
                    uint8_t*buf2=(void*)0x400000;
                    fs_load_file(&de2,buf2);
                    int pid=elf_spawn(buf2);
                    char pb[TERM_W+1]; int pbi=0;
                    if(pid>=0){
                        pb[pbi++]='P';pb[pbi++]='I';pb[pbi++]='D';pb[pbi++]='=';
                        u32str((uint32_t)pid,pb+pbi,8);
                        strncpy(g_mw[cid].co_buf[g_mw[cid].co_n++],pb,TERM_W);
                    } else {
                        strncpy(g_mw[cid].co_buf[g_mw[cid].co_n++],"! failed",TERM_W);
                        g_mw[cid].co_running=0;
                    }
                }
            }
        } else if(is_text){
            /* 텍스트: notepad로 열기 */
            int nid=-1;
            for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){nid=i;break;}
            if(nid>=0){
                mw_open(nid,TT_NOTE);
                strncpy(g_mw[nid].np_fname,nm,23);
                dirent_t de3;
                if(fs_find(w->fm_dir,nm,&de3)&&de3.size>0){
                    uint8_t*buf3=(void*)0x200000;
                    fs_load_file(&de3,buf3);
                    int li=0,ci=0;
                    for(uint32_t i2=0;i2<de3.size&&li<NOTE_LINES;i2++){
                        char c3=(char)buf3[i2];
                        if(c3=='\n'||c3=='\r'){g_mw[nid].np_lines[li][ci]=0;li++;ci=0;}
                        else if(ci<NOTE_COLS-1) g_mw[nid].np_lines[li][ci++]=c3;
                    }
                    if(ci>0){g_mw[nid].np_lines[li][ci]=0;li++;}
                    g_mw[nid].np_nlines=li>0?li:1;
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  GUI IME (2-벌식) – 터미널·노트패드에서 한글 입력
 * ═══════════════════════════════════════════════════════════════════ */
/* gui_ime_on, gui_ime_L, gui_ime_V, gui_ime_T: draw_term 앞에서 선언됨 */

static void gui_ime_reset(void){ gui_ime_L=-1; gui_ime_V=-1; gui_ime_T=0; }

/* 조합 중인 음절을 UTF-8 3바이트로 buf에 추가 (화면 갱신 없음) */
static int gui_ime_flush(char *buf, int *len, int max){
    if(gui_ime_L < 0 && gui_ime_V < 0){ gui_ime_reset(); return 0; }
    if(gui_ime_L < 0) gui_ime_L = 11; /* 초성 없으면 ㅇ */
    if(gui_ime_V < 0){
        /* 자음 단독 → 독립 자모 유니코드 */
        if(gui_ime_L >= 0 && gui_ime_L < 19){
            uint32_t cp2 = 0x3131u + cho_to_jamo_offset[gui_ime_L];
            char tmp2[4]; int n2 = utf8_encode(cp2, tmp2);
            if(*len + n2 < max){ for(int i=0;i<n2;i++) buf[(*len)++]=tmp2[i]; buf[*len]=0; }
        }
        gui_ime_reset(); return 0;
    }
    uint32_t cp = make_syllable_cp(gui_ime_L, gui_ime_V, gui_ime_T);
    char tmp[4]; int n = utf8_encode(cp, tmp);
    if(*len + n < max){
        for(int i=0;i<n;i++) buf[(*len)++] = tmp[i];
        buf[*len] = 0;
    }
    gui_ime_reset();
    return n;
}

/* 백스페이스: 조합 중이면 조합 단계 되돌림, 아니면 0 반환 */
static int gui_ime_backspace(char *buf, int *len){
    if(gui_ime_T){ gui_ime_T=0; return 1; }
    if(gui_ime_V>=0){ gui_ime_V=-1; return 1; }
    if(gui_ime_L>=0){ gui_ime_L=-1; return 1; }
    /* 버퍼에서 마지막 UTF-8 문자 제거 */
    if(*len > 0){
        int rm=1;
        while(*len-rm > 0 && ((buf[*len-rm] & 0xC0) == 0x80)) rm++;
        *len -= rm; buf[*len]=0;
        return 2; /* 버퍼에서 제거됨 */
    }
    return 0;
}

/*
 * 키 하나를 GUI IME로 처리.
 * 반환: 1 = 키 소비됨, 0 = 처리 안 함 (ENG 처리로 위임)
 * buf/len: 현재 입력 버퍼 (UTF-8)
 * need_redraw: 1로 세팅하면 화면 재그리기 요청
 */
static int gui_ime_key(uint8_t k, char *buf, int *len, int max){
    if(!gui_ime_on) return 0;
    if(k < 32 || k > 126) return 0;
    char c = (char)k;

    if(c==' '){
        gui_ime_flush(buf, len, max);
        if(*len < max-1){ buf[(*len)++]=' '; buf[*len]=0; }
        return 1;
    }

    int newL = map_L_from_2beolsik(c);
    int newV = map_V_from_2beolsik(c);

    if(newV >= 0){
        /* 모음 입력 */
        if(gui_ime_L < 0 && gui_ime_V < 0){ gui_ime_L=11; gui_ime_V=newV; return 1; }
        if(gui_ime_L >= 0 && gui_ime_V < 0){ gui_ime_V=newV; return 1; }
        if(gui_ime_L >= 0 && gui_ime_V >= 0 && gui_ime_T==0){
            int comb = combine_V(gui_ime_V, newV);
            if(comb>=0){ gui_ime_V=comb; return 1; }
            gui_ime_flush(buf, len, max);
            gui_ime_L=11; gui_ime_V=newV; gui_ime_T=0; return 1;
        }
        if(gui_ime_L >= 0 && gui_ime_V >= 0 && gui_ime_T != 0){
            /* 받침 뒤에 모음이 오면 받침을 다음 음절 초성으로 이동.
             * 겹받침이면 앞자음만 받침으로 남기고 뒷자음만 넘긴다.
             * 예) "닭"+"ㅏ" → "달가", "값"+"ㅏ" → "갑사". */
            int save_T = gui_ime_T;
            int a,b; jong_composite(save_T,&a,&b);
            if(b >= 0){
                gui_ime_T = L_to_T_simple(a);       /* 앞자음을 받침으로 */
                gui_ime_flush(buf, len, max);       /* 현재 음절 확정 */
                gui_ime_L = b;                      /* 뒷자음이 새 초성 */
            } else {
                int next_L = jong_to_cho(save_T);
                gui_ime_T = 0;
                gui_ime_flush(buf, len, max);       /* 받침 없이 확정 */
                gui_ime_L = (next_L >= 0) ? next_L : 11;
            }
            gui_ime_V = newV; gui_ime_T = 0;
            return 1;
        }
        gui_ime_flush(buf, len, max);
        gui_ime_L=11; gui_ime_V=newV; gui_ime_T=0; return 1;
    }

    if(newL >= 0){
        /* 자음 입력 */
        if(gui_ime_L < 0 && gui_ime_V < 0){ gui_ime_L=newL; return 1; }
        if(gui_ime_L >= 0 && gui_ime_V < 0){
            gui_ime_flush(buf, len, max);
            gui_ime_L=newL; gui_ime_V=-1; gui_ime_T=0; return 1;
        }
        if(gui_ime_L >= 0 && gui_ime_V >= 0){
            if(gui_ime_T == 0){
                int t = L_to_T_simple(newL);
                if(t >= 0){ gui_ime_T=t; return 1; }
                gui_ime_flush(buf, len, max);
                gui_ime_L=newL; gui_ime_V=-1; gui_ime_T=0; return 1;
            } else {
                int combT = combine_T(gui_ime_T, newL);
                if(combT >= 0){ gui_ime_T=combT; return 1; }
                /* 결합 안 됨 → 현재 음절 확정, 새 초성 시작 */
                gui_ime_flush(buf, len, max);
                gui_ime_L=newL; gui_ime_V=-1; gui_ime_T=0; return 1;
            }
        }
        gui_ime_flush(buf, len, max);
        gui_ime_L=newL; gui_ime_V=-1; gui_ime_T=0; return 1;
    }

    /* 자음도 모음도 아님 → 조합 확정 후 ASCII 추가 */
    gui_ime_flush(buf, len, max);
    if(*len < max-1){ buf[(*len)++]=c; buf[*len]=0; }
    return 1;
}

/* 조합 중인 음절을 미리보기용 UTF-8로 변환 (확정 안 함) */
static int gui_ime_preview(char *out4){
    if(gui_ime_L < 0 && gui_ime_V < 0) return 0;
    if(gui_ime_L >= 0 && gui_ime_V < 0){
        /* 자음 단독 미리보기: 독립 자모 */
        if(gui_ime_L < 19){
            uint32_t cp2 = 0x3131u + cho_to_jamo_offset[gui_ime_L];
            return utf8_encode(cp2, out4);
        }
        return 0;
    }
    int L2 = (gui_ime_L >= 0) ? gui_ime_L : 11;
    int V2 = (gui_ime_V >= 0) ? gui_ime_V : 0;
    uint32_t cp = make_syllable_cp(L2, V2, gui_ime_T);
    return utf8_encode(cp, out4);
}

/* ─── GUI 한글 상태 표시 (메뉴바에 [HAN]/[ENG]) ─── */
static void gui_ime_update_mbar(void){
    strncpy(g_mbar_msg, gui_ime_on ? "[HAN] Auroral" : "[ENG] Auroral", 39);
}

/* ─── GUI syscall 핸들러 (그래픽/창/마우스/알림) ─── */
static uint32_t gui_syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx){
    switch(eax){
        case 12: px((int)ebx,(int)ecx,(uint8_t)edx); return 0;
        case 13: {
            int *p=(int*)edx; if(!p) return 0;
            gfx_rect((int)ebx,(int)ecx,p[0],p[1],(uint8_t)p[2]);
            return 0;
        }
        case 14: {
            int *xp=(int*)ebx, *yp=(int*)ecx;
            if(xp) *xp=mouse_x;
            if(yp) *yp=mouse_y;
            return 0;
        }
        case 15: return (uint32_t)mouse_btn;
        case 16: strncpy(g_mbar_msg,(const char*)ebx,39); g_mbar_msg[39]=0; return 0;
        case 17: {
            int type=(int)ebx;
            for(int i=0;i<MW_MAX;i++){
                if(!g_mw[i].open){ mw_open(i,type); return (uint32_t)i; }
            }
            return (uint32_t)-1;
        }
        case 18: {
            int wid=(int)ebx;
            if(wid>=0&&wid<MW_MAX&&g_mw[wid].open) mw_close(wid);
            return 0;
        }
        default: return (uint32_t)-1;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  메인 GUI 루프
 * ═══════════════════════════════════════════════════════════════════ */
static void gui_run(void){
    for(int i=0;i<MW_MAX;i++) g_mw[i].open=0;
    g_focus=-1; g_cur_sx=-1;
    strncpy(g_mbar_msg,"Auroral 3.0",40);
    rtc_clock_str(g_clock); g_clock[8]=0; g_ck_tick=0;

    /* GUI syscall 훅 등록 */
    gui_syscall_hook = gui_syscall_handler;

    if(!mouse_enabled) mouse_init();
    mouse_x = SCR_W/2; mouse_y = SCR_H/2;
    mouse_ax = 0; mouse_ay = 0;

    gfx_enter_mode13();
    gfx_init_palette();
    draw_all();
    cur_draw();
    gfx_present();

    /* 첫 창으로 파일매니저 */
    mw_open(0,TT_FILES);
    cur_erase(); draw_all(); cur_draw(); gfx_present();

    int prev_btn=0;
    int need=0;

    for(;;){
        need=0;
        int old_ck=g_ck_tick;
        clock_tick();
        int ck_upd=(g_ck_tick==0&&old_ck!=0);

        /* 마우스 */
        int opx2,opy2; mxy(&opx2,&opy2);
        mouse_process();
        int npx,npy; mxy(&npx,&npy);
        int click=(!prev_btn)&&(mouse_btn&1);
        int rclick=(!prev_btn)&&(mouse_btn&2);
        prev_btn=mouse_btn;

        /* 드래그 */
        if(mouse_btn&1){
            for(int i=0;i<MW_MAX;i++){
                mw_t*w=&g_mw[i];
                if(!w->open||!w->drag) continue;
                int nx=npx-w->ddx, ny=npy-w->ddy;
                if(nx<0)nx=0; if(ny<MBAR_H)ny=MBAR_H;
                if(nx+w->w>SCR_W)nx=SCR_W-w->w;
                if(ny+w->h>SCR_H-DOCK_H)ny=SCR_H-DOCK_H-w->h;
                if(nx!=w->x||ny!=w->y){w->x=nx;w->y=ny;need=1;}
            }
        } else {
            for(int i=0;i<MW_MAX;i++) g_mw[i].drag=0;
        }

        if(opx2!=npx||opy2!=npy) need=1;

        /* 클릭 처리 */
        if(click){
            need=1;
            /* 도크 클릭? */
            if(npy>=SCR_H-DOCK_H){
                int total=DOCK_ITEMS*(DOCK_ICON_SZ+18);
                int sx2=(SCR_W-total)/2;
                int di=(npx-sx2)/(DOCK_ICON_SZ+18);
                if(di>=0&&di<DOCK_ITEMS){
                    int dtype=g_dock[di].type;
                    /* 이미 열려있으면 포커스, 없으면 오픈 */
                    int found=-1;
                    for(int j=0;j<MW_MAX;j++) if(g_mw[j].open&&g_mw[j].type==dtype){found=j;break;}
                    if(found>=0){
                        /* 최소화 상태면 복원 */
                        if(g_mw[found].minimized){
                            g_mw[found].minimized=0;
                        }
                        g_focus=found;
                    }
                    else{
                        int s2=-1;for(int j=0;j<MW_MAX;j++) if(!g_mw[j].open){s2=j;break;}
                        if(s2>=0) mw_open(s2,dtype);
                    }
                }
            } else {
                /* 창 히트 */
                int wid2=-1;
                if(win_hit(npx,npy,&wid2)){
                    g_focus=wid2;
                    mw_t*w=&g_mw[wid2];
                    /* 닫기 버튼 (핑크 원, 중심 x+8, y+bcy, 반지름 4) */
                    int bcx2=w->x+8, bcy2=w->y+1+TBAR_H/2;
                    int dx2=npx-bcx2, dy2=npy-bcy2;
                    if(dx2*dx2+dy2*dy2<=25){ /* r=5 click area */
                        mw_close(wid2);
                        goto next_frame;
                    }
                    /* 최소화 버튼 (노랑 원, x+20) */
                    int mnx=w->x+20, mny=w->y+1+TBAR_H/2;
                    int mdx=npx-mnx, mdy=npy-mny;
                    if(mdx*mdx+mdy*mdy<=25){
                        w->minimized ^= 1;
                        if(w->minimized && g_focus==wid2){
                            g_focus=-1;
                            for(int ii=MW_MAX-1;ii>=0;ii--) if(g_mw[ii].open&&!g_mw[ii].minimized){g_focus=ii;break;}
                        }
                        goto next_frame;
                    }
                    /* 최대화 버튼 (민트 원, x+32) */
                    int mxx=w->x+32, mxy2=w->y+1+TBAR_H/2;
                    int mxdx=npx-mxx, mxdy=npy-mxy2;
                    if(mxdx*mxdx+mxdy*mxdy<=25){
                        if(!w->maximized){
                            /* 현재 크기 저장 후 최대화 */
                            w->pre_x=w->x; w->pre_y=w->y;
                            w->pre_w=w->w; w->pre_h=w->h;
                            w->x=0; w->y=MBAR_H;
                            w->w=SCR_W; w->h=SCR_H-MBAR_H-DOCK_H;
                            w->maximized=1;
                        } else {
                            /* 원래 크기 복원 */
                            w->x=w->pre_x; w->y=w->pre_y;
                            w->w=w->pre_w; w->h=w->pre_h;
                            w->maximized=0;
                        }
                        goto next_frame;
                    }
                    /* 타이틀바 더블클릭 → 최대화 토글 */
                    /* 타이틀바 드래그 */
                    if(!w->maximized && npy<w->y+1+TBAR_H+1){
                        w->drag=1; w->ddx=npx-w->x; w->ddy=npy-w->y;
                    }
                    /* 파일매니저 항목 클릭 */
                    else if(w->type==TT_FILES){
                        int list_y=CLI_Y(w)+21;  /* header+pathbar */
                        if(npy>=list_y){
                            int row2=(npy-list_y)/9;
                            int idx=w->fm_top+row2;
                            if(idx<g_fm_cnt){
                                /* 더블클릭 감지 */
                                if(g_last_click_wid==wid2&&g_last_click_row==idx&&g_click_timer>0){
                                    w->fm_sel=idx;
                                    fm_open_selected(wid2);
                                    g_click_count=0;
                                } else {
                                    w->fm_sel=idx;
                                    g_last_click_wid=wid2;
                                    g_last_click_row=idx;
                                    g_click_timer=80;
                                }
                            }
                        }
                    }
                    /* 뷰어 스크롤 */
                    else if(w->type==TT_VIEW){
                        int sbx=w->x+w->w-5;
                        if(npx>=sbx){
                            int sby=CLI_Y(w),sbh=CLI_H(w);
                            int tpos=(npy-sby)*100/sbh;
                            w->vw_top=tpos*(w->vw_size/40+1)/100;
                        }
                    }
                    /* 노트패드 클릭 → 커서 이동 */
                    else if(w->type==TT_NOTE){
                        int ea_y=CLI_Y(w)+11;
                        if(npy>=ea_y){
                            int li=(npy-ea_y)/8+w->np_top;
                            if(li<w->np_nlines){
                                w->np_cur=li;
                                int ci=(npx-CLI_X(w)-2)/8;
                                int maxc=strlen(w->np_lines[li]);
                                if(ci>maxc)ci=maxc; if(ci<0)ci=0;
                                w->np_col=ci;
                            }
                        }
                    }
                }
            }
        }

        /* 더블클릭 타이머 */
        if(g_click_timer>0) g_click_timer--;

        /* 키보드 */
        uint8_t k=getch_raw_u8_poll();
        if(k==0){
            /* 변화가 있을 때만 백버퍼를 새로 그려 화면에 반영.
             * 완전 idle(마우스 정지·시계 그대로)일 땐 present를 건너뛰어
             * 키/마우스 폴링 반응성을 유지한다. */
            if(ck_upd){ cur_erase(); redraw_clock(); cur_draw(); gfx_present(); }
            else if(need){ cur_erase(); draw_all(); cur_draw(); gfx_present(); }
            continue;
        }
        need=1;

        /* ESC: 종료 */
        if(k==27) break;

        /* Ctrl+Space: 한/영 전환 */
        if(k==KEY_TOGGLE_HAN){
            gui_ime_on ^= 1;
            if(!gui_ime_on) gui_ime_reset();
            gui_ime_update_mbar();
            goto next_frame;
        }

        /* 전역 단축키 */
        if(k=='1'){ int s2=-1;for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){s2=i;break;} if(s2>=0) mw_open(s2,TT_FILES); goto next_frame; }
        if(k=='2'){ int s2=-1;for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){s2=i;break;} if(s2>=0) mw_open(s2,TT_TERM);  goto next_frame; }
        if(k=='3'){ int s2=-1;for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){s2=i;break;} if(s2>=0) mw_open(s2,TT_NOTE);  goto next_frame; }
        if(k=='4'){ int s2=-1;for(int i=0;i<MW_MAX;i++) if(!g_mw[i].open){s2=i;break;} if(s2>=0) mw_open(s2,TT_INFO);  goto next_frame; }
        if((k=='q'||k=='Q')&&g_focus>=0){ mw_close(g_focus); goto next_frame; }
        if((k=='w'||k=='W')&&k!='w'){ /* W는 노트패드에서 쓰임, 단 Ctrl+W로 닫기는 미구현 */ }
        /* Tab: 창 전환 */
        if(k=='\t'){
            int nx=(g_focus<0?0:(g_focus+1)%MW_MAX);
            for(int t=0;t<MW_MAX;t++){if(g_mw[nx].open){g_focus=nx;break;}nx=(nx+1)%MW_MAX;}
            goto next_frame;
        }

        /* 포커스 창에 키 전달 */
        if(g_focus>=0){
            mw_t*w=&g_mw[g_focus];
            if(w->minimized) goto next_frame; /* 최소화 상태면 키 무시 */
            if(w->type==TT_FILES){
                int vis=(CLI_H(w)-32)/9;
                if(k==KEY_UP){ if(w->fm_sel>0){w->fm_sel--; if(w->fm_sel<w->fm_top)w->fm_top--;} }
                else if(k==KEY_DOWN){ if(w->fm_sel<g_fm_cnt-1){w->fm_sel++; if(w->fm_sel>=w->fm_top+vis)w->fm_top++;} }
                else if(k=='\n'||k=='\r'){ fm_open_selected(g_focus); }
                else if(k==8){ /* 뒤로: 상위 디렉토리 */
                    if(path_depth>0){path_depth--;cur_dir=path_lba[path_depth];w->fm_dir=cur_dir;w->fm_sel=0;w->fm_top=0;fm_load(g_focus);}
                }
            }
            else if(w->type==TT_TERM){
                if(k==8){
                    if(gui_ime_on){
                        int r2=gui_ime_backspace(w->te_in, &w->te_ilen);
                        if(r2==0 && w->te_ilen>0){
                            /* UTF-8 마지막 문자 제거 */
                            int rm=1;
                            while(w->te_ilen-rm>0&&((w->te_in[w->te_ilen-rm]&0xC0)==0x80)) rm++;
                            w->te_ilen-=rm; w->te_in[w->te_ilen]=0;
                        }
                    } else {
                        if(w->te_ilen>0) w->te_in[--w->te_ilen]=0;
                    }
                }
                else if(k=='\n'||k=='\r'){
                    /* 조합 중인 음절 확정 */
                    if(gui_ime_on) gui_ime_flush(w->te_in, &w->te_ilen, 63);
                    te_exec(g_focus);
                }
                else if(gui_ime_on){
                    /* 한글 IME 처리 */
                    gui_ime_key(k, w->te_in, &w->te_ilen, 63);
                }
                else if(k>=32&&k<128&&w->te_ilen<TERM_W-1){
                    w->te_in[w->te_ilen++]=(char)k; w->te_in[w->te_ilen]=0;
                }
            }
            else if(w->type==TT_NOTE){
                if(k==8){  /* 백스페이스 */
                    if(gui_ime_on){
                        int r2=gui_ime_backspace(w->np_lines[w->np_cur], &w->np_col);
                        if(r2) { w->np_dirty=1; goto next_frame; }
                        /* 조합 없음 → 이전 문자 제거 */
                    }
                    /* 기존 백스페이스 로직 (바이트 단위) */
                    if(w->np_col>0){
                        char*line=w->np_lines[w->np_cur];
                        int len2=strlen(line);
                        /* UTF-8: 커서 앞 문자 크기 */
                        int rm=1;
                        while(w->np_col-rm>0&&((line[w->np_col-rm]&0xC0)==0x80)) rm++;
                        for(int i=w->np_col-rm;i<len2-rm;i++) line[i]=line[i+rm];
                        for(int i=0;i<rm;i++) line[len2-rm+i]=0;
                        w->np_col-=rm; w->np_dirty=1;
                    } else if(w->np_cur>0){
                        /* 이전 줄과 합치기 */
                        int prev=w->np_cur-1;
                        int plen=strlen(w->np_lines[prev]);
                        int clen=strlen(w->np_lines[w->np_cur]);
                        if(plen+clen<NOTE_COLS){
                            strncpy(w->np_lines[prev]+plen,w->np_lines[w->np_cur],NOTE_COLS-plen);
                            for(int i=w->np_cur;i<w->np_nlines-1;i++) strncpy(w->np_lines[i],w->np_lines[i+1],NOTE_COLS);
                            w->np_nlines--;
                            w->np_cur=prev; w->np_col=plen; w->np_dirty=1;
                        }
                    }
                }
                else if(k=='\n'||k=='\r'){  /* 엔터: 줄 삽입 */
                    /* 조합 중인 음절 먼저 확정 */
                    if(gui_ime_on) gui_ime_flush(w->np_lines[w->np_cur], &w->np_col, NOTE_COLS);
                    if(w->np_nlines<NOTE_LINES){
                        /* 현재 커서 뒤 내용을 다음 줄로 */
                        char*line=w->np_lines[w->np_cur];
                        char rest[NOTE_COLS+1];
                        strncpy(rest,line+w->np_col,NOTE_COLS);
                        line[w->np_col]=0;
                        /* 이후 줄 밀기 */
                        for(int i=w->np_nlines;i>w->np_cur+1;i--) strncpy(w->np_lines[i],w->np_lines[i-1],NOTE_COLS);
                        strncpy(w->np_lines[w->np_cur+1],rest,NOTE_COLS);
                        w->np_nlines++; w->np_cur++; w->np_col=0; w->np_dirty=1;
                        /* 스크롤 */
                        int vis2=CLI_H(w)/10-2;
                        if(w->np_cur>=w->np_top+vis2) w->np_top++;
                    }
                }
                else if(k==KEY_UP){ if(w->np_cur>0){w->np_cur--; if(w->np_cur<w->np_top)w->np_top--;} }
                else if(k==KEY_DOWN){ if(w->np_cur<w->np_nlines-1){w->np_cur++; int vis2=CLI_H(w)/10-2; if(w->np_cur>=w->np_top+vis2)w->np_top++;} }
                else if(k==KEY_LEFT){
                    if(gui_ime_on && (gui_ime_L>=0||gui_ime_V>=0)){ gui_ime_flush(w->np_lines[w->np_cur],&w->np_col,NOTE_COLS); }
                    if(w->np_col>0){
                        /* UTF-8 이전 문자로 */
                        int c2=w->np_col-1;
                        while(c2>0&&((w->np_lines[w->np_cur][c2]&0xC0)==0x80)) c2--;
                        w->np_col=c2;
                    }
                }
                else if(k==KEY_RIGHT){
                    if(gui_ime_on && (gui_ime_L>=0||gui_ime_V>=0)){ gui_ime_flush(w->np_lines[w->np_cur],&w->np_col,NOTE_COLS); }
                    int ml=strlen(w->np_lines[w->np_cur]);
                    if(w->np_col<ml){
                        /* UTF-8 다음 문자로 */
                        uint32_t cp2=0;
                        int nn=utf8_decode_one((const uint8_t*)(w->np_lines[w->np_cur]+w->np_col),&cp2);
                        if(nn>0) w->np_col+=nn; else w->np_col++;
                    }
                }
                else if(k==19){  /* Ctrl+S = 저장 */
                    if(gui_ime_on) gui_ime_flush(w->np_lines[w->np_cur],&w->np_col,NOTE_COLS);
                    np_save(g_focus);
                    strncpy(g_mbar_msg,"Saved!",40);
                }
                else if(gui_ime_on && k>=32 && k<128){
                    /* 한글 IME 처리 */
                    gui_ime_key(k, w->np_lines[w->np_cur], &w->np_col, NOTE_COLS);
                    w->np_dirty=1;
                }
                else if(!gui_ime_on && k>=32&&k<127){
                    char*line=w->np_lines[w->np_cur];
                    int len2=strlen(line);
                    if(len2<NOTE_COLS-1&&w->np_col<=len2){
                        for(int i=len2;i>=w->np_col;i--) line[i+1]=line[i];
                        line[w->np_col++]=(char)k; w->np_dirty=1;
                    }
                }
            }
            else if(w->type==TT_VIEW){
                if(k==KEY_DOWN||k=='j') w->vw_top++;
                else if(k==KEY_UP||k=='k'){ if(w->vw_top>0)w->vw_top--; }
                else if(k==' '){ w->vw_top+=10; }
            }
        }

next_frame:
        cur_erase();
        draw_all();
        cur_draw();
        gfx_present();
    }

    /* 텍스트 모드 복귀 – 조용히 */
    cur_erase();
    gui_syscall_hook = 0;
    gui_ime_on = 0; gui_ime_reset();
    gfx_return_text();
    gfx_restore_font();           /* ★ VGA plane2 폰트 복원 (줄무늬 방지) */
    for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++) VGA[r*COLS+c]=(0x07<<8)|' ';
    row=0; col=0;
}

/* ---------- CLI loop ---------- */
static char line[128];

static void cli(void){
    for(;;){
        if(shell_paused && fg_pid!=-1){
            uint8_t k = getch_raw_u8_poll();
            if(k == KEY_CTRL_C){ kill_foreground_from_kbd('C'); continue; }
            if(k == KEY_CTRL_E){ kill_foreground_from_kbd('E'); continue; }
            schedule();
            continue;
        }

        puts("OS/2026> ");
        ime_hangul = 0;
        getline(line,sizeof line);

        if(!strcmp(line,"help")){
            puts("help ls cd <d> mkdir <d> cat <f> write <f> append <f>\n"
                 "run <f> spawn <f> jobs fg <pid> bg kill <pid>\n"
                 "rm <f> rmdir <d> mv <old> <new> cp <src> <dst>\n"
                 "root pwd reboot devs gui vgui date history env\n"
                 "df                       -- disk usage / reclaimed space\n"
                 "fetch <url> [file]       -- HTTP download (print or save)\n"
                 "web [url]                -- text web browser (arrows/enter/g/b/q, Ctrl+Space=Hangul)\n"
                 "ping <host>  netinfo     -- network tools\n"
                 "echo <text> [> file | >> file | pipe]\n"
                 "grep <keyword> <file>    -- search lines\n"
                 "wc <file>                -- line/word/byte count\n"
                 "stat <file>              -- file metadata\n"
                 "edit <file>              -- text editor (Ctrl+S=save Ctrl+C=quit)\n"
                 "set KEY=VALUE            -- set env variable\n"
                 "unset KEY                -- delete env variable\n"
                 "nice <pid> <1-4>         -- set task priority\n"
                 "fontedit cho|jung|jong N -- Hangul font editor\n"
                 "fontedit reset|show ...\n"
                 "fontload                 -- load font from LBA 90 (dd로 넣은 경우)\n"
                 "fontload <file.kfont>    -- load font from filesystem\n"
                 "Hangul input: Ctrl+Space toggle\n"
                 "Pipe:  echo hello | grep hel\n"
                 "Redir: echo hi > file.txt  echo more >> file.txt\n"
                 "Env:   echo $HOME   (dollar-sign expansion)\n"
                 "SYS:   int 0x80 eax=0..11 from ELF tasks\n");
        }
        else if(!strcmp(line,"gui")){ gui_run(); }
        else if(!strcmp(line,"vgui")){ gui_run(); }  /* vgui → gui */
        else if(!strcmp(line,"date")){ cmd_date(); }
        else if(!strcmp(line,"history")){
            int total=hist_len < HIST_MAX ? hist_len : HIST_MAX;
            for(int i=0;i<total;i++){
                int idx=(hist_len - total + i) % HIST_MAX;
                print_uint((uint32_t)(i+1)); puts("  "); puts(hist[idx]); putc('\n');
            }
        }
        else if(!strcmp(line,"env")){
            for(int i=0;i<MAX_ENV;i++){
                if(!env_table[i].used) continue;
                puts(env_table[i].key); putc('='); puts(env_table[i].val); putc('\n');
            }
        }
        else if(!strncmp(line,"set ",4)){
            /* set KEY=VALUE */
            char *kv=line+4;
            char *eq=kv; while(*eq&&*eq!='=') eq++;
            if(*eq=='='){
                *eq=0;
                char key[MAX_ENV_KEY]; strncpy(key,kv,MAX_ENV_KEY-1); key[MAX_ENV_KEY-1]=0;
                env_set(key,eq+1);
                *eq='='; /* restore */
            } else { puts("usage: set KEY=VALUE\n"); }
        }
        else if(!strncmp(line,"unset ",6)){ env_unset(line+6); }
        else if(!strncmp(line,"echo ",5)){
            /* $VAR 확장 */
            char expanded[256];
            env_expand(line+5, expanded, 255);
            /* 리다이렉션 파싱 */
            char *rfile=0;
            int rmode=parse_redirect(expanded, &rfile);
            /* 파이프 파싱 */
            char *right_cmd=0;
            int has_pipe=0;
            if(!rmode) has_pipe=parse_pipe(expanded,&right_cmd);
            /* 오른쪽 공백 제거 */
            char *text=expanded; while(*text==' ')text++;
            int tl=strlen(text); while(tl>0&&(text[tl-1]==' '||text[tl-1]=='\n')) text[--tl]=0;
            if(rmode){
                strncpy(redir_file, rfile, 23); redir_file[23]=0;
                redir_active=rmode; redir_buf_len=0;
                cmd_echo_to(text, 1);
                redir_flush();
            } else if(has_pipe && right_cmd){
                pipe_active=1; pipe_buf_len=0;
                cmd_echo_to(text, 0);
                pipe_active=0;
                exec_pipe_right(right_cmd);
                pipe_buf_len=0;
            } else {
                puts(text); putc('\n');
            }
        }
        else if(!strncmp(line,"grep ",5)){
            /* grep kw file  OR  grep kw (파이프에서) */
            char *args=line+5;
            char kw[64]; int ki=0;
            while(args[ki]&&args[ki]!=' '&&ki<63) kw[ki]=args[ki++];
            kw[ki]=0;
            while(args[ki]==' ') ki++;
            if(args[ki]) cmd_grep(kw, args+ki);
            else { puts("usage: grep <keyword> <file>\n"); }
        }
        else if(!strncmp(line,"wc ",3)){ cmd_wc(line+3); }
        else if(!strncmp(line,"stat ",5)){ cmd_stat(line+5); }
        else if(!strncmp(line,"edit ",5)){ cmd_edit(line+5); }
        else if(!strncmp(line,"nice ",5)){ cmd_nice(line+5); }
        else if(!strcmp(line,"ls")) cmd_ls();
        else if(!strncmp(line,"cd ",3)){
            dirent_t e;
            if(!fs_find(cur_dir,line+3,&e)||e.size!=0){puts("not dir\n");continue;}
            if(path_depth<MAX_DEPTH){path_lba[path_depth]=cur_dir;strncpy(path_name[path_depth],line+3,23);++path_depth;}
            cur_dir=e.lba;
        }
        else if(!strncmp(line,"mkdir ",6)) cmd_mkdir(line+6);
        else if(!strncmp(line,"cat ",4)){
            dirent_t e;
            if(!fs_find(cur_dir,line+4,&e)){puts("no file\n");continue;}
            if(e.size==0){puts("is dir\n");continue;}
            uint8_t*buf=(void*)0x200000;
            fs_load_file(&e,buf);
            cat_print_utf8(buf, e.size);
            putc('\n');
        }
        else if(!strncmp(line,"write ",6)) file_write_or_append(line+6,0);
        else if(!strncmp(line,"append ",7)) file_write_or_append(line+7,1);

        else if(!strncmp(line,"run ",4)){
            uint8_t *buf = (void*)0x400000;
            if(!load_file_for_exec(line+4, buf)) continue;
            int pid = elf_spawn(buf);
            if(pid>=0){
                fg_pid = pid;
                shell_paused = 1;
                puts("foreground: pid="); print_uint((uint32_t)pid); putc('\n');
            }
        }
        else if(!strncmp(line,"spawn ",6)){
            uint8_t *buf = (void*)0x400000;
            if(!load_file_for_exec(line+6, buf)) continue;
            elf_spawn(buf);
        }
        else if(!strcmp(line,"jobs")) cmd_jobs();
        else if(!strncmp(line,"fg ",3)){
            uint32_t pid;
            if(!parse_uint(line+3,&pid)){ puts("usage: fg <pid>\n"); continue; }
            if(pid>=MAX_TASKS || !tasks[pid].used){ puts("no such pid\n"); continue; }
            fg_pid = (int)pid;
            shell_paused = 1;
            puts("foreground: pid="); print_uint(pid); putc('\n');
        }
        else if(!strcmp(line,"bg")){
            fg_pid = -1;
            shell_paused = 0;
            puts("shell foreground\n");
        }
        else if(!strncmp(line,"kill ",5)){
            uint32_t pid;
            if(!parse_uint(line+5,&pid)){ puts("usage: kill <pid>\n"); continue; }
            cmd_kill((int)pid);
        }

        else if(!strcmp(line,"reboot")){ outb(0x64,0xFE); __asm__ volatile("hlt"); }
        else if(!strncmp(line,"rm ",3)) cmd_rm(line+3);
        else if(!strncmp(line,"rmdir ",6)) cmd_rmdir(line+6);
        else if(!strcmp(line,"root")){ cur_dir=ROOT_LBA; path_depth=0; }
        else if(!strcmp(line,"df")){ cmd_df(); }
        else if(!strcmp(line,"pwd")){
            putc('/'); for(int i=0;i<path_depth;++i){ puts(path_name[i]); putc('/'); } putc('\n');
        }

        else if(!strncmp(line,"mv ",3)){
            char *args=line+3;
            char *space=args; while(*space && *space!=' ') ++space;
            if(!*space){puts("usage: mv <src> <dst>\n"); continue;}
            *space='\0';
            char *src=args, *dst=space+1;

            dirent_t e;
            if(!fs_find(cur_dir,src,&e)){ puts("no such file\n"); continue; }

            char dirpart[64], namepart[24];
            split_path(dst, dirpart, namepart);

            uint32_t dst_dir = resolve_dir(dirpart);
            if(!dst_dir){ puts("no such directory\n"); continue; }

            if(namepart[0]=='\0') strncpy(namepart, src, 24);

            dirent_t tmp;
            if(fs_find(dst_dir,namepart,&tmp)){ puts("dest exists\n"); continue; }

            if(dst_dir != cur_dir){
                fs_add_entry(dst_dir,namepart,e.lba,e.size);
                ata_read28(cur_dir,1,sec);
                for(int o=0;o<512;o+=sizeof(dirent_t)){
                    dirent_t*de=(dirent_t*)(sec+o);
                    if(!strcmp(de->name,src)){ memset(de,0,sizeof(dirent_t)); break; }
                }
                fs_write_dir(cur_dir);
                puts("moved\n");
            } else {
                ata_read28(cur_dir,1,sec);
                for(int o=0;o<512;o+=sizeof(dirent_t)){
                    dirent_t*de=(dirent_t*)(sec+o);
                    if(!strcmp(de->name,src)){ strncpy(de->name,namepart,24); fs_write_dir(cur_dir); puts("renamed\n"); break; }
                }
            }
        }
        else if(!strncmp(line,"cp ",3)){
            char *args=line+3;
            char *space=args; while(*space && *space!=' ') ++space;
            if(!*space){ puts("usage: cp <src> <dst>\n"); continue; }
            *space='\0';
            char *src=args, *dst=space+1;

            dirent_t e;
            if(!fs_find(cur_dir,src,&e)){ puts("no such file\n"); continue; }
            if(e.size==0){ puts("dir copy unsupported\n"); continue; }

            char dirpart[64], namepart[24];
            split_path(dst, dirpart, namepart);

            uint32_t dst_dir = resolve_dir(dirpart);
            if(!dst_dir){ puts("no such directory\n"); continue; }

            if(namepart[0]=='\0') strncpy(namepart, src, 24);

            dirent_t dummy;
            if(strlen(namepart)>23){ puts("name too long\n"); continue; }
            if(fs_find(dst_dir,namepart,&dummy)){ puts("dest exists\n"); continue; }

            uint8_t*buf=(void*)0x300000;
            fs_load_file(&e,buf);
            uint32_t sc=(e.size+511)/512;
            uint32_t lba=get_free_lba(sc);
            ata_write28(lba,sc,buf);
            fs_add_entry(dst_dir,namepart,lba,e.size);
            puts("copied\n");
        }

        else if(!strcmp(line,"devs")) cmd_devs();

        else if(!strncmp(line,"fetch ",6)) cmd_fetch(line+6);
        else if(!strcmp(line,"fetch"))     cmd_fetch("");
        else if(!strncmp(line,"web ",4))   cmd_web(line+4);
        else if(!strcmp(line,"web"))       cmd_web("");
        else if(!strncmp(line,"browser ",8)) cmd_web(line+8);
        else if(!strcmp(line,"browser"))   cmd_web("");
        else if(!strncmp(line,"ping ",5))  cmd_ping(line+5);
        else if(!strcmp(line,"netinfo"))   cmd_netinfo();

        else if(!strncmp(line,"fontedit",8)){
            cmd_fontedit(line[8]==' ' ? line+9 : "");
        }
        else if(!strcmp(line,"fontload")){
            cmd_fontload("");
        }
        else if(!strncmp(line,"fontload ",9)){
            cmd_fontload(line+9);
        }

        else {
            uint8_t *buf = (void*)0x400000;
            if(load_file_for_exec(line, buf)){
                int pid = elf_spawn(buf);
                if(pid>=0){
                    fg_pid = pid;
                    shell_paused = 1;
                    puts("foreground: pid="); print_uint((uint32_t)pid); putc('\n');
                }
            } else {
                puts("?\n");
            }
        }
    }
}

/* ---------- entry ---------- */
void kmain(void){
    fs_scan_root();
    init_jamo_fonts();
    font_load_disk();
    syscall_init();
    nic_init();         /* RTL8139 네트워크 초기화 */
    for(int i=0;i<6;i++) my_mac[i]=nic_mac[i];
    /* 기본 환경 변수 */
    env_set("HOME","/");
    env_set("SHELL","OS/2026sh");
    env_set("OS","OS/2026");

    puts("boot success\n========================================\n"
         "   OS/2026 v3.1 NEON  (red horse build)\n"
         "   Developed by kdy010010\n"
         "   Type 'devs' for contributors\n"
         "========================================\n\n"
         "Type 'help' for commands.\n"
         "Type 'gui'  to launch the NEON Desktop.\n"
         "Hangul: Ctrl+Space toggle.\n"
         "Mouse: PS/2 (GUI only).\n\n");

    cli();
}
