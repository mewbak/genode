/*
 * \brief   Platform interface implementation
 * \author  Norman Feske
 * \date    2015-05-01
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <base/sleep.h>
#include <base/thread.h>
#include <base/log.h>

/* core includes */
#include <core_parent.h>
#include <platform.h>
#include <map_local.h>
#include <cnode.h>
#include <untyped_memory.h>

/* base-internal includes */
#include <base/internal/globals.h>
#include <base/internal/stack_area.h>

using namespace Genode;

static bool const verbose_boot_info = true;


/*
 * Memory-layout information provided by the linker script
 */

/* virtual address range consumed by core's program image */
extern unsigned _prog_img_beg, _prog_img_end;


/******************
 ** Boot modules **
 ******************/

struct Boot_module_header
{
	char   const *name;  /* physical address of null-terminated string */
	addr_t const  base;  /* physical address of module data */
	size_t const  size;  /* size of module data in bytes */
};

extern Boot_module_header _boot_modules_headers_begin;
extern Boot_module_header _boot_modules_headers_end;
extern int                _boot_modules_binaries_begin;
extern int                _boot_modules_binaries_end;


/****************************************
 ** Support for core memory management **
 ****************************************/

bool Mapped_mem_allocator::_map_local(addr_t virt_addr, addr_t phys_addr,
                                      unsigned size)
{
	size_t const num_pages = size / get_page_size();

	Untyped_memory::convert_to_page_frames(phys_addr, num_pages);

	return map_local(phys_addr, virt_addr, num_pages);
}


bool Mapped_mem_allocator::_unmap_local(addr_t virt_addr, unsigned size)
{
	return unmap_local(virt_addr, size / get_page_size());
}


/************************
 ** Platform interface **
 ************************/

void Platform::_init_unused_phys_alloc()
{
	/* enable log support early */
	init_log();

	_unused_phys_alloc.add_range(0, ~0UL);
}


static inline void init_sel4_ipc_buffer()
{
	asm volatile ("movl %0, %%gs" :: "r"(IPCBUF_GDT_SELECTOR) : "memory");
}


void Platform::_init_allocators()
{
	/* interrupt allocator */
	_irq_alloc.add_range(0, 255);

	/*
	 * XXX allocate intermediate CNodes for organizing the untyped pages here
	 */

	/* register remaining untyped memory to physical memory allocator */
	auto add_phys_range = [&] (Initial_untyped_pool::Range const &range) {

		addr_t const page_aligned_offset =
			align_addr(range.free_offset, get_page_size_log2());

		if (page_aligned_offset >= range.size)
			return;

		addr_t const base = range.phys + page_aligned_offset;
		size_t const size = range.size - page_aligned_offset;

		_core_mem_alloc.phys_alloc()->add_range(base, size);
		_unused_phys_alloc.remove_range(base, size);
	};
	_initial_untyped_pool.for_each_range(add_phys_range);

	/* turn remaining untyped memory ranges into untyped pages */
	_initial_untyped_pool.turn_remainder_into_untyped_pages();

	/*
	 * From this point on, we can no longer create kernel objects from the
	 * '_initial_untyped_pool' because the pool is empty.
	 */

	/* I/O memory ranges */
//	init_allocator(_io_mem_alloc, _unused_phys_alloc,
//	               bi, bi.deviceUntyped.start,
//	               bi.deviceUntyped.end - bi.deviceUntyped.start);

	/* core's virtual memory */
	_core_mem_alloc.virt_alloc()->add_range(_vm_base, _vm_size);

	/* remove core image from core's virtual address allocator */

	/*
	 * XXX Why do we need to skip a few KiB after the end of core?
	 *     When allocating a PTE immediately after _prog_img_end, the
	 *     kernel would complain "Mapping already present" on the
	 *     attempt to map a page frame.
	 */
	addr_t const core_virt_beg = trunc_page((addr_t)&_prog_img_beg),
	             core_virt_end = round_page((addr_t)&_boot_modules_binaries_end)
	                           + 64*1024;
	size_t const core_size     = core_virt_end - core_virt_beg;

	_core_mem_alloc.virt_alloc()->remove_range(core_virt_beg, core_size);

	if (verbose_boot_info) {
		log("core image:");
		log("  virtual address range [",
		    Hex(core_virt_beg, Hex::OMIT_PREFIX, Hex::PAD), ",",
		    Hex(core_virt_end, Hex::OMIT_PREFIX, Hex::PAD), ") size=",
		    Hex(core_size));
	}

	/* preserve stack area in core's virtual address space */
	_core_mem_alloc.virt_alloc()->remove_range(stack_area_virtual_base(),
	                                           stack_area_virtual_size());
}


void Platform::_switch_to_core_cspace()
{
	Cnode_base const initial_cspace(Cap_sel(seL4_CapInitThreadCNode), 32);

	/* copy initial selectors to core's CNode */
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapInitThreadTCB));
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapInitThreadVSpace));
	_core_cnode.move(initial_cspace, Cnode_index(seL4_CapIRQControl)); /* cannot be copied */
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapASIDControl));
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapInitThreadASIDPool));
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapIOPort));
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapBootInfoFrame));
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapInitThreadIPCBuffer));
	_core_cnode.copy(initial_cspace, Cnode_index(seL4_CapDomain));

	/* replace seL4_CapInitThreadCNode with new top-level CNode */
	_core_cnode.copy(initial_cspace, Cnode_index(Core_cspace::TOP_CNODE_SEL),
	                                 Cnode_index(seL4_CapInitThreadCNode));

	/* copy untyped memory selectors to core's CNode */
	seL4_BootInfo const &bi = sel4_boot_info();

	/*
	 * We have to move (not copy) the selectors for the initial untyped ranges
	 * because some of them are already populated with kernel objects allocated
	 * via '_initial_untyped_pool'. For such an untyped memory range, the
	 * attempt to copy its selector would result in the following error:
	 *
	 *   <<seL4: Error deriving cap for CNode Copy operation.>>
	 */
	for (unsigned sel = bi.untyped.start; sel < bi.untyped.end; sel++)
		_core_cnode.move(initial_cspace, Cnode_index(sel));

//	for (unsigned sel = bi.deviceUntyped.start; sel < bi.deviceUntyped.end; sel++)
//		_core_cnode.copy(initial_cspace, sel);

	for (unsigned sel = bi.userImageFrames.start; sel < bi.userImageFrames.end; sel++)
		_core_cnode.copy(initial_cspace, Cnode_index(sel));

	/* copy statically created CNode selectors to core's CNode */
	_core_cnode.copy(initial_cspace, Cnode_index(Core_cspace::TOP_CNODE_SEL));
	_core_cnode.copy(initial_cspace, Cnode_index(Core_cspace::CORE_PAD_CNODE_SEL));
	_core_cnode.copy(initial_cspace, Cnode_index(Core_cspace::CORE_CNODE_SEL));
	_core_cnode.copy(initial_cspace, Cnode_index(Core_cspace::PHYS_CNODE_SEL));

	/*
	 * Construct CNode hierarchy of core's CSpace
	 */

	/* insert 3rd-level core CNode into 2nd-level core-pad CNode */
	_core_pad_cnode.copy(initial_cspace, Cnode_index(Core_cspace::CORE_CNODE_SEL),
	                                     Cnode_index(0));

	/* insert 2nd-level core-pad CNode into 1st-level CNode */
	_top_cnode.copy(initial_cspace, Cnode_index(Core_cspace::CORE_PAD_CNODE_SEL),
	                                Cnode_index(Core_cspace::TOP_CNODE_CORE_IDX));

	/* insert 2nd-level phys-mem CNode into 1st-level CNode */
	_top_cnode.copy(initial_cspace, Cnode_index(Core_cspace::PHYS_CNODE_SEL),
	                                Cnode_index(Core_cspace::TOP_CNODE_PHYS_IDX));

	/* insert 2nd-level untyped-pages CNode into 1st-level CNode */
	_top_cnode.copy(initial_cspace, Cnode_index(Core_cspace::UNTYPED_CNODE_SEL),
	                                Cnode_index(Core_cspace::TOP_CNODE_UNTYPED_IDX));

	/* activate core's CSpace */
	{
		seL4_CapData_t null_data = { { 0 } };

		int const ret = seL4_TCB_SetSpace(seL4_CapInitThreadTCB,
		                                  seL4_CapNull, /* fault_ep */
		                                  Core_cspace::TOP_CNODE_SEL, null_data,
		                                  seL4_CapInitThreadPD, null_data);

		if (ret != seL4_NoError)
			error(__FUNCTION__, ": seL4_TCB_SetSpace returned ", ret);
	}
}


Cap_sel Platform::_init_asid_pool()
{
	return Cap_sel(seL4_CapInitThreadASIDPool);
}


void Platform::_init_core_page_table_registry()
{
	seL4_BootInfo const &bi = sel4_boot_info();

	/*
	 * Register initial page tables
	 */
	addr_t virt_addr = (addr_t)(&_prog_img_beg);
	for (unsigned sel = bi.userImagePaging.start; sel < bi.userImagePaging.end; sel++) {

		_core_page_table_registry.insert_page_table(virt_addr, Cap_sel(sel));

		/* one page table has 1024 entries */
		virt_addr += 1024*get_page_size();
	}

	/*
	 * Register initial page frames
	 */
	virt_addr = (addr_t)(&_prog_img_beg);
	for (unsigned sel = bi.userImageFrames.start; sel < bi.userImageFrames.end; sel++) {

		_core_page_table_registry.insert_page_table_entry(virt_addr, sel);

		virt_addr += get_page_size();
	}
}


void Platform::_init_rom_modules()
{
	seL4_BootInfo const &bi = sel4_boot_info();

	/*
	 * Slab allocator for allocating 'Rom_module' meta data.
	 */
	static long slab_block[4096];
	static Tslab<Rom_module, sizeof(slab_block)>
		rom_module_slab(core_mem_alloc(), (Genode::Slab_block *)slab_block);

	/*
	 * Allocate unused range of phys CNode address space where to make the
	 * boot modules available.
	 */
	void *out_ptr = nullptr;
	size_t const modules_size = (addr_t)&_boot_modules_binaries_end
	                          - (addr_t)&_boot_modules_binaries_begin + 1;

	Range_allocator::Alloc_return const alloc_ret =
		_unused_phys_alloc.alloc_aligned(modules_size, &out_ptr, get_page_size_log2());

	if (alloc_ret.error()) {
		error("could not reserve phys CNode space for boot modules");
		struct Init_rom_modules_failed { };
		throw Init_rom_modules_failed();
	}

	/*
	 * Calculate frame frame selector used to back the boot modules
	 */
	addr_t const unused_range_start      = (addr_t)out_ptr;
	addr_t const unused_first_frame_sel  = unused_range_start >> get_page_size_log2();
	addr_t const modules_start           = (addr_t)&_boot_modules_binaries_begin;
	addr_t const modules_core_offset     = modules_start
	                                     - (addr_t)&_prog_img_beg;
	addr_t const modules_first_frame_sel = bi.userImageFrames.start
	                                     + (modules_core_offset >> get_page_size_log2());

	Boot_module_header const *header = &_boot_modules_headers_begin;
	for (; header < &_boot_modules_headers_end; header++) {

		/* offset relative to first module */
		addr_t const module_offset        = header->base - modules_start;
		addr_t const module_offset_frames = module_offset >> get_page_size_log2();
		size_t const module_size          = round_page(header->size);
		addr_t const module_frame_sel     = modules_first_frame_sel
		                                  + module_offset_frames;
		size_t const module_num_frames    = module_size >> get_page_size_log2();

		/*
		 * Destination frame within phys CNode
		 */
		addr_t const dst_frame = unused_first_frame_sel + module_offset_frames;

		/*
		 * Install the module's frame selectors into phys CNode
		 */
		Cnode_base const initial_cspace(Cap_sel(seL4_CapInitThreadCNode), 32);
		for (unsigned i = 0; i < module_num_frames; i++)
			_phys_cnode.copy(initial_cspace, Cnode_index(module_frame_sel + i),
			                                 Cnode_index(dst_frame + i));

		PLOG("boot module '%s' (%zd bytes)", header->name, header->size);

		/*
		 * Register ROM module, the base address refers to location of the
		 * ROM module within the phys CNode address space.
		 */
		Rom_module * rom_module = new (rom_module_slab)
			Rom_module(dst_frame << get_page_size_log2(), header->size,
			           (const char*)header->name);

		_rom_fs.insert(rom_module);
	}
}


Platform::Platform()
:

	_io_mem_alloc(core_mem_alloc()), _io_port_alloc(core_mem_alloc()),
	_irq_alloc(core_mem_alloc()),
	_unused_phys_alloc(core_mem_alloc()),
	_init_unused_phys_alloc_done((_init_unused_phys_alloc(), true)),
	_vm_base(0x2000), /* 2nd page is used as IPC buffer of main thread */
	_vm_size(3*1024*1024*1024UL - _vm_base), /* use the lower 3GiB */
	_init_sel4_ipc_buffer_done((init_sel4_ipc_buffer(), true)),
	_switch_to_core_cspace_done((_switch_to_core_cspace(), true)),
	_core_page_table_registry(*core_mem_alloc()),
	_init_core_page_table_registry_done((_init_core_page_table_registry(), true)),
	_init_allocators_done((_init_allocators(), true)),
	_core_vm_space(Cap_sel(seL4_CapInitThreadPD),
	               _core_sel_alloc,
	               _phys_alloc,
	               _top_cnode,
	               _core_cnode,
	               _phys_cnode,
	               Core_cspace::CORE_VM_ID,
	               _core_page_table_registry)
{
	/* I/O port allocator (only meaningful for x86) */
	_io_port_alloc.add_range(0, 0x10000);

	/*
	 * Log statistics about allocator initialization
	 */
	log("VM area at [", Hex(_vm_base, Hex::OMIT_PREFIX, Hex::PAD), ",",
	    Hex(_vm_base + _vm_size, Hex::OMIT_PREFIX, Hex::PAD), ")");

	if (verbose_boot_info) {
		log(":phys_alloc:       "); (*_core_mem_alloc.phys_alloc())()->dump_addr_tree();
		log(":unused_phys_alloc:"); _unused_phys_alloc()->dump_addr_tree();
		log(":virt_alloc:       "); (*_core_mem_alloc.virt_alloc())()->dump_addr_tree();
		log(":io_mem_alloc:     "); _io_mem_alloc()->dump_addr_tree();
	}

	_init_rom_modules();
}


unsigned Platform::alloc_core_rcv_sel()
{
	Cap_sel rcv_sel = _core_sel_alloc.alloc();

	seL4_SetCapReceivePath(_core_cnode.sel().value(), rcv_sel.value(),
	                       _core_cnode.size_log2());

	return rcv_sel.value();
}


void Platform::reset_sel(unsigned sel)
{
	_core_cnode.remove(Cap_sel(sel));
}


void Platform::wait_for_exit()
{
	sleep_forever();
}


void Core_parent::exit(int exit_value) { }
