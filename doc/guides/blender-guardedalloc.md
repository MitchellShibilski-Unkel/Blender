# MEMORY MANAGEMENT IN BLENDER (guardedalloc)

**NOTE:** This file does not cover memutil and smart pointers and reference counted
      garbage collection, which are contained in the memutil module. <br />

<br />

Blender takes care of dynamic memory allocation using a set of own functions
which are recognizable through their MEM_ prefix. All memory allocation and
deallocation in blender is done through these functions. <br />

<br />

The following functions are available through `MEM_guardedalloc.h`:

## Normal Operation:

`void *MEM_[mc]allocN(unsigned int len, char * str);`

- Nearest ANSI counterpart: malloc()
- String must be a static string describing the memory block (used for debugging memory management problems)
- Returns a memory block of length len
- MEM_callocN clears the memory block to 0

<hr />

`void *MEM_dupallocN(void *vmemh);`

- Nearest ANSI counterpart: combination malloc() and memcpy()
- Returns a pointer to a copy of the given memory area

<hr />

`short MEM_freeN(void *vmemh);`

- Nearest ANSI counterpart: free()
- Frees the memory area given by the pointer
- Returns 0 on success and !=0 on error

<hr />

`int MEM_allocN_len(void *vmemh);`

- Nearest ANSI counterpart: none known
- Returns the length of the given memory area

## Debugging

`void MEM_set_error_stream(FILE*);`

- This sets the file the memory manager should use to output debugging messages
- If the parameter is NULL the messages are suppressed
- Default is that messages are suppressed

`void MEM_printmemlist(void);`

- If err_stream is set by MEM_set_error_stream() this function dumps a list of all
currently allocated memory blocks with length and name to the stream

`bool MEM_consistency_check(void);`

- This function tests if the internal structures of the memory manager are intact
- Returns 0 on success and !=0 on error