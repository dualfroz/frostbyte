ENTRY(_start)

/* Two PT_LOAD segments, not one: text+rodata read+execute only (FLAGS(5)
   = PF_R|PF_X), data+bss read+write only (FLAGS(6) = PF_R|PF_W) -- real
   W^X for every binary this links, mirroring the kernel's own linker.ld
   (:text/:rodata/:data). elf.c's elfLoad reads each segment's own p_flags
   (elfSegmentPagingFlags) to map it with paging.h's pageWrite/pageNoExecute
   accordingly, so a binary that writes into its own string literals or
   jumps into its own .data now takes a real, catchable SIGSEGV instead of
   silently succeeding the way one shared RWX segment always allowed --
   see user/elfwritevictim.c and user/elfexecvictim.c for the live proof. */
PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = USER_BASE + SIZEOF_HEADERS;

    .text   : { *(.text .text.*) }         :text
    .rodata : { *(.rodata .rodata.*) }      :text

    /* Fresh page for .data: the two PT_LOAD segments above must never
       share a page, or the CPU would have to apply the union of both
       segments' flags to that shared page, quietly reintroducing a
       writable+executable (or writable .rodata) page exactly where this
       split exists to remove one. */
    . = ALIGN(4096);

    .data   : { *(.data .data.*) }          :data
    .bss    : { *(.bss .bss.*) *(COMMON) }  :data

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
