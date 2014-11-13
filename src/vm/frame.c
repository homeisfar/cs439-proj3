#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Student code */
#include "vm/page.h"
#include <bitmap.h>
#include "vm/frame.h"
#include "vm/swap.h"

/* Amount of physical memory in 4kb pages */
#define FRAME_MAX init_ram_pages

#define index(x) (((x) - (int) ptov (1024 * 1024)) / PGSIZE - ((int) \
ptov (init_ram_pages * PGSIZE) - (int) ptov (1024 * 1024)) / PGSIZE / 2 - 1)

size_t user_pages;

static struct lock frame_lock;
/* Create a frame table that has 2^20 frame entries,
   or the size of physical memory */ 
frame_entry *frame_table;
bool frame_get_page (uint32_t *, void *, bool , page_entry *);
void *frame_get_multiple (enum palloc_flags, size_t);
void frame_clear_page (int, uint32_t *);
uintptr_t *frame_evict_page ();

/* Initialize the elements of the frame table allocating and clearing a
   set of memory */
void
init_frame_table ()
{
	uint8_t *free_start = ptov (1024 * 1024);
 	uint8_t *free_end = ptov (init_ram_pages * PGSIZE);
 	size_t free_pages = (free_end - free_start) / PGSIZE;
 	user_pages = free_pages / 2 - 1;
 	//PANIC("User pages: %lu\n", user_pages);
	frame_table = calloc (user_pages, sizeof (frame_entry));
	lock_init (&frame_lock);
}

/* Obtain a single frame */
bool
frame_get_page (uint32_t *pd, void *upage, bool writable, page_entry *fault_entry)
{
	void *page;
	page = palloc_get_page (PAL_USER);
	if (!page)
	{
		page = frame_evict_page();//eviction here
		// check if page is NULL
		if (!page)
			return 0;
	}

	uint32_t index = index((int) page);
	//printf("ACQUIRED REG PAGE: %d\n", index);
	// make frame entry point to supplemental page dir entry
	frame_table[index].page = page;
	frame_table[index].page_dir_entry = fault_entry;
	fault_entry->phys_page = page;
	set_in_frame(frame_table[index].page_dir_entry->meta);
	return pagedir_set_page (pd, upage, page, writable);
}




/* Obtain a frame for a stack page */
// should work for valid, non-eviction cases in current state,
// assuming that page_insert_entry_exec and frame_get_page are correctly implemented

void *
frame_get_stack_page (void * vaddr)
{
	void *kpage;
	uint8_t *upage = pg_round_down (vaddr); // used in supp.p.t. also
	kpage = palloc_get_page (PAL_USER | PAL_ZERO);


	if (!kpage)
	{
		lock_acquire (&frame_lock);
		kpage = frame_evict_page(); // eviction here
		lock_release (&frame_lock);
		if (!kpage)
			return NULL;
	}

	uint32_t index = index((int) kpage);
	//printf("ACQUIRED STACK PAGE: %lu\n", index);


	// create supplemental page table entry
	ASSERT ( page_insert_entry_stack (upage) == NULL);

	page_entry *fault_entry = page_get_entry (&thread_current ()->page_table_hash, upage);
	

	if (!fault_entry)
	{
		palloc_free_page (kpage);
		return NULL;
	}

	// make frame entry point to supplemental page dir entry
	frame_table[index].page = kpage;
	frame_table[index].page_dir_entry = fault_entry;
	fault_entry->phys_page = kpage;
	set_in_frame (frame_table[index].page_dir_entry->meta);
	
	return kpage;
}

/* Free page_cnt number of frames from memory */
void
frame_clear_page (int frame_index, uint32_t *pd)
{
	clear_in_frame (frame_table[frame_index].page_dir_entry->meta);
	//frame_table[frame_index].page = NULL;

	//palloc_free_page (frame_table[frame_index].page);
	pagedir_clear_page(pd, frame_table[frame_index].page_dir_entry->upage);
	frame_table[frame_index].page_dir_entry = NULL;
}

/* Free a single frame from memory */
uintptr_t *
frame_evict_page () 
{
	static int clock_hand = 0;
	// printf("Evicted\n");
	// return NULL;
	// eviction algorithm goes here
	// for now, panic/fail
	// only goes into swap if dirty
	
	uint32_t *pd = thread_current ()->pagedir;
	void *evict_frame = NULL;
	while (evict_frame == NULL)
	{
		// if(!frame_table[clock_hand].page_dir_entry) 
		// {
		// 	PANIC("%d\n", clock_hand);
		// 	clock_hand = (clock_hand + 1) % user_pages;/
		// 	continue;
		// }

		//reference bit 
		if (pagedir_is_accessed(pd, frame_table[clock_hand].page))
		{
			//set reference to 0
			pagedir_set_accessed(pd, frame_table[clock_hand].page, 0);
			clock_hand = (clock_hand + 1) % user_pages;
		}
		else 
		{
			// if page is dirty, move to swap
			if (pagedir_is_dirty(pd, frame_table[clock_hand].page))
			{
				size_t index = swap_acquire ();
				if (index == BITMAP_ERROR)
					PANIC ("OUT OF SWAP SPACE");

				//update supp table for swap index
				frame_table[clock_hand].page_dir_entry->swap_index = index;
				//write to swap
				swap_write (index, frame_table[clock_hand].page);
				set_in_swap(frame_table[clock_hand].page_dir_entry->meta);

				// if (swap_find_free())
			//swap to swap area
			//if swap table is full, panic
			}
			//else just clear out the page

			//updates to frame, supplemental page table, pagedir
			frame_clear_page (clock_hand, pd);
			evict_frame = frame_table[clock_hand].page;
			clock_hand = (clock_hand + 1) % user_pages;
		}
	}
	return evict_frame;
	
	// pagedir_clear_page (uint32_t *pd, void *upage) 

}
