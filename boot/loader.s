%include "boot.inc"

SECTION LOADER vstart=LOADER_BASE_ADDR
; 刚进入保护模式，内核还未加载前的栈顶
LOADER_STACK_TOP equ LOADER_BASE_ADDR

;构建GDT以及内部的段描述符
GDT_BASE: dd 0x00000000
          dd 0x00000000
CODE_DESC: dd 0x0000FFFF
           dd DESC_CODE_HIGH4
DATA_STACK_DESC: dd 0x0000FFFF
                 dd DESC_DATA_HIGH4
VIDEO_DESC: dd 0x80000007 ; 显存段 limit=(0xbffff+1-0xb8000)/4k-1 = 7
            dd DESC_VIDEO_HIGH4

GDT_SIZE equ $-GDT_BASE
GDT_LIMIT equ GDT_SIZE-1
; 预留60个8字节空位
times 60 dq 0x0000

; 用于保存内存容量大小，此处地址为0x900+0x200(512)=0xb00
total_mem_bytes dd 0

; 指向GDT的48位指针，要存入GDTR中
gdt_ptr dw GDT_LIMIT
        dd GDT_BASE

; 手动对齐，total_mem_bytes4 + gdt_ptr6 + ards_buff244 + ards_nr2 = (0x100)256字节
; 为了使loader_start在文件中的偏移地址为0x200+0x100=0x300
ards_buf times 244 db 0
ards_nr dw 0


; 定义段选择子
SELECTOR_CODE equ (0x0001<<3) + TI_GDT + RPL0
SELECTOR_DATA equ (0x0002<<3) + TI_GDT + RPL0
SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0

loader_start:
; ---0xE820号方式获取内存信息, 内存信息使用ards结构描述, edx=0x534d4150('SMAP')---
    xor ebx, ebx ; 第一次ebx要请0，之后的循环中会自动改变为下一个待操作的ARDS的编号
    mov edx, 0x534d4150
    mov di, ards_buf ; 记录ARDS的缓冲区
.e820_mem_get_loop:
    mov eax, 0x0000e820 ; 每次中断以后，eax会变成'SMAP的ASCII编码0x534d4150, 所以每次循环都要重新赋值
    mov ecx, 20 ; ARDS的结构大小为20字节
    int 0x15
    jc .e820_failed_so_try_e801 ; 如果cf位为1则有错误发生，尝试下一种获取内存信息的方法
    add di, cx ; 移动ARDS缓冲区指针指向下一个待记录的位置
    inc word [ards_nr] ; ARDS的数量+1
    cmp ebx, 0 ; 如果ebx为0且cf不为1，则说明ards已经全部获取完成
    jnz .e820_mem_get_loop
; 遍历ards缓冲区中的ards
; 因为是32位系统，所以找出base_add_low + length_low最大值即内存的容量
    mov cx, [ards_nr]
    mov ebx, ards_buf
    xor edx, edx
.find_max_mem_area: ; 无需判断type，最大的内存一定是可被操作系统使用的
    mov eax, [ebx] ; base_add_low
    add eax, [ebx + 8] ; length
    add ebx, 20
    cmp edx, eax
    jge .next_ards
    mov edx, eax
.next_ards:
    loop .find_max_mem_area
    jmp .mem_get_ok
; ---0xe801号方式获取内存大小，最大支持4g---
.e820_failed_so_try_e801:
    mov ax, 0xe801
    int 0x15
    jc .e801_failed_so_try_88
; 先算出低15MB大小：ax * 1kb + 1mb(位于15mb-16mb之间，用于isa设备)
    mov cx, 0x400 ; 1kb
    mul cx
    shl edx, 16
    and eax, 0x0000ffff
    or edx, eax
    add edx, 0x100000 ; 1mb
    mov esi, edx ; 暂存低15mb的容量
; 再计算16mb直到4gb的内存大小: bx * 64kb
    xor eax, eax
    mov ax, bx
    mov ecx, 0x10000 ; 64kb
    mul ecx
    ; 最大也就4g，32位寄存器足够了
    add esi, eax
    mov edx, esi
    jmp .mem_get_ok
; --- 0x88号方式获取内存大小，最高只支持64mb---
.e801_failed_so_try_88:
    mov ah, 0x88
    int 0x15 ; 中断后ax中存入的是kb为单位的内存容量
    jc .error_hlt
    and eax, 0x0000ffff
    mov cx, 0x400
    mul cx
    shl edx, 16
    or edx, eax
    add edx, 0x100000 ; 0x88号只返回1mb以上的内存，所以总容量要加上这1mb
.mem_get_ok:
    mov [total_mem_bytes], edx

; 准备进入保护模式
; 1. 打开A20
; 2. 加载GDT
; 3. 将CR0的PE位置1
; ---打开A20---
    in al, 0x92
    or al, 00000010B
    out 0x92, al

; ---加载GDT---
    lgdt [gdt_ptr]

; ---打开PE开关---
    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax

    jmp dword SELECTOR_CODE:p_mode_start ;刷新流水线
; 出错的时候挂起
.error_hlt:
    hlt

[bits 32]
p_mode_start:
    mov ax, SELECTOR_DATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, LOADER_STACK_TOP
    mov ax, SELECTOR_VIDEO
    mov gs, ax
    ; 往显存段中写入数据
    mov byte [gs:160], 'P'
    mov byte [gs:162], ' '
    mov byte [gs:164], 'M'
    mov byte [gs:166], 'O'
    mov byte [gs:168], 'D'
    mov byte [gs:170], 'E'
    ; 读取磁盘中的内核到内存中
    mov eax, KERNEL_START_SECTOR
    mov ebx, KERNEL_BIN_BASE_ADDR
    mov ecx, 200
    call rd_disk_m_32
    ; 创建分页目录表及页表
    call setup_pages
; 为了把gdt在内存中的映射放到内核态，当前gdt的地址为0x900，因此要放到高1gb中，即0xc0000000+0x900
; 显存也要放到内核态去，因此gdt中关于显存的段描述符中的段基址也要加上0xc0000000
    sgdt [gdt_ptr] ; 先存gdtr的值到gdt_ptr的位置
    ; 将显存的段基址+0xc0000000
    mov ebx, [gdt_ptr + 2] ; ebx为gdt的起始位置
    or dword [ebx + 0x18 + 4], 0xc0000000
    ; 将gdt的基址+ 0xc0000000
    add dword [gdt_ptr + 2], 0xc0000000

    add esp, 0xc0000000

    ; 赋值目录表地址给CR3
    mov eax, PAGE_DIR_TABLE_POS
    mov cr3, eax

    ; 打开CR0的PG位
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ; 开启分页后重新加载GDT
    lgdt [gdt_ptr]

    ; 往显存段中写入数据
    mov byte [gs:320], 'V'
    mov byte [gs:322], ' '
    mov byte [gs:324], 'A'
    mov byte [gs:326], 'D'
    mov byte [gs:328], 'D'
    mov byte [gs:330], 'R'
    ; 强制刷新流水线，进入内核
    jmp SELECTOR_CODE:enter_kernel

enter_kernel:
    call kernel_init
    mov esp, 0xc009f000 ; 4k对齐，所以选1m内内核可用的空间里的最大的4k对齐的地址
    jmp KERNEL_ENTRY_POINT



; 将kernel拷贝到内存中，并将段拷贝到编译的地址
kernel_init:
    xor eax, eax
    xor ebx, ebx ; elf程序头表的起始位置
    xor ecx, ecx ; elf程序头表中program_header的个数
    xor edx, edx ; 程序头每一项的大小

    mov dx, [KERNEL_BIN_BASE_ADDR+42] ; e_phentsize
    mov ebx, [KERNEL_BIN_BASE_ADDR+28] ; e_phoff
    add ebx, KERNEL_BIN_BASE_ADDR ; 程序头表的起始位置
    mov cx, [KERNEL_BIN_BASE_ADDR+44] ; e_phnum

.each_segment:
    cmp byte [ebx+0], PT_NULL ; 比较p_type如果等于PT_NULL则说明这个段没用
    je .PTNULL

    ; 为mem_cpy压入参数：mem_cpy(dst, src, size)
    push dword [ebx+16] ; p_filesz
    mov eax, [ebx+4] ; p_offset
    add eax, KERNEL_BIN_BASE_ADDR ; 该段的起始地址
    push eax
    push dword [ebx+8] ; p_vaddr, 目标地址
    call mem_cpy
    add esp, 12 ; 清理之前入栈的3个参数
.PTNULL:
    add ebx, edx ; 指向下一个段头
    loop .each_segment
    ret


; 逐字节拷贝
; DS:ESI -> ES:EDI
mem_cpy:
    cld
    push ebp
    mov ebp, esp
    push ecx; ecx 存栈， 外层循环可能用到，先保存
    mov edi, [ebp + 8] ; dst
    mov esi, [ebp + 12] ; src
    mov ecx, [ebp + 16] ; size
    rep movsb

    pop ecx
    pop ebp
    ret


; 创建目录及页表
; 目录表起始地址0x100000, 第一个页表地址0x101000, 内核在最低的1mb空间，用户态程序映射高1gb到内核
setup_pages:
    ; 先清空目录表所在内存
    mov ecx, 4096
    mov esi, 0
.clear_page_dir:
    mov byte [PAGE_DIR_TABLE_POS + esi], 0
    inc esi
    loop .clear_page_dir
; 开始创建目录表，添加目录表项PDE
.create_pde:
    mov eax, PAGE_DIR_TABLE_POS
    add eax, 0x1000 ; eax为第一个页表所在的位置，第一个页表将用来描述内核所在的最低1mb物理地址
    mov ebx, eax
    ; 页属性为用户态特权级可用，可读写，在内存中存在
    or eax, PG_US_U | PG_RW_W | PG_P
    ; 将目录第一项映射到第一个页表
    ; 为了切换到分页机制后，loader中的通过段机制访问到的物理内存地址能和通过分页机制访问到的物理地址相同
    ; 都为最低的1mb, 即在最低的1mb中，虚拟地址等于物理地址
    mov [PAGE_DIR_TABLE_POS + 0x0], eax
    ; 将第768项直到1022项，共255个页表映射1gb-4m空间到内核态，虽然内核代码只占据了最低的1mb
    ; 开启分页机制以后，访问0xc0000000以上的地址即访问内核
    mov [PAGE_DIR_TABLE_POS + 0xc00], eax
    ; 将目录表的最后一项映射到目录表本身，为了以后通过这个地址可以修改页表内容
    sub eax, 0x1000
    mov [PAGE_DIR_TABLE_POS + 4092], eax
    ; 创建页表项
    mov ecx, 256
    mov esi, 0
    mov edx, PG_US_U | PG_RW_W | PG_P
.create_pte: ; ebx为第一个页表项的地址，先映射1m到内核（物理内存最低的1mb, 0x00000-0xfffff）, 1mb / 4k = 256项
    mov [ebx+esi*4], edx
    add edx, 4096
    inc esi
    loop .create_pte
    ; 填写目录表中的769-1022项，即映射到内核的其他目录项
    ; 虽然内核当前只有1mb大小，把目录项全部填满是为了以后各个用户态之间关于内核映射的同步实现起来方便
    mov eax, PAGE_DIR_TABLE_POS
    add eax, 0x2000 ; eax为第二个页表的位置
    or eax, PG_US_U | PG_RW_W | PG_P
    mov ebx, PAGE_DIR_TABLE_POS
    mov ecx, 254
    mov esi, 769
.create_kernel_pde:
    mov [ebx+esi*4], eax
    inc esi
    add eax, 4096 ; 指向下一个页表
    loop .create_kernel_pde
    ret

; 功能:读取硬盘n个扇区
; eax=LBA扇区号
; ebx=将数据写入的内存地址
; ecx=读入的扇区数
rd_disk_m_32:	   
    mov esi, eax	   ; 备份eax
    mov di, cx		   ; 备份扇区数到di
; 读写硬盘:
; 第1步：设置要读取的扇区数
    mov dx, 0x1f2
    mov al, cl
    out dx, al            ;读取的扇区数
    mov eax, esi	   ;恢复ax
; 第2步：将LBA地址存入0x1f3 ~ 0x1f6
    ; LBA地址7~0位写入端口0x1f3
    mov dx, 0x1f3                       
    out dx, al                          
    ; LBA地址15~8位写入端口0x1f4
    mov cl, 8
    shr eax, cl
    mov dx, 0x1f4
    out dx, al
    ; LBA地址23~16位写入端口0x1f5
    shr eax, cl
    mov dx, 0x1f5
    out dx, al
    ; LBA扇区号第27 ～ 24位写入到device端口低4位，高4位设置成1110，表示lba模式下的主盘
    shr eax, cl
    and al, 0x0f	   
    or al, 0xe0	   
    mov dx, 0x1f6
    out dx, al
;第3步：向0x1f7端口写入读命令，0x20 
    mov dx, 0x1f7
    mov al, 0x20                        
    out dx, al
;第4步：检测硬盘状态
.not_ready:		   ;测试0x1f7端口(status寄存器)的的BSY位
    nop
    in al, dx
    and al, 0x88	   ;第4位为1表示硬盘控制器已准备好数据传输,第7位为1表示硬盘忙
    cmp al, 0x08
    jnz .not_ready	   ;若未准备好,继续等。
;第5步：从0x1f0端口读数据
    mov ax, di
    mov dx, 256	   ;di为要读取的扇区数,一个扇区有512字节,每次读入一个字,共需di*512/2次,所以di*256
    mul dx
    mov cx, ax	   
    mov dx, 0x1f0
.go_on_read:
    in ax, dx		
    mov [ebx], ax
    add ebx, 2
    loop .go_on_read
    ret