/*  OS/2025 – writable 2025FS + CLI + ELF32 loader (fixed 32‑byte dirent)
 *
 *  build:
 *     gcc   -m32 -ffreestanding -nostdlib -c kernel.c
 *     ld    -m elf_i386 -T link.ld -nostdlib -o kernel.elf kernel.o
 *     objcopy -O binary kernel.elf kernel.bin
 */

#include <stdint.h>
#define MAX_DEPTH 16

static uint32_t path_lba[MAX_DEPTH];         // directory LBA stack
static char     path_name[MAX_DEPTH][24];    // directory names
static int      path_depth = 0;              // depth (0 = root)

/* ---------- tiny C helpers ---------- */
static int  strlen(const char*s){int n=0;while(s[n])++n;return n;}
static int  strcmp(const char*a,const char*b){while(*a&&*a==*b){++a;++b;}return (unsigned char)*a-(unsigned char)*b;}
static int  strncmp(const char*a,const char*b,int n){while(n--&&*a&&*a==*b){++a;++b;}return n<0?0:(unsigned char)*a-(unsigned char)*b;}
static void strncpy(char*d,const char*s,int n){int i=0;for(;i<n&&s[i];++i)d[i]=s[i];for(;i<n;++i)d[i]=0;}
static void* memcpy(void*d,const void*s,uint32_t n){uint8_t*D=d;const uint8_t*S=s;while(n--)*D++=*S++;return d;}
static void* memset(void*d,int c,uint32_t n){uint8_t*D=d;while(n--)*D++=(uint8_t)c;return d;}

/* ---------- low‑level I/O ---------- */
static inline void  outb(uint16_t p,uint8_t  v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline void  outw(uint16_t p,uint16_t v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t  inb(uint16_t p){uint8_t r;__asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p));return r;}
static inline uint16_t inw(uint16_t p){uint16_t r;__asm__ volatile("inw %1,%0":"=a"(r):"Nd"(p));return r;}

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

static void putc(char c){
    if(c == '\n') { row++; col = 0; }
    else {
        VGA[row * COLS + col] = (0x1F << 8) | c;
        if (++col == COLS) { col = 0; ++row; }
    }
    if (row == ROWS) scroll();
}

static void puts(const char* s){while(*s)putc(*s++);} 
static void print_uint(uint32_t x){char b[11];int i=0;if(!x){putc('0');return;}while(x){b[i++]='0'+x%10;x/=10;}while(i--)putc(b[i]);}

/* ---------- keyboard (busy‑wait) ---------- */
static const char keymap[128]={
 0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,'\t',
 'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
 'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
 'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};
static char getch(void){while(!(inb(0x64)&1));uint8_t sc=inb(0x60);if(sc&0x80)return 0;return keymap[sc];}
static int getline(char*buf,int max){int len=0;char c;while(len<max-1&&(c=getch())!='\n'){if(c==8&&len){--len;puts("\b \b");}else if(c){buf[len++]=c;putc(c);} }buf[len]='\0';putc('\n');return len;}

/* ---------- ATA‑PIO ---------- */
#define ATA_DATA 0x1F0
static void ata_wait(void){while(inb(0x1F7)&0x80);} 
static void ata_read28(uint32_t lba,uint8_t sc,void*dst){
    ata_wait();outb(0x1F2,sc);outb(0x1F3,lba);outb(0x1F4,lba>>8);outb(0x1F5,lba>>16);outb(0x1F6,0xE0|((lba>>24)&0xF));outb(0x1F7,0x20);
    uint16_t*buf=dst;for(int s=0;s<sc;++s){ata_wait();for(int i=0;i<256;++i)buf[s*256+i]=inw(ATA_DATA);} }
static void ata_write28(uint32_t lba,uint8_t sc,const void*src){
    ata_wait();outb(0x1F2,sc);outb(0x1F3,lba);outb(0x1F4,lba>>8);outb(0x1F5,lba>>16);outb(0x1F6,0xE0|((lba>>24)&0xF));outb(0x1F7,0x30);
    const uint16_t*buf=src;for(int s=0;s<sc;++s){ata_wait();for(int i=0;i<256;++i)outw(ATA_DATA,buf[s*256+i]);}ata_wait(); }

/* ---------- 2025FS structures ---------- */
#define ROOT_LBA 100
typedef struct {
    char     name[24];
    uint32_t lba;
    uint32_t size;   /* size==0 → directory */
} __attribute__((packed)) dirent_t;
_Static_assert(sizeof(dirent_t)==32,"dirent_t must be 32 bytes");

static uint32_t cur_dir = ROOT_LBA;
static uint32_t next_free_lba;
static uint8_t  sec[512];

/* --- FS helpers --- */
static void fs_write_dir(uint32_t dir){ata_write28(dir,1,sec);} 
static void fs_scan_root(void){                        /* find first free LBA */
    ata_read28(ROOT_LBA,1,sec);
    uint32_t max = ROOT_LBA+1;
    for(int o=0;o<512;o+=sizeof(dirent_t)){
        dirent_t*e=(dirent_t*)(sec+o);
        if(!e->name[0])break;
        uint32_t used_sectors = (e->size==0)?1:((e->size+511)/512);
        uint32_t end = e->lba + used_sectors;
        if(end>max)max=end;
    }
    next_free_lba=max;
}
static uint32_t get_free_lba(uint32_t sectors){uint32_t lba=next_free_lba;next_free_lba+=sectors;return lba;}

static int fs_find(uint32_t dir,const char*name,dirent_t*out){
    ata_read28(dir,1,sec);
    for(int o=0;o<512;o+=sizeof(dirent_t)){
        dirent_t*e=(dirent_t*)(sec+o);
        if(!e->name[0])break;
        if(!strcmp(name,e->name)){*out=*e;return 1;}
    }
    return 0;
}
static int fs_add_entry(uint32_t dir,const char*name,uint32_t lba,uint32_t sz){
    ata_read28(dir,1,sec);
    for(int o=0;o<512;o+=sizeof(dirent_t)){
        dirent_t*e=(dirent_t*)(sec+o);
        if(!e->name[0]){
            memset(e,0,sizeof(dirent_t));
            strncpy(e->name,name,24);
            e->lba=lba; e->size=sz;
            fs_write_dir(dir);
            return 1;
        }
    }
    return 0;
}
static int fs_load_file(const dirent_t*e,void*dst){uint32_t sc=(e->size+511)/512;ata_read28(e->lba,sc,dst);return e->size;}

/* ---------- ELF32 loader ---------- */
typedef struct{uint8_t e[52];} Elf32_Ehdr;
typedef struct{uint8_t p[32];} Elf32_Phdr;
#define PT_LOAD 1
static int elf_exec(void*img){
    Elf32_Ehdr*eh=img;if(eh->e[0]!=0x7F||eh->e[1]!='E'||eh->e[2]!='L'||eh->e[3]!='F'){puts("bad ELF\n");return -1;}
    uint32_t phoff=*(uint32_t*)(eh->e+0x1C);uint16_t phnum=*(uint16_t*)(eh->e+0x2C);
    for(uint16_t i=0;i<phnum;++i){
        Elf32_Phdr*ph=(void*)((uint8_t*)img+phoff+i*sizeof(Elf32_Phdr));
        if(*(uint32_t*)ph->p!=PT_LOAD)continue;
        uint32_t off=*(uint32_t*)(ph->p+4),vaddr=*(uint32_t*)(ph->p+8),sz=*(uint32_t*)(ph->p+16);
        memcpy((void*)vaddr,(uint8_t*)img+off,sz);
    }
    void(*entry)(void)=(void*)*(uint32_t*)(eh->e+0x18);
    puts("running elf...\n");entry();puts("elf returned\n");return 0;
}

/* ---------- CLI commands ---------- */
static void cmd_ls(void){
    ata_read28(cur_dir,1,sec);
    puts("Name                LBA   Size  T\n");
    for(int o=0;o<512;o+=sizeof(dirent_t)){
        dirent_t*e=(dirent_t*)(sec+o);if(!e->name[0])break;
        puts(e->name);for(int i=strlen(e->name);i<20;++i)putc(' ');
        print_uint(e->lba);putc(' ');print_uint(e->size);putc(' ');
        putc(e->size==0?'D':'F');putc('\n');
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
    for(;;){char t[100];getline(t,100);if(!strcmp(t,"."))break;int l=strlen(t);memcpy(buf+len,t,l);len+=l;buf[len++]='\n';}
    uint32_t sc_old= append ? (e.size+511)/512 : 0; uint32_t sc_new=(len+511)/512;
    if(!append||sc_new>sc_old){lba=get_free_lba(sc_new);}
    memset(buf+len,0,sc_new*512-len);
    ata_write28(lba,sc_new,buf);
    if(append){e.lba=lba;e.size=len;}
    if(!append) fs_add_entry(cur_dir,fn,lba,len);
    else {
        ata_read28(cur_dir,1,sec);
        for(int o=0;o<512;o+=sizeof(dirent_t)){
            dirent_t*de=(dirent_t*)(sec+o);
            if(!strcmp(de->name,fn)){*de=e;break;}
        }
        fs_write_dir(cur_dir);
    }
    puts(append?"appended\n":"written\n");
}

static void cmd_mkdir(const char*dn){
    if(strlen(dn)>23||fs_find(cur_dir,dn,&(dirent_t){0})){puts("exists or name too long\n");return;}
    uint32_t lba=get_free_lba(1);memset(sec,0,512);ata_write28(lba,1,sec);
    if(fs_add_entry(cur_dir,dn,lba,0))puts("directory created\n");
    else puts("dir full\n");
}

static void cmd_rm(const char*name){
    dirent_t e;if(!fs_find(cur_dir,name,&e)){puts("no such file\n");return;}
    if(e.size==0){puts("not a file\n");return;}
    ata_read28(cur_dir,1,sec);
    for(int o=0;o<512;o+=sizeof(dirent_t)){
        dirent_t*de=(dirent_t*)(sec+o);
        if(!strcmp(de->name,name)){memset(de,0,sizeof(dirent_t));fs_write_dir(cur_dir);puts("file deleted\n");return;}
    }
}

static void cmd_rmdir(const char*name){
    dirent_t e;if(!fs_find(cur_dir,name,&e)){puts("no such dir\n");return;}
    if(e.size!=0){puts("not a directory\n");return;}
    uint8_t buf[512];ata_read28(e.lba,1,buf);
    for(int i=0;i<512;i+=sizeof(dirent_t)){dirent_t*de=(dirent_t*)(buf+i);if(de->name[0]){puts("not empty\n");return;}}
    ata_read28(cur_dir,1,sec);
    for(int o=0;o<512;o+=sizeof(dirent_t)){
        dirent_t*de=(dirent_t*)(sec+o);
        if(!strcmp(de->name,name)){memset(de,0,sizeof(dirent_t));fs_write_dir(cur_dir);puts("directory removed\n");return;}
    }
}

static void cmd_devs(void){
    puts("\n  OS/2025 Contributors:\n  ----------------------\n  - kdy010010  (Lead developer)\n  - modemaker\n  - tajo1243\n  - JK\n\n");
}
/* --- 경로에서 디렉터리와 파일명 분리 --- */
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

/* --- 지정된 경로의 디렉터리 LBA 찾기 (상대 또는 절대) --- */
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
                if(!fs_find(dir,token,&e) || e.size!=0) return 0; /* not dir */
                dir = e.lba;
                j=0;
            }
            if(path[i]=='\0') break;
        } else token[j++]=path[i];
        i++;
    }
    return dir;
}

/* ---------- CLI loop ---------- */
static char line[128];
static void cli(void){
    for(;;){
        puts("OS/2025> ");getline(line,sizeof line);
        if(!strcmp(line,"help"))
            puts("help ls cd <d> mkdir <d> cat <f> write <f> append <f>\n"
                 "run <f> rm <f> rmdir <d> mv <old> <new> cp <src> <dst> root pwd reboot devs\n");
        else if(!strcmp(line,"ls")) cmd_ls();
        else if(!strncmp(line,"cd ",3)){
            dirent_t e;if(!fs_find(cur_dir,line+3,&e)||e.size!=0){puts("not dir\n");continue;}
            if(path_depth<MAX_DEPTH){path_lba[path_depth]=cur_dir;strncpy(path_name[path_depth],line+3,23);++path_depth;}cur_dir=e.lba;
        }
        else if(!strncmp(line,"mkdir ",6)) cmd_mkdir(line+6);
        else if(!strncmp(line,"cat ",4)){
            dirent_t e;if(!fs_find(cur_dir,line+4,&e)){puts("no file\n");continue;}if(e.size==0){puts("is dir\n");continue;}
            uint8_t*buf=(void*)0x200000;fs_load_file(&e,buf);for(uint32_t i=0;i<e.size;++i)putc(buf[i]);putc('\n');
        }
        else if(!strncmp(line,"write ",6)) file_write_or_append(line+6,0);
        else if(!strncmp(line,"append ",7)) file_write_or_append(line+7,1);
else if(!strncmp(line,"run ",4)){
    const char *fname = line + 4;
    dirent_t e;

    /* 1️ 현재 디렉터리 우선 */
    if(!fs_find(cur_dir, fname, &e)){
        /* 2️ 루트(/)의 bin/ 폴더 fallback */
        dirent_t bin;
        if( fs_find(ROOT_LBA, "bin", &bin) && bin.size == 0 ){   /* bin is dir */
            if( fs_find(bin.lba, fname, &e) == 0 ){              /* 여기도 실패 */
                puts("no file\n"); continue;
            }
        } else { puts("no file\n"); continue; }
    }

    if(e.size == 0){ puts("dir\n"); continue; }

    uint8_t *buf = (void*)0x400000;      /* 4 MiB 워킹 버퍼 */
    fs_load_file(&e, buf);
    elf_exec(buf);
}

        else if(!strcmp(line,"reboot")){outb(0x64,0xFE);__asm__ volatile("hlt");}
        else if(!strncmp(line,"rm ",3)) cmd_rm(line+3);
        else if(!strncmp(line,"rmdir ",6)) cmd_rmdir(line+6);
        else if(!strcmp(line,"root")){cur_dir=ROOT_LBA;path_depth=0;}
        else if(!strcmp(line,"pwd")){
            putc('/');for(int i=0;i<path_depth;++i){puts(path_name[i]);putc('/');}putc('\n');
        }
else if(!strncmp(line,"mv ",3)){
    char *args=line+3;
    char *space=args;
    while(*space && *space!=' ') ++space;
    if(!*space){puts("usage: mv <src> <dst>\n"); continue;}
    *space='\0';
    char *src=args, *dst=space+1;

    dirent_t e;
    if(!fs_find(cur_dir,src,&e)){ puts("no such file\n"); continue; }

    /* 1️ 대상 경로 분리 */
    char dirpart[64], namepart[24];
    split_path(dst, dirpart, namepart);

    /* 2️ 대상 디렉터리 찾기 */
    uint32_t dst_dir = resolve_dir(dirpart);
    if(!dst_dir){ puts("no such directory\n"); continue; }

    /* 3️ 이름 없으면 기존 이름 유지 */
    if(namepart[0]=='\0') strncpy(namepart, src, 24);

    /* 4️ 중복 이름 방지 */
    dirent_t tmp;
    if(fs_find(dst_dir,namepart,&tmp)){ puts("dest exists\n"); continue; }

    /* 5️ 복사 dirent */
    if(dst_dir != cur_dir){
        ata_read28(dst_dir,1,sec);
        fs_add_entry(dst_dir,namepart,e.lba,e.size);
        /* 원본 dirent 삭제 */
        ata_read28(cur_dir,1,sec);
        for(int o=0;o<512;o+=sizeof(dirent_t)){
            dirent_t*de=(dirent_t*)(sec+o);
            if(!strcmp(de->name,src)){ memset(de,0,sizeof(dirent_t)); break; }
        }
        fs_write_dir(cur_dir);
        puts("moved\n");
    } else {
        /* 같은 디렉터리 이름 변경 */
        ata_read28(cur_dir,1,sec);
        for(int o=0;o<512;o+=sizeof(dirent_t)){
            dirent_t*de=(dirent_t*)(sec+o);
            if(!strcmp(de->name,src)){ strncpy(de->name,namepart,24); fs_write_dir(cur_dir); puts("renamed\n"); break; }
        }
    }
}

else if(!strncmp(line,"cp ",3)){
    char *args=line+3;
    char *space=args;
    while(*space && *space!=' ') ++space;
    if(!*space){ puts("usage: cp <src> <dst>\n"); continue; }
    *space='\0';
    char *src=args, *dst=space+1;

    dirent_t e;
    if(!fs_find(cur_dir,src,&e)){ puts("no such file\n"); continue; }
    if(e.size==0){ puts("dir copy unsupported\n"); continue; }

    /* 1️ 경로 분리 */
    char dirpart[64], namepart[24];
    split_path(dst, dirpart, namepart);

    /* 2️ 대상 디렉터리 */
    uint32_t dst_dir = resolve_dir(dirpart);
    if(!dst_dir){ puts("no such directory\n"); continue; }

    /* 3️ 이름 없으면 원본 이름 */
    if(namepart[0]=='\0') strncpy(namepart, src, 24);

    if(strlen(namepart)>23){ puts("name too long\n"); continue; }
    if(fs_find(dst_dir,namepart,&(dirent_t){0})){ puts("dest exists\n"); continue; }

    /* 4️ 파일 복사 */
    uint8_t*buf=(void*)0x300000;
    fs_load_file(&e,buf);
    uint32_t sc=(e.size+511)/512;
    uint32_t lba=get_free_lba(sc);
    ata_write28(lba,sc,buf);
    fs_add_entry(dst_dir,namepart,lba,e.size);
    puts("copied\n");
}

        else if(!strcmp(line,"devs")) cmd_devs();
/* ── CLI 끝부분 ────────────────────────────────────────── */
/* --- 실행 시도: 현재 dir → /bin → .elf 보강 -------------------------- */
/* ─── 내부 전용 “이름 + .elf” 조합기 ─────────────────── */
/* ── CLI 끝부분 (작동을 안하는) */
else {                                    /* 아무 명령에도 안 맞을 때 */
    const char *fname = line;
    dirent_t e;

    /* 1) 현재 디렉터리 우선 */
    if (!fs_find(cur_dir, fname, &e)) {
        /* 2) /bin 폴더 fallback */
        dirent_t bin;
        if (fs_find(ROOT_LBA, "bin", &bin) && bin.size == 0) {
            if (!fs_find(bin.lba, fname, &e)) {   /* 거기에도 없으면 실패 */
                puts("?\n");
                continue;
            }
        } else {               /* /bin이 없거나 파일 못 찾음 */
            puts("?\n");
            continue;
        }
    }

    if (e.size == 0) {         /* 디렉터리라면 실행 거부 */
        puts("dir\n");
        continue;
    }

    uint8_t *buf = (void*)0x400000;   /* 4 MiB 임시 버퍼 */
    fs_load_file(&e, buf);
    elf_exec(buf);                    /* 정상 실행 */
}




    }
}

/* ---------- entry ---------- */
void kmain(void){
    fs_scan_root();
    puts("boot success\n========================================\n        OS/2025 v2 (dirent32)\n        Developed by kdy010010\n        Type 'devs' for contributors\n========================================\n\nType 'help' to see available commands.\n\n");
    cli();
}
