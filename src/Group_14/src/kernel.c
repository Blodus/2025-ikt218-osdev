/**
 * kernel.c - Main kernel entry point with Multiboot memory parsing.
 */

 #include "types.h"
 #include <string.h>
 #include <multiboot2.h> // Use the standard header
 
 #include "terminal.h"
 #include "gdt.h"
 #include "tss.h"
 #include "idt.h"
 #include "pit.h"
 #include "keyboard.h"
 #include "keymap.h"
 #include "buddy.h"
 #include "kmalloc.h"
 #include "pc_speaker.h"
 #include "song.h"
 #include "song_player.h"
 #include "my_songs.h"
 #include "paging.h"     // Provides PAGE_SIZE, KERNEL_SPACE_VIRT_START etc.
 #include "elf_loader.h"
 #include "process.h"
 #include "syscall.h"
 #include "scheduler.h"
 #include "get_cpu_id.h"
 #include "fs_init.h"
 #include "read_file.h"
 
 #define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289
 
 // Kernel stack (temporary boot stack)
 #define BOOT_STACK_SIZE (4096 * 4)
 static __attribute__((aligned(16))) uint8_t boot_stack[BOOT_STACK_SIZE];
 
 extern uint32_t end; // Provided by linker script
 uintptr_t kernel_image_end_phys = (uintptr_t)&end;
 
 extern uint32_t* kernel_page_directory; // Defined in paging.c
 
 // --- Multiboot Tag Iteration ---
 struct multiboot_tag *find_multiboot_tag(uint32_t mb_info_phys_addr, uint16_t type) {
     // First tag is 8 bytes after the total_size and reserved fields
     struct multiboot_tag *tag = (struct multiboot_tag *)(mb_info_phys_addr + 8);
     // Iterate through tags
     while (tag->type != MULTIBOOT_TAG_TYPE_END) {
         if (tag->type == type) {
             return tag;
         }
         // Move to the next tag: address + size, aligned up to 8 bytes
         tag = (struct multiboot_tag *)((uintptr_t)tag + ((tag->size + 7) & ~7));
     }
     return NULL; // Tag not found
 }
 
 // --- Find Largest Usable Memory Area ---
 // Finds the largest available RAM region above 1MB physical address.
 bool find_largest_memory_area(struct multiboot_tag_mmap *mmap_tag, uintptr_t *out_base_addr, size_t *out_size) {
     uintptr_t best_base = 0;
     uint64_t best_size = 0; // Use 64-bit for size comparison
 
     multiboot_memory_map_t *mmap_entry = mmap_tag->entries;
     uintptr_t mmap_end = (uintptr_t)mmap_tag + mmap_tag->size;
 
     terminal_write("Memory Map (from Multiboot):\n");
     while ((uintptr_t)mmap_entry < mmap_end) {
         terminal_printf("  Addr: 0x%x%x, Len: 0x%x%x, Type: %d\n",
                         (uint32_t)(mmap_entry->addr >> 32), (uint32_t)mmap_entry->addr,
                         (uint32_t)(mmap_entry->len >> 32), (uint32_t)mmap_entry->len,
                         mmap_entry->type);
 
         // Check if it's available RAM (Type 1) and above 1MB
         if (mmap_entry->type == MULTIBOOT_MEMORY_AVAILABLE && mmap_entry->addr >= 0x100000) {
              // Adjust start address if it overlaps with kernel image end
              uintptr_t entry_phys_start = (uintptr_t)mmap_entry->addr;
              uint64_t entry_len = mmap_entry->len;
              uintptr_t usable_start = entry_phys_start;
 
              if (usable_start < kernel_image_end_phys) {
                   if (entry_phys_start + entry_len > kernel_image_end_phys) {
                        // Region starts below kernel end but extends past it
                        uint64_t overlap = kernel_image_end_phys - usable_start;
                        usable_start = kernel_image_end_phys;
                        entry_len -= overlap;
                   } else {
                        // Region is entirely below kernel end, skip
                        entry_len = 0;
                   }
              }
 
              // Ensure usable_start is page aligned for buddy init? Buddy might handle internal alignment.
              // uintptr_t aligned_start = (usable_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
              // uint64_t alignment_loss = aligned_start - usable_start;
              // if (entry_len > alignment_loss) entry_len -= alignment_loss; else entry_len = 0;
              // usable_start = aligned_start;
 
 
             if (entry_len > best_size) {
                 best_size = entry_len;
                 best_base = usable_start; // Use the potentially adjusted start
             }
         }
 
         // Move to the next entry
         mmap_entry = (multiboot_memory_map_t *)((uintptr_t)mmap_entry + mmap_tag->entry_size);
     }
 
     if (best_size > 0) {
         *out_base_addr = best_base;
         *out_size = (size_t)best_size; // Truncate to size_t, assumes size fits
         terminal_printf("  Selected Region for Heap: Phys Addr=0x%x, Size=%u bytes (%u MB)\n",
                         best_base, (size_t)best_size, (size_t)best_size / (1024*1024));
         return true;
     } else {
         terminal_write("  Error: No suitable memory region found for heap!\n");
         return false;
     }
 }
 
 
 // --- Utility Functions --- (print_hex same as before)
 static void print_hex(uint32_t value) { /* ... same ... */
     char hex[9]; hex[8] = '\0';
     for (int i = 0; i < 8; i++) { uint8_t n = (value >> ((7 - i) * 4)) & 0xF; hex[i] = (n < 10) ? ('0' + n) : ('A' + n - 10); }
     terminal_write(hex);
 }
 
 static void print_memory_layout(uintptr_t heap_start, size_t heap_size) {
     terminal_write("\n[Kernel] Memory Layout:\n");
     terminal_write("  - Kernel Image End (Phys): 0x"); print_hex(kernel_image_end_phys); terminal_write("\n");
     terminal_write("  - Heap Start     (Phys): 0x"); print_hex(heap_start); terminal_write("\n");
     terminal_write("  - Heap Size            : "); terminal_printf("%u MB\n", heap_size / (1024*1024));
     terminal_write("  - Heap End       (Phys): 0x"); print_hex(heap_start + heap_size); terminal_write("\n");
 }
 
 // --- Memory Initialization ---
 // Returns true on success, false on failure
 static bool init_memory_management(uint32_t mb_info_phys_addr) {
     terminal_write("[Kernel] Initializing Memory Management...\n");
 
     // --- Find Memory Map ---
     struct multiboot_tag_mmap *mmap_tag = (struct multiboot_tag_mmap *)find_multiboot_tag(mb_info_phys_addr, MULTIBOOT_TAG_TYPE_MMAP);
     if (!mmap_tag) {
         terminal_write("  [ERROR] Multiboot memory map tag not found!\n");
         return false;
     }
 
     // --- Determine Heap Region ---
     uintptr_t heap_phys_start = 0;
     size_t heap_size = 0;
     if (!find_largest_memory_area(mmap_tag, &heap_phys_start, &heap_size)) {
         terminal_write("  [ERROR] Failed to find suitable heap region from memory map.\n");
         return false;
     }
 
     // Limit heap size if needed (e.g., to fit within initial mapping or MAX_ORDER limit)
     size_t max_buddy_size = (size_t)1 << MAX_ORDER; // MAX_ORDER from buddy.c (e.g., 22 for 4MB)
     if (heap_size > max_buddy_size) {
         terminal_printf("  Warning: Largest memory region (%u MB) > Max Buddy Size (%u MB). Clamping heap size.\n",
                         heap_size / (1024*1024), max_buddy_size / (1024*1024));
         heap_size = max_buddy_size;
     }
     if (heap_size < (1024 * 1024)) { // Require at least 1MB for heap?
          terminal_write("  [ERROR] Selected heap region is too small.\n");
          return false;
     }
 
     // Ensure heap_phys_start is page aligned (buddy allocator might prefer this)
     uintptr_t aligned_heap_start = (heap_phys_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
     size_t alignment_diff = aligned_heap_start - heap_phys_start;
     if (heap_size <= alignment_diff) {
          terminal_write("  [ERROR] Heap region too small after alignment.\n");
          return false;
     }
     heap_phys_start = aligned_heap_start;
     heap_size -= alignment_diff;
     // Ensure heap_size is also suitable (e.g., power of 2 or handled by buddy_init)
     // Buddy init will round it up/down internally based on order.
 
 
     // --- Initialize Buddy Allocator ---
     terminal_printf("  Initializing Buddy Allocator (Phys Addr: 0x%x, Size: %u bytes)\n", heap_phys_start, heap_size);
     // **CRITICAL FIX: Pass the PHYSICAL address to buddy_init**
     buddy_init((void *)heap_phys_start, heap_size);
     if (buddy_free_space() == 0) {
         terminal_write("  [ERROR] Buddy allocator initialization failed (check size/MAX_ORDER vs available RAM).\n");
         return false;
     }
     terminal_printf("  Buddy Allocator free space: %u bytes\n", buddy_free_space());
     print_memory_layout(heap_phys_start, heap_size); // Show final layout
 
 
     // --- Paging Setup ---
     terminal_write("  Setting up Paging...\n");
     uint32_t *initial_pd = (uint32_t *)buddy_alloc(PAGE_SIZE);
     if (!initial_pd) { /* ... error handling ... */ terminal_write("  [ERROR] Failed to allocate kernel page directory.\n"); return false; }
     memset(initial_pd, 0, PAGE_SIZE);
 
     // Map initial physical memory (identity + higher-half)
     // Make sure this mapping covers the kernel image AND the chosen heap region.
     uintptr_t required_mapping_end = heap_phys_start + heap_size;
     uint32_t phys_mapping_size = (uint32_t)required_mapping_end;
     // Ensure mapping is at least, say, 16MB, or covers up to end of heap, whichever is larger, rounded up.
     if (phys_mapping_size < (16 * 1024 * 1024)) phys_mapping_size = (16 * 1024 * 1024);
     phys_mapping_size = (phys_mapping_size + 0xFFFFF) & ~0xFFFFF; // Round up to MB? Or 4MB? Page align is min.
     phys_mapping_size = (phys_mapping_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
 
 
     terminal_printf("  Mapping physical memory up to 0x%x (%u MB) identity & higher-half...\n", phys_mapping_size, phys_mapping_size/(1024*1024));
     if (paging_init_identity_map(initial_pd, phys_mapping_size, PAGE_PRESENT | PAGE_RW) != 0) { /* ... error ... */ terminal_write("  [ERROR] Failed identity map.\n"); return false; }
     if (paging_map_range(initial_pd, KERNEL_SPACE_VIRT_START, phys_mapping_size, PAGE_PRESENT | PAGE_RW) != 0) { /* ... error ... */ terminal_write("  [ERROR] Failed higher-half map.\n"); return false; }
 
     // Activate Paging
     paging_set_directory(initial_pd);
     paging_activate(initial_pd);
     terminal_write("  [OK] Paging enabled.\n");
 
 
     // --- Kmalloc Initialization ---
     terminal_write("  Initializing Kmalloc Allocator...\n");
     kmalloc_init();
     terminal_write("  [OK] Kmalloc Allocator initialized.\n");
 
     terminal_write("[OK] Memory Management initialized.\n");
     return true;
 }
 
 // --- Idle Task & Main --- (kernel_idle_task same as before)
 void kernel_idle_task() { /* ... same ... */
     terminal_write("[Idle] Kernel idle task started.\n"); while(1) { asm volatile("hlt"); }
 }
 
 // Main function now takes physical address of multiboot info
 void main(uint32_t magic, uint32_t mb_info_phys_addr) {
 
     terminal_init();
     terminal_write("=== UiAOS Kernel Booting ===\n\n");
 
     if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) { /* ... error ... */ terminal_write("[ERR] MB Magic\n"); goto halt_system; }
     terminal_printf("[Boot] Multiboot magic OK (Info at phys 0x%x).\n", mb_info_phys_addr);
 
     // GDT & TSS
     terminal_write("[Kernel] Initializing GDT & TSS...\n");
     gdt_init();
     terminal_write("  [OK] GDT & TSS initialized.\n");
 
     // IDT & PIC
     terminal_write("[Kernel] Initializing IDT & PIC...\n");
     idt_init();
     terminal_write("  [OK] IDT & PIC initialized.\n");
 
     // Memory Management (Buddy, Paging, Kmalloc) using Multiboot info
     // **CRITICAL:** Pass the physical address mb_info_phys_addr
     // We need to map this address temporarily to read it if accessed after paging,
     // OR parse it *before* enabling paging. Let's parse first.
     // BUT init_memory_management enables paging. This is tricky.
     // Solution: map the multiboot info structure itself before enabling paging?
     // Alternative: Pass physical address, map it temporarily inside init_memory_management?
     // Let's pass phys address and assume init_memory_management accesses it before CR3 load.
     if (!init_memory_management(mb_info_phys_addr)) {
         terminal_write("[FATAL] Memory management initialization failed!\n");
         goto halt_system;
     }
 
     // PIT
     terminal_write("[Kernel] Initializing PIT...\n");
     init_pit();
     terminal_write("  [OK] PIT initialized.\n");
 
     // Keyboard
     terminal_write("[Kernel] Initializing Keyboard...\n");
     keyboard_init();
     keymap_load(KEYMAP_NORWEGIAN);
     terminal_write("  [OK] Keyboard initialized.\n");
 
     // Filesystem
      terminal_write("[Kernel] Initializing Filesystem Layer...\n");
      if (fs_init() != FS_SUCCESS) { /* ... error ... */ terminal_write("  [ERR] FS Init\n"); }
      else { terminal_write("  [OK] FS Initialized.\n"); }
 
     // Scheduler
     terminal_write("[Kernel] Initializing Scheduler...\n");
     scheduler_init();
     terminal_write("  [OK] Scheduler initialized.\n");
 
     // Create Initial Process
     terminal_write("[Kernel] Creating initial user process...\n");
     const char *user_prog_path = "/kernel.bin"; // Make sure this exists on your FAT image
     pcb_t *user_proc_pcb = create_user_process(user_prog_path);
     if (user_proc_pcb) {
         if (scheduler_add_task(user_proc_pcb) == 0) { /* OK */ }
         else { /* Error adding task */ destroy_process(user_proc_pcb); }
     } else { /* Error creating process */ }
 
 
     // Enable Interrupts
     terminal_write("\n[Kernel] Enabling interrupts (STI). Starting scheduler...\n");
     __asm__ volatile ("sti");
 
     // Idle Loop
     terminal_write("[Kernel] Entering main kernel idle loop (HLT).\n");
     kernel_idle_task();
 
 halt_system:
     terminal_write("\n[KERNEL HALTED]\n");
     while (1) { __asm__ volatile ("cli; hlt"); }
 }