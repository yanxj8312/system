#include "memory.h"
#include "stdint.h"
#include "print.h"
#include "debug.h"
#include "bitmap.h"
#include "string.h"
#include "global.h"

#define PG_SIZE 4096

/*位图地址
 *因为0xc009f000是内核主线程的栈顶，0xc009f000就是内核主线程的pcb。
 *一个页框大小的位图可以表示128M内存，位图位置安排在地址0xc009a000
 *这样系统最大支持4个页框的位图，即512M
 */ 
#define MEM_BITMAP_BASE 0xc009a000

/*0xc0000000是从内核的3G开始
 * 0x100000意指跨过低端内存的1MB是虚拟地址在逻辑上连续*/
#define K_HEAP_START 0xc0100000
#define PDE_IDX(addr)	((addr & 0xffc00000) >> 22)
#define PTE_IDX(addr)	((addr & 0x003ff000) >> 12)

/*内存池结构，生成两个实例用户管理内核内存池和用户内存池*/
struct pool {
	struct bitmap pool_bitmap;	//内存池的位图结构，用于管理物理内存
	uint32_t phy_addr_start;	//内存池所管理物理内存的起始地址
	uint32_t pool_size;		//内存池的字节容量
};

struct pool kernel_pool, user_pool;	//生成内核内存池和用户内存池
struct virtual_addr kernel_vaddr;	//内核虚拟地址分配

/*在pf表示的虚拟内存池中申请pg_cnt个虚拟页，
 * 成功则返回虚拟页的地址，失败返回NULL*/
static void* vaddr_get(enum pool_flags pf, uint32_t pg_cnt) {
	int vaddr_start = 0, bit_idx_start = -1;
	uint32_t cnt = 0;
	if(pf == PF_KERNEL) {
		bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap,pg_cnt);
		if(bit_idx_start == -1) {
			return NULL;
		}
		while(cnt < pg_cnt) {
			bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++ ,1);
		}
		vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
	} else {
		//用户内存池，之后补充
	}

	return (void*)vaddr_start;
}

/*得到虚拟地址vaddr对应的pte指针*/
uint32_t* pte_ptr(uint32_t vaddr) {
	/*先访问页表自己
	 * 再用也目录pde作为pte的索引访问页表
	 * 再用pte的索引作为页内偏移*/
	uint32_t* pte = (uint32_t*)(0xffc00000 + \
		       	((vaddr & 0xffc00000) >> 10) + \
			PTE_IDX(vaddr) * 4);
	return pte;
}

/*得到虚地址vaddr对应的pde的指针*/
uint32_t* pde_ptr(uint32_t vaddr) { 
	/*0xfffff用来访问到页表本身所在的地址
	 * */
	uint32_t* pde = (uint32_t*)((0xfffff000) + PDE_IDX(vaddr) * 4);
	return pde;
}

/*在m_pool指向的物理内存池中分配一个物理页面，
 * 成功返回页框的物理地址，失败返回NULL*/
static void* palloc(struct pool* m_pool) {
	/*扫描或设置位图要保证原子操作*/
	int bit_idx = bitmap_scan(&m_pool->pool_bitmap,1);	//找一个物理页
	if(bit_idx == -1) {
		return NULL;
	}
	bitmap_set(&m_pool->pool_bitmap,bit_idx,1);	//将此bit_idx置1
	uint32_t page_phyaddr = ((bit_idx * PG_SIZE) + m_pool->phy_addr_start);
	return (void*)page_phyaddr;
}

/*页表中添加虚拟地址vaddr与物理地址page_phyaddr作映射*/
static void page_table_add(void* _vaddr,void* _page_phyaddr) {
	uint32_t vaddr = (uint32_t)_vaddr, page_phyaddr = (uint32_t)_page_phyaddr;
	uint32_t* pde = pde_ptr(vaddr);
	uint32_t* pte = pte_ptr(vaddr);

/***************************注意****************************
 *执行*pte，会访问到空的pde。所以确保pde创建完成之后才执行*pte
 否则会引发page_fault.因此在*pde为0时
 pte只能在else语句中的*pde后面
 */ 
	/*现在页目录内判断目录项的P，若为1表示已经存在*/
	if(*pde & 0x00000001) {
	//页目录项和页表项的第0位为P，此处判断目录项是否存在
		ASSERT(!(*pte & 0x00000001));
		if(!(*pte & 0x00000001)) {
		//只要是创建页表，pte就应该不存在
			*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
		} else {
			PANIC("pte repeat");
			*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
		}
	} else {	//页目录项不存在
	/*页表中用到的页框一律从内核空间分配*/
		uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);
		*pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
		/*分配到的物理页地址pde_phyaddr对应的物理内存清零
		 *避免成就数据变成页表项，从而让页表混乱
		 *访问到pde对应的物理地址，用pte取高20位便可。
		 *因为pte基于该pde对应的物理地址内再寻址，
		 *把低12位置0，便是该pde对应的物理页的起始地址。
		 */ 
		memset((void*)((int)pte & 0xfffff000), 0, PG_SIZE);

		ASSERT(!(*pte & 0x00000001));
		*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
	}
}

/*分配pg_cnt个页空间，成功返回起始虚拟地址，失败返回NULL*/
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt) {
	ASSERT(pg_cnt > 0 && pg_cnt < 3840);
/******************* malloc_page的原理是三个动作的合成********************
 *1,通过vaddr_get在虚拟内存池中申请虚拟地址
 *2,通过palloc在物理内存池中申请物理页
 *3,通过page_table_add将以上得到的虚拟地址和物理地址在页表中完成映射
 * ***********************************************************************/
	void* vaddr_start = vaddr_get(pf,pg_cnt);
	if(vaddr_start == NULL) {
		return NULL;
	}
	uint32_t vaddr = (uint32_t)vaddr_start, cnt = pg_cnt;
	struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;

	/*因为虚拟地址是连续的但物理地址可以是不连续的，所以需要逐个做映射。*/
	while(cnt-- > 0) {
		void* page_phyaddr = palloc(mem_pool);
		if(page_phyaddr == NULL) {	//失败需要将曾经申请的虚拟地址和物理页全部回滚。
			return NULL;
		}
		page_table_add((void*)vaddr, page_phyaddr);	//在页表中做映射
		vaddr += PG_SIZE;	//下一个虚拟页
	}
	return vaddr_start;
}

/*从内核物理内存池中申请1页内存，
 * 成功返回虚拟地址，失败返回NULL*/
void* get_kernel_pages(uint32_t pg_cnt) {
	void* vaddr = malloc_page(PF_KERNEL, pg_cnt);
	if(vaddr != NULL) {	//分配不为空，将页清零后返回
		memset(vaddr, 0, pg_cnt * PG_SIZE);
	}
	return vaddr;
}


/*初始化内存池*/
static void mem_pool_init(uint32_t all_mem) {
	put_str(" mem_pool_init start\n");
	uint32_t page_table_size = PG_SIZE * 256;
	//页表大小 = 1页的也目录表+第0和第768个页目录项指向同一个页表+
	//第769～1022个页目录项指向254个页表，共256个页框。
	uint32_t used_mem = page_table_size + 0x100000; //使用过的内存
	uint32_t free_mem = all_mem - used_mem;
	uint16_t all_free_pages = free_mem / PG_SIZE;
	//一页为4K，不管内存是不是4K的倍数，对于以页为单位的内存分配策略，
	//不足1页的内存不用考虑。
	
	uint16_t kernel_free_pages = all_free_pages / 2;
	uint16_t user_free_pages = all_free_pages - kernel_free_pages;

/*为了简化位图操作，余数不出力，坏处就是丢失内存。好处就是不用做内存越界检查，因为位图表示的内存少于实际物理内存*/
	uint32_t kbm_length = kernel_free_pages / 8;
//Kernel BitMap 的长度，位图中的一位表示一页，以字节为单位
	uint32_t ubm_length = user_free_pages / 8;
//user BitMap的长度
	
	uint32_t kp_start = used_mem;
//kernel pool start，内核内存池起始地址
	uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;
//user pool start, 用户内存池起始地址
	
	kernel_pool.phy_addr_start = kp_start;
	user_pool.phy_addr_start = up_start;

	kernel_pool.pool_size = kernel_free_pages * PG_SIZE;
	user_pool.pool_size = user_free_pages * PG_SIZE;

	kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
	user_pool.pool_bitmap.btmp_bytes_len = ubm_length;

/*	内核内存池和用户内存池位图
 *位图是全局的数据，长度不固定。
 *全局或静态的数组需要在编译的时候知道其长度
 *而我们需要根据总内存大小计算出需要多少字节
 *所以改为指定一块内存来生成位图。
 */ 
//内核使用的最高地址是0xc009f000,这是主线程的栈地址
//内核的大小预计为70KB左右
//32MB内存占用的位图是2KB
//内核内存池的位图先顶在MEM_BITMAP_BASE处
	kernel_pool.pool_bitmap.bits = (void*)MEM_BITMAP_BASE;
//用户内存池的位图紧紧更在内核内存池位图后
	user_pool.pool_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length);	
/*****************输出内存池信息**********************/
	put_str("	kernel_pool_bitmap_start:");
	put_int((int)kernel_pool.pool_bitmap.bits);
	put_str(" kernel_pool_phy_addr_start:");
	put_int(kernel_pool.phy_addr_start);
	put_str("\n");
	put_str("user_pool_bitmap_start:");
	put_int((int)user_pool.pool_bitmap.bits);
	put_str(" user_pool_phy_addr_start:");
	put_int(user_pool.phy_addr_start);
	put_str("\n");

	//将位图置0
	bitmap_init(&kernel_pool.pool_bitmap);
	bitmap_init(&user_pool.pool_bitmap);

	/*下面初始化内核虚拟地址的位图，按照实际物理内存大小生成数组*/
	kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;
	//用于维护内核堆的虚拟地址，所以要和内核池大小一致
	
	/*位图的数组指向一块未使用的内存，目前定位在内核内存知和用户内存池之外*/
	kernel_vaddr.vaddr_bitmap.bits = \
	(void*)(MEM_BITMAP_BASE + kbm_length + ubm_length);

	kernel_vaddr.vaddr_start = K_HEAP_START;
	bitmap_init(&kernel_vaddr.vaddr_bitmap);
	put_str("	mem_pool_init done\n");
}

/*内存管理部分初始化入口*/
void mem_init() {
	put_str("mem_init start\n");
	uint32_t mem_bytes_total = (*(uint32_t *)(0xb00));
	mem_pool_init(mem_bytes_total);		//初始化内存池
	put_str("mem_init done!");
}
