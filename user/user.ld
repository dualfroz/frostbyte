ENTRY(_start)

PHDRS
{
    load PT_LOAD FILEHDR PHDRS FLAGS(7);
}

SECTIONS
{
    . = USER_BASE + SIZEOF_HEADERS;

    .text   : { *(.text .text.*) }         :load
    .rodata : { *(.rodata .rodata.*) }      :load
    .data   : { *(.data .data.*) }          :load
    .bss    : { *(.bss .bss.*) *(COMMON) }  :load

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
