#encoding=utf-8

# Copyright (C) 2016 Intel Corporation
# Copyright (C) 2016 Broadcom
# Copyright (C) 2020 Collabora, Ltd.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

import xml.parsers.expat
import sys
import operator
from functools import reduce

global_prefix = "agx"

pack_header = """
/* Generated code, see midgard.xml and gen_pack_header.py
 *
 * Packets, enums and structures for Panfrost.
 *
 * This file has been generated, do not hand edit.
 */

#ifndef AGX_PACK_H
#define AGX_PACK_H

#include <stdio.h>
#include <inttypes.h>

#include "util/bitpack_helpers.h"

#define __gen_unpack_float(x, y, z) uif(__gen_unpack_uint(x, y, z))

static inline uint64_t
__gen_unpack_uint(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
   uint64_t val = 0;
   const int width = end - start + 1;
   const uint64_t mask = (width == 64 ? ~0 : (1ull << width) - 1 );

   for (unsigned byte = start / 8; byte <= end / 8; byte++) {
      val |= ((uint64_t) cl[byte]) << ((byte - start / 8) * 8);
   }

   return (val >> (start % 8)) & mask;
}

/*
 * LODs are 4:6 fixed point. We must clamp before converting to integers to
 * avoid undefined behaviour for out-of-bounds inputs like +/- infinity.
 */
static inline uint32_t
__gen_pack_lod(float f, uint32_t start, uint32_t end)
{
    uint32_t fixed = CLAMP(f * (1 << 6), 0 /* 0.0 */, 0x380 /* 14.0 */);
    return util_bitpack_uint(fixed, start, end);
}

static inline float
__gen_unpack_lod(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
    return ((float) __gen_unpack_uint(cl, start, end)) / (1 << 6);
}

static inline uint64_t
__gen_unpack_sint(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
   int size = end - start + 1;
   int64_t val = __gen_unpack_uint(cl, start, end);

   return util_sign_extend(val, size);
}

#define agx_prepare(dst, T)                                 \\
   *(dst) = (struct AGX_ ## T){ AGX_ ## T ## _header }

#define agx_pack(dst, T, name)                              \\
   for (struct AGX_ ## T name = { AGX_ ## T ## _header }, \\
        *_loop_terminate = (void *) (dst);                  \\
        __builtin_expect(_loop_terminate != NULL, 1);       \\
        ({ AGX_ ## T ## _pack((uint32_t *) (dst), &name);  \\
           _loop_terminate = NULL; }))

#define agx_unpack(fp, src, T, name)                        \\
        struct AGX_ ## T name;                         \\
        AGX_ ## T ## _unpack(fp, (uint8_t *)(src), &name)

#define agx_print(fp, T, var, indent)                   \\
        AGX_ ## T ## _print(fp, &(var), indent)

#define agx_pixel_format_print(fp, format) do {\\
   fprintf(fp, "%*sFormat: ", indent, ""); \\
   \\
   if (agx_channels_as_str((enum agx_channels)(format & 0x7F))) \\
      fputs(agx_channels_as_str((enum agx_channels)(format & 0x7F)), fp); \\
   else \\
      fprintf(fp, "unknown channels %02X", format & 0x7F); \\
   \\
   fputs(" ", fp); \\
   \\
   if (agx_texture_type_as_str((enum agx_texture_type)(format >> 7))) \\
      fputs(agx_texture_type_as_str((enum agx_texture_type)(format >> 7)), fp); \\
   else \\
      fprintf(fp, "unknown type %02X", format >> 7); \\
   \\
   fputs("\\n", fp); \\
} while(0) \\

"""

def to_alphanum(name):
    substitutions = {
        ' ': '_',
        '/': '_',
        '[': '',
        ']': '',
        '(': '',
        ')': '',
        '-': '_',
        ':': '',
        '.': '',
        ',': '',
        '=': '',
        '>': '',
        '#': '',
        '&': '',
        '*': '',
        '"': '',
        '+': '',
        '\'': '',
    }

    for i, j in substitutions.items():
        name = name.replace(i, j)

    return name

def safe_name(name):
    name = to_alphanum(name)
    if not name[0].isalpha():
        name = '_' + name

    return name

def prefixed_upper_name(prefix, name):
    if prefix:
        name = prefix + "_" + name
    return safe_name(name).upper()

def enum_name(name):
    return "{}_{}".format(global_prefix, safe_name(name)).lower()

def num_from_str(num_str):
    if num_str.lower().startswith('0x'):
        return int(num_str, base=16)
    else:
        assert(not num_str.startswith('0') and 'octals numbers not allowed')
        return int(num_str)

MODIFIERS = ["shr", "minus", "align", "log2"]

def parse_modifier(modifier):
    if modifier is None:
        return None

    for mod in MODIFIERS:
        if modifier[0:len(mod)] == mod:
            if mod == "log2":
                assert(len(mod) == len(modifier))
                return [mod]

            if modifier[len(mod)] == '(' and modifier[-1] == ')':
                ret = [mod, int(modifier[(len(mod) + 1):-1])]
                if ret[0] == 'align':
                    align = ret[1]
                    # Make sure the alignment is a power of 2
                    assert(align > 0 and not(align & (align - 1)));

                return ret

    print("Invalid modifier")
    assert(False)

class Field(object):
    def __init__(self, parser, attrs):
        self.parser = parser
        if "name" in attrs:
            self.name = safe_name(attrs["name"]).lower()
            self.human_name = attrs["name"]

        if ":" in str(attrs["start"]):
            (word, bit) = attrs["start"].split(":")
            self.start = (int(word) * 32) + int(bit)
        else:
            self.start = int(attrs["start"])

        self.end = self.start + int(attrs["size"]) - 1
        self.type = attrs["type"]

        if self.type == 'bool' and self.start != self.end:
            print("#error Field {} has bool type but more than one bit of size".format(self.name));

        if "prefix" in attrs:
            self.prefix = safe_name(attrs["prefix"]).upper()
        else:
            self.prefix = None

        if "exact" in attrs:
            self.exact = int(attrs["exact"])
        else:
            self.exact = None

        self.default = attrs.get("default")

        # Map enum values
        if self.type in self.parser.enums and self.default is not None:
            self.default = safe_name('{}_{}_{}'.format(global_prefix, self.type, self.default)).upper()

        self.modifier  = parse_modifier(attrs.get("modifier"))

    def emit_template_struct(self, dim):
        if self.type == 'address':
            type = 'uint64_t'
        elif self.type == 'bool':
            type = 'bool'
        elif self.type in ['float', 'lod']:
            type = 'float'
        elif self.type in ['uint', 'hex'] and self.end - self.start > 32:
            type = 'uint64_t'
        elif self.type == 'int':
            type = 'int32_t'
        elif self.type in ['uint', 'uint/float', 'Pixel Format', 'hex']:
            type = 'uint32_t'
        elif self.type in self.parser.structs:
            type = 'struct ' + self.parser.gen_prefix(safe_name(self.type.upper()))
        elif self.type in self.parser.enums:
            type = 'enum ' + enum_name(self.type)
        else:
            print("#error unhandled type: %s" % self.type)
            type = "uint32_t"

        print("   %-36s %s%s;" % (type, self.name, dim))

        for value in self.values:
            name = prefixed_upper_name(self.prefix, value.name)
            print("#define %-40s %d" % (name, value.value))

    def overlaps(self, field):
        return self != field and max(self.start, field.start) <= min(self.end, field.end)

class Group(object):
    def __init__(self, parser, parent, start, count, label):
        self.parser = parser
        self.parent = parent
        self.start = start
        self.count = count
        self.label = label
        self.size = 0
        self.length = 0
        self.fields = []

    def get_length(self):
        # Determine number of bytes in this group.
        calculated = max(field.end // 8 for field in self.fields) + 1 if len(self.fields) > 0 else 0
        if self.length > 0:
            assert(self.length >= calculated)
        else:
            self.length = calculated
        return self.length


    def emit_template_struct(self, dim):
        if self.count == 0:
            print("   /* variable length fields follow */")
        else:
            if self.count > 1:
                dim = "%s[%d]" % (dim, self.count)

            if len(self.fields) == 0:
                print("   int dummy;")

            for field in self.fields:
                if field.exact is not None:
                    continue

                field.emit_template_struct(dim)

    class Word:
        def __init__(self):
            self.size = 32
            self.contributors = []

    class FieldRef:
        def __init__(self, field, path, start, end):
            self.field = field
            self.path = path
            self.start = start
            self.end = end

    def collect_fields(self, fields, offset, path, all_fields):
        for field in fields:
            field_path = '{}{}'.format(path, field.name)
            field_offset = offset + field.start

            if field.type in self.parser.structs:
                sub_struct = self.parser.structs[field.type]
                self.collect_fields(sub_struct.fields, field_offset, field_path + '.', all_fields)
                continue

            start = field_offset
            end = offset + field.end
            all_fields.append(self.FieldRef(field, field_path, start, end))

    def collect_words(self, fields, offset, path, words):
        for field in fields:
            field_path = '{}{}'.format(path, field.name)
            start = offset + field.start

            if field.type in self.parser.structs:
                sub_fields = self.parser.structs[field.type].fields
                self.collect_words(sub_fields, start, field_path + '.', words)
                continue

            end = offset + field.end
            contributor = self.FieldRef(field, field_path, start, end)
            first_word = contributor.start // 32
            last_word = contributor.end // 32
            for b in range(first_word, last_word + 1):
                if not b in words:
                    words[b] = self.Word()
                words[b].contributors.append(contributor)

    def emit_pack_function(self):
        self.get_length()

        words = {}
        self.collect_words(self.fields, 0, '', words)

        # Validate the modifier is lossless
        for field in self.fields:
            if field.modifier is None:
                continue

            assert(field.exact is None)

            if field.modifier[0] == "shr":
                shift = field.modifier[1]
                mask = hex((1 << shift) - 1)
                print("   assert((values->{} & {}) == 0);".format(field.name, mask))
            elif field.modifier[0] == "minus":
                print("   assert(values->{} >= {});".format(field.name, field.modifier[1]))
            elif field.modifier[0] == "log2":
                print("   assert(util_is_power_of_two_nonzero(values->{}));".format(field.name))

        for index in range(self.length // 4):
            # Handle MBZ words
            if not index in words:
                print("   cl[%2d] = 0;" % index)
                continue

            word = words[index]

            word_start = index * 32

            v = None
            prefix = "   cl[%2d] =" % index

            for contributor in word.contributors:
                field = contributor.field
                name = field.name
                start = contributor.start
                end = contributor.end
                contrib_word_start = (start // 32) * 32
                start -= contrib_word_start
                end -= contrib_word_start

                value = str(field.exact) if field.exact is not None else "values->{}".format(contributor.path)
                if field.modifier is not None:
                    if field.modifier[0] == "shr":
                        value = "{} >> {}".format(value, field.modifier[1])
                    elif field.modifier[0] == "minus":
                        value = "{} - {}".format(value, field.modifier[1])
                    elif field.modifier[0] == "align":
                        value = "ALIGN_POT({}, {})".format(value, field.modifier[1])
                    elif field.modifier[0] == "log2":
                        value = "util_logbase2({})".format(value)

                if field.type in ["uint", "hex", "Pixel Format", "address"]:
                    s = "util_bitpack_uint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type in self.parser.enums:
                    s = "util_bitpack_uint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type == "int":
                    s = "util_bitpack_sint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type == "bool":
                    s = "util_bitpack_uint(%s, %d, %d)" % \
                        (value, start, end)
                elif field.type == "float":
                    assert(start == 0 and end == 31)
                    s = "util_bitpack_float({})".format(value)
                elif field.type == "lod":
                    assert(end - start + 1 == 10)
                    s = "__gen_pack_lod(%s, %d, %d)" % (value, start, end)
                else:
                    s = "#error unhandled field {}, type {}".format(contributor.path, field.type)

                if not s == None:
                    shift = word_start - contrib_word_start
                    if shift:
                        s = "%s >> %d" % (s, shift)

                    if contributor == word.contributors[-1]:
                        print("%s %s;" % (prefix, s))
                    else:
                        print("%s %s |" % (prefix, s))
                    prefix = "           "

            continue

    # Given a field (start, end) contained in word `index`, generate the 32-bit
    # mask of present bits relative to the word
    def mask_for_word(self, index, start, end):
        field_word_start = index * 32
        start -= field_word_start
        end -= field_word_start
        # Cap multiword at one word
        start = max(start, 0)
        end = min(end, 32 - 1)
        count = (end - start + 1)
        return (((1 << count) - 1) << start)

    def emit_unpack_function(self):
        # First, verify there is no garbage in unused bits
        words = {}
        self.collect_words(self.fields, 0, '', words)

        for index in range(self.length // 4):
            base = index * 32
            word = words.get(index, self.Word())
            masks = [self.mask_for_word(index, c.start, c.end) for c in word.contributors]
            mask = reduce(lambda x,y: x | y, masks, 0)

            ALL_ONES = 0xffffffff

            if mask != ALL_ONES:
                TMPL = '   if (((const uint32_t *) cl)[{}] & {}) fprintf(fp, "XXX: Unknown field of {} unpacked at word {}: got %X, bad mask %X\\n", ((const uint32_t *) cl)[{}], ((const uint32_t *) cl)[{}] & {});'
                print(TMPL.format(index, hex(mask ^ ALL_ONES), self.label, index, index, index, hex(mask ^ ALL_ONES)))

        fieldrefs = []
        self.collect_fields(self.fields, 0, '', fieldrefs)
        for fieldref in fieldrefs:
            field = fieldref.field
            convert = None

            args = []
            args.append('cl')
            args.append(str(fieldref.start))
            args.append(str(fieldref.end))

            if field.type in set(["uint", "uint/float", "address", "Pixel Format", "hex"]) | self.parser.enums:
                convert = "__gen_unpack_uint"
            elif field.type == "int":
                convert = "__gen_unpack_sint"
            elif field.type == "bool":
                convert = "__gen_unpack_uint"
            elif field.type == "float":
                convert = "__gen_unpack_float"
            elif field.type == "lod":
                convert = "__gen_unpack_lod"
            else:
                s = "/* unhandled field %s, type %s */\n" % (field.name, field.type)

            suffix = ""
            prefix = ""
            if field.modifier:
                if field.modifier[0] == "minus":
                    suffix = " + {}".format(field.modifier[1])
                elif field.modifier[0] == "shr":
                    suffix = " << {}".format(field.modifier[1])
                if field.modifier[0] == "log2":
                    prefix = "1 << "

            if field.type in self.parser.enums:
                prefix = f"(enum {enum_name(field.type)}) {prefix}"

            decoded = '{}{}({}){}'.format(prefix, convert, ', '.join(args), suffix)

            print('   values->{} = {};'.format(fieldref.path, decoded))
            if field.modifier and field.modifier[0] == "align":
                mask = hex(field.modifier[1] - 1)
                print('   assert(!(values->{} & {}));'.format(fieldref.path, mask))

    def emit_print_function(self):
        for field in self.fields:
            convert = None
            name, val = field.human_name, 'values->{}'.format(field.name)

            if field.type in self.parser.structs:
                pack_name = self.parser.gen_prefix(safe_name(field.type)).upper()
                print('   fprintf(fp, "%*s{}:\\n", indent, "");'.format(field.human_name))
                print("   {}_print(fp, &values->{}, indent + 2);".format(pack_name, field.name))
            elif field.type == "address":
                # TODO resolve to name
                print('   fprintf(fp, "%*s{}: 0x%" PRIx64 "\\n", indent, "", {});'.format(name, val))
            elif field.type in self.parser.enums:
                print('   if ({}_as_str({}))'.format(enum_name(field.type), val))
                print('     fprintf(fp, "%*s{}: %s\\n", indent, "", {}_as_str({}));'.format(name, enum_name(field.type), val))
                print('    else')
                print('     fprintf(fp, "%*s{}: unknown %X (XXX)\\n", indent, "", {});'.format(name, val))
            elif field.type == "int":
                print('   fprintf(fp, "%*s{}: %d\\n", indent, "", {});'.format(name, val))
            elif field.type == "bool":
                print('   fprintf(fp, "%*s{}: %s\\n", indent, "", {} ? "true" : "false");'.format(name, val))
            elif field.type in ["float", "lod"]:
                print('   fprintf(fp, "%*s{}: %f\\n", indent, "", {});'.format(name, val))
            elif field.type in ["uint", "hex"] and (field.end - field.start) >= 32:
                print('   fprintf(fp, "%*s{}: 0x%" PRIx64 "\\n", indent, "", {});'.format(name, val))
            elif field.type == "hex":
                print('   fprintf(fp, "%*s{}: 0x%" PRIx32 "\\n", indent, "", {});'.format(name, val))
            elif field.type in "Pixel Format":
                print('   agx_pixel_format_print(fp, {});'.format(val))
            elif field.type == "uint/float":
                print('   fprintf(fp, "%*s{}: 0x%X (%f)\\n", indent, "", {}, uif({}));'.format(name, val, val))
            else:
                print('   fprintf(fp, "%*s{}: %u\\n", indent, "", {});'.format(name, val))

class Value(object):
    def __init__(self, attrs):
        self.name = attrs["name"]
        self.value = int(attrs["value"], 0)

class Parser(object):
    def __init__(self):
        self.parser = xml.parsers.expat.ParserCreate()
        self.parser.StartElementHandler = self.start_element
        self.parser.EndElementHandler = self.end_element

        self.struct = None
        self.structs = {}
        # Set of enum names we've seen.
        self.enums = set()

    def gen_prefix(self, name):
        return '{}_{}'.format(global_prefix.upper(), name)

    def start_element(self, name, attrs):
        if name == "agxml":
            print(pack_header)
        elif name == "struct":
            name = attrs["name"]
            self.no_direct_packing = attrs.get("no-direct-packing", False)
            object_name = self.gen_prefix(safe_name(name.upper()))
            self.struct = object_name

            self.group = Group(self, None, 0, 1, name)
            if "size" in attrs:
                self.group.length = int(attrs["size"])
            self.group.align = int(attrs["align"]) if "align" in attrs else None
            self.structs[attrs["name"]] = self.group
        elif name == "field":
            self.group.fields.append(Field(self, attrs))
            self.values = []
        elif name == "enum":
            self.values = []
            self.enum = safe_name(attrs["name"])
            self.enums.add(attrs["name"])
            if "prefix" in attrs:
                self.prefix = attrs["prefix"]
            else:
                self.prefix= None
        elif name == "value":
            self.values.append(Value(attrs))

    def end_element(self, name):
        if name == "struct":
            self.emit_struct()
            self.struct = None
            self.group = None
        elif name  == "field":
            self.group.fields[-1].values = self.values
        elif name  == "enum":
            self.emit_enum()
            self.enum = None
        elif name == "agxml":
            print('#endif')

    def emit_header(self, name):
        default_fields = []
        for field in self.group.fields:
            if not type(field) is Field:
                continue
            if field.default is not None:
                default_fields.append("   .{} = {}".format(field.name, field.default))
            elif field.type in self.structs:
                default_fields.append("   .{} = {{ {}_header }}".format(field.name, self.gen_prefix(safe_name(field.type.upper()))))

        print('#define %-40s\\' % (name + '_header'))
        if default_fields:
            print(",  \\\n".join(default_fields))
        else:
            print('   0')
        print('')

    def emit_template_struct(self, name, group):
        print("struct %s {" % name)
        group.emit_template_struct("")
        print("};\n")

    def emit_pack_function(self, name, group):
        print("static inline void\n%s_pack(uint32_t * restrict cl,\n%sconst struct %s * restrict values)\n{" %
              (name, ' ' * (len(name) + 6), name))

        group.emit_pack_function()

        print("}\n\n")

        print('#define {} {}'.format (name + "_LENGTH", self.group.length))
        if self.group.align != None:
            print('#define {} {}'.format (name + "_ALIGN", self.group.align))
        print('struct {}_packed {{ uint32_t opaque[{}]; }};'.format(name.lower(), self.group.length // 4))

    def emit_unpack_function(self, name, group):
        print("static inline void")
        print("%s_unpack(FILE *fp, const uint8_t * restrict cl,\n%sstruct %s * restrict values)\n{" %
              (name.upper(), ' ' * (len(name) + 8), name))

        group.emit_unpack_function()

        print("}\n")

    def emit_print_function(self, name, group):
        print("static inline void")
        print("{}_print(FILE *fp, const struct {} * values, unsigned indent)\n{{".format(name.upper(), name))

        group.emit_print_function()

        print("}\n")

    def emit_struct(self):
        name = self.struct

        self.emit_template_struct(self.struct, self.group)
        self.emit_header(name)
        if self.no_direct_packing == False:
            self.emit_pack_function(self.struct, self.group)
            self.emit_unpack_function(self.struct, self.group)
        self.emit_print_function(self.struct, self.group)

    def enum_prefix(self, name):
        return 

    def emit_enum(self):
        e_name = enum_name(self.enum)
        prefix = e_name if self.enum != 'Format' else global_prefix
        print('enum {} {{'.format(e_name))

        for value in self.values:
            name = '{}_{}'.format(prefix, value.name)
            name = safe_name(name).upper()
            print('        % -36s = %6d,' % (name, value.value))
        print('};\n')

        print("static inline const char *")
        print("{}_as_str(enum {} imm)\n{{".format(e_name.lower(), e_name))
        print("    switch (imm) {")
        for value in self.values:
            name = '{}_{}'.format(prefix, value.name)
            name = safe_name(name).upper()
            print('    case {}: return "{}";'.format(name, value.name))
        print('    default: break;')
        print("    }")
        print("    return NULL;")
        print("}\n")

    def parse(self, filename):
        file = open(filename, "rb")
        self.parser.ParseFile(file)
        file.close()

if len(sys.argv) < 2:
    print("No input xml file specified")
    sys.exit(1)

input_file = sys.argv[1]

p = Parser()
p.parse(input_file)
