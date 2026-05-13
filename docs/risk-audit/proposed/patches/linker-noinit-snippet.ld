/*
 * Proposed linker snippet for persistent fault dump storage.
 * Add this NOLOAD section before `._user_heap_stack`.
 */
.noinit (NOLOAD) :
{
  . = ALIGN(8);
  *(.noinit)
  *(.noinit*)
  . = ALIGN(8);
} >RAM
