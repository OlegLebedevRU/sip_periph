# Proposed linker snippet for persistent fault dump storage

Add a NOLOAD section before `._user_heap_stack`:

```ld
.noinit (NOLOAD) :
{
  . = ALIGN(8);
  *(.noinit)
  *(.noinit*)
  . = ALIGN(8);
} >RAM
```

Typical usage in C:

```c
__attribute__((section(".noinit"))) volatile fault_dump_t g_fault_dump;
```
