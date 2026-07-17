// tests/mtp_protocol_test.cpp
//
// Host suite for the MTP wire format. mtp_data.hpp is deliberately free of USB
// and libnx — its own header says so — which makes it the one part of the MTP
// responder that can be checked anywhere but on a Switch. mtp_server (the
// transport) cannot be, and nothing here pretends otherwise.
//
// The centrepiece is ObjectPropList parsing, added for slice 4c. The 64-bit
// object size arrives in SendObjectPropList's COMMAND PARAMETERS (high/low
// across params 4 and 5), not in this dataset — but the dataset is where the
// filename comes from, it is entirely host-supplied, and it is walked by
// datatype-implied lengths, which is a decoder waiting to be fed something
// malformed.

#include "services/mtp_data.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Services::Mtp;

static int g_checks = 0;
#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);                         \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

// ── Builders: encode a dataset the way a host would ──────────────────────────

struct PropListBuilder {
    Writer   w;
    uint32_t count = 0;
    std::vector<uint8_t> body;

    void add_u64(uint16_t code, uint64_t v) {
        Writer e;
        e.u32(0);              // handle: 0 while the object is being created
        e.u16(code);
        e.u16(DataType::UInt64);
        e.u64(v);
        append(e);
    }
    void add_u32(uint16_t code, uint32_t v) {
        Writer e; e.u32(0); e.u16(code); e.u16(DataType::UInt32); e.u32(v); append(e);
    }
    void add_u16(uint16_t code, uint16_t v) {
        Writer e; e.u32(0); e.u16(code); e.u16(DataType::UInt16); e.u16(v); append(e);
    }
    void add_str(uint16_t code, const std::string& v) {
        Writer e; e.u32(0); e.u16(code); e.u16(DataType::Str); e.str(v); append(e);
    }
    void add_raw_type(uint16_t code, uint16_t type) {   // datatype with no value
        Writer e; e.u32(0); e.u16(code); e.u16(type); append(e);
    }
    void append(const Writer& e) {
        body.insert(body.end(), e.data().begin(), e.data().end());
        count++;
    }
    std::vector<uint8_t> build() const {
        Writer out;
        out.u32(count);
        out.raw(body.data(), body.size());
        return out.data();
    }
};

// ── Tests ────────────────────────────────────────────────────────────────────

// The whole reason 4c needs this operation: a size that cannot fit in the u32
// SendObjectInfo carries. If this rounds, wraps, or truncates, a 6 GB transfer
// silently becomes a 1.7 GB one.
static void test_u64_survives_the_round_trip() {
    const uint64_t sizes[] = {
        0,
        4ull * 1024 * 1024 * 1024 - 1,      // last value a u32 could hold
        4ull * 1024 * 1024 * 1024,          // where SendObjectInfo gives up
        26ull * 1024 * 1024 * 1024,         // a real decompressed XCI
        0xFFFFFFFFFFFFFFFFull,              // saturated
    };
    for (uint64_t sz : sizes) {
        PropListBuilder b;
        b.add_str(ObjProp::ObjectFileName, "title.xci");
        b.add_u64(ObjProp::ObjectSize, sz);

        const auto data = b.build();
        std::vector<ObjectProp> props;
        CHECK(parse_object_prop_list(data.data(), data.size(), props), "parses");
        CHECK(props.size() == 2, "two elements");

        const ObjectProp* p = find_prop(props, ObjProp::ObjectSize);
        CHECK(p != nullptr, "ObjectSize present");
        CHECK(p->num == sz, "u64 size survives exactly — no truncation to 32 bits");
    }
    std::printf("  ok: u64 ObjectSize exact from 0 through 2^64-1 (incl. >4GiB)\n");
}

// The filename is the only thing the gate has to work with.
static void test_filename_round_trip_including_non_ascii() {
    const char* names[] = {
        "title.nsp",
        "Game [0100ABCDEF012000][v0].nsz",
        "\xc3\x9c" "bergame.xci",                   // U+00DC
        "\xe6\xb8\xb8\xe6\x88\xb2.nsp",             // CJK
        "\xf0\x9f\x8e\xae.nsz",                     // emoji: surrogate pair
    };
    for (const char* n : names) {
        PropListBuilder b;
        b.add_str(ObjProp::ObjectFileName, n);
        const auto data = b.build();
        std::vector<ObjectProp> props;
        CHECK(parse_object_prop_list(data.data(), data.size(), props), "parses");
        const ObjectProp* p = find_prop(props, ObjProp::ObjectFileName);
        CHECK(p && p->text == n, "UTF-16LE filename round-trips, surrogates included");
    }
    std::printf("  ok: filenames round-trip through UTF-16LE (ASCII/latin/CJK/emoji)\n");
}

// A realistic host dataset: mixed datatypes, in any order.
static void test_mixed_realistic_dataset() {
    PropListBuilder b;
    b.add_u32(ObjProp::StorageId, 0x00020001);
    b.add_u16(ObjProp::ObjectFormat, Fmt::Undefined);
    b.add_str(ObjProp::ObjectFileName, "big.xci");
    b.add_u64(ObjProp::ObjectSize, 26ull * 1024 * 1024 * 1024);
    b.add_u32(ObjProp::ParentObject, 0);
    b.add_str(ObjProp::DateModified, "20260716T120000");

    const auto data = b.build();
    std::vector<ObjectProp> props;
    CHECK(parse_object_prop_list(data.data(), data.size(), props), "parses");
    CHECK(props.size() == 6, "all six elements");
    CHECK(find_prop(props, ObjProp::ObjectFileName)->text == "big.xci", "filename found by code");
    CHECK(find_prop(props, ObjProp::ObjectSize)->num == 26ull * 1024 * 1024 * 1024, "size found");
    CHECK(find_prop(props, ObjProp::StorageId)->num == 0x00020001, "u32 zero-extends");
    CHECK(find_prop(props, 0xDEAD) == nullptr, "absent property returns nullptr");
    std::printf("  ok: mixed-datatype dataset parses, properties located by code\n");
}

// Every byte here is host-supplied. None of it may be trusted.
static void test_malformed_datasets_are_rejected() {
    PropListBuilder b;
    b.add_str(ObjProp::ObjectFileName, "x.nsp");
    b.add_u64(ObjProp::ObjectSize, 12345);
    const auto good = b.build();

    // Truncation at every possible length must be refused, never read past.
    for (size_t cut = 0; cut < good.size(); ++cut) {
        std::vector<ObjectProp> props;
        std::vector<uint8_t> t(good.begin(), good.begin() + cut);
        CHECK(!parse_object_prop_list(t.data(), t.size(), props),
              "every truncation is refused");
    }
    { // count claims more elements than are present
        Writer w; w.u32(9999);
        std::vector<ObjectProp> props;
        CHECK(!parse_object_prop_list(w.data().data(), w.data().size(), props),
              "implausible element count refused");
    }
    { // empty buffer
        std::vector<ObjectProp> props;
        CHECK(!parse_object_prop_list(nullptr, 0, props), "empty dataset refused");
    }
    { // count == 0 is legal, just useless
        Writer w; w.u32(0);
        std::vector<ObjectProp> props;
        CHECK(parse_object_prop_list(w.data().data(), w.data().size(), props),
              "zero elements is well-formed");
        CHECK(props.empty(), "and yields nothing");
    }
    std::printf("  ok: truncated / over-counted / empty datasets all refused\n");
}

// An unknown datatype has an unknown length, so nothing after it can be located.
// Guessing would desynchronise the parse and hand back plausible garbage.
static void test_unknown_datatype_reports_its_index() {
    PropListBuilder b;
    b.add_str(ObjProp::ObjectFileName, "x.nsp");   // 0
    b.add_u32(ObjProp::StorageId, 1);              // 1
    b.add_raw_type(ObjProp::Name, 0x4242);         // 2: bogus datatype
    b.add_u64(ObjProp::ObjectSize, 99);            // 3: unreachable

    const auto data = b.build();
    std::vector<ObjectProp> props;
    uint32_t bad = 0xFFFFFFFF;
    CHECK(!parse_object_prop_list(data.data(), data.size(), props, &bad),
          "unknown datatype refused rather than guessed");
    CHECK(bad == 2, "the offending element index is reported back for the response");
    std::printf("  ok: unknown datatype refused, index reported (element %u)\n", bad);
}

// 128-bit properties are consumed but not represented. A host may send
// PersistentUniqueObjectIdentifier; it must not derail the parse.
static void test_uint128_is_stepped_over() {
    Writer e;
    e.u32(0); e.u16(ObjProp::PersistentUid); e.u16(DataType::UInt128);
    e.u64(0x1122334455667788ull); e.u64(0x99AABBCCDDEEFF00ull);

    PropListBuilder b;
    b.append(e);
    b.add_u64(ObjProp::ObjectSize, 777);   // must still be reachable

    const auto data = b.build();
    std::vector<ObjectProp> props;
    CHECK(parse_object_prop_list(data.data(), data.size(), props), "128-bit prop parses");
    const ObjectProp* p = find_prop(props, ObjProp::ObjectSize);
    CHECK(p && p->num == 777, "the property AFTER a 128-bit value is still found");
    std::printf("  ok: 128-bit properties are stepped over without derailing the parse\n");
}

// GetObjectPropDesc: a host asks for a description of EVERY property we claim
// to support, and libmtp abandons the entire send if one is missing — the
// failure reads "could not get property description" and names nothing useful.
// So the contract is: everything GetObjectPropsSupported advertises must be
// describable here, and must be decodable by parse_object_prop_list().
static void test_every_advertised_property_has_a_description() {
    // Exactly what mtp_server's GetObjectPropsSupported answers.
    const uint16_t advertised[] = {
        ObjProp::StorageId, ObjProp::ObjectFormat, ObjProp::ProtectionStatus,
        ObjProp::ObjectSize, ObjProp::ObjectFileName, ObjProp::ParentObject,
    };
    for (uint16_t code : advertised) {
        Writer w;
        CHECK(build_object_prop_desc(code, w), "every advertised property is describable");

        // Walk the dataset back the way a host does.
        Reader r(w.data().data(), w.size());
        uint16_t got_code = 0, got_type = 0;
        uint8_t  get_set = 0;
        CHECK(r.u16(got_code) && r.u16(got_type) && r.u8(get_set), "desc header reads");
        CHECK(got_code == code, "code echoes back");
        CHECK(get_set <= 1, "get_set is 0 or 1");

        switch (got_type) {   // default value, width implied by datatype
            case DataType::UInt16: { uint16_t v; CHECK(r.u16(v), "u16 default"); break; }
            case DataType::UInt32: { uint32_t v; CHECK(r.u32(v), "u32 default"); break; }
            case DataType::UInt64: { uint64_t v; CHECK(r.u64(v), "u64 default"); break; }
            case DataType::Str:    { std::string v; CHECK(r.str(v), "str default");
                                     CHECK(v.empty(), "empty default filename"); break; }
            default: CHECK(false, "advertised property has a describable datatype");
        }
        uint32_t group = 0; uint8_t form = 0;
        CHECK(r.u32(group) && r.u8(form), "group code and form flag read");
        CHECK(form == 0, "form flag none: no form field follows");
        CHECK(r.offset() == w.size(), "the dataset is consumed EXACTLY — no trailing bytes");
    }
    std::printf("  ok: all 6 advertised properties describable, datasets exact\n");
}

// The two halves must agree. A property we describe but cannot decode would make
// the host send a value we then reject; the dataset it arrives in dies with it.
static void test_describable_implies_decodable() {
    const uint16_t advertised[] = {
        ObjProp::StorageId, ObjProp::ObjectFormat, ObjProp::ProtectionStatus,
        ObjProp::ObjectSize, ObjProp::ObjectFileName, ObjProp::ParentObject,
    };
    for (uint16_t code : advertised) {
        Writer d;
        CHECK(build_object_prop_desc(code, d), "describable");
        Reader r(d.data().data(), d.size());
        uint16_t c2 = 0, type = 0;
        r.u16(c2); r.u16(type);

        // Encode a value of that datatype and prove the parser handles it.
        PropListBuilder b;
        switch (type) {
            case DataType::UInt16: b.add_u16(code, 0x1234); break;
            case DataType::UInt32: b.add_u32(code, 0xDEADBEEF); break;
            case DataType::UInt64: b.add_u64(code, 0x1122334455667788ull); break;
            case DataType::Str:    b.add_str(code, "probe.nsp"); break;
            default: CHECK(false, "unreachable datatype");
        }
        const auto data = b.build();
        std::vector<ObjectProp> props;
        CHECK(parse_object_prop_list(data.data(), data.size(), props),
              "a described datatype is always a decodable datatype");
        CHECK(find_prop(props, code) != nullptr, "and the property is found");
    }
    // The inverse: a property we never advertise has no description.
    Writer w;
    CHECK(!build_object_prop_desc(0xDC44, w), "unadvertised property has no description");
    CHECK(!build_object_prop_desc(0x0000, w), "nonsense property code refused");
    std::printf("  ok: describable <-> decodable for every advertised property\n");
}

// Pin the header codec down too — it frames every container.
static void test_header_codec() {
    Writer w;
    const std::vector<uint8_t> payload = {1, 2, 3, 4};
    const auto c = make_data(Op::SendObjectPropList, 0x1234, payload);
    Header h;
    CHECK(parse_header(c.data(), c.size(), h), "header parses");
    CHECK(h.length == kHeaderSize + payload.size(), "length includes the header");
    CHECK(h.type == Type::Data, "type is Data");
    CHECK(h.code == 0x9808, "SendObjectPropList is 0x9808");
    CHECK(h.transaction_id == 0x1234, "transaction id preserved");
    CHECK(!parse_header(c.data(), kHeaderSize - 1, h), "a short header is refused");
    std::printf("  ok: container header codec round-trips, short headers refused\n");
}

int main() {
    std::printf("MTP protocol harness (wire format + ObjectPropList)\n");
    test_u64_survives_the_round_trip();
    test_filename_round_trip_including_non_ascii();
    test_mixed_realistic_dataset();
    test_malformed_datasets_are_rejected();
    test_unknown_datatype_reports_its_index();
    test_uint128_is_stepped_over();
    test_every_advertised_property_has_a_description();
    test_describable_implies_decodable();
    test_header_codec();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
