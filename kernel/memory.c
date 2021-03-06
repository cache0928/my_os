#include "memory.h"
#include "stdint.h"
#include "print.h"
#include "bitmap.h"
#include "string.h"
#include "debug.h"
#include "global.h"
#include "sync.h"
#include "interrupt.h"
#include "stdio-kernel.h"


// 获取虚拟地址的目录表索引
#define PDE_IDX(addr) ((addr & 0xffc00000) >> 22)
// 获取虚拟地址的页表索引
#define PTE_IDX(addr) ((addr & 0x003ff000) >> 12)
// 获取虚拟地址对应的PTE指针
uint32_t *pte_ptr(uint32_t vaddr) {
    uint32_t *pte = (uint32_t *)(0xffc00000 + ((vaddr & 0xffc00000) >> 10) + PTE_IDX(vaddr) * 4);
    return pte;
}
// 获取虚拟地址对应的PDE指针
uint32_t *pde_ptr(uint32_t vaddr) {
    uint32_t *pde = (uint32_t *)((0xfffff000) + PDE_IDX(vaddr) * 4);
    return pde;
}

// 内核主线程栈顶为0xc009f000
// 设计上0xc009e000 - 0xc009efff 为主线程PCB
// 一个页框对应128M内存，设计为0xc009a000 - 0xc009dfff共4页来存放各种内存池的位图
#define MEM_BITMAP_BASE 0xc009a000

// 内核堆的起始地址, 跨过低端1mb连续
#define K_HEAP_START 0xc0100000

// 物理内存池结构
struct pool {
    struct bitmap pool_bitmap; // 物理内存池的位图
    uint32_t phy_addr_start; // 物理内存池的起始地址
    uint32_t pool_size; // 物理内存池的字节容量
    struct lock lock; // 操作物理内存池要加锁
};

// 堆内存arena分配模型
struct arena {
    struct mem_block_desc *desc; // 指向对应的内存描述符
    // 大于1024的属于大内存，小于等于1024的属于小内存
    bool large;
    // 大内存时，表示该arena所占用的页数，小内存时表示所剩的空闲小内存块数
    uint32_t cnt;
};

// 内核态所用的小内存描述符数组，7个，用户态的直接存自己的pcb中
struct mem_block_desc k_block_descs[DESC_CNT];

struct pool kernel_pool, user_pool;
struct virtual_addr kernel_vaddr;

// 在pf表示的虚拟内存池中申请连续pg_cnt个虚拟页，成功返回虚拟页的起始地址，失败返回NULL
static void *vaddr_get(enum pool_flags pf, uint32_t pg_cnt) {
    int vaddr_start = 0;
    int bit_idx_start = -1;
    uint32_t cnt = 0;
    if (pf == PF_KERNEL) {
        // 内核虚拟池
        bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);
        if (bit_idx_start == -1) {
            return NULL;
        }
        while (cnt < pg_cnt) {
            // 位图中标记这些页已经使用
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
        }
        vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
    } else {
        // 用户态程序虚拟池
        struct task_struct *cur = running_thread();
        bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap, pg_cnt);
        if (bit_idx_start == -1) {
            return NULL;
        }
        while (cnt < pg_cnt) {
            bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
        }
        vaddr_start = cur->userprog_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
        // 用户栈和堆没碰撞
        ASSERT((uint32_t)vaddr_start < (0xc0000000 - PG_SIZE));
    }
    return (void *)vaddr_start;
}

// 在m_pool指向的物理池中分配一个物理页，成功返回页框的物理地址，失败则返回NULL
static void *palloc(struct pool *m_pool) {
    int bit_idx = bitmap_scan(&m_pool->pool_bitmap, 1);
    if (bit_idx == -1) {
        return NULL;
    }
    // 在位图中标记该页已使用
    bitmap_set(&m_pool->pool_bitmap, bit_idx, 1);
    uint32_t page_phyaddr = ((bit_idx * PG_SIZE) + m_pool->phy_addr_start);
    return (void *)page_phyaddr;
}

// 关联某个虚拟地址和物理地址，即在虚拟地址对应的PTE中填写物理地址和一些属性
static void page_table_add(void *_vaddr, void *_page_phyaddr) {
    uint32_t vaddr = (uint32_t)_vaddr;
    uint32_t page_phyaddr = (uint32_t)_page_phyaddr;
    uint32_t *pde = pde_ptr(vaddr);
    uint32_t *pte = pte_ptr(vaddr);
    // 填表的步骤：
    // 先查看虚拟地址对应的页表在不在内存中，即看pde指向的内容的P位是不是1
    // 如果页表不在内存中，在内核的物理池中申请一个页框来放页表
    // 查看虚拟地址对应的物理页在不在内存中，即看pte指向的内容的P位是不是1
    // 新申请的物理地址一般肯定还没有关联到页表中的
    // 如果不是1，即代表该物理页还不在内存中，因此可以关联
    // 就将PTE填写成与该物理页相关的数据
    if (*pde & 0x00000001) {
        ASSERT(!(*pte & 0x00000001));
        if (!(*pte & 0x00000001)) {
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        } else {
            PANIC("pte repeat");
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        }
    } else {
        // 页表都还不存在，因此要先创页表，再对物理页进行映射
        uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);
        *pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        // 新的页表清零，避免有些脏数据
        memset((void *)((uint32_t)pte & 0xfffff000), 0, PG_SIZE);
        ASSERT(!(*pte & 0x00000001));
        *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
    }
}

// 请求分配pg_cnt个页，成功返回起始虚拟地址，失败返回NULL
static void *malloc_page(enum pool_flags pf, uint32_t pg_cnt) {
    // 请求分配的内存要大于0，小于物理池上限
    ASSERT(pg_cnt > 0 && pg_cnt < 3840);
    // 请求分配内存的过程：
    // 1. 通过vaddr_get在虚拟池中申请连续的虚拟页
    // 2. 通过palloc在物理池中申请物理页（不一定要连续）
    // 3. 通过page_table_add将以上的虚拟页和物理页进行关联
    void *vaddr_start = vaddr_get(pf, pg_cnt);
    if (vaddr_start == NULL) {
        return NULL;
    }
    uint32_t vaddr = (uint32_t)vaddr_start, cnt = pg_cnt;
    struct pool *mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    // 虚拟地址连续，而物理地址可以不连续，所以逐个映射
    while (cnt-- > 0) {
        void *page_phyaddr = palloc(mem_pool);
        if (page_phyaddr == NULL) {
            // 这里还差回滚，因为失败的话其实位图已经被改了，需要改回去
            return NULL;
        }
        page_table_add((void *)vaddr, page_phyaddr); // 关联虚拟页和物理页
        vaddr += PG_SIZE;
    }
    return vaddr_start;
}

// 在内核物理池中申请pg_cnt个页，成功返回虚拟页起始地址，失败返回NULL
void *get_kernel_pages(uint32_t pg_cnt) {
    lock_acquire(&kernel_pool.lock);
    void *vaddr = malloc_page(PF_KERNEL, pg_cnt);
    if (vaddr != NULL) {
        // 将申请到的物理空间全部清0
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
    lock_release(&kernel_pool.lock);
    return vaddr;
}

// 在用户态物理池中申请pg_cnt个页，成功返回虚拟页起始地址，失败返回NULL
void *get_user_pages(uint32_t pg_cnt) {
    lock_acquire(&user_pool.lock);
    void *vaddr = malloc_page(PF_USER, pg_cnt);
    if (vaddr != NULL) {
        memset(vaddr, 0, pg_cnt * PG_SIZE);
    }
    lock_release(&user_pool.lock);
    return vaddr;
}

// 根据提供的虚拟地址申请一页，成功返回该虚拟地址，失败返回NULL
void *get_a_page(enum pool_flags pf, uint32_t vaddr) {
    struct pool *mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);
    struct task_struct *cur = running_thread();
    int32_t bit_idx = -1;
    if (cur->pgdir != NULL && pf == PF_USER) {
        // 用户进程，直接修改用户虚拟池位图
        bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
    } else if (cur->pgdir == NULL && pf == PF_KERNEL) {
        // 内核线程，修改内核虚拟池位图
        bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
    } else {
        // 不允许跨界访问内存
        PANIC("get_a_page: not allow kernel alloc userspace or user alloc kernel space by get_a_page");
    }
    void *page_phyaddr = palloc(mem_pool);
    if (page_phyaddr == NULL) {
        return NULL;
    }
    page_table_add((void *)vaddr, page_phyaddr);
    lock_release(&mem_pool->lock);
    return (void *)vaddr;
}

// 根据提供的虚拟地址申请一页，但是不改变位图，专门用于fork时的用户虚拟池复制
void *get_a_page_without_opvaddrbitmap(enum pool_flags pf, uint32_t vaddr) {
    struct pool *mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);
    void *page_phyaddr = palloc(mem_pool);
    if (page_phyaddr == NULL) {
        lock_release(&mem_pool->lock);
        return NULL;
    }
    page_table_add((void *)vaddr, page_phyaddr); 
    lock_release(&mem_pool->lock);
    return (void *)vaddr;
}

// 从虚拟地址获取到物理地址
uint32_t addr_v2p(uint32_t vaddr) {
    uint32_t *pte = pte_ptr(vaddr);
    return (*pte & 0xfffff000) + (vaddr & 0x00000fff);
}

// 初始化内存池，参数为总物理内存的字节容量
static void mem_pool_init(uint32_t all_mem) {
    put_str("   mem_pool_init start\n");
    // 已经占用了空间的页表：1个目录表+对应内核的255个页表
    uint32_t page_table_size = PG_SIZE * 256;
    // 已使用的物理内存大小，已占空间的页表+低端1mb
    uint32_t used_mem = page_table_size + 0x100000;
    // 可用物理内存
    uint32_t free_mem = all_mem - used_mem;
    // 可用物理内存对应的页数, 不足4k的直接忽略不要了
    uint16_t all_free_pages = free_mem / PG_SIZE;
    // 分给内核的物理页数量
    uint16_t kernel_free_pages = all_free_pages / 2;
    // 分给用户态程序的物理页数量
    uint16_t user_free_pages = all_free_pages - kernel_free_pages;

    // 内核物理池位图的字节容量，位图中1位表示1页
    uint32_t kbm_length = kernel_free_pages / 8;
    // 用户态程序物理池的字节容量
    uint32_t ubm_length = user_free_pages / 8;
    // 内核物理池的起始地址直接从紧贴着低端1mb+已占空间的页表的地址之后开始
    uint32_t kp_start = used_mem;
    // 用户物理池紧贴着内核物理池
    uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;

    kernel_pool.phy_addr_start = kp_start;
    user_pool.phy_addr_start = up_start;
    kernel_pool.pool_size = kernel_free_pages * PG_SIZE;
    user_pool.pool_size = user_free_pages * PG_SIZE;
    kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
    user_pool.pool_bitmap.btmp_bytes_len = ubm_length;
    // 设计上从0xc009a000开始部署位图
    kernel_pool.pool_bitmap.bits = (void *)MEM_BITMAP_BASE;
    user_pool.pool_bitmap.bits = (void *)(MEM_BITMAP_BASE + kbm_length);
    
    put_str("      kernel_pool_bitmap_start:");
    put_int((int)kernel_pool.pool_bitmap.bits);
    put_str(" kernel_pool_phy_addr_start:");
    put_int(kernel_pool.phy_addr_start);
    put_char('\n');
    put_str("      user_pool_bitmap_start:");
    put_int((int)user_pool.pool_bitmap.bits);
    put_str(" user_pool_phy_addr_start:");
    put_int(user_pool.phy_addr_start);
    put_char('\n');

    // 将位图置0
    bitmap_init(&kernel_pool.pool_bitmap);
    bitmap_init(&user_pool.pool_bitmap);

    // 锁初始化
    lock_init(&kernel_pool.lock);
    lock_init(&user_pool.lock);

    // 设置内核虚拟池位图
    kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;
    kernel_vaddr.vaddr_bitmap.bits = (void *)(MEM_BITMAP_BASE + kbm_length + ubm_length);
    kernel_vaddr.vaddr_start = K_HEAP_START;
    bitmap_init(&kernel_vaddr.vaddr_bitmap);

    put_str("   mem_pool_init done\n");
}

// 构造内存块描述符数组，每组7个
void block_desc_init(struct mem_block_desc *desc_array) {
    uint16_t desc_idx, block_size = 16;
    for (desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
        desc_array[desc_idx].block_size = block_size;
        desc_array[desc_idx].blocks_per_arena = (PG_SIZE - sizeof(struct arena)) / block_size;
        list_init(&desc_array[desc_idx].free_list);
        block_size *= 2;
    }
}

void mem_init() {
    put_str("mem_init_start\n");
    // 在bootloader里保存了获取到的物理内存大小到0xb00
    uint32_t mem_bytes_total = (*(uint32_t *)(0xb00));
    mem_pool_init(mem_bytes_total);
    block_desc_init(k_block_descs);
    put_str("mem_init done\n");
}

// 返回arean中第idx个内存块地址
static struct mem_block *arena2block(struct arena *a, uint32_t idx) {
    return (struct mem_block *)((uint32_t)a + sizeof(struct arena) + idx * a->desc->block_size);
}

// 根据内存块找到areana的地址
static struct arena *block2arena(struct mem_block *b) {
    return (struct arena *)((uint32_t)b & 0xfffff000);
}

// 在堆中申请size字节的内存
void *sys_malloc(uint32_t size) {
    enum pool_flags PF;
    struct pool *mem_pool;
    uint32_t pool_size;
    struct mem_block_desc *descs;
    struct task_struct *cur_thread = running_thread();
    if (cur_thread->pgdir == NULL) {
        // 内核线程
        PF = PF_KERNEL;
        pool_size = kernel_pool.pool_size;
        mem_pool = &kernel_pool;
        descs = k_block_descs;
    } else {
        // 用户进程
        PF = PF_USER;
        pool_size = user_pool.pool_size;
        mem_pool = &user_pool;
        descs = cur_thread->u_blcok_desc;
    }
    // 如果申请的内存过大
    if (!(size > 0 && size < pool_size)) {
        return NULL;
    }
    struct arena *a;
    struct mem_block *b;
    lock_acquire(&mem_pool->lock);
    if (size > 1024) {
        // 大内存，直接按页分配
        uint32_t page_cnt = DIV_ROUND_UP(size + sizeof(struct arena), PG_SIZE);
        a = malloc_page(PF, page_cnt);
        if (a != NULL) {
            memset(a, 0, page_cnt * PG_SIZE);
            a->desc = NULL; // 按页分配的直接不要对应描述符了
            a->cnt = page_cnt;
            a->large = true;
            lock_release(&mem_pool->lock);
            // 跨过arena头信息大小，直接返回可用空间的起始地址
            return (void *)(a + 1);
        } else {
            lock_release(&mem_pool->lock);
            return NULL;
        }

    } else {
        // 小内存，从arena的free_list中寻找空闲内存块
        // 先适配最佳的内存描述符，比如申请100byte，那用128byte的描述符最合适
        uint8_t desc_idx;
        for (desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
            if (size <= descs[desc_idx].block_size) {
                // 找到最匹配的，也就是最小的
                break;
            }
        }
        if (list_empty(&descs[desc_idx].free_list)) {
            // 该描述符已经没有对应的拥有空闲控件的arena
            // 新建一个arena，并与描述符关联
            a = malloc_page(PF, 1);
            if (a == NULL) {
                lock_release(&mem_pool->lock);
                return NULL;
            }
            memset(a, 0, PG_SIZE);
            a->desc = &descs[desc_idx];
            a->large = false;
            a->cnt = descs[desc_idx].blocks_per_arena;

            uint32_t block_idx;
            enum intr_status old_status = intr_disable();
            // 拆分新建的arena的内存块，添加到描述符的free_list中
            for (block_idx = 0; block_idx < descs[desc_idx].blocks_per_arena; block_idx++) {
                b = arena2block(a, block_idx);
                ASSERT(!elem_find(&a->desc->free_list, &b->free_elem));
                list_append(&a->desc->free_list, &b->free_elem);
            }
            intr_set_status(old_status);
        }
        // 分配
        b = elem2entry(struct mem_block, free_elem, list_pop(&(descs[desc_idx].free_list)));
        memset(b, 0, descs[desc_idx].block_size);
        a = block2arena(b);
        a->cnt--;
        lock_release(&mem_pool->lock);
        return (void *)b;
    }
}

// 将物理地址pg_phy_addr的物理页回收到物理池，即将对应位图的位置0
static void pfree(uint32_t pg_phy_addr) {
    struct pool *mem_pool;
    uint32_t bit_idx = 0;
    if (pg_phy_addr >= user_pool.phy_addr_start) {
        // 用户态物理池
        mem_pool = &user_pool;
        bit_idx = (pg_phy_addr - user_pool.phy_addr_start) / PG_SIZE;
    } else {
        // 内核物理池
        mem_pool = &kernel_pool;
        bit_idx = (pg_phy_addr - kernel_pool.phy_addr_start) / PG_SIZE;
    }
    // 位图置0
    bitmap_set(&mem_pool->pool_bitmap, bit_idx, 0);
}

// 解绑虚拟地址vaddr与其对应的物理地址，即将相应的PTE的P位置0
static void page_table_pte_remove(uint32_t vaddr) {
    uint32_t *pte = pte_ptr(vaddr);
    *pte &= ~PG_P_1;
    // 刷新TLB
    asm volatile ("invlpg %0" : : "m" (vaddr) : "memory");
}

// 在虚拟池中释放以_vaddr开头的pg_cnt个虚拟页地址，即将对应位图置0
static void vaddr_remove(enum pool_flags pf, void *_vaddr, uint32_t pg_cnt) {
    uint32_t bit_idx_start = 0, vaddr = (uint32_t)_vaddr, cnt = 0;
    if (pf == PF_KERNEL) {
        // 内核虚拟池
        bit_idx_start = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        while (cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 0);
        }
    } else {
        // 用户虚拟池
        struct task_struct *cur_thread = running_thread();
        bit_idx_start = (vaddr - cur_thread->userprog_vaddr.vaddr_start) / PG_SIZE;
        while (cnt < pg_cnt) {
            bitmap_set(&cur_thread->userprog_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 0);
        }
    }
}

// 释放以vaddr起始的对应的pg_cnt个物理页
void mfree_page(enum pool_flags pf, void *_vaddr, uint32_t pg_cnt) {
    // 1. 先释放物理池
    // 2. 删除页表中对应的pte，即P位置0
    // 3. 最后释放虚拟池
    uint32_t pg_phy_addr;
    uint32_t vaddr = (uint32_t)_vaddr, page_cnt = 0;
    ASSERT(pg_cnt >= 1 && vaddr % PG_SIZE == 0);
    // 获取虚拟地址对应的物理地址
    pg_phy_addr = addr_v2p(vaddr);
    // 确保不释放最低的1mb内核空间和2kb内核页表
    ASSERT(pg_phy_addr % PG_SIZE == 0 && pg_phy_addr >= 0x102000);
    if (pg_phy_addr >= user_pool.phy_addr_start) {
        // 用户池
        vaddr -= PG_SIZE;
        while (page_cnt < pg_cnt) {
            // 物理池是散的，非连续的，所以一页页来
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);
            ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= user_pool.phy_addr_start);
            pfree(pg_phy_addr);
            page_table_pte_remove(vaddr);
            page_cnt++;
        }
        vaddr_remove(pf, _vaddr, pg_cnt);
    } else {
        // 内核池
        vaddr -= PG_SIZE;
        while (page_cnt < pg_cnt) {
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);
            ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= kernel_pool.phy_addr_start && pg_phy_addr < user_pool.phy_addr_start);
            pfree(pg_phy_addr);
            page_table_pte_remove(vaddr);
            page_cnt++;
        }
        vaddr_remove(pf, _vaddr, pg_cnt);
    }
}

// 回收内存, 按小内存还是大内存来分
// 如果是大内存，直接按页回收
// 如果是小内存，则将对应的mem_block加入对应的内存描述符的free_list
// 如果arena中所有的mem_block都空闲，则直接回收这个arena
void sys_free(void *ptr) {
    ASSERT(ptr != NULL);
    if (ptr != NULL) {
        enum pool_flags PF;
        struct pool *mem_pool;
        if (running_thread()->pgdir == NULL) {
            // 线程
            ASSERT((uint32_t)ptr >= K_HEAP_START);
            PF = PF_KERNEL;
            mem_pool = &kernel_pool;
        } else {
            // 用户进程
            PF = PF_USER;
            mem_pool = &user_pool;
        }

        lock_acquire(&mem_pool->lock);
        struct mem_block *b = ptr;
        struct arena *a = block2arena(b);
        ASSERT(a->large == 0 || a->large == 1);
        if (a->desc == NULL && a->large == true) {
            // 大内存，直接按页回收
            mfree_page(PF, a, a->cnt);
        } else {
            // 小内存
            list_append(&a->desc->free_list, &b->free_elem);
            // 判断是否该arena中所有的mem_block 都空闲
            if (++a->cnt == a->desc->blocks_per_arena) {
                uint32_t block_idx;
                for (block_idx = 0; block_idx < a->desc->blocks_per_arena; block_idx++) {
                    struct mem_block *b = arena2block(a, block_idx); 
                    ASSERT(elem_find(&a->desc->free_list, &b->free_elem)); 
                    list_remove(&b->free_elem);
                }
                mfree_page(PF, a, 1);
            }
        }
        lock_release(&mem_pool->lock);
    }
}

// 根据物理页框地址pg_phy_addr在相应的内存池的位图清0,不改动页表
void free_a_phy_page(uint32_t pg_phy_addr) {
    struct pool *mem_pool;
    uint32_t bit_idx = 0;
    if (pg_phy_addr >= user_pool.phy_addr_start) {
        mem_pool = &user_pool;
        bit_idx = (pg_phy_addr - user_pool.phy_addr_start) / PG_SIZE;
    } else {
        mem_pool = &kernel_pool;
        bit_idx = (pg_phy_addr - kernel_pool.phy_addr_start) / PG_SIZE;
    }
    bitmap_set(&mem_pool->pool_bitmap, bit_idx, 0);
 }