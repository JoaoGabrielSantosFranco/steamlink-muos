"""Extract embedded protobuf FileDescriptorProto blobs from a binary and dump them as .proto files.

Usage: uv run --with protobuf python -I tools/extract_protos.py <binary> <out_dir>
"""
import pathlib
import re
import sys

from google.protobuf import descriptor_pb2


def read_varint(buf, pos):
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def find_descriptors(data):
    # A serialized FileDescriptorProto starts with field 1 (name): 0x0A <len> "<name>.proto"
    for m in re.finditer(rb"\x0a([\x01-\x7f])([a-z0-9_/]+\.proto)", data):
        if m.group(1)[0] != len(m.group(2)):
            continue
        start = m.start()
        best = None
        # Walk top-level fields until they stop looking valid; keep the longest clean parse.
        pos = start
        while pos < len(data):
            try:
                tag, p = read_varint(data, pos)
                field, wire = tag >> 3, tag & 7
                if field == 0 or field > 20:
                    break
                if wire == 2:
                    ln, p = read_varint(data, p)
                    p += ln
                elif wire == 0:
                    _, p = read_varint(data, p)
                else:
                    break
            except IndexError:
                break
            if p > len(data):
                break
            pos = p
            fd = descriptor_pb2.FileDescriptorProto()
            try:
                fd.ParseFromString(data[start:pos])
                best = fd
            except Exception:
                break
        if best is not None:
            yield best


LABEL = {1: "optional", 2: "required", 3: "repeated"}
TYPE = {
    1: "double", 2: "float", 3: "int64", 4: "uint64", 5: "int32", 6: "fixed64",
    7: "fixed32", 8: "bool", 9: "string", 12: "bytes", 13: "uint32", 15: "sfixed32",
    16: "sfixed64", 17: "sint32", 18: "sint64",
}


def fmt_field(f, indent):
    typ = f.type_name.lstrip(".") if f.type_name else TYPE[f.type]
    opts = []
    if f.HasField("default_value"):
        dv = f.default_value
        if f.type == 9:
            dv = '"%s"' % dv
        opts.append("default = %s" % dv)
    opt = " [%s]" % ", ".join(opts) if opts else ""
    return "%s%s %s %s = %d%s;\n" % (indent, LABEL[f.label], typ, f.name, f.number, opt)


def fmt_enum(e, indent):
    out = "%senum %s {\n" % (indent, e.name)
    for v in e.value:
        out += "%s\t%s = %d;\n" % (indent, v.name, v.number)
    return out + indent + "}\n\n"


def fmt_message(m, indent=""):
    out = "%smessage %s {\n" % (indent, m.name)
    for e in m.enum_type:
        out += fmt_enum(e, indent + "\t")
    for n in m.nested_type:
        out += fmt_message(n, indent + "\t")
    for f in m.field:
        out += fmt_field(f, indent + "\t")
    return out + indent + "}\n\n"


def fmt_file(fd):
    out = 'syntax = "%s";\n' % (fd.syntax or "proto2")
    for dep in fd.dependency:
        out += 'import "%s";\n' % dep
    if fd.package:
        out += "package %s;\n" % fd.package
    out += "\n"
    for e in fd.enum_type:
        out += fmt_enum(e, "")
    for m in fd.message_type:
        out += fmt_message(m)
    for s in fd.service:
        out += "service %s {\n" % s.name
        for meth in s.method:
            out += "\trpc %s (%s) returns (%s);\n" % (
                meth.name, meth.input_type.lstrip("."), meth.output_type.lstrip("."))
        out += "}\n\n"
    return out


def main():
    data = pathlib.Path(sys.argv[1]).read_bytes()
    out_dir = pathlib.Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    seen = {}
    for fd in find_descriptors(data):
        if fd.name not in seen or fd.ByteSize() > seen[fd.name].ByteSize():
            seen[fd.name] = fd
    for name, fd in sorted(seen.items()):
        path = out_dir / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(fmt_file(fd))
        print("%-55s %4d messages %3d enums" % (name, len(fd.message_type), len(fd.enum_type)))


if __name__ == "__main__":
    main()
