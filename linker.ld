ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    boot   PT_LOAD FLAGS(7);
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 1M;
    kernelPhysStart = .;

    .boot ALIGN(8) :
    {
        *(.multiboot)
        *(.boot.text)
        *(.boot.data)
        . = ALIGN(4096);
        *(.boot.pagetables)
    } :boot

    . += KERNEL_VMA;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text .text.*)
    } :text

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        ktestArrayStart = .;
        KEEP(*(.ktests .ktests.*))
        ktestArrayEnd = .;
    } :rodata

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
    } :data

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        bssStart = .;
        *(COMMON)
        *(.bss .bss.*)
        bssEnd = .;
    } :data

    . = ALIGN(4096);
    kernelEnd = .;
    kernelPhysEnd = . - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note .note.*)
    }
}
