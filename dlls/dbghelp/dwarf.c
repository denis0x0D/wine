/*
 * File dwarf.c - read dwarf2 information from the ELF modules
 *
 * Copyright (C) 2005, Raphael Junqueira
 * Copyright (C) 2006, Eric Pouech
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <sys/types.h>
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <stdio.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#include <assert.h>
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winnls.h"

#include "dbghelp_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dbghelp_dwarf);

#if 0
static void dump(const void* ptr, unsigned len)
{
  int         i, j;
  BYTE        msg[128];
  static const char hexof[] = "0123456789abcdef";
  const BYTE* x = (const BYTE*)ptr;

  for (i = 0; i < len; i += 16)
  {
    sprintf(msg, "%08x: ", i);
    memset(msg + 10, ' ', 3 * 16 + 1 + 16);
    for (j = 0; j < min(16, len - i); j++)
    {
      msg[10 + 3 * j + 0] = hexof[x[i + j] >> 4];
      msg[10 + 3 * j + 1] = hexof[x[i + j] & 15];
      msg[10 + 3 * j + 2] = ' ';
      msg[10 + 3 * 16 + 1 + j] = (x[i + j] >= 0x20 && x[i + j] < 0x7f) ?
	x[i + j] : '.';
    }
    msg[10 + 3 * 16] = ' ';
    msg[10 + 3 * 16 + 1 + 16] = '\0';
    TRACE("%s\n", msg);
  }
}
#endif

/**
 *
 * Main Specs:
 *  http://www.eagercon.com/dwarf/dwarf3std.htm
 *  http://www.eagercon.com/dwarf/dwarf-2.0.0.pdf
 *
 * dwarf2.h: http://www.hakpetzna.com/b/binutils/dwarf2_8h-source.html
 *
 * example of projects who do dwarf2 parsing:
 *  http://www.x86-64.org/cgi-bin/cvsweb.cgi/binutils.dead/binutils/readelf.c?rev=1.1.1.2
 *  http://elis.ugent.be/diota/log/ltrace_elf.c
 */
#include "dwarf.h"

/**
 * Parsers
 */

typedef struct dwarf2_abbrev_entry_attr_s {
  unsigned long attribute;
  unsigned long form;
  struct dwarf2_abbrev_entry_attr_s* next;
} dwarf2_abbrev_entry_attr_t;

typedef struct dwarf2_abbrev_entry_s {
  unsigned long entry_code;
  unsigned long tag;
  unsigned char have_child;
  dwarf2_abbrev_entry_attr_t* attrs;
  struct dwarf2_abbrev_entry_s* next;
} dwarf2_abbrev_entry_t;

typedef struct dwarf2_abbrev_table_s {
  dwarf2_abbrev_entry_t* first;
  unsigned n_entries;
} dwarf2_abbrev_table_t;

typedef struct dwarf2_parse_context_s {
  dwarf2_abbrev_table_t* abbrev_table;
  const unsigned char* data_stream;
  const unsigned char* data;
  const unsigned char* start_data;
  const unsigned char* end_data;
  const unsigned char* str_section;
  unsigned long offset;
  unsigned char word_size;
  unsigned char level;
} dwarf2_parse_context_t;

/* forward declarations */
static struct symt_enum* dwarf2_parse_enumeration_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx);

static unsigned char dwarf2_parse_byte(dwarf2_parse_context_t* ctx)
{
  unsigned char uvalue = *(const unsigned char*) ctx->data;
  ctx->data += 1;
  return uvalue;
}

static unsigned short dwarf2_parse_u2(dwarf2_parse_context_t* ctx)
{
  unsigned short uvalue = *(const unsigned short*) ctx->data;
  ctx->data += 2;
  return uvalue;
}

static unsigned long dwarf2_parse_u4(dwarf2_parse_context_t* ctx)
{
  unsigned long uvalue = *(const unsigned int*) ctx->data;
  ctx->data += 4;
  return uvalue;
}

static unsigned long dwarf2_leb128_as_unsigned(dwarf2_parse_context_t* ctx)
{
  unsigned long ret = 0;
  unsigned char byte;
  unsigned shift = 0;

  assert( NULL != ctx );

  while (1) {
    byte = dwarf2_parse_byte(ctx);
    ret |= (byte & 0x7f) << shift;
    shift += 7;
    if (0 == (byte & 0x80)) { break ; }
  }

  return ret;
}

static long dwarf2_leb128_as_signed(dwarf2_parse_context_t* ctx)
{
  long ret = 0;
  unsigned char byte;
  unsigned shift = 0;
  const unsigned size = sizeof(int) * 8;

  assert( NULL != ctx );

  while (1) {
    byte = dwarf2_parse_byte(ctx);
    ret |= (byte & 0x7f) << shift;
    shift += 7;
    if (0 == (byte & 0x80)) { break ; }
  }
  /* as spec: sign bit of byte is 2nd high order bit (80x40)
   *  -> 0x80 is used as flag.
   */
  if ((shift < size) && (byte & 0x40)) {
    ret |= - (1 << shift);
  }
  return ret;
}

static const char* dwarf2_debug_ctx(dwarf2_parse_context_t* ctx) 
{
  /*return wine_dbg_sprintf("ctx(0x%x,%u)", ctx->data - ctx->start_data, ctx->level); */
  return wine_dbg_sprintf("ctx(0x%x,%u)", ctx->data - ctx->data_stream, ctx->level); 
}
static const char* dwarf2_debug_attr(dwarf2_abbrev_entry_attr_t* attr) 
{
  return wine_dbg_sprintf("attr(attr:0x%lx,form:0x%lx)", attr->attribute, attr->form);
}

static void dwarf2_check_sibling(dwarf2_parse_context_t* ctx, unsigned long next_sibling)
{ 
  if (0 < next_sibling && ctx->data != ctx->data_stream + next_sibling) {
    if ((ctx->data + 1) != ctx->data_stream + next_sibling) {
      /** padding check */
      WARN("cursor error for %s should be sibling<0x%lx>\n", dwarf2_debug_ctx(ctx), next_sibling);
    }
    ctx->data = ctx->data_stream + next_sibling;
  }
}


static dwarf2_abbrev_entry_attr_t* dwarf2_abbrev_entry_add_attr(dwarf2_abbrev_entry_t* abbrev_entry, unsigned long attribute, unsigned long form)
{
  dwarf2_abbrev_entry_attr_t* ret = NULL;
  dwarf2_abbrev_entry_attr_t* it = NULL;

  assert( NULL != abbrev_entry );
  ret = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(dwarf2_abbrev_entry_attr_t));
  assert( NULL != ret );

  ret->attribute = attribute;
  ret->form      = form;

  ret->next = NULL;
  if (NULL == abbrev_entry->attrs) {
    abbrev_entry->attrs = ret;
  } else {
    for (it = abbrev_entry->attrs; NULL != it->next; it = it->next) ;
    it->next = ret;
  }
  return ret;
}

static dwarf2_abbrev_entry_t* dwarf2_abbrev_table_add_entry(dwarf2_abbrev_table_t* abbrev_table, unsigned long entry_code, unsigned long tag, unsigned char have_child)
{
  dwarf2_abbrev_entry_t* ret = NULL;

  assert( NULL != abbrev_table );
  ret = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(dwarf2_abbrev_entry_t));
  assert( NULL != ret );

  TRACE("(table:%p,n_entries:%u) entry_code(%lu) tag(0x%lx) have_child(%u) -> %p\n", abbrev_table, abbrev_table->n_entries, entry_code, tag, have_child, ret);

  ret->entry_code = entry_code;
  ret->tag        = tag;
  ret->have_child = have_child;
  ret->attrs      = NULL;

  ret->next       = abbrev_table->first;
  abbrev_table->first = ret;
  abbrev_table->n_entries++;
  return ret;
}

static dwarf2_abbrev_entry_t* dwarf2_abbrev_table_find_entry(dwarf2_abbrev_table_t* abbrev_table, unsigned long entry_code)
{
  dwarf2_abbrev_entry_t* ret = NULL;

  assert( NULL != abbrev_table );
  for (ret = abbrev_table->first; ret; ret = ret->next) {
    if (ret->entry_code == entry_code) { break ; }
  }
  return ret;
}

static void dwarf2_abbrev_table_free(dwarf2_abbrev_table_t* abbrev_table)
{
  dwarf2_abbrev_entry_t* entry = NULL;
  dwarf2_abbrev_entry_t* next_entry = NULL;
  assert( NULL != abbrev_table );
  for (entry = abbrev_table->first; NULL != entry; entry = next_entry) {
    dwarf2_abbrev_entry_attr_t* attr = NULL;
    dwarf2_abbrev_entry_attr_t* next_attr = NULL;
    for (attr = entry->attrs; NULL != attr; attr = next_attr) {
      next_attr = attr->next;
      HeapFree(GetProcessHeap(), 0, attr);
    }
    next_entry = entry->next;
    HeapFree(GetProcessHeap(), 0, entry);
  }
  abbrev_table->first = NULL;
  abbrev_table->n_entries = 0;
}

static dwarf2_abbrev_table_t* dwarf2_parse_abbrev_set(dwarf2_parse_context_t* abbrev_ctx)
{
  dwarf2_abbrev_table_t* abbrev_table = NULL;

  TRACE("%s, end at %p\n", dwarf2_debug_ctx(abbrev_ctx), abbrev_ctx->end_data); 

  assert( NULL != abbrev_ctx );
  abbrev_table = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(dwarf2_abbrev_table_t));
  assert( NULL != abbrev_table );

  while (abbrev_ctx->data < abbrev_ctx->end_data) {
    unsigned long entry_code;
    unsigned long tag;
    unsigned char have_child;
    dwarf2_abbrev_entry_t* abbrev_entry;

    TRACE("now at %s\n", dwarf2_debug_ctx(abbrev_ctx)); 
    entry_code = dwarf2_leb128_as_unsigned(abbrev_ctx);
    TRACE("found entry_code %lu\n", entry_code);
    if (0 == entry_code) {
      TRACE("NULL entry code at %s\n", dwarf2_debug_ctx(abbrev_ctx)); 
      break ;
    }
    tag = dwarf2_leb128_as_unsigned(abbrev_ctx);
    have_child = dwarf2_parse_byte(abbrev_ctx);

    abbrev_entry = dwarf2_abbrev_table_add_entry(abbrev_table, entry_code, tag, have_child);
    assert( NULL != abbrev_entry );
    while (1) {
      unsigned long attribute;
      unsigned long form;
      attribute = dwarf2_leb128_as_unsigned(abbrev_ctx);
      form = dwarf2_leb128_as_unsigned(abbrev_ctx);
      if (0 == attribute) break;
      dwarf2_abbrev_entry_add_attr(abbrev_entry, attribute, form);
    }
  }

  TRACE("found %u entries\n", abbrev_table->n_entries);
  return abbrev_table;
}

static const char* dwarf2_parse_attr_as_string(dwarf2_abbrev_entry_attr_t* attr,
					       dwarf2_parse_context_t* ctx)
{
  const char* ret = NULL;
  switch (attr->form) {
  case DW_FORM_string:
    ret = (const char*) ctx->data;
    ctx->data += strlen(ret) + 1;
    break;
  case DW_FORM_strp:
    {
      unsigned long offset = dwarf2_parse_u4(ctx);
      ret = (const char*) ctx->str_section + offset;
      /*FIXME("Unsupported indirect string format offset 0x%lx (in .debug_str)\n", offset);*/
    }
    break;
  default:
    ERR("Unsupported string format 0x%lx for attr 0x%lx\n", attr->form, attr->attribute);
  }
  return ret;
}

static unsigned long dwarf2_parse_attr_as_addr(dwarf2_abbrev_entry_attr_t* attr,
					       dwarf2_parse_context_t* ctx)
{
  unsigned long offset = 0;
  switch (ctx->word_size) {
  case 4:
    offset = *(const unsigned int*) ctx->data;
    break;
  case 8:
  default:
    FIXME("Unsupported Word Size %u\n", ctx->word_size);
  }
  ctx->data += ctx->word_size;
  return offset;  
}

static unsigned long dwarf2_parse_attr_as_ref(dwarf2_abbrev_entry_attr_t* attr,
					      dwarf2_parse_context_t* ctx)
{
  unsigned long uvalue = 0;
  switch (attr->form) {
  case DW_FORM_ref1:
    uvalue = ctx->offset + dwarf2_parse_byte(ctx);
    TRACE("ref1<0x%lx>\n", uvalue);
    break;

  case DW_FORM_ref2:
    uvalue = ctx->offset + dwarf2_parse_u2(ctx);
    TRACE("ref2<0x%lx>\n", uvalue);
    break;

  case DW_FORM_ref4:
    uvalue = ctx->offset + dwarf2_parse_u4(ctx);
    TRACE("ref4<0x%lx>\n", uvalue);
    break;

  case DW_FORM_ref8:
    /* FIXME: 64bits support */
    /*
    uvalue = ctx->offset + dwarf2_parse_u8(ctx);
    TRACE("ref8<0x%lx>\n", uvalue);
    */
    ctx->data += 8;
    break;
  }
  return uvalue;
}


static unsigned long dwarf2_parse_attr_as_data(dwarf2_abbrev_entry_attr_t* attr,
					       dwarf2_parse_context_t* ctx)
{
  unsigned long uvalue = 0;
  switch (attr->form) {
  case DW_FORM_data1:
    uvalue = dwarf2_parse_byte(ctx);
    TRACE("data1<%lu>\n", uvalue);
    break;

  case DW_FORM_data2:
    uvalue = dwarf2_parse_u2(ctx);
    TRACE("data2<%lu>\n", uvalue);
    break;

  case DW_FORM_data4:
    uvalue = dwarf2_parse_u4(ctx);
    TRACE("data4<%lu>\n", uvalue);
    break;

  case DW_FORM_data8:
    FIXME("Unsupported 64bits support\n");
    ctx->data += 8;
    break;
  }
  return uvalue;
}

static void dwarf2_parse_attr(dwarf2_abbrev_entry_attr_t* attr,
			      dwarf2_parse_context_t* ctx)
{
  const unsigned long attribute = attr->attribute;
  const unsigned long form = attr->form;
  unsigned long uvalue = 0;
  long svalue = 0;
  const char* str = NULL;

  TRACE("(attr:0x%lx,form:0x%lx)\n", attribute, form);

  switch (form) {
  case DW_FORM_ref_addr:
  case DW_FORM_addr:
    uvalue = dwarf2_parse_attr_as_addr(attr, ctx);
    break;

  case DW_FORM_flag:
    uvalue = dwarf2_parse_byte(ctx);
    TRACE("flag<0x%lx>\n", uvalue);
    break;

  case DW_FORM_data1:
    uvalue = dwarf2_parse_byte(ctx);
    TRACE("data1<%lu>\n", uvalue);
    break;

  case DW_FORM_data2:
    uvalue = dwarf2_parse_u2(ctx);
    TRACE("data2<%lu>\n", uvalue);
    break;

  case DW_FORM_data4:
    uvalue = dwarf2_parse_u4(ctx);
    TRACE("data4<%lu>\n", uvalue);
    break;

  case DW_FORM_ref1:
  case DW_FORM_ref2:
  case DW_FORM_ref4:
  case DW_FORM_ref8:
    uvalue = dwarf2_parse_attr_as_ref(attr, ctx);
    /*TRACE("ref<0x%lx>\n", ctx->offset + uvalue);*/
    break;

  case DW_FORM_data8:
    FIXME("Unsupported 64bits support\n");
    ctx->data += 8;
    break;

  case DW_FORM_sdata:
    svalue = dwarf2_leb128_as_signed(ctx);
    break;

  case DW_FORM_ref_udata:
  case DW_FORM_udata:
    uvalue = dwarf2_leb128_as_unsigned(ctx);
    break;

  case DW_FORM_string:
  case DW_FORM_strp:
    str = dwarf2_parse_attr_as_string(attr, ctx);
    TRACE("string<%s>\n", str);
    break;

  case DW_FORM_block:
    uvalue = dwarf2_leb128_as_unsigned(ctx);
    ctx->data += uvalue;
    break;

  case DW_FORM_block1:
    uvalue = dwarf2_parse_byte(ctx);
    ctx->data += uvalue;
    break;

  case DW_FORM_block2:
    uvalue = dwarf2_parse_u2(ctx);
    ctx->data += uvalue;
    break;

  case DW_FORM_block4:
    uvalue = dwarf2_parse_u4(ctx);
    ctx->data += uvalue;
    break;

  default:
    break;
  }
}

static struct symt* dwarf2_find_symt_by_ref(struct module* module, unsigned long ref)
{
  TRACE("want ref<0x%lx>\n", ref); 
  return NULL;
}

static struct symt* dwarf2_add_symt_ref(struct module* module, unsigned long ref, struct symt* symt)
{
  if (NULL != symt) return NULL;
  return NULL;
}

static struct symt_basic* dwarf2_parse_base_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_basic* symt = NULL;
  const char* name = NULL;
  unsigned size = 0;
  unsigned encoding = 0;
  enum BasicType bt;
  dwarf2_abbrev_entry_attr_t* attr = NULL;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_byte_size:
      size = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_encoding:
      encoding = dwarf2_parse_byte(ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  switch (encoding) {
  case DW_ATE_void: bt = btVoid; break;
  case DW_ATE_address: bt = btULong; break;
  case DW_ATE_boolean: bt = btBool; break;
  case DW_ATE_complex_float: bt = btComplex; break;
  case DW_ATE_float: bt = btFloat; break;
  case DW_ATE_signed: bt = btInt; break;
  case DW_ATE_unsigned: bt = btUInt; break;
  case DW_ATE_signed_char: bt = btChar; break;
  case DW_ATE_unsigned_char: bt = btChar; break;
  default:
    bt = btNoType;
  }
  /*TRACE("symt_new_basic(%p, %u, %s, %u)", module, bt, name, size);*/
  symt = symt_new_basic(module, bt, name, size);

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }
  return symt;
}

static struct symt_typedef* dwarf2_parse_typedef(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_typedef* symt = NULL;
  struct symt* ref_type = NULL;
  const char* name = NULL;
  dwarf2_abbrev_entry_attr_t* attr = NULL;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	ref_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  if (NULL != name) {
    symt = symt_new_typedef(module, ref_type, name);
  }

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }
  return symt;
}

static struct symt_pointer* dwarf2_parse_pointer_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_pointer* symt = NULL;
  struct symt* ref_type = NULL;
  unsigned size = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_byte_size:
      size = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	ref_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  symt = symt_new_pointer(module, ref_type);

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }
  return symt;
}

static void dwarf2_parse_array_subrange_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx, struct symt_array* parent)
{
  unsigned min = 0;
  unsigned max = 0;
  struct symt* idx_type = NULL;
  dwarf2_abbrev_entry_attr_t* attr = NULL;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	idx_type = dwarf2_find_symt_by_ref(module, ref);
	/** check if idx_type is a basic_type integer */
      }
      break;
    case DW_AT_lower_bound:
      TRACE("%s %s, lower_bound\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      min = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_upper_bound:
      TRACE("%s %s, upper_bound\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      max = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_count:
      TRACE("%s %s, count min:%u\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr), min); 
      max = min + dwarf2_parse_attr_as_data(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  parent->start = min;
  parent->end = max;
  parent->index_type = idx_type;

  TRACE("found min:%u max:%u\n", min, max);   

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }
}


static struct symt_array* dwarf2_parse_array_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_array* symt = NULL;
  struct symt* ref_type = NULL;
  unsigned min = 0;
  unsigned max = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	ref_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }

  /* FIXME: ugly as hell */
  symt = symt_new_array(module, min, max, ref_type, NULL);

  if (entry->have_child) { /** any interest to not have child ? */
    ++ctx->level;
    while (ctx->data < ctx->end_data) {
      dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx->data - ctx->data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	break ;
      }

      entry = dwarf2_abbrev_table_find_entry(ctx->abbrev_table, entry_code);
      assert( NULL != entry );

      switch (entry->tag) {
      case DW_TAG_subrange_type:
	dwarf2_parse_array_subrange_type(module, entry, ctx, symt);
	break;
      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, ctx);
	  }
	}
	break;
      }
    }
    --ctx->level;
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return symt;
}

static struct symt* dwarf2_parse_const_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt* ref_type = NULL;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	ref_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return ref_type;
}

static struct symt* dwarf2_parse_reference_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt* symt = NULL;
  struct symt* ref_type = NULL;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	ref_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  /* FIXME: for now, we hard-wire C++ references to pointers */
  symt = &symt_new_pointer(module, ref_type)->symt;

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return symt;
}

static void dwarf2_parse_udt_member(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx, struct symt_udt* parent)
{
  struct symt* elt_type = NULL;
  const char* name = NULL; 
  unsigned long offset = 0;
  unsigned size = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  assert( NULL != parent );

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	elt_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    case DW_AT_data_member_location:
      {
	unsigned long uvalue = 0;
	TRACE("found member_location at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr));
	/*offset = dwarf2_parse_attr_as_data(attr, ctx);*/
	switch (attr->form) {
	case DW_FORM_block:
	  uvalue = dwarf2_leb128_as_unsigned(ctx);
	  break;
	case DW_FORM_block1:
	  uvalue = dwarf2_parse_byte(ctx);
	  break;
	case DW_FORM_block2:
	  uvalue = dwarf2_parse_u2(ctx);
	  break;
	case DW_FORM_block4:
	  uvalue = dwarf2_parse_u4(ctx);
	  break;
	default:
	  TRACE("Unhandled attr form at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
	  dwarf2_parse_attr(attr, ctx);
	}
	if (uvalue) {
	  unsigned char op = dwarf2_parse_byte(ctx);
	  --uvalue;
	  switch (op) {
	  case DW_OP_plus_uconst:
	    offset = dwarf2_leb128_as_unsigned(ctx);
	    break;
	  default:
	    TRACE("Unhandled attr op at %s, for %s, op:%u\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr), op);
	    ctx->data += uvalue;
	  }
	  TRACE("found offset:%lu\n", offset); 		  
	}
      }
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  symt_add_udt_element(module, parent, name, elt_type, offset, size);

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);
}

static void dwarf2_parse_udt_members(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx, struct symt_udt* symt)
{
  if (entry->have_child) { /** any interest to not have child ? */
    ++ctx->level;
    while (ctx->data < ctx->end_data) {
      dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx->data - ctx->data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	break ;
      }

      entry = dwarf2_abbrev_table_find_entry(ctx->abbrev_table, entry_code);
      assert( NULL != entry );

      switch (entry->tag) {
      case DW_TAG_member:
	dwarf2_parse_udt_member(module, entry, ctx, symt);
	break;
      case DW_TAG_enumeration_type:
	{
	  struct symt_enum* symt = dwarf2_parse_enumeration_type(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, ctx);
	  }
	}
	break;
      }
    }
    --ctx->level;
  }
}

static struct symt_udt* dwarf2_parse_class_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_udt* symt = NULL;
  const char* name = NULL;
  unsigned size = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_byte_size:
      size = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  symt = symt_new_udt(module, name, size, UdtClass);
  dwarf2_parse_udt_members(module, entry, ctx, symt);

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return symt;
}

static struct symt_udt* dwarf2_parse_struct_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_udt* symt = NULL;
  const char* name = NULL;
  unsigned size = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_byte_size:
      size = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  symt = symt_new_udt(module, name, size, UdtStruct);
  dwarf2_parse_udt_members(module, entry, ctx, symt);

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return symt;
}

static struct symt_udt* dwarf2_parse_union_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_udt* symt = NULL;
  const char* name = NULL;
  unsigned size = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_byte_size:
      size = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  symt = symt_new_udt(module, name, size, UdtUnion);
  dwarf2_parse_udt_members(module, entry, ctx, symt);

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return symt;
}

static void dwarf2_parse_enumerator(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx, struct symt_enum* parent)
{
  const char* name = NULL;
  long value = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_const_value:
      switch (attr->form) {
      case DW_FORM_sdata:
	value = dwarf2_leb128_as_signed(ctx);
	TRACE("found value %ld\n", value);
	break;
      case DW_FORM_udata:
	value = dwarf2_leb128_as_unsigned(ctx);
	TRACE("found value %ld\n", value);
	break;
      default:
	TRACE("Unhandled attr form at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr));
	dwarf2_parse_attr(attr, ctx);
      }
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr));
      dwarf2_parse_attr(attr, ctx);
    }
  }
  symt_add_enum_element(module, parent, name, value);

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }
}

static struct symt_enum* dwarf2_parse_enumeration_type(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_enum* symt = NULL;
  const char* name = NULL;
  unsigned long size = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_byte_size:
      size = dwarf2_parse_attr_as_data(attr, ctx);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  symt = symt_new_enum(module, name);

  if (entry->have_child) { /** any interest to not have child ? */
    ++ctx->level;
    while (ctx->data < ctx->end_data) {
      dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx->data - ctx->data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	break ;
      }

      entry = dwarf2_abbrev_table_find_entry(ctx->abbrev_table, entry_code);
      assert( NULL != entry );

      switch (entry->tag) {
      case DW_TAG_enumerator:
	dwarf2_parse_enumerator(module, entry, ctx, symt);
	break;
      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, ctx);
	  }
	}
	break;
      }
    }
    --ctx->level;
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return symt;
}

static void dwarf2_parse_variable(struct module* module, 
				  dwarf2_abbrev_entry_t* entry, 
				  dwarf2_parse_context_t* ctx) 
{
  struct symt* var_type = NULL;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  const char* name = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	var_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);  
}

static void dwarf2_parse_subprogram_parameter(struct module* module, 
					      dwarf2_abbrev_entry_t* entry, 
					      dwarf2_parse_context_t* ctx, 
					      struct symt_function_signature* sig_type,
					      struct symt_function* func_type)
{
  struct symt* param_type = NULL;
  const char* name = NULL;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	param_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    case DW_AT_location:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  if (NULL != sig_type) {
    symt_add_function_signature_parameter(module, sig_type, param_type);
  }

  if (entry->have_child) {
    FIXME("Unsupported children\n");
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);  
}

static void dwarf2_parse_inlined_subroutine(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  const char* name = NULL;
  unsigned long addr = 0;
  unsigned long low_pc = 0;
  unsigned long high_pc = 0;
  unsigned size = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_low_pc:
      low_pc = dwarf2_parse_attr_as_addr(attr, ctx);
      addr = module->module.BaseOfImage + low_pc;
      break;
    case DW_AT_high_pc:
      high_pc = dwarf2_parse_attr_as_addr(attr, ctx);
      size = high_pc - low_pc;
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }

  if (entry->have_child) { /** any interest to not have child ? */
    ++ctx->level;
    while (ctx->data < ctx->end_data) {
      dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx->data - ctx->data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	break ;
      }

      entry = dwarf2_abbrev_table_find_entry(ctx->abbrev_table, entry_code);
      assert( NULL != entry );

      switch (entry->tag) {
      case DW_TAG_formal_parameter:
	dwarf2_parse_subprogram_parameter(module, entry, ctx, NULL, NULL);
	break;
      case DW_TAG_variable:
	dwarf2_parse_variable(module, entry, ctx);
	break;
      case DW_TAG_label:
      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, ctx);
	  }
	}
	break;
      }
    }
    --ctx->level;
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);
}


static void dwarf2_parse_subprogram_block(struct module* module, 
					  dwarf2_abbrev_entry_t* entry, 
					  dwarf2_parse_context_t* ctx, 
					  struct symt_function_signature* sig_type,
					  struct symt_function* func_type)
{
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  const char* name = NULL;
  unsigned long next_sibling = 0;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_decl_file:
    case DW_AT_decl_line:
      dwarf2_parse_attr(attr, ctx);
      break;
    case DW_AT_ranges: /** what to do ? */
      dwarf2_parse_attr(attr, ctx);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  
  if (entry->have_child) { /** any interest to not have child ? */
    ++ctx->level;
    while (ctx->data < ctx->end_data) {
      dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx->data - ctx->data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	break ;
      }

      entry = dwarf2_abbrev_table_find_entry(ctx->abbrev_table, entry_code);
      assert( NULL != entry );

      switch (entry->tag) {
      case DW_TAG_inlined_subroutine:
	dwarf2_parse_inlined_subroutine(module, entry, ctx);
	break;
      case DW_TAG_variable:
	dwarf2_parse_variable(module, entry, ctx);
	break;
      case DW_TAG_label:
      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, ctx);
	  }
	}
	break;
      }
    }
    --ctx->level;
  }

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);
}

static void dwarf2_parse_subprogram_content(struct module* module, 
					    dwarf2_abbrev_entry_t* entry, 
					    dwarf2_parse_context_t* ctx, 
					    struct symt_function_signature* sig_type,
					    struct symt_function* func_type)
{
  if (entry->have_child) { /** any interest to not have child ? */
    ++ctx->level;
    while (ctx->data < ctx->end_data) {
      dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx->data - ctx->data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	break ;
      }

      entry = dwarf2_abbrev_table_find_entry(ctx->abbrev_table, entry_code);
      assert( NULL != entry );

      switch (entry->tag) {
      case DW_TAG_formal_parameter:
	dwarf2_parse_subprogram_parameter(module, entry, ctx, sig_type, func_type);
	break;
      case DW_TAG_lexical_block:
	dwarf2_parse_subprogram_block(module, entry, ctx, sig_type, func_type);
	break;
      case DW_TAG_variable:
	dwarf2_parse_variable(module, entry, ctx);
	break;
      case DW_TAG_label:
      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, ctx);
	  }
	}
	break;
      }
    }
    --ctx->level;
  }
}

static struct symt_function* dwarf2_parse_subprogram(struct module* module, dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx, struct symt_compiland* compiland)
{
  struct symt_function* func_type = NULL;
  const char* name = NULL;
  struct symt* ret_type = NULL;
  struct symt_function_signature* sig_type = NULL;
  unsigned long addr = 0;
  unsigned long low_pc = 0;
  unsigned long high_pc = 0;
  unsigned size = 0;
  unsigned char is_decl = 0;
  unsigned char inl_flags = 0;
  unsigned char decl_file = 0;
  unsigned char decl_line = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;
  unsigned long next_sibling = 0;
  enum CV_call_e call_conv = CV_CALL_FAR_C; /* FIXME: assuming C source code */
  unsigned cc;

  TRACE("%s, for %lu\n", dwarf2_debug_ctx(ctx), entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_low_pc:
      low_pc = dwarf2_parse_attr_as_addr(attr, ctx);
      addr = module->module.BaseOfImage + low_pc;
      break;
    case DW_AT_high_pc:
      high_pc = dwarf2_parse_attr_as_addr(attr, ctx);
      size = high_pc - low_pc;
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    case DW_AT_type:
      {
	unsigned long ref = dwarf2_parse_attr_as_ref(attr, ctx);
	ret_type = dwarf2_find_symt_by_ref(module, ref);
      }
      break;
    case DW_AT_declaration:
      is_decl = dwarf2_parse_byte(ctx);
      break;
    case DW_AT_inline:
      inl_flags = dwarf2_parse_byte(ctx);
      break;
    case DW_AT_calling_convention:
        switch (cc = dwarf2_parse_byte(ctx))
        {
        case DW_CC_normal: break;
        case DW_CC_nocall: call_conv = -1;
        default: FIXME("Unsupported calling convention %d\n", cc);
        }
        break;
    /* not work yet, need parsing .debug_line and using Compil Unit stmt_list
    case DW_AT_decl_file:
      decl_file =  dwarf2_parse_byte(ctx);
      break;
    case DW_AT_decl_line:
      decl_line =  dwarf2_parse_byte(ctx);
      break;
    */
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  sig_type = symt_new_function_signature(module, ret_type, call_conv);
  if (!is_decl) {
    func_type = symt_new_function(module, compiland, name, addr, size, &sig_type->symt);
    if (low_pc && high_pc) {
      symt_add_function_point(module, func_type, SymTagFuncDebugStart, low_pc, NULL);
      symt_add_function_point(module, func_type, SymTagFuncDebugEnd, high_pc, NULL);
    }
    if (decl_file && decl_line) {
      symt_add_func_line(module, func_type, decl_file, decl_line, low_pc);
    }
  }
  dwarf2_parse_subprogram_content(module, entry, ctx, sig_type, func_type);
  symt_normalize_function(module, func_type);

  /** set correct data cursor */
  dwarf2_check_sibling(ctx, next_sibling);

  return func_type;
}

static void dwarf2_parse_compiland_content(struct module* module, const dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx, struct symt_compiland* compiland)
{
  if (entry->have_child) { /** any interest to not have child ? */
    ++ctx->level;
    while (ctx->data < ctx->end_data) {
      dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx->data - ctx->data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	break ;
      }

      entry = dwarf2_abbrev_table_find_entry(ctx->abbrev_table, entry_code);
      assert( NULL != entry );

      switch (entry->tag) {
      case DW_TAG_typedef:
	{
	  struct symt_typedef* symt = dwarf2_parse_typedef(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_base_type:
	{
	  struct symt_basic* symt = dwarf2_parse_base_type(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_pointer_type:
	{
	  struct symt_pointer* symt = dwarf2_parse_pointer_type(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_class_type:
	{
	  struct symt_udt* symt = dwarf2_parse_class_type(module, entry, ctx);
	  if (NULL != symt) dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_structure_type:
	{
	  struct symt_udt* symt = dwarf2_parse_struct_type(module, entry, ctx);
	  if (NULL != symt) dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_union_type:
	{
	  struct symt_udt* symt = dwarf2_parse_union_type(module, entry, ctx);
	  if (NULL != symt) dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_array_type:
	{
	  struct symt_array* symt = dwarf2_parse_array_type(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_const_type:
	{
	  struct symt* symt = dwarf2_parse_const_type(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, symt);
	}
	break;
      case DW_TAG_reference_type:
	{
	  struct symt* symt = dwarf2_parse_reference_type(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, symt);
	}
	break;
      case DW_TAG_enumeration_type:
	{
	  struct symt_enum* symt = dwarf2_parse_enumeration_type(module, entry, ctx);
	  dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;
      case DW_TAG_subprogram:
	{
	  struct symt_function* symt = dwarf2_parse_subprogram(module, entry, ctx, compiland);
	  if (NULL != symt) dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	}
	break;

      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, ctx);
	  }
	}
	break;
      }
    }
    --ctx->level;
  }
}

static struct symt_compiland* dwarf2_parse_compiland(struct module* module, const dwarf2_abbrev_entry_t* entry, dwarf2_parse_context_t* ctx)
{
  struct symt_compiland* compiland = NULL;
  const char* name = NULL;
  unsigned long next_sibling = 0;
  dwarf2_abbrev_entry_attr_t* attr = NULL;

  TRACE("beginning at Ox%x, for %lu\n", ctx->data - ctx->start_data, entry->entry_code); 

  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
    switch (attr->attribute) {
    case DW_AT_sibling:
      next_sibling = dwarf2_parse_attr_as_ref(attr, ctx);
      break;
    case DW_AT_name:
      name = dwarf2_parse_attr_as_string(attr, ctx);
      TRACE("found name %s\n", name);
      break;
    default:
      TRACE("Unhandled attr at %s, for %s\n", dwarf2_debug_ctx(ctx), dwarf2_debug_attr(attr)); 
      dwarf2_parse_attr(attr, ctx);
    }
  }
  compiland = symt_new_compiland(module, name);
  dwarf2_parse_compiland_content(module, entry, ctx, compiland);

  dwarf2_check_sibling(ctx, next_sibling);

  return compiland;
}

BOOL dwarf2_parse(struct module* module, unsigned long load_offset,
		  const unsigned char* debug, unsigned int debug_size,
		  const unsigned char* abbrev, unsigned int abbrev_size,
		  const unsigned char* str, unsigned int str_sz)
{
  const unsigned char* comp_unit_cursor = debug;
  const unsigned char* end_debug = debug + debug_size;

  while (comp_unit_cursor < end_debug) {
    dwarf2_abbrev_table_t* abbrev_table;
    const dwarf2_comp_unit_stream_t* comp_unit_stream;
    dwarf2_comp_unit_t comp_unit;
    dwarf2_parse_context_t ctx;
    dwarf2_parse_context_t abbrev_ctx;
    struct symt_compiland* compiland = NULL;
    
    comp_unit_stream = (const dwarf2_comp_unit_stream_t*) comp_unit_cursor;

    comp_unit.length = *(unsigned long*)  comp_unit_stream->length;
    comp_unit.version = *(unsigned short*) comp_unit_stream->version;
    comp_unit.abbrev_offset = *(unsigned long*) comp_unit_stream->abbrev_offset;
    comp_unit.word_size = *(unsigned char*) comp_unit_stream->word_size;

    TRACE("Compilation Unit Herder found at 0x%x:\n", comp_unit_cursor - debug);
    TRACE("- length:        %lu\n", comp_unit.length);
    TRACE("- version:       %u\n",  comp_unit.version);
    TRACE("- abbrev_offset: %lu\n", comp_unit.abbrev_offset);
    TRACE("- word_size:     %u\n",  comp_unit.word_size);

    ctx.data_stream = debug;
    ctx.data = ctx.start_data = comp_unit_cursor + sizeof(dwarf2_comp_unit_stream_t);
    ctx.offset = comp_unit_cursor - debug;
    ctx.word_size = comp_unit.word_size;
    ctx.str_section = str;
    ctx.level = 0;

    comp_unit_cursor += comp_unit.length + sizeof(unsigned);
    ctx.end_data = comp_unit_cursor;

    if (2 != comp_unit.version) {
      WARN("%u DWARF version unsupported. Wine dbghelp only support DWARF 2.\n", comp_unit.version);
      continue ;
    }

    abbrev_ctx.abbrev_table = NULL;
    abbrev_ctx.data_stream = abbrev;
    abbrev_ctx.data = abbrev_ctx.start_data = abbrev + comp_unit.abbrev_offset;
    abbrev_ctx.end_data = abbrev + abbrev_size;
    abbrev_ctx.offset = comp_unit.abbrev_offset;
    abbrev_ctx.str_section = str;
    abbrev_table = dwarf2_parse_abbrev_set(&abbrev_ctx);    

    ctx.abbrev_table = abbrev_table;

    while (ctx.data < ctx.end_data) {
      const dwarf2_abbrev_entry_t* entry = NULL;
      unsigned long entry_code;
      unsigned long entry_ref = 0;

      entry_ref = ctx.data - ctx.data_stream;
      
      entry_code = dwarf2_leb128_as_unsigned(&ctx);
      TRACE("found entry_code %lu at 0x%lx\n", entry_code, entry_ref);
      if (0 == entry_code) {
	continue ;
      }
      entry = dwarf2_abbrev_table_find_entry(abbrev_table, entry_code);
      if (NULL == entry) {
	WARN("Cannot find abbrev entry for %lu at 0x%lx\n", entry_code, entry_ref);
	dwarf2_abbrev_table_free(abbrev_table);
	return FALSE;
      }

      switch (entry->tag) {
      case DW_TAG_compile_unit:
	{
	  struct symt_compiland* symt = dwarf2_parse_compiland(module, entry, &ctx);
	  dwarf2_add_symt_ref(module, entry_ref, &symt->symt);
	  compiland = symt;
	}
	break;
      default:
	{
	  dwarf2_abbrev_entry_attr_t* attr;
	  TRACE("Unhandled Tag type 0x%lx at %s, for %lu\n", entry->tag, dwarf2_debug_ctx(&ctx), entry->entry_code); 
	  for (attr = entry->attrs; NULL != attr; attr = attr->next) {
	    dwarf2_parse_attr(attr, &ctx);
	  }
	}
	break;
      }
    }
    dwarf2_abbrev_table_free(abbrev_table);
  }
  
  module->module.SymType = SymDia;
  return TRUE;
}
