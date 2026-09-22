ENTRY(_start)

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
