/* PAPP file and story-memory backend for the AZIP/Jzip interpreter. */
#include "ztypes.h"

static unsigned char *s_story;
static unsigned long s_story_size;

void azip_set_story(unsigned char *data, unsigned long size)
{
    s_story = data;
    s_story_size = size;
}

unsigned char *azip_story_ptr(void)
{
    return s_story;
}

unsigned long azip_story_size(void)
{
    return s_story_size;
}

void open_story(char *) {}
void close_story(void) {}

unsigned int get_story_size(void)
{
    return (unsigned int)s_story_size;
}

static zbyte_t story_read(unsigned long address)
{
    return (s_story && address < s_story_size) ? s_story[address] : 0;
}

static void story_write(unsigned long address, zbyte_t value)
{
    if (s_story && address < s_story_size) s_story[address] = value;
}

zword_t read_code_word(void)
{
    return (zword_t)(((zword_t)read_code_byte() << 8) | read_code_byte());
}

zbyte_t read_code_byte(void)
{
    zbyte_t value = story_read(pc);
    pc++;
    return value;
}

zword_t read_data_word(unsigned long *addr)
{
    return (zword_t)(((zword_t)read_data_byte(addr) << 8) | read_data_byte(addr));
}

void write_data_word(unsigned long *addr, zword_t value)
{
    write_data_byte(addr, (zbyte_t)(value >> 8));
    write_data_byte(addr, (zbyte_t)value);
}

zbyte_t read_data_byte(unsigned long *addr)
{
    zbyte_t value = story_read(*addr);
    (*addr)++;
    return value;
}

void write_data_byte(unsigned long *addr, zbyte_t value)
{
    story_write(*addr, value);
    (*addr)++;
}

zbyte_t get_byte(unsigned long offset)
{
    return story_read(offset);
}

zword_t get_word(unsigned long offset)
{
    return (zword_t)(((zword_t)story_read(offset) << 8) | story_read(offset + 1));
}

void set_byte(unsigned long offset, zbyte_t value)
{
    story_write(offset, value);
}

void set_word(unsigned long offset, zword_t value)
{
    story_write(offset, (zbyte_t)(value >> 8));
    story_write(offset + 1, (zbyte_t)value);
}

int z_save(int, zword_t, zword_t, zword_t)
{
    int status = azip_save_state();
    if (h_type < V4) conditional_jump(status == 0);
    else store_operand((zword_t)(status ? 1 : 0));
    return status;
}

int z_restore(int, zword_t, zword_t, zword_t)
{
    int status = azip_restore_state();
    if (h_type < V4) conditional_jump(status == 0);
    else store_operand((zword_t)(status ? 2 : 0));
    return status ? 2 : 0;
}

void z_save_undo(void) { store_operand((zword_t)-1); }
void z_restore_undo(void) { store_operand((zword_t)-1); }

void z_verify(void) { conditional_jump(TRUE); }
