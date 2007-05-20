/* Needed so the AMD K8 runs correctly.  */
/* this should be done by Eric
 * 2004.11 yhlu add d0 e0 support
 * 2004.12 yhlu add dual core support
 * 2005.02 yhlu add e0 memory hole support

 * Copyright 2005 AMD
 * 2005.08 yhlu add microcode support 
*/
#include <console/console.h>
#include <cpu/x86/msr.h>
#include <cpu/amd/mtrr.h>
#include <device/device.h>
#include <device/pci.h>
#include <string.h>
#include <cpu/x86/msr.h>
#include <cpu/x86/pae.h>
#include <pc80/mc146818rtc.h>
#include <cpu/x86/lapic.h>

#include "../../../northbridge/amd/amdk8/amdk8.h"

#include <cpu/amd/model_fxx_rev.h>
#include <cpu/cpu.h>
#include <cpu/x86/cache.h>
#include <cpu/x86/mtrr.h>
#include <cpu/x86/mem.h>

#include <cpu/amd/dualcore.h>

#include <cpu/amd/model_fxx_msr.h>

void cpus_ready_for_init(void)
{
#if MEM_TRAIN_SEQ == 1
        struct sys_info *sysinfox = (struct sys_info *)((CONFIG_LB_MEM_TOPK<<10) - DCACHE_RAM_GLOBAL_VAR_SIZE);
        // wait for ap memory to trained
        wait_all_core0_mem_trained(sysinfox);
#endif
}


#if K8_REV_F_SUPPORT == 0
int is_e0_later_in_bsp(int nodeid)
{
        uint32_t val;
        uint32_t val_old;
        int e0_later;
        if(nodeid==0) { // we don't need to do that for node 0 in core0/node0
                return !is_cpu_pre_e0();
        }
        // d0 will be treated as e0 with this methods, but the d0 nb_cfg_54 always 0
        device_t dev;
        dev = dev_find_slot(0, PCI_DEVFN(0x18+nodeid,2));
        if(!dev) return 0;
        val_old = pci_read_config32(dev, 0x80);
        val = val_old;
        val |= (1<<3);
        pci_write_config32(dev, 0x80, val);
        val = pci_read_config32(dev, 0x80);
        e0_later = !!(val & (1<<3));
        if(e0_later) { // pre_e0 bit 3 always be 0 and can not be changed
                pci_write_config32(dev, 0x80, val_old); // restore it
        }

        return e0_later;
}
#endif

#if K8_REV_F_SUPPORT == 1
int is_cpu_f0_in_bsp(int nodeid)
{
        uint32_t dword;
        device_t dev;
        dev = dev_find_slot(0, PCI_DEVFN(0x18+nodeid, 3));
        dword = pci_read_config32(dev, 0xfc);
        return (dword & 0xfff00) == 0x40f00;
}
#endif

#define MCI_STATUS 0x401

static inline msr_t rdmsr_amd(unsigned index)
{
        msr_t result;
        __asm__ __volatile__ (
                "rdmsr"
                : "=a" (result.lo), "=d" (result.hi)
                : "c" (index), "D" (0x9c5a203a)
                );
        return result;
}

static inline void wrmsr_amd(unsigned index, msr_t msr)
{
        __asm__ __volatile__ (
                "wrmsr"
                : /* No outputs */
                : "c" (index), "a" (msr.lo), "d" (msr.hi), "D" (0x9c5a203a)
                );
}


#define MTRR_COUNT 8
#define ZERO_CHUNK_KB 0x800UL /* 2M */
#define TOLM_KB 0x400000UL

struct mtrr {
	msr_t base;
	msr_t mask;
};
struct mtrr_state {
	struct mtrr mtrrs[MTRR_COUNT];
	msr_t top_mem, top_mem2;
	msr_t def_type;
};

static void save_mtrr_state(struct mtrr_state *state)
{
	int i;
	for(i = 0; i < MTRR_COUNT; i++) {
		state->mtrrs[i].base = rdmsr(MTRRphysBase_MSR(i));
		state->mtrrs[i].mask = rdmsr(MTRRphysMask_MSR(i));
	}
	state->top_mem  = rdmsr(TOP_MEM);
	state->top_mem2 = rdmsr(TOP_MEM2);
	state->def_type = rdmsr(MTRRdefType_MSR);
}

static void restore_mtrr_state(struct mtrr_state *state)
{
	int i;
	disable_cache();

	for(i = 0; i < MTRR_COUNT; i++) {
		wrmsr(MTRRphysBase_MSR(i), state->mtrrs[i].base);
		wrmsr(MTRRphysMask_MSR(i), state->mtrrs[i].mask);
	}
	wrmsr(TOP_MEM,         state->top_mem);
	wrmsr(TOP_MEM2,        state->top_mem2);
	wrmsr(MTRRdefType_MSR, state->def_type);

	enable_cache();
}


#if 0
static void print_mtrr_state(struct mtrr_state *state)
{
	int i;
	for(i = 0; i < MTRR_COUNT; i++) {
		printk_debug("var mtrr %d: %08x%08x mask: %08x%08x\n",
			i,
			state->mtrrs[i].base.hi, state->mtrrs[i].base.lo,
			state->mtrrs[i].mask.hi, state->mtrrs[i].mask.lo);
	}
	printk_debug("top_mem:  %08x%08x\n",
		state->top_mem.hi, state->top_mem.lo);
	printk_debug("top_mem2: %08x%08x\n",
		state->top_mem2.hi, state->top_mem2.lo);
	printk_debug("def_type: %08x%08x\n",
		state->def_type.hi, state->def_type.lo);
}
#endif

static void set_init_ecc_mtrrs(void)
{
	msr_t msr;
	int i;
	disable_cache();

	/* First clear all of the msrs to be safe */
	for(i = 0; i < MTRR_COUNT; i++) {
		msr_t zero;
		zero.lo = zero.hi = 0;
		wrmsr(MTRRphysBase_MSR(i), zero);
		wrmsr(MTRRphysMask_MSR(i), zero);
	}

	/* Write back cache the first 1MB */
	msr.hi = 0x00000000;
	msr.lo = 0x00000000 | MTRR_TYPE_WRBACK;
	wrmsr(MTRRphysBase_MSR(0), msr);
	msr.hi = 0x000000ff;
	msr.lo = ~((CONFIG_LB_MEM_TOPK << 10) - 1) | 0x800;
	wrmsr(MTRRphysMask_MSR(0), msr);

	/* Set the default type to write combining */
	msr.hi = 0x00000000;
	msr.lo = 0xc00 | MTRR_TYPE_WRCOMB;
	wrmsr(MTRRdefType_MSR, msr);

	/* Set TOP_MEM to 4G */
	msr.hi = 0x00000001;
	msr.lo = 0x00000000;
	wrmsr(TOP_MEM, msr);

	enable_cache();
}

static inline void clear_2M_ram(unsigned long basek, struct mtrr_state *mtrr_state) 
{
                unsigned long limitk;
                unsigned long size;
                void *addr;

                /* Report every 64M */
                if ((basek % (64*1024)) == 0) {

                        /* Restore the normal state */
                        map_2M_page(0);
                        restore_mtrr_state(mtrr_state);
                        enable_lapic();

                        /* Print a status message */
                        printk_debug("%c", (basek >= TOLM_KB)?'+':'-');

                        /* Return to the initialization state */
                        set_init_ecc_mtrrs();
                        disable_lapic();

                }

                limitk = (basek + ZERO_CHUNK_KB) & ~(ZERO_CHUNK_KB - 1);
#if 0
		/* couldn't happen, memory must on 2M boundary */
		if(limitk>endk) {
			limitk = enk; 
		}
#endif
                size = (limitk - basek) << 10;
                addr = map_2M_page(basek >> 11);
                if (addr == MAPPING_ERROR) {
                        printk_err("Cannot map page: %x\n", basek >> 11);
                        return;
                }

                /* clear memory 2M (limitk - basek) */
                addr = (void *)(((uint32_t)addr) | ((basek & 0x7ff) << 10));
                clear_memory(addr, size);
}

static void init_ecc_memory(unsigned node_id)
{
	unsigned long startk, begink, endk;
	unsigned long hole_startk = 0;
	unsigned long basek;
	struct mtrr_state mtrr_state;

	device_t f1_dev, f2_dev, f3_dev;
	int enable_scrubbing;
	uint32_t dcl;

	f1_dev = dev_find_slot(0, PCI_DEVFN(0x18 + node_id, 1));
	if (!f1_dev) {
		die("Cannot find cpu function 1\n");
	}
	f2_dev = dev_find_slot(0, PCI_DEVFN(0x18 + node_id, 2));
	if (!f2_dev) {
		die("Cannot find cpu function 2\n");
	}
	f3_dev = dev_find_slot(0, PCI_DEVFN(0x18 + node_id, 3));
	if (!f3_dev) {
		die("Cannot find cpu function 3\n");
	}

	/* See if we scrubbing should be enabled */
	enable_scrubbing = 1;
	get_option(&enable_scrubbing, "hw_scrubber");

	/* Enable cache scrubbing at the lowest possible rate */
	if (enable_scrubbing) {
		pci_write_config32(f3_dev, SCRUB_CONTROL,
			(SCRUB_84ms << 16) | (SCRUB_84ms << 8) | (SCRUB_NONE << 0));
	} else {
		pci_write_config32(f3_dev, SCRUB_CONTROL,
			(SCRUB_NONE << 16) | (SCRUB_NONE << 8) | (SCRUB_NONE << 0));
		printk_debug("Scrubbing Disabled\n");
	}
	

	/* If ecc support is not enabled don't touch memory */
	dcl = pci_read_config32(f2_dev, DRAM_CONFIG_LOW);
	if (!(dcl & DCL_DimmEccEn)) {
		printk_debug("ECC Disabled\n");
		return;
	}

	startk = (pci_read_config32(f1_dev, 0x40 + (node_id*8)) & 0xffff0000) >> 2;
	endk   = ((pci_read_config32(f1_dev, 0x44 + (node_id*8)) & 0xffff0000) >> 2) + 0x4000;

#if HW_MEM_HOLE_SIZEK != 0
	#if K8_REV_F_SUPPORT == 0
        if (!is_cpu_pre_e0()) 
	{
	#endif

                uint32_t val;
                val = pci_read_config32(f1_dev, 0xf0);
                if(val & 1) {
        	        hole_startk = ((val & (0xff<<24)) >> 10);
                }
	#if K8_REV_F_SUPPORT == 0
        }
	#endif
#endif
	

	/* Don't start too early */
	begink = startk;
	if (begink < CONFIG_LB_MEM_TOPK) { 
		begink = CONFIG_LB_MEM_TOPK;
	}

	printk_debug("Clearing memory %uK - %uK: ", begink, endk);

	/* Save the normal state */
	save_mtrr_state(&mtrr_state);

	/* Switch to the init ecc state */
	set_init_ecc_mtrrs();
	disable_lapic();

	/* Walk through 2M chunks and zero them */
#if HW_MEM_HOLE_SIZEK != 0
	/* here hole_startk can not be equal to begink, never. Also hole_startk is in 2M boundary, 64M? */
        if ( (hole_startk != 0) && ((begink < hole_startk) && (endk>(4*1024*1024)))) {
		        for(basek = begink; basek < hole_startk;
        		        basek = ((basek + ZERO_CHUNK_KB) & ~(ZERO_CHUNK_KB - 1)))
		        {
				clear_2M_ram(basek, &mtrr_state);
                	}
			for(basek = 4*1024*1024; basek < endk;
                                basek = ((basek + ZERO_CHUNK_KB) & ~(ZERO_CHUNK_KB - 1)))
                        {
                                clear_2M_ram(basek, &mtrr_state);
                        }
        }
	else 
#endif
        for(basek = begink; basek < endk;
                basek = ((basek + ZERO_CHUNK_KB) & ~(ZERO_CHUNK_KB - 1))) 
	{
		clear_2M_ram(basek, &mtrr_state);
	}


	/* Restore the normal state */
	map_2M_page(0);
	restore_mtrr_state(&mtrr_state);
	enable_lapic();

	/* Set the scrub base address registers */
	pci_write_config32(f3_dev, SCRUB_ADDR_LOW,  startk << 10);
	pci_write_config32(f3_dev, SCRUB_ADDR_HIGH, startk >> 22);

	/* Enable the scrubber? */
	if (enable_scrubbing) {
		/* Enable scrubbing at the lowest possible rate */
		pci_write_config32(f3_dev, SCRUB_CONTROL,
			(SCRUB_84ms << 16) | (SCRUB_84ms << 8) | (SCRUB_84ms << 0));
	}

	printk_debug(" done\n");
}


static inline void k8_errata(void)
{
	msr_t msr;
#if K8_REV_F_SUPPORT == 0
	if (is_cpu_pre_c0()) {
		/* Erratum 63... */
		msr = rdmsr(HWCR_MSR);
		msr.lo |= (1 << 6);
		wrmsr(HWCR_MSR, msr);

		/* Erratum 69... */
		msr = rdmsr_amd(BU_CFG_MSR);
		msr.hi |= (1 << (45 - 32));
		wrmsr_amd(BU_CFG_MSR, msr);

		/* Erratum 81... */
		msr = rdmsr_amd(DC_CFG_MSR);
		msr.lo |=  (1 << 10);
		wrmsr_amd(DC_CFG_MSR, msr);
			
	}
	/* I can't touch this msr on early buggy cpus */
	if (!is_cpu_pre_b3()) {

		/* Erratum 89 ... */
		msr = rdmsr(NB_CFG_MSR);
		msr.lo |= 1 << 3;
		
		if (!is_cpu_pre_c0() && is_cpu_pre_d0()) {
			/* D0 later don't need it */
			/* Erratum 86 Disable data masking on C0 and 
			 * later processor revs.
			 * FIXME this is only needed if ECC is enabled.
			 */
			msr.hi |= 1 << (36 - 32);
		}
		wrmsr(NB_CFG_MSR, msr);
	}
	
	/* Erratum 97 ... */
	if (!is_cpu_pre_c0() && is_cpu_pre_d0()) {
		msr = rdmsr_amd(DC_CFG_MSR);
		msr.lo |= 1 << 3;
		wrmsr_amd(DC_CFG_MSR, msr);
	}	
	
	/* Erratum 94 ... */
	if (is_cpu_pre_d0()) {
		msr = rdmsr_amd(IC_CFG_MSR);
		msr.lo |= 1 << 11;
		wrmsr_amd(IC_CFG_MSR, msr);
	}

	/* Erratum 91 prefetch miss is handled in the kernel */

	/* Erratum 106 ... */
	msr = rdmsr_amd(LS_CFG_MSR);
	msr.lo |= 1 << 25;
	wrmsr_amd(LS_CFG_MSR, msr);

	/* Erratum 107 ... */
	msr = rdmsr_amd(BU_CFG_MSR);
	msr.hi |= 1 << (43 - 32);
	wrmsr_amd(BU_CFG_MSR, msr);

	if(is_cpu_d0()) {
		/* Erratum 110 ...*/
		msr = rdmsr_amd(CPU_ID_HYPER_EXT_FEATURES);
		msr.hi |=1;
		wrmsr_amd(CPU_ID_HYPER_EXT_FEATURES, msr);
 	}
#endif

#if K8_REV_F_SUPPORT == 0
	if (!is_cpu_pre_e0()) 
#endif
	{
		/* Erratum 110 ... */
                msr = rdmsr_amd(CPU_ID_EXT_FEATURES_MSR);
                msr.hi |=1;
                wrmsr_amd(CPU_ID_EXT_FEATURES_MSR, msr);
	}

	/* Erratum 122 */
	msr = rdmsr(HWCR_MSR);
	msr.lo |= 1 << 6;
	wrmsr(HWCR_MSR, msr);

#if K8_REV_F_SUPPORT == 1
        /* Erratum 131... */
        msr = rdmsr(NB_CFG_MSR);
        msr.lo |= 1 << 20;
        wrmsr(NB_CFG_MSR, msr);
#endif

}

#if K8_REV_F_SUPPORT == 1
static void amd_set_name_string_f(device_t dev)
{
	unsigned socket;
	unsigned cmpCap;
	unsigned pwrLmt;
	unsigned brandId;
	unsigned brandTableIndex;
	unsigned nN;
	unsigned unknown = 1;

	uint8_t str[48];
	uint32_t *p;

	msr_t msr;
	unsigned i;
	
	brandId = cpuid_ebx(0x80000001) & 0xffff;

	printk_debug("brandId=%04x\n", brandId);	
	pwrLmt = ((brandId>>14) & 1) | ((brandId>>5) & 0x0e);
	brandTableIndex = (brandId>>9) & 0x1f;
	nN = (brandId & 0x3f) | ((brandId>>(15-6)) &(1<<6));

	socket = (dev->device >> 4) & 0x3;

	cmpCap = cpuid_ecx(0x80000008) & 0xff;


        if((brandTableIndex == 0) && (pwrLmt == 0)) {
        	memset(str, 0, 48);
                sprintf(str, "AMD Engineering Sample");
                unknown = 0;
        } else {

		memset(str, 0, 48);
		sprintf(str, "AMD Processor model unknown");

	#if CPU_SOCKET_TYPE == 0x10 
		if(socket == 0x01) { // socket F
			if ((cmpCap == 1) && ((brandTableIndex==0) ||(brandTableIndex ==1) ||(brandTableIndex == 4)) ) {
				uint8_t pc[2];
				unknown = 0;
				switch (pwrLmt) {
					case   2: pc[0]= 'E'; pc[1] = 'E'; break;
					case   6: pc[0]= 'H'; pc[1] = 'E'; break;
					case 0xa: pc[0]= ' '; pc[1] = ' '; break;
					case 0xc: pc[0]= 'S'; pc[1] = 'E'; break;
					default: unknown = 1;
					
				}
				if(!unknown) {
					memset(str, 0, 48);
					sprintf(str, "Dual-Core AMD Opteron(tm) Processor %1d2%2d %c%c", brandTableIndex<<1, (nN-1)&0x3f, pc[0], pc[1]);
				}
			}
		}
	#else
		#if CPU_SOCKET_TYPE == 0x11
		if(socket == 0x00) { // socket AM2
			if(cmpCap == 0) {
				sprintf(str, "Athlon 64");	
			} else {
				sprintf(str, "Athlon 64 Dual Core");
			}

		}
		#endif
	#endif
	}

	p = str; 
	for(i=0;i<6;i++) {
		msr.lo = *p;  p++; msr.hi = *p; p++;
		wrmsr(0xc0010030+i, msr);
	}
	

}
#endif

extern void model_fxx_update_microcode(unsigned cpu_deviceid);
int init_processor_name(void);

static unsigned ehci_debug_addr;

void model_fxx_init(device_t dev)
{
	unsigned long i;
	msr_t msr;
	struct node_core_id id;
#if CONFIG_LOGICAL_CPUS == 1
	unsigned siblings;
#endif

#if K8_REV_F_SUPPORT == 1
	struct cpuinfo_x86 c;
	
	get_fms(&c, dev->device);

	if((c.x86_model & 0xf0) == 0x40) {
		amd_set_name_string_f(dev);
	} 
#endif

#if CONFIG_USBDEBUG_DIRECT
	if(!ehci_debug_addr) 
		ehci_debug_addr = get_ehci_debug();
	set_ehci_debug(0);
#endif

	/* Turn on caching if we haven't already */
	x86_enable_cache();
	amd_setup_mtrrs();
	x86_mtrr_check();

#if CONFIG_USBDEBUG_DIRECT
	set_ehci_debug(ehci_debug_addr);
#endif

        /* Update the microcode */
	model_fxx_update_microcode(dev->device);

	disable_cache();
	
	/* zero the machine check error status registers */
	msr.lo = 0;
	msr.hi = 0;
	for(i=0; i<5; i++) {
		wrmsr(MCI_STATUS + (i*4),msr);
	}

	k8_errata();
	
	/* Set SMMLOCK to avoid exploits messing with SMM */
	msr = rdmsr(HWCR_MSR);
	msr.lo |= (1 << 0);
	wrmsr(HWCR_MSR, msr);
	
	/* Set the processor name string */
	init_processor_name();
	
	enable_cache();

	/* Enable the local cpu apics */
	setup_lapic();

#if CONFIG_LOGICAL_CPUS == 1
        siblings = cpuid_ecx(0x80000008) & 0xff;

       	if(siblings>0) {
                msr = rdmsr_amd(CPU_ID_FEATURES_MSR);
                msr.lo |= 1 << 28; 
                wrmsr_amd(CPU_ID_FEATURES_MSR, msr);

       	        msr = rdmsr_amd(LOGICAL_CPUS_NUM_MSR);
                msr.lo = (siblings+1)<<16; 
       	        wrmsr_amd(LOGICAL_CPUS_NUM_MSR, msr);

                msr = rdmsr_amd(CPU_ID_EXT_FEATURES_MSR);
       	        msr.hi |= 1<<(33-32); 
               	wrmsr_amd(CPU_ID_EXT_FEATURES_MSR, msr);
	} 

#endif

	id = get_node_core_id(read_nb_cfg_54()); // pre e0 nb_cfg_54 can not be set

	/* Is this a bad location?  In particular can another node prefecth
	 * data from this node before we have initialized it?
	 */
	if (id.coreid == 0) init_ecc_memory(id.nodeid); // only do it for core 0

#if CONFIG_LOGICAL_CPUS==1
        /* Start up my cpu siblings */
//	if(id.coreid==0)  amd_sibling_init(dev); // Don't need core1 is already be put in the CPU BUS in bus_cpu_scan
#endif

}

static struct device_operations cpu_dev_ops = {
	.init = model_fxx_init,
};
static struct cpu_device_id cpu_table[] = {
#if K8_REV_F_SUPPORT == 0
	{ X86_VENDOR_AMD, 0xf50 }, /* B3 */
	{ X86_VENDOR_AMD, 0xf51 }, /* SH7-B3 */
	{ X86_VENDOR_AMD, 0xf58 }, /* SH7-C0 */
	{ X86_VENDOR_AMD, 0xf48 },

	{ X86_VENDOR_AMD, 0xf5A }, /* SH7-CG */
	{ X86_VENDOR_AMD, 0xf4A },
	{ X86_VENDOR_AMD, 0xf7A },
	{ X86_VENDOR_AMD, 0xfc0 }, /* DH7-CG */
	{ X86_VENDOR_AMD, 0xfe0 },
	{ X86_VENDOR_AMD, 0xff0 },
	{ X86_VENDOR_AMD, 0xf82 }, /* CH7-CG */
	{ X86_VENDOR_AMD, 0xfb2 },
//AMD_D0_SUPPORT
	{ X86_VENDOR_AMD, 0x10f50 }, /* SH7-D0 */
	{ X86_VENDOR_AMD, 0x10f40 },
	{ X86_VENDOR_AMD, 0x10f70 },
        { X86_VENDOR_AMD, 0x10fc0 }, /* DH7-D0 */
        { X86_VENDOR_AMD, 0x10ff0 },
        { X86_VENDOR_AMD, 0x10f80 }, /* CH7-D0 */
        { X86_VENDOR_AMD, 0x10fb0 },
//AMD_E0_SUPPORT
        { X86_VENDOR_AMD, 0x20f50 }, /* SH8-E0*/
        { X86_VENDOR_AMD, 0x20f40 },
        { X86_VENDOR_AMD, 0x20f70 },
        { X86_VENDOR_AMD, 0x20fc0 }, /* DH8-E0 */ /* DH-E3 */
        { X86_VENDOR_AMD, 0x20ff0 },
        { X86_VENDOR_AMD, 0x20f10 }, /* JH8-E1 */
        { X86_VENDOR_AMD, 0x20f30 },
        { X86_VENDOR_AMD, 0x20f51 }, /* SH-E4 */
        { X86_VENDOR_AMD, 0x20f71 },
        { X86_VENDOR_AMD, 0x20f42 }, /* SH-E5 */
        { X86_VENDOR_AMD, 0x20ff2 }, /* DH-E6 */
        { X86_VENDOR_AMD, 0x20fc2 },
        { X86_VENDOR_AMD, 0x20f12 }, /* JH-E6 */
        { X86_VENDOR_AMD, 0x20f32 },
	{ X86_VENDOR_AMD, 0x30ff2 }, /* E4 ? */
#endif

#if K8_REV_F_SUPPORT == 1
//AMD_F0_SUPPORT
	{ X86_VENDOR_AMD, 0x40f50 }, /* SH-F0	   Socket F (1207): Opteron */ 
	{ X86_VENDOR_AMD, 0x40f70 },		/*        AM2: Athlon64/Athlon64 FX  */
	{ X86_VENDOR_AMD, 0x40f40 },		/*        S1g1: Mobile Athlon64 */
	{ X86_VENDOR_AMD, 0x40f11 }, /* JH-F1 	   Socket F (1207): Opteron Dual Core */
	{ X86_VENDOR_AMD, 0x40f31 },		/*        AM2: Athlon64 x2/Athlon64 FX Dual Core */ 
	{ X86_VENDOR_AMD, 0x40f01 },		/*        S1g1: Mobile Athlon64 */
        { X86_VENDOR_AMD, 0x40f12 }, /* JH-F2      Socket F (1207): Opteron Dual Core */
        { X86_VENDOR_AMD, 0x40f32 },            /*        AM2 : Opteron Dual Core/Athlon64 x2/ Athlon64 FX Dual Core */
        { X86_VENDOR_AMD, 0x40fb2 }, /* BH-F2      Socket AM2:Athlon64 x2/ Mobile Athlon64 x2 */
	{ X86_VENDOR_AMD, 0x40f82 }, 		/* 	  S1g1:Turion64 x2 */
        { X86_VENDOR_AMD, 0x40ff2 }, /* DH-F2      Socket AM2: Athlon64 */
        { X86_VENDOR_AMD, 0x50ff2 }, /* DH-F2      Socket AM2: Athlon64 */
        { X86_VENDOR_AMD, 0x40fc2 },            /*        S1g1:Turion64 */
        { X86_VENDOR_AMD, 0x40f13 }, /* JH-F3      Socket F (1207): Opteron Dual Core */
        { X86_VENDOR_AMD, 0x40f33 },            /*        AM2 : Opteron Dual Core/Athlon64 x2/ Athlon64 FX Dual Core */
        { X86_VENDOR_AMD, 0xc0f13 },            /*        AM2 : Athlon64 FX*/
        { X86_VENDOR_AMD, 0x50ff3 }, /* DH-F3      Socket AM2: Athlon64 */
#endif

	{ 0, 0 },
};
static struct cpu_driver model_fxx __cpu_driver = {
	.ops      = &cpu_dev_ops,
	.id_table = cpu_table,
};
